#ifndef HOSTLINK_JOBS_H
#define HOSTLINK_JOBS_H

#include <sys/types.h>
#include <stddef.h>
#include "../common/config.h"

/*
 * Detached jobs — the registry.
 *
 * WHY IT LIVES ON DISK. The daemon forks a worker per request and answers each
 * one from that child, so there is no process in which an in-memory job table
 * would be visible to both the job that is running and the client asking about
 * it. A shared-memory table would fix that and lose everything on restart. The
 * spool directory is the registry: it is what both sides can see, and it is
 * what survives the daemon.
 *
 * WHY THE IDS COME FROM THE DAEMON. A job id is a path component. If the
 * client picked it, `--job ../../etc` would be a directory traversal and two
 * clients could pick the same one and interleave their output. The daemon
 * mints them from /dev/urandom and every id arriving in a request is checked
 * against jobs_id_valid before it is allowed near the filesystem.
 *
 * Layout, one directory per job under <output_tmpdir>/jobs/<id>/:
 *   command   the command text, verbatim (may contain newlines)
 *   meta      key=value scalars, written once by the intermediate fork
 *   stdout    spooled output, capped at cfg->job_max_spool_bytes
 *   stderr    same
 *   status    key=value, written once when the job ends. Its PRESENCE is what
 *             makes a job terminal — readers never have to guess.
 */

/* A job id is exactly this many lowercase hex characters. A fixed width and a
 * fixed alphabet are what make validation a one-liner. */
#define HL_JOB_ID_LEN  12
#define HL_JOB_DIR_MAX 512

/* States a reader can observe. `starting` and `lost` are not failures of the
 * job so much as facts about what the daemon can still see:
 *   starting — spool directory exists, supervisor has not yet published meta
 *              (a sub-millisecond window right after submission)
 *   running  — meta present, no status file, supervisor process alive
 *   lost     — meta present, no status file, supervisor GONE. The usual cause
 *              is the daemon's cgroup being torn down: `systemctl restart`
 *              kills detached descendants too, because setsid does not leave a
 *              cgroup. The exit code is unknowable, and reporting "running"
 *              forever would be a lie.
 *   exited / timeout / error — terminal, read from the status file.
 */
typedef enum {
    HL_JOB_STARTING = 0,
    HL_JOB_RUNNING,
    HL_JOB_LOST,
    HL_JOB_EXITED,
    HL_JOB_TIMEOUT,
    HL_JOB_ERROR,
    HL_JOB_UNKNOWN      /* no such job */
} job_state_t;

typedef struct {
    char        id[HL_JOB_ID_LEN + 1];
    job_state_t state;
    long long   created_at;      /* unix seconds */
    long long   ended_at;        /* unix seconds; 0 while not terminal */
    int         exit_code;       /* meaningful only in a terminal state */
    long        duration_ms;
    long long   stdout_bytes;    /* live size of the spool files */
    long long   stderr_bytes;
    /* What the command actually produced, which is larger than the spool when
     * the cap bit. "truncated" without a number tells you something was lost
     * but not how much. */
    long long   stdout_original;
    long long   stderr_original;
    int         stdout_truncated;
    int         stderr_truncated;
    int         timeout_ms;
    long        supervisor_pid;
    char        command[8192];
    char        workdir[256];
    char        error_msg[256];
} job_info_t;

const char *job_state_name(job_state_t s);
int         job_state_is_terminal(job_state_t s);

/* True if `id` is exactly HL_JOB_ID_LEN lowercase hex characters. Every
 * request-supplied id passes through this before use. */
int  jobs_id_valid(const char *id);

/* Compose <output_tmpdir>/jobs/<id> into `out`. -1 if the id is invalid or the
 * composed path would not fit (truncating a path yields a path to something
 * else, which is the bug class this release exists to remove). */
int  jobs_dir_for(const daemon_config_t *cfg, const char *id,
                  char *out, size_t outlen);

/* Mint an id, create its spool directory 0700, write <dir>/command. */
int  jobs_create(const daemon_config_t *cfg, const char *command,
                 char *id_out, char *dir_out, size_t dirlen);

/* Written by the intermediate fork — the only process that knows the
 * supervisor's pid. Atomic (tmp + rename). */
int  jobs_write_meta(const char *dir, long supervisor_pid,
                     const char *workdir, int timeout_ms);

/* Written by the supervisor when the command finishes. Atomic. */
int  jobs_write_status(const char *dir, const char *state, int exit_code,
                       long duration_ms, long long stdout_bytes,
                       long long stderr_bytes, int stdout_trunc,
                       int stderr_trunc, const char *error_msg);

/* Everything a client can ask about one job. -1 if it does not exist. */
int  jobs_read(const daemon_config_t *cfg, const char *id, job_info_t *out);

/* Drop spool directories for jobs that ended more than cfg->job_retention_s
 * ago. Called opportunistically at submission, so a long-lived daemon does not
 * accumulate spools forever. */
void jobs_gc(const daemon_config_t *cfg);

/* Newest first by creation time. Returns how many were written. */
int  jobs_list(const daemon_config_t *cfg, job_info_t *out, int max);

#endif /* HOSTLINK_JOBS_H */
