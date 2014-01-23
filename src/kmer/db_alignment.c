#include "global.h"
#include "db_alignment.h"
#include "seq_reader.h"


#define INIT_BUFLEN 1024

// Estimate initial memory required
size_t db_alignment_est_mem()
{
  return (sizeof(size_t)+sizeof(dBNode))*INIT_BUFLEN;
}

void db_alignment_alloc(dBAlignment *alignment)
{
  db_node_buf_alloc(&alignment->nodes, INIT_BUFLEN);
  uint32_buf_alloc(&alignment->gaps, INIT_BUFLEN);
}

void db_alignment_dealloc(dBAlignment *alignment)
{
  db_node_buf_dealloc(&alignment->nodes);
  uint32_buf_dealloc(&alignment->gaps);
}

// Returns number of kmers lost from the end
size_t db_alignment_from_read(dBAlignment *alignment, const read_t *r,
                              uint8_t qcutoff, uint8_t hp_cutoff,
                              const dBGraph *db_graph)
{
  size_t contig_start, contig_end = 0, search_start = 0, exp_kmer_offset = 0;
  const size_t kmer_size = db_graph->kmer_size;

  BinaryKmer bkmer, tmp_key;
  Nucleotide nuc;
  hkey_t node;
  size_t offset, nxtbse;

  dBNodeBuffer *nodes = &alignment->nodes;
  uint32Buffer *gaps = &alignment->gaps;

  assert(nodes->len == gaps->len);
  size_t n = gaps->len;

  db_node_buf_ensure_capacity(nodes, n + r->seq.end);
  uint32_buf_ensure_capacity(gaps, n + r->seq.end);

  while((contig_start = seq_contig_start(r, search_start, kmer_size,
                                         qcutoff, hp_cutoff)) < r->seq.end)
  {
    contig_end = seq_contig_end(r, contig_start, kmer_size,
                                qcutoff, hp_cutoff, &search_start);

    const char *contig = r->seq.b + contig_start;
    size_t contig_len = contig_end - contig_start;

    bkmer = binary_kmer_from_str(contig, kmer_size);
    bkmer = binary_kmer_right_shift_one_base(bkmer);

    for(offset=0, nxtbse=kmer_size-1; nxtbse < contig_len; nxtbse++,offset++)
    {
      nuc = dna_char_to_nuc(contig[nxtbse]);
      bkmer = binary_kmer_left_shift_add(bkmer, kmer_size, nuc);
      tmp_key = db_node_get_key(bkmer, kmer_size);
      node = hash_table_find(&db_graph->ht, tmp_key);

      if(node != HASH_NOT_FOUND)
      {
        nodes->data[n].key = node;
        nodes->data[n].orient = db_node_get_orientation(bkmer, tmp_key);
        gaps->data[n] = (uint32_t)(offset - exp_kmer_offset);
        exp_kmer_offset = offset+1;
        n++;
      }
      else alignment->seq_gaps = true;
    }
  }

  nodes->len = gaps->len = n;

  return r->seq.end - contig_end;
}


void db_alignment_from_reads(dBAlignment *alignment,
                             const read_t *r1, const read_t *r2,
                             uint8_t qcutoff1, uint8_t qcutoff2,
                             uint8_t hp_cutoff, const dBGraph *db_graph)
{
  db_node_buf_reset(&alignment->nodes);
  uint32_buf_reset(&alignment->gaps);
  alignment->seq_gaps = false;
  alignment->r2enderr = 0;
  alignment->passed_r2 = (r2 != NULL);

  alignment->r1enderr = db_alignment_from_read(alignment, r1,
                                               qcutoff1, hp_cutoff, db_graph);

  alignment->r2strtidx = alignment->nodes.len;

  if(r2 != NULL) {
    alignment->r2enderr = db_alignment_from_read(alignment, r2,
                                                 qcutoff2, hp_cutoff, db_graph);
  }

  alignment->used_r1 = (alignment->r1enderr < r1->seq.end);
  alignment->used_r2 = (r2 != NULL && alignment->r2enderr < r2->seq.end);
}

// Returns index of node just after next gap,
// or aln->nodes.len if no more gaps
size_t db_alignment_next_gap(const dBAlignment *aln, size_t start)
{
  size_t i, end = aln->nodes.len;

  if(aln->used_r1 && aln->used_r2 && start < aln->r2strtidx)
    end = aln->r2strtidx;

  for(i = start+1; i < end && aln->gaps.data[i] == 0; i++) {}
  return i;
}

// This is for debugging
#include <pthread.h>

void db_alignment_print(const dBAlignment *aln, const dBGraph *db_graph)
{
  pthread_mutex_lock(&biglock);

  printf("dBAlignment:\n");
  size_t i, start = 0, end = db_alignment_next_gap(aln, 0);
  while(start < aln->nodes.len)
  {
    if(start == aln->r2strtidx)
      printf("    gap: %zu -[ins]- %u\n", aln->r1enderr, aln->gaps.data[start]);
    else if(start == 0)
      printf("    start gap: %u\n", aln->gaps.data[start]);
    else
      printf("    gap: %u\n", aln->gaps.data[start]);

    printf("  %zu nodes\n", end-start);

    for(i = start; i < end; i++)
      printf(" %zu:%i", (size_t)aln->nodes.data[i].key, (int)aln->nodes.data[i].orient);
    printf(": ");
    db_nodes_print(aln->nodes.data+start,end-start,db_graph,stdout);
    printf("\n");

    start = end;
    end = db_alignment_next_gap(aln, end);
  }
  if(aln->passed_r2) {
    if(!aln->used_r2) printf("    [ins] unused r2: %u\n", aln->gaps.data[end]);
    else printf(" end gap: %zu\n", aln->r2enderr);
  }
  else printf(" end gap: %zu\n", aln->r1enderr);

  pthread_mutex_unlock(&biglock);
}