#ifndef HOSTLINK_CONFIG_H
#define HOSTLINK_CONFIG_H

#include <stddef.h>

/* ---- Daemon config ---- */
typedef struct {
    char   node_name[64];
    char   auth_token[256];

    int    unix_enabled;
    char   unix_path[256];
    int    unix_mode;        /* octal, e.g. 0660 */
    char   unix_group[64];

    int    tcp_enabled;
    char   tcp_bind[64];
    int    tcp_port;

    int    max_concurrent;       /* exec workers (forked, one per command)            */
    int    max_concurrent_io;    /* I/O workers (forked, one per get/put transfer)    */
    int    default_timeout_ms;
    int    max_timeout_ms;
    char   shell[256];

    long long default_max_output_bytes;
    long long max_output_bytes;
    char   output_tmpdir[256];

    /* Detached jobs. These deliberately do NOT reuse the exec limits above: a
     * job exists precisely to outlive the 30 s conversation the exec defaults
     * are tuned for, and clamping a build to max_timeout_ms would defeat the
     * feature. The spool cap is separate for the opposite reason — a job
     * writes to disk unattended, so it needs a ceiling that an interactive
     * exec does not. */
    int    job_default_timeout_ms;  /* used when the caller passes no -T */
    int    job_max_timeout_ms;      /* hard ceiling for a job              */
    long long job_max_spool_bytes;  /* per stream, per job                 */
    int    job_retention_s;         /* delete finished spools older than   */

    char   log_target[32];   /* "syslog", "stderr", or file path */
    char   log_level[16];    /* "debug","info","warn","error" */

    char   run_as_user[64];
} daemon_config_t;

/* ---- Client target ---- */
typedef struct target_entry {
    char name[64];
    char transport[8];  /* "unix" or "tcp" */
    char socket[256];
    char address[64];
    int  port;
    char token[256];
    /* Per-target default command timeout, ms. 0 = use the client's own
     * default. This exists because the right patience is a property of the
     * TARGET, not of the caller: the build-lab runs compiles that take
     * minutes, the host answers in milliseconds. Before this, every wrapper
     * that wanted a longer default had to inject -T itself. */
    int  timeout_ms;
    struct target_entry *next;
} target_entry_t;

/* Parse daemon config from file. Returns 0 on success, -1 on error. */
int  daemon_config_load(const char *path, daemon_config_t *cfg);
void daemon_config_defaults(daemon_config_t *cfg);

/* Parse targets config. Returns head of linked list, or NULL on error.
   Caller must free with targets_free(). */
target_entry_t *targets_load(const char *path);
target_entry_t *targets_find(target_entry_t *head, const char *name);
void            targets_free(target_entry_t *head);

#endif /* HOSTLINK_CONFIG_H */
