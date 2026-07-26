#ifndef HOSTLINK_EXECUTOR_H
#define HOSTLINK_EXECUTOR_H

#include <sys/types.h>
#include "../common/config.h"

typedef struct {
    /* inputs */
    char   request_id[65];
    char   command[8192];
    char   workdir[256];
    char  *env_keys[256];
    char  *env_vals[256];
    int    env_count;
    int    timeout_ms;
    long long max_stdout_bytes;
    long long max_stderr_bytes;
    int    output_to_file;
    char   output_tmpdir[256];
    char   shell[256];
    int    detach;   /* 1 = double-fork, return immediately, no output captured */

    /* Job mode. When job_id is set, detach spools into an explicit directory
     * instead of discarding output, and a supervisor records the exit status.
     * out_path/err_path override the tmpdir-derived spool names; file_cap_bytes
     * bounds each stream in file mode (0 = unbounded, which is what plain -O
     * has always been and stays). */
    char   job_id[16];
    char   job_dir[512];
    /* Sized above job_dir + the longest suffix we append, so composing them
     * cannot truncate — a truncated path names a different file. */
    char   out_path[576];
    char   err_path[576];
    char   pid_path[576];   /* if set, the child's pid is written here on fork */
    long long file_cap_bytes;

    /* outputs */
    char  *stdout_buf;
    size_t stdout_len;
    char  *stderr_buf;
    size_t stderr_len;
    long long stdout_original_bytes;
    long long stderr_original_bytes;
    int    stdout_truncated;
    int    stderr_truncated;
    char   stdout_file[576];
    char   stderr_file[576];
    int    exit_code;
    int    timed_out;
    long   duration_ms;
    char   error_msg[256];
    int    exec_error;  /* 1 if we couldn't even start */
} exec_result_t;

/* Execute a command synchronously (blocking). Fills exec_result_t.
   If r->detach is set, double-forks the child and returns immediately
   with exit_code=0 once the grandchild is confirmed launched. */
void executor_run(exec_result_t *r);

/* Free buffers in exec_result_t */
void executor_free(exec_result_t *r);

#endif /* HOSTLINK_EXECUTOR_H */
