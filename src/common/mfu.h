/* enable C++ codes to include this header directly */
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MFU_H
#define MFU_H

#define MFU_SUCCESS (0)
#define MFU_FAILURE (1)

/* TODO: ugly hack until we get a configure test */
// HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
// HAVE_STRUCT_STAT_ST_MTIME_N
// HAVE_STRUCT_STAT_ST_UMTIME
// HAVE_STRUCT_STAT_ST_MTIME_USEC

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#include <limits.h>

#include "mfu_util.h"
#include "mfu_path.h"
#include "mfu_io.h"
#include "mfu_param_path.h"
#include "mfu_flist.h"
#include "mfu_pred.h"
#include "mfu_bz2.h"
#include <regex.h>

typedef struct {
  // https://www.gnu.org/software/findutils/manual/html_mono/find.html
  char * workdir;
  int just_count;
  int print_by_process;
  char * results_dir;
  int stonewall_timer;
  int print_rates;

  char * timestamp_file;
  char * name_pattern;
  regex_t name_regex;
  uint64_t size;
  // optimizing parameters
  int queue_length;
  int max_entries_per_iter;
  int steal_from_next; // if true, then steal from the next process
  int parallel_single_dir_access; // if 1, use hashing to parallelize single directory access, if 2 sequential increment

  int stonewall;
  int verbosity;
} pfind_options_t;

typedef struct {
  uint64_t job_steal_inbound;
  uint64_t work_send;
  uint64_t job_steal_tries;
  uint64_t work_stolen;
  uint64_t job_steal_mpitime_us; // microseconds spend in job steal attempts (MPI)
  uint64_t completion_tokens_send;
} pfind_monitoring_t;

typedef struct{
  uint64_t errors;
  uint64_t unknown_file;

  uint64_t found_files;
  uint64_t total_files;

  uint64_t checked_dirents;

  double rate;
  double runtime;

  pfind_monitoring_t monitor;
  MPI_Comm com;
} pfind_find_results_t;

pfind_find_results_t * pfind_find(pfind_options_t * opt);

#endif /* MFU_H */

/* enable C++ codes to include this header directly */
#ifdef __cplusplus
} /* extern "C" */
#endif
