#include "global.h"
#include <ctype.h> // toupper
#include "db_graph.h"
#include "db_node.h"
#include "util.h"

// For a given kmer, get the BinaryKmer 'key':
// the lower of the kmer vs reverse complement of itself
// kmer and kmer_key must NOT point to overlapping memory
BinaryKmer bkmer_get_key(const BinaryKmer bkmer, size_t kmer_size)
{
  BinaryKmer bkey;

  // Get first and last nucleotides
  Nucleotide first = binary_kmer_first_nuc(bkmer, kmer_size);
  Nucleotide last = binary_kmer_last_nuc(bkmer);
  Nucleotide rev_last = dna_nuc_complement(last);

  if(first < rev_last) return bkmer;

  // Don't know which is going to be correct -- this will happen 1 in 4 times
  bkey = binary_kmer_reverse_complement(bkmer, kmer_size);
  return (binary_kmer_less_than(bkmer, bkey) ? bkmer : bkey);
}

//
// Edges
//

// Edges restricted to this colour, only in one direction (node.orient)
Edges db_node_edges_in_col(dBNode node, size_t col, const dBGraph *db_graph)
{
  if(db_graph->num_edge_cols == db_graph->num_of_cols) {
    Edges edges = db_node_get_edges(db_graph, node.key, col);
    return edges_mask_orientation(edges, node.orient);
  }

  // Edges are merged into one colour
  ctx_assert(db_graph->num_edge_cols == 1);
  Edges edges = db_node_get_edges(db_graph, node.key, 0);

  // Check which next nodes are in the given colour
  BinaryKmer bkmer = db_node_get_bkmer(db_graph, node.key);
  dBNode nodes[4];
  Nucleotide nucs[4];
  size_t i, n;

  n = db_graph_next_nodes(db_graph, bkmer, node.orient,
                          edges, nodes, nucs);

  edges = 0;
  if(db_graph->node_in_cols != NULL) {
    for(i = 0; i < n; i++)
      if(db_node_has_col(db_graph, nodes[i].key, col))
        edges = edges_set_edge(edges, nucs[i], node.orient);
  }
  else if(db_graph->col_covgs != NULL) {
    for(i = 0; i < n; i++)
      if(db_node_col_covg(db_graph, nodes[i].key, col) > 0)
        edges = edges_set_edge(edges, nucs[i], node.orient);
  }
  else ctx_assert(0);

  return edges;
}

bool edges_has_precisely_one_edge(Edges edges, Orientation orientation,
                                     Nucleotide *nucleotide)
{
  edges = edges_with_orientation(edges, orientation);
  if(edges == 0) return false;

  // bit-hack: b == 0 iff number of edges is 1
  // 1100 & 1011 => 1000
  // 0100 & 0011 => 0000
  int b = edges & (edges - 1);

  // __builtin_ctz returns number of trailing zeros
  // 0b0001 returns 0
  // 0b0010 returns 1
  // 0b0100 returns 2
  // 0b1000 returns 3
  *nucleotide = (Nucleotide)__builtin_ctz(edges);

  return (b == 0);
}

//
// Edges per colour
//

Edges edges_get_union(const Edges *edges_arr, size_t num)
{
  if(num == 1) return edges_arr[0];

  // Slow version
  // Edges edges = 0; size_t i;
  // for(i = 0; i < NUM_OF_COLOURS; i++) edges |= edges_arr[i];
  // return edges;

  // Faster version deals with 8 bytes at a time
  uint64_t i, edges = 0, tmp, end = 8*(num/8);

  for(i = 0; i < end; i += 8) {
    memcpy(&tmp, edges_arr+i, sizeof(uint64_t));
    edges |= tmp;
  }

  memcpy(&tmp, edges_arr+i, sizeof(uint64_t));
  edges |= tmp & (~(uint64_t)0 >> (64-(8*(num % 8))));

  // with unaligned memory access
  // const uint64_t *ptr = (const uint64_t*)((size_t)edges_arr);
  // for(i = 0; i < num / 8; i++, ptr++) edges |= *ptr;
  // edges |= *ptr & (~(uint64_t)0 >> (64-(8*(num % 8))));

  edges |= edges >> 32;
  edges |= edges >> 16;
  edges |= edges >> 8;
  return edges & 0xFF;
}

// kmer_col_edge_str should be 9 chars long
// Return pointer to kmer_col_edge_str
char* db_node_get_edges_str(Edges edges, char* kmer_col_edge_str)
{
  int i;
  char str[] = "acgt";

  char left = edges >> 4;
  left = (char)rev_nibble(left);
  char right = edges & 0xf;

  for(i = 0; i < 4; i++)
    kmer_col_edge_str[i] = (left & (0x1 << i) ? str[i] : '.');

  for(i = 0; i < 4; i++)
    kmer_col_edge_str[i+4] = (char)toupper(right & (0x1 << i) ? str[i] : '.');

  kmer_col_edge_str[8] = '\0';

  return kmer_col_edge_str;
}

//
// Coverages
//

static inline void safe_add_covg(Covg *a, Covg b)
{
  uint64_t y = (uint64_t)*a+b;
  *a = y > COVG_MAX ? COVG_MAX : (Covg)y;
}

void db_node_add_col_covg(dBGraph *graph, hkey_t hkey, Colour col, Covg update)
{
  safe_add_covg(&db_node_col_covg(graph,col,hkey), update);
}

void db_node_increment_coverage(dBGraph *graph, hkey_t hkey, Colour col)
{
  safe_add_covg(&db_node_col_covg(graph,col,hkey), 1);
}

// Thread safe, overflow safe, coverage increment
void db_node_increment_coverage_mt(dBGraph *graph, hkey_t hkey, Colour col)
{
  Covg v;
  while((v = db_node_col_covg(graph,col,hkey)) < COVG_MAX &&
        !__sync_bool_compare_and_swap(&db_node_col_covg(graph,col,hkey), v, v+1));
}

Covg db_node_sum_covg(const dBGraph *graph, hkey_t hkey)
{
  const Covg *covgs = &db_node_col_covg(graph,0,hkey);
  Covg sum_covg = 0;
  size_t col;

  for(col = 0; col < graph->num_of_cols; col++)
    safe_add_covg(&sum_covg, covgs[col]);

  return sum_covg;
}

//
// dBNode reversal and shifting
//

// Reverse ordering without changing node orientation
void db_nodes_reverse(dBNode *nlist, size_t n)
{
  size_t i, j;
  dBNode tmp;
  if(n <= 1) return;
  for(i = 0, j = n-1; i < j; i++, j--) {
    SWAP(nlist[i], nlist[j], tmp);
  }
}

// see http://www.geeksforgeeks.org/array-rotation/
void db_nodes_left_shift(dBNode *nlist, size_t n, size_t shift)
{
  if(n == 0 || shift == 0) return;

  // Method A) Using three reverse operations
  // db_nodes_reverse(nlist, shift);
  // db_nodes_reverse(nlist+shift, n-shift);
  // db_nodes_reverse(nlist, n);

  // Method B) Using GCD
  size_t i, j, k, gcd = calc_GCD(n, shift);
  dBNode tmp;

  // Copy from k -> j, stop if k == i, since nlist[i] already overwritten
  for(i = 0; i < gcd; i++) {
    tmp = nlist[i];
    for(j = i; 1; j = k) {
      k = j+shift;
      if(k >= n) k -= n;
      if(k == i) break;
      nlist[j] = nlist[k];
    }
    nlist[j] = tmp;
  }
}

//
// dBNode array printing
//

void db_nodes_to_str(const dBNode *nodes, size_t num,
                     const dBGraph *db_graph, char *str)
{
  if(num == 0) return;

  size_t i;
  size_t kmer_size = db_graph->kmer_size;
  BinaryKmer bkmer = db_node_get_bkmer(db_graph, nodes[0].key);
  Nucleotide nuc;

  binary_kmer_to_str(bkmer, kmer_size, str);
  if(nodes[0].orient == REVERSE) dna_reverse_complement_str(str, kmer_size);

  for(i = 1; i < num; i++) {
    nuc = db_node_get_last_nuc(nodes[i], db_graph);
    str[kmer_size+i-1] = dna_nuc_to_char(nuc);
  }

  str[kmer_size+num-1] = '\0';
}

void db_nodes_print(const dBNode *nodes, size_t num,
                    const dBGraph *db_graph, FILE *out)
{
  const size_t kmer_size = db_graph->kmer_size;
  size_t i;
  Nucleotide nuc;
  BinaryKmer bkmer;
  char tmp[MAX_KMER_SIZE+1];

  bkmer = db_graph_oriented_bkmer(db_graph, nodes[0]);
  binary_kmer_to_str(bkmer, kmer_size, tmp);
  fputs(tmp, out);

  for(i = 1; i < num; i++) {
    nuc = db_node_get_last_nuc(nodes[i], db_graph);
    fputc(dna_nuc_to_char(nuc), out);
  }
}

void db_nodes_gzprint(const dBNode *nodes, size_t num,
                      const dBGraph *db_graph, gzFile out)
{
  size_t i, kmer_size = db_graph->kmer_size;
  Nucleotide nuc;
  BinaryKmer bkmer;
  char tmp[MAX_KMER_SIZE+1];

  bkmer = db_graph_oriented_bkmer(db_graph, nodes[0]);
  binary_kmer_to_str(bkmer, kmer_size, tmp);
  gzputs(out, tmp);

  for(i = 1; i < num; i++) {
    nuc = db_node_get_last_nuc(nodes[i], db_graph);
    gzputc(out, dna_nuc_to_char(nuc));
  }
}

// Print:
// 0: AAACCCAAATGCAAACCCAAATGCAAACCCA:1 TGGGTTTGCATTTGGGTTTGCATTTGGGTTT
// 1: CAAACCCAAATGCAAACCCAAATGCAAACCC:1 GGGTTTGCATTTGGGTTTGCATTTGGGTTTG
// ...
void db_nodes_print_verbose(const dBNode *nodes, size_t num,
                            const dBGraph *db_graph, FILE *out)
{
  if(num == 0) return;

  const size_t kmer_size = db_graph->kmer_size;
  size_t i;
  BinaryKmer bkmer, bkey;
  char kmerstr[MAX_KMER_SIZE+1], keystr[MAX_KMER_SIZE+1];

  bkmer = db_node_get_bkmer(db_graph, nodes[0].key);
  bkey = db_graph_oriented_bkmer(db_graph, nodes[0]);
  binary_kmer_to_str(bkmer, kmer_size, kmerstr);
  binary_kmer_to_str(bkey, kmer_size, keystr);
  fprintf(out, "%3zu: %s:%i %s\n", (size_t)0, kmerstr, (int)nodes[0].orient, keystr);

  for(i = 1; i < num; i++) {
    bkmer = db_node_get_bkmer(db_graph, nodes[i].key);
    bkey = db_graph_oriented_bkmer(db_graph, nodes[i]);
    binary_kmer_to_str(bkmer, kmer_size, kmerstr);
    binary_kmer_to_str(bkey, kmer_size, keystr);
    fprintf(out, "%3zu: %s:%i %s\n", i, kmerstr, (int)nodes[i].orient, keystr);
  }
}

// Print in/outdegree - For debugging mostly
// indegree/outdegree (2 means >=2)
// 00: ! 01: + 02: {
// 10: - 11: = 12: <
// 20: } 21: > 22: *
void db_nodes_print_edges(const dBNode *nodes, size_t num,
                          const dBGraph *db_graph, FILE *out)
{
  size_t i, indegree, outdegree;
  Edges edges;
  const char symbols[3][3] = {"!+{","-=<","}>*"};
  for(i = 0; i < num; i++) {
    edges = db_node_get_edges_union(db_graph, nodes[i].key);
    indegree  = MIN2(edges_get_indegree(edges,  nodes[i].orient), 2);
    outdegree = MIN2(edges_get_outdegree(edges, nodes[i].orient), 2);
    fputc(symbols[indegree][outdegree], out);
  }
}

//
// Integrity checks
//
// Check an array of nodes denote a contigous path
bool db_node_check_nodes(const dBNode *nodes, size_t num,
                            const dBGraph *db_graph)
{
  if(num == 0) return true;

  const size_t kmer_size = db_graph->kmer_size;
  BinaryKmer bkmer0, bkmer1, tmp;
  Nucleotide nuc;
  size_t i;

  bkmer0 = db_graph_oriented_bkmer(db_graph, nodes[0]);

  for(i = 0; i+1 < num; i++)
  {
    bkmer1 = db_graph_oriented_bkmer(db_graph, nodes[i+1]);
    nuc = binary_kmer_last_nuc(bkmer1);
    tmp = binary_kmer_left_shift_add(bkmer0, kmer_size, nuc);
    check_ret(binary_kmers_are_equal(tmp, bkmer1));
    bkmer0 = bkmer1;
  }

  return true;
}
