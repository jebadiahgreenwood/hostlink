#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "jobs.h"
#include "../common/log.h"

const char *job_state_name(job_state_t s) {
    switch (s) {
        case HL_JOB_STARTING: return "starting";
        case HL_JOB_RUNNING:  return "running";
        case HL_JOB_LOST:     return "lost";
        case HL_JOB_EXITED:   return "exited";
        case HL_JOB_TIMEOUT:  return "timeout";
        case HL_JOB_ERROR:    return "error";
        default:              return "unknown";
    }
}

/* `lost` counts as terminal for a waiter: nothing further will ever be
 * written for that job, so blocking on it would block forever. */
int job_state_is_terminal(job_state_t s) {
    return s == HL_JOB_EXITED || s == HL_JOB_TIMEOUT ||
           s == HL_JOB_ERROR  || s == HL_JOB_LOST;
}

int jobs_id_valid(const char *id) {
    if (!id) return 0;
    size_t n = strlen(id);
    if (n != HL_JOB_ID_LEN) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static long long now_s(void) { return (long long)time(NULL); }

/* Upper bound on directories examined per opportunistic sweep. */
#define HL_JOB_GC_SCAN_MAX 512

/* <output_tmpdir>/jobs — the parent of every spool directory. */
static int jobs_root(const daemon_config_t *cfg, char *out, size_t outlen) {
    int n = snprintf(out, outlen, "%s/jobs", cfg->output_tmpdir);
    return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

int jobs_dir_for(const daemon_config_t *cfg, const char *id,
                 char *out, size_t outlen) {
    if (!jobs_id_valid(id)) return -1;
    int n = snprintf(out, outlen, "%s/jobs/%s", cfg->output_tmpdir, id);
    return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

/* mkdir -p for the two levels we own. Anything above output_tmpdir is the
 * operator's business. */
static int ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
}

/* ── small key=value file helpers ─────────────────────────────────────────
 * These files are written once and read many times; a two-function parser is
 * cheaper and more predictable here than pulling in the INI machinery. */

/* Write `content` to <dir>/<name> via a temp file + rename, so a reader never
 * sees a half-written file. Rename within a directory is atomic. */
static int write_atomic(const char *dir, const char *name,
                        const char *content, size_t len) {
    char tmp[HL_JOB_DIR_MAX + 64], final[HL_JOB_DIR_MAX + 64];
    if (snprintf(tmp,   sizeof(tmp),   "%s/.%s.tmp", dir, name) >= (int)sizeof(tmp))   return -1;
    if (snprintf(final, sizeof(final), "%s/%s",      dir, name) >= (int)sizeof(final)) return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp); return -1;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Look up `key` in a key=value file. Returns 0 and fills `val` on a hit. */
static int read_kv(const char *dir, const char *name, const char *key,
                   char *val, size_t vallen) {
    char path[HL_JOB_DIR_MAX + 64];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[9216];
    size_t klen = strlen(key);
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        char *v = line + klen + 1;
        size_t vl = strlen(v);
        while (vl > 0 && (v[vl - 1] == '\n' || v[vl - 1] == '\r')) v[--vl] = '\0';
        snprintf(val, vallen, "%s", v);
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

static int file_exists(const char *dir, const char *name) {
    char path[HL_JOB_DIR_MAX + 64];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) return 0;
    return access(path, F_OK) == 0;
}

static long long file_size(const char *dir, const char *name) {
    char path[HL_JOB_DIR_MAX + 64];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_size;
}

static int read_whole(const char *dir, const char *name, char *out, size_t outlen) {
    char path[HL_JOB_DIR_MAX + 64];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, outlen - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
}

/*
 * Is the supervisor still alive?
 *
 * kill(pid, 0) alone is not enough: pids are recycled, and a job directory can
 * outlive a wrap of the pid space, at which point some unrelated process would
 * make a dead job look like a running one. /proc/<pid>/stat field 22 is the
 * process start time in clock ticks since boot — pid plus start time is unique
 * for the life of the boot, so we record both and compare both.
 *
 * The field is read by counting from the END of the line, because field 2
 * (comm) is an arbitrary string in parentheses that may itself contain spaces
 * and parentheses. Everything after the closing paren is fixed-width in
 * fields, so index-from-the-right is the reliable read.
 */
static long proc_starttime(long pid) {
    char path[64], buf[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char *rp = strrchr(buf, ')');
    if (!rp) return -1;
    /* fields after comm: state(3) ppid(4) ... starttime(22) → the 20th token */
    char *p = rp + 1;
    int field = 2;
    char *tok = strtok(p, " ");
    while (tok) {
        field++;
        if (field == 22) return atol(tok);
        tok = strtok(NULL, " ");
    }
    return -1;
}

static int supervisor_alive(long pid, long recorded_start) {
    if (pid <= 0) return 0;
    long st = proc_starttime(pid);
    if (st < 0) return 0;                      /* process is gone */
    if (recorded_start > 0 && st != recorded_start) return 0;  /* pid reused */
    return 1;
}

int jobs_create(const daemon_config_t *cfg, const char *command,
                char *id_out, char *dir_out, size_t dirlen) {
    char root[HL_JOB_DIR_MAX];
    if (jobs_root(cfg, root, sizeof(root)) != 0) return -1;
    if (ensure_dir(cfg->output_tmpdir, 0750) != 0) return -1;
    if (ensure_dir(root, 0700) != 0) return -1;

    /* Opportunistic sweep. Doing it here rather than on a timer keeps the
     * daemon's event loop free of periodic work, and submission is exactly
     * when new space is about to be needed. */
    jobs_gc(cfg);

    int urandom = open("/dev/urandom", O_RDONLY);
    if (urandom < 0) return -1;

    /* mkdir is the allocator: it fails with EEXIST on a collision, which makes
     * id allocation atomic against other workers without a lock. */
    for (int attempt = 0; attempt < 16; attempt++) {
        unsigned char raw[HL_JOB_ID_LEN / 2];
        if (read(urandom, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) continue;
        char id[HL_JOB_ID_LEN + 1];
        for (size_t i = 0; i < sizeof(raw); i++)
            snprintf(id + i * 2, 3, "%02x", raw[i]);

        char dir[HL_JOB_DIR_MAX];
        if (snprintf(dir, sizeof(dir), "%s/%s", root, id) >= (int)sizeof(dir)) break;
        if (mkdir(dir, 0700) != 0) {
            if (errno == EEXIST) continue;
            break;
        }
        close(urandom);

        if (write_atomic(dir, "command", command, strlen(command)) != 0) {
            /* Leave the directory; gc will collect it. Better a stray empty
             * dir than a job whose command we cannot show. */
            return -1;
        }
        char created[64];
        snprintf(created, sizeof(created), "%lld\n", now_s());
        if (write_atomic(dir, "created", created, strlen(created)) != 0) return -1;

        memcpy(id_out, id, sizeof(id));
        snprintf(dir_out, dirlen, "%s", dir);
        return 0;
    }
    close(urandom);
    return -1;
}

int jobs_write_meta(const char *dir, long supervisor_pid,
                    const char *workdir, int timeout_ms) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "supervisor_pid=%ld\n"
                     "supervisor_start=%ld\n"
                     "timeout_ms=%d\n"
                     "workdir=%s\n",
                     supervisor_pid, proc_starttime(supervisor_pid),
                     timeout_ms, workdir ? workdir : "");
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return write_atomic(dir, "meta", buf, (size_t)n);
}

int jobs_write_status(const char *dir, const char *state, int exit_code,
                      long duration_ms, long long stdout_bytes,
                      long long stderr_bytes, int stdout_trunc,
                      int stderr_trunc, const char *error_msg) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "state=%s\n"
                     "exit_code=%d\n"
                     "duration_ms=%ld\n"
                     "ended_at=%lld\n"
                     "stdout_bytes=%lld\n"
                     "stderr_bytes=%lld\n"
                     "stdout_truncated=%d\n"
                     "stderr_truncated=%d\n"
                     "error_msg=%s\n",
                     state, exit_code, duration_ms, now_s(),
                     stdout_bytes, stderr_bytes,
                     stdout_trunc ? 1 : 0, stderr_trunc ? 1 : 0,
                     error_msg ? error_msg : "");
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return write_atomic(dir, "status", buf, (size_t)n);
}

int jobs_read(const daemon_config_t *cfg, const char *id, job_info_t *out) {
    char dir[HL_JOB_DIR_MAX];
    if (jobs_dir_for(cfg, id, dir, sizeof(dir)) != 0) return -1;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;

    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    out->exit_code = -1;

    char v[9216];
    if (read_whole(dir, "command", out->command, sizeof(out->command)) != 0)
        out->command[0] = '\0';
    if (read_whole(dir, "created", v, sizeof(v)) == 0) out->created_at = atoll(v);

    out->stdout_bytes = file_size(dir, "stdout");
    out->stderr_bytes = file_size(dir, "stderr");

    long sup_pid = 0, sup_start = 0;
    int have_meta = 0;
    if (read_kv(dir, "meta", "supervisor_pid", v, sizeof(v)) == 0) {
        sup_pid = atol(v); have_meta = 1;
    }
    if (read_kv(dir, "meta", "supervisor_start", v, sizeof(v)) == 0) sup_start = atol(v);
    if (read_kv(dir, "meta", "timeout_ms", v, sizeof(v)) == 0) out->timeout_ms = atoi(v);
    read_kv(dir, "meta", "workdir", out->workdir, sizeof(out->workdir));
    out->supervisor_pid = sup_pid;

    if (file_exists(dir, "status")) {
        char state[32] = "";
        if (read_kv(dir, "status", "state", state, sizeof(state)) != 0)
            snprintf(state, sizeof(state), "error");
        if      (!strcmp(state, "exited"))  out->state = HL_JOB_EXITED;
        else if (!strcmp(state, "timeout")) out->state = HL_JOB_TIMEOUT;
        else                                out->state = HL_JOB_ERROR;

        if (read_kv(dir, "status", "exit_code",   v, sizeof(v)) == 0) out->exit_code   = atoi(v);
        if (read_kv(dir, "status", "duration_ms", v, sizeof(v)) == 0) out->duration_ms = atol(v);
        if (read_kv(dir, "status", "ended_at",    v, sizeof(v)) == 0) out->ended_at    = atoll(v);
        if (read_kv(dir, "status", "stdout_bytes", v, sizeof(v)) == 0) out->stdout_original = atoll(v);
        if (read_kv(dir, "status", "stderr_bytes", v, sizeof(v)) == 0) out->stderr_original = atoll(v);
        if (read_kv(dir, "status", "stdout_truncated", v, sizeof(v)) == 0) out->stdout_truncated = atoi(v);
        if (read_kv(dir, "status", "stderr_truncated", v, sizeof(v)) == 0) out->stderr_truncated = atoi(v);
        read_kv(dir, "status", "error_msg", out->error_msg, sizeof(out->error_msg));
    } else if (!have_meta) {
        out->state = HL_JOB_STARTING;
    } else if (supervisor_alive(sup_pid, sup_start)) {
        out->state = HL_JOB_RUNNING;
    } else {
        out->state = HL_JOB_LOST;
        snprintf(out->error_msg, sizeof(out->error_msg),
                 "supervisor pid %ld is gone and wrote no status; the job was "
                 "most likely killed with the daemon (a restart tears down the "
                 "whole cgroup)", sup_pid);
    }
    return 0;
}

/* Remove one spool directory. Only the files we create are unlinked, then the
 * directory itself — deliberately not a recursive delete, so a bug here can
 * never walk off into the filesystem. */
static void remove_job_dir(const char *dir) {
    static const char *names[] = {
        "command", "created", "meta", "status", "stdout", "stderr",
        ".command.tmp", ".created.tmp", ".meta.tmp", ".status.tmp", NULL
    };
    char path[HL_JOB_DIR_MAX + 64];
    for (int i = 0; names[i]; i++) {
        if (snprintf(path, sizeof(path), "%s/%s", dir, names[i]) < (int)sizeof(path))
            unlink(path);
    }
    if (rmdir(dir) != 0 && errno != ENOENT)
        log_warn("jobs: cannot remove %s: %s", dir, strerror(errno));
}

void jobs_gc(const daemon_config_t *cfg) {
    if (cfg->job_retention_s <= 0) return;

    char root[HL_JOB_DIR_MAX];
    if (jobs_root(cfg, root, sizeof(root)) != 0) return;
    DIR *d = opendir(root);
    if (!d) return;

    long long cutoff = now_s() - cfg->job_retention_s;
    struct dirent *de;
    /* Submission runs on the daemon's event loop, so the sweep is bounded
     * rather than proportional to however many spools have accumulated: a
     * backlog is cleared over several submissions instead of stalling one. */
    int examined = 0;
    while ((de = readdir(d)) != NULL && examined < HL_JOB_GC_SCAN_MAX) {
        if (!jobs_id_valid(de->d_name)) continue;   /* also skips . and .. */
        examined++;
        char dir[HL_JOB_DIR_MAX];
        if (snprintf(dir, sizeof(dir), "%s/%s", root, de->d_name) >= (int)sizeof(dir)) continue;

        /* Only finished jobs are collected: a running one has no status file,
         * and its age says nothing about whether it is still needed. */
        char path[HL_JOB_DIR_MAX + 64];
        if (snprintf(path, sizeof(path), "%s/status", dir) >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if ((long long)st.st_mtime > cutoff) continue;

        remove_job_dir(dir);
    }
    closedir(d);
}

int jobs_list(const daemon_config_t *cfg, job_info_t *out, int max) {
    char root[HL_JOB_DIR_MAX];
    if (jobs_root(cfg, root, sizeof(root)) != 0) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;

    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max) {
        if (!jobs_id_valid(de->d_name)) continue;
        if (jobs_read(cfg, de->d_name, &out[n]) == 0) n++;
    }
    closedir(d);

    /* Newest first. n is small (bounded by max) so an insertion sort is the
     * right amount of machinery. */
    for (int i = 1; i < n; i++) {
        job_info_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].created_at < key.created_at) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}
