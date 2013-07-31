#include "global.h"

#include "cmd.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "graph_info.h"
#include "binary_format.h"
#include "file_reader.h"
#include "seq_reader.h"

static const char usage[] =
"usage: "CMD" build [options] <out.ctx>\n"
"  Build a cortex binary.  \n"
"\n"
"  -m <mem>   Memory to use (e.g. 100G or 12M)\n"
"  -k <kmer>  Kmer size\n"
"\n"
"  Sequence Options:\n"
"  --fq_threshold <qual>  Filter quality scores [default: 0 (off)]\n"
"  --fq_offset <offset>   FASTQ ASCII offset    [default: 0 (auto-detected)]\n"
"  --cut_hp <len>         Breaks reads at homopolymers >= <len> [default: off]\n"
"  --remove_pcr           Remove (or keep) PCR duplicate reads [default: keep]\n"
"  --keep_pcr\n"
"\n"
"  PCR duplicate removal works by ignoring read pairs (PE-only) if both reads\n"
"  start at the same k-mer as any previous read.\n"
"\n"
"  Loading sequence:\n"
"  --sample <name>                   Sample name (required before any seq args)\n"
"  --seq <in.fa|fq|sam>              Load sequence data\n"
"  --seq2 <in1> <in2>                Load paired end sequence data\n"
"\n"
"  --sample <name> is required before sequence can be loaded.\n"
"  Consecutive sequence options are loaded into the same colour.\n"
"\n"
"  Loading binaries\n"
"  --load_binary <in.ctx>[:cols]     Load samples from a binary\n"
"\n"
"  Colour can be specifed after binary path, e.g. in.ctx:0,6-8 will load\n"
"  samples 0,6,7,8\n"
"\n"
" Example - build a two sample graph with k=31 using 60G RAM:\n"
"   "CMD" build -k 31 -m 60G --fq_threshold 5 --cut_hp 8 \\\n"
"               --sample bob --seq bob.bam \\\n"
"               --sample dylan --remove_pcr --seq dylan.sam \\\n"
"                              --keep_pcr --fq_offset 33 \\\n"
"                              --seq2 dylan.1.fq.gz dylan.2.fq.gz\n";

static void update_ginfo(GraphInfo *ginfo, SeqLoadingStats *stats,
                         uint64_t *bases_loaded, uint64_t *contigs_loaded)
{
  uint64_t bases = stats->total_bases_loaded - *bases_loaded;
  uint64_t contigs = stats->contigs_loaded - *contigs_loaded;
  graph_info_update_contigs(ginfo, bases, contigs);
  *bases_loaded = stats->total_bases_loaded;
  *contigs_loaded = stats->contigs_loaded;
}

int ctx_build(CmdArgs *args)
{
  cmd_accept_options(args, "mhk");
  cmd_require_options(args, "k", usage);
  int argc = args->argc;
  char **argv = args->argv;
  if(argc < 3) print_usage(usage, NULL);

  uint32_t kmer_size = args->kmer_size;

  const char *out_path = argv[argc-1];
  uint32_t colours_used = 0;
  boolean sample_named = false, sample_used = false;

  // Validate arguments
  int argi, argend = argc-1;
  uint32_t tmp;
  for(argi = 0; argi < argend; argi++)
  {
    if(strcmp(argv[argi],"--fq_threshold") == 0) {
      if(argi + 1 >= argend)
        print_usage(usage, "--fq_threshold <qual> requires an argument");
      if(!parse_entire_uint(argv[argi+1], &tmp) || tmp > 128)
        die("Invalid --fq_threshold argument: %s", argv[argi+1]);
      argi += 1;
    }
    else if(strcmp(argv[argi],"--fq_offset") == 0) {
      if(argi + 1 >= argend)
        print_usage(usage, "--fq_offset <offset> requires an argument");
      if(!parse_entire_uint(argv[argi+1], &tmp) || tmp > 128)
        die("Invalid --fq_offset argument: %s", argv[argi+1]);
      argi += 1;
    }
    else if(strcmp(argv[argi],"--cut_hp") == 0) {
      if(argi + 1 >= argend)
        print_usage(usage, "--cut_hp <len> requires an argument");
      if(!parse_entire_uint(argv[argi+1], &tmp))
        die("Invalid --cut_hp argument: %s", argv[argi+1]);
      argi += 1;
    }
    else if(!strcmp(argv[argi],"--remove_pcr") || !strcmp(argv[argi],"--keep_pcr"))
    {}
    else if(strcmp(argv[argi],"--seq") == 0) {
      if(!sample_named)
        print_usage(usage, "Please use --sample <name> before giving sequence");
      if(argi + 1 >= argend)
        print_usage(usage, "--seq <file> requires an argument");
      if(!test_file_readable(argv[argi+1]))
        die("Cannot read --seq file: %s", argv[argi+1]);
      argi += 1;
      sample_used = true;
    }
    else if(strcmp(argv[argi],"--seq2") == 0) {
      if(!sample_named)
        print_usage(usage, "Please use --sample <name> before giving sequence");
      if(argi + 2 >= argend)
        print_usage(usage, "--seq2 <file1> <file2> requires two arguments");
      if(!test_file_readable(argv[argi+1]))
        die("Cannot read first --seq2 file: %s", argv[argi+1]);
      if(!test_file_readable(argv[argi+2]))
        die("Cannot read second --seq2 file: %s", argv[argi+2]);
      argi += 2;
      sample_used = true;
    }
    else if(strcmp(argv[argi],"--load_binary") == 0) {
      if(argi + 1 >= argend)
        print_usage(usage, "--load_binary requires an arg");
      // probe binary to get number of colours
      boolean is_binary = false;
      uint32_t bin_kmer_size, bin_num_of_cols, max_col;
      uint64_t bin_num_kmers;
      if(!binary_probe(argv[argi+1], &is_binary, &bin_kmer_size,
                       &bin_num_of_cols, &max_col, &bin_num_kmers)) {
        print_usage(usage, "Cannot read binary file: %s", argv[argi+1]);
      } else if(!is_binary) {
        print_usage(usage, "Input binary file isn't valid: %s", argv[argi+1]);
      } else if(bin_kmer_size != kmer_size) {
        print_usage(usage, "Input binary kmer_size doesn't match [%u vs %u]",
                    bin_kmer_size, kmer_size);
      }
      argi++;
      colours_used += bin_num_of_cols;
      sample_named = false;
    }
    else if(strcmp(argv[argi],"--sample") == 0) {
      if(argi + 1 >= argend)
        print_usage("--sample <name> requires an argument", NULL);
      if(!sample_used) warn("Empty colour (maybe you intended this)");
      argi++;
      colours_used++;
      sample_named = true;
      sample_used = false;
    }
    else {
      print_usage(usage, "Unknown command: %s", argv[argi]);
    }
  }

  if(!test_file_writable(out_path))
    die("Cannot write to file: %s", out_path);

  // Pick hash table size
  size_t mem_per_kmer = sizeof(BinaryKmer) + sizeof(Covg) + sizeof(Edges);
  size_t kmers_in_hash = cmd_get_kmers_in_hash(args, mem_per_kmer);

  message("Writing %u colour binary to %s\n", colours_used, out_path);

  // Create db_graph
  dBGraph db_graph;
  db_graph_alloc(&db_graph, kmer_size, colours_used, kmers_in_hash);
  db_graph.col_edges = calloc(kmers_in_hash * colours_used, sizeof(Edges));
  db_graph.col_covgs = calloc(kmers_in_hash * colours_used, sizeof(Covg));

  hash_table_print_stats(&db_graph.ht);

  // Parse arguments, load
  SeqLoadingStats *stats = seq_loading_stats_create(1000);
  SeqLoadingPrefs prefs = {.into_colour = -1, .merge_colours = false,
                           .boolean_covgs = false,
                           .load_seq = true,
                           .quality_cutoff = 0, .ascii_fq_offset = 0,
                           .homopolymer_cutoff = 0,
                           .remove_dups_se = false, .remove_dups_pe = false,
                           .load_binaries = true,
                           .must_exist_in_graph = false, .empty_colours = false,
                           .update_ginfo = true, .db_graph = &db_graph};

  read_t r1, r2;
  seq_read_alloc(&r1);
  seq_read_alloc(&r2);

  uint64_t bases_loaded = 0, contigs_loaded = 0;

  for(argi = 0; argi < argend; argi++)
  {
    if(strcmp(argv[argi],"--fq_threshold") == 0) {
      parse_entire_uint(argv[argi+1], &tmp);
      prefs.quality_cutoff = tmp;
      argi += 1;
    }
    else if(strcmp(argv[argi],"--fq_offset") == 0) {
      parse_entire_uint(argv[argi+1], &tmp);
      prefs.ascii_fq_offset = tmp;
      argi += 1;
    }
    else if(strcmp(argv[argi],"--remove_pcr") == 0) {
      prefs.remove_dups_pe = true;
    }
    else if(strcmp(argv[argi],"--keep_pcr") == 0) {
      prefs.remove_dups_pe = false;
    }
    else if(strcmp(argv[argi],"--cut_hp") == 0) {
      parse_entire_uint(argv[argi+1], &tmp);
      prefs.homopolymer_cutoff = tmp;
      argi += 1;
    }
    else if(strcmp(argv[argi],"--seq") == 0) {
      seq_parse_se(argv[argi+1], &r1, &r2, &prefs, stats,
                   seq_load_into_db_graph, NULL);
      update_ginfo(&db_graph.ginfo[prefs.into_colour],
                   stats, &bases_loaded, &contigs_loaded);
      argi += 1;
    }
    else if(strcmp(argv[argi],"--seq2") == 0) {
      seq_parse_pe(argv[argi+1], argv[argi+2], &r1, &r2, &prefs, stats,
                   seq_load_into_db_graph, NULL);
      update_ginfo(&db_graph.ginfo[prefs.into_colour],
                   stats, &bases_loaded, &contigs_loaded);
      argi += 2;
    }
    else if(strcmp(argv[argi],"--load_binary") == 0) {
      binary_load(argv[argi+1], &db_graph, &prefs, stats, NULL);
      argi += 1;
    }
    else if(strcmp(argv[argi],"--sample") == 0) {
      prefs.into_colour++;
      strbuf_set(&db_graph.ginfo[prefs.into_colour].sample_name, argv[argi+1]);
      argi += 1;
    }
    else {
      die("Unknown command: %s", argv[argi]);
    }
  }

  seq_read_dealloc(&r1);
  seq_read_dealloc(&r2);
  seq_loading_stats_free(stats);

  hash_table_print_stats(&db_graph.ht);

  binary_dump_graph(out_path, &db_graph, CURR_CTX_VERSION, NULL, 0, colours_used);

  message("Dumped cortex binary to: %s (format version %u; %u colour%s)\n",
          out_path, CURR_CTX_VERSION, colours_used, colours_used != 1 ? "s" : "");

  message("Done.\n");

  return EXIT_SUCCESS;
}