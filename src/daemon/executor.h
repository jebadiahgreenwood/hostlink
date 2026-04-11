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

    /* outputs */
    char  *stdout_buf;
    size_t stdout_len;
    char  *stderr_buf;
    size_t stderr_len;
    long long stdout_original_bytes;
    long long stderr_original_bytes;
    int    stdout_truncated;
    int    stderr_truncated;
    char   stdout_file[512];
    char   stderr_file[512];
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
