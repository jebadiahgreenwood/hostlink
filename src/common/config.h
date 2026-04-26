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
