#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <fnmatch.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include "connection.h"
#include "../common/protocol.h"
#include "../common/config.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/sha256.h"
#include "../common/cjson/cJSON.h"

#define VERSION "1.6.0"

/*
 * Exit codes — ssh's model, for the same reason ssh uses it.
 *
 * `exec` exits with the REMOTE command's status, verbatim, so that the
 * ordinary shell idioms work across the link:
 *     hl "test -f /x" && hl "do-thing"
 *     hl "grep -q pat file"; case $? in 0) ;; 1) ;; esac
 * Before 1.5.0 every remote failure collapsed to 1, so a remote `exit 7` and a
 * remote `false` were indistinguishable, and any caller inspecting a specific
 * status (grep's 1-vs-2, diff's 1-vs-2) silently got the wrong answer.
 *
 * hostlink's OWN failures — it never got as far as running your command — live
 * in a reserved high block so they cannot be mistaken for a remote status.
 * 124/125 follow the GNU timeout(1)/env(1) convention.
 *
 * Distinguishing *which* transport failure occurred is still available on
 * stderr and, machine-readably, in `-j` JSON output.
 */
#define EXIT_OK           0    /* remote command succeeded */
                               /* 1..123, 126..255: remote command's own status */
#define EXIT_TIMEOUT      124  /* remote command was killed at the timeout */
#define EXIT_HOSTLINK_ERR 125  /* hostlink itself failed: connect/auth/protocol/usage */

/* Retained for the non-exec subcommands (put/get/ping/targets), where there is
 * no remote status to pass through and a plain pass/fail is the whole story. */
#define EXIT_REMOTE_ERR   1
#define EXIT_CONN_FAILED  EXIT_HOSTLINK_ERR
#define EXIT_AUTH_FAILED  EXIT_HOSTLINK_ERR
#define EXIT_BAD_REQUEST  EXIT_HOSTLINK_ERR
#define EXIT_PROTO_ERR    EXIT_HOSTLINK_ERR
#define EXIT_CLIENT_ERR   EXIT_HOSTLINK_ERR

#define HL_MAX_EXCLUDES 32

typedef struct {
    char  target[64];
    char  socket_path[256];
    char  address[64];
    int   port;
    char  token[256];
    int   timeout_ms;
    int   connect_timeout_ms;
    int   json_output;
    char  targets_file[256];
    char  env_pairs[64][256];
    int   env_count;
    char  workdir[256];
    long long max_stdout;
    long long max_stderr;
    int   output_to_file;
    int   detach;           /* --detach: fire-and-forget exec */
    /* Whether -T came from the command line, as opposed to a target default or
     * the built-in one. It matters for -D: an explicit -T is the user saying
     * how long the JOB may run, while an inherited default is only about how
     * long a client is prepared to sit on the wire — and must not become a
     * ceiling on a detached build. */
    int   timeout_explicit;
    char  put_mode[4];      /* "644" etc — unused, kept for mode parsing */
    int   put_mkdir;        /* --mkdir: create parent dirs on put */
    int   put_mode_val;     /* octal file mode for put */
    int   stream;           /* --stream: force streaming mode for get/put */
    int   verify;           /* --verify: sha256 manifest for a directory put */
    char  excludes[HL_MAX_EXCLUDES][256];  /* --exclude globs, directory put */
    int   exclude_count;
} cli_opts_t;

static const char *find_targets_file(const char *override) {
    static char path[512];
    if (override && override[0]) return override;
    const char *env = getenv("HOSTLINK_TARGETS");
    if (env && env[0]) return env;
    if (access("/etc/hostlink/targets.conf", R_OK) == 0)
        return "/etc/hostlink/targets.conf";
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/hostlink/targets.conf", home);
        if (access(path, R_OK) == 0) return path;
    }
    return NULL;
}

static int resolve_target(cli_opts_t *opts, target_entry_t **out_targets) {
    *out_targets = NULL;
    if (opts->target[0] == '\0') return 0;
    const char *tf = find_targets_file(opts->targets_file);
    if (!tf) { fprintf(stderr, "No targets config file found\n"); return -1; }
    *out_targets = targets_load(tf);
    if (!*out_targets) { fprintf(stderr, "Failed to load targets from %s\n", tf); return -1; }
    target_entry_t *t = targets_find(*out_targets, opts->target);
    if (!t) {
        fprintf(stderr, "Target '%s' not found in config\n", opts->target);
        targets_free(*out_targets); *out_targets = NULL; return -1;
    }
    if (!strcmp(t->transport, "unix"))
        snprintf(opts->socket_path, sizeof(opts->socket_path), "%s", t->socket);
    else {
        snprintf(opts->address, sizeof(opts->address), "%s", t->address);
        opts->port = t->port;
    }
    if (opts->token[0] == '\0')
        snprintf(opts->token, sizeof(opts->token), "%s", t->token);
    /* Per-target default timeout (F6.2). The right patience belongs to the
     * target: build-lab compiles for minutes, the host answers in
     * milliseconds. An explicit -T always wins. */
    if (!opts->timeout_explicit && t->timeout_ms > 0)
        opts->timeout_ms = t->timeout_ms;
    return 0;
}

static int open_connection(cli_opts_t *opts) {
    if (opts->socket_path[0]) {
        int fd = connect_unix(opts->socket_path, opts->connect_timeout_ms);
        if (fd < 0) {
            fprintf(stderr, "Cannot connect to Unix socket %s: %s\n",
                    opts->socket_path, strerror(errno));
            return -1;
        }
        return fd;
    }
    if (opts->address[0]) {
        int fd = connect_tcp(opts->address, opts->port, opts->connect_timeout_ms);
        if (fd < 0) {
            fprintf(stderr, "Cannot connect to %s:%d: %s\n",
                    opts->address, opts->port, strerror(errno));
            return -1;
        }
        return fd;
    }
    fprintf(stderr, "No connection target specified (use -t, -s, or -a)\n");
    return -1;
}

static void make_request_id(char *buf, size_t len) {
    static unsigned int counter = 0;
    snprintf(buf, len, "cli-%u-%d", ++counter, (int)getpid());
}

static int cmd_ping(cli_opts_t *opts) {
    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "ping");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(fd);
        fprintf(stderr, "Failed to send ping\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); return EXIT_PROTO_ERR; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }

    const char *status = "", *node = "";
    double uptime = 0;
    cJSON *j;
    j = cJSON_GetObjectItem(resp, "status");   if (cJSON_IsString(j)) status = j->valuestring;
    j = cJSON_GetObjectItem(resp, "node");     if (cJSON_IsString(j)) node   = j->valuestring;
    j = cJSON_GetObjectItem(resp, "uptime_s"); if (cJSON_IsNumber(j)) uptime = j->valuedouble;

    int ret = EXIT_OK;
    if (!strcmp(status, "ok"))
        printf("[%s] pong - uptime %.0fs\n", node, uptime);
    else if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n"); ret = EXIT_AUTH_FAILED;
    } else ret = EXIT_PROTO_ERR;
    cJSON_Delete(resp);
    return ret;
}

static int cmd_exec(cli_opts_t *opts, const char *command) {
    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version",    1);
    cJSON_AddStringToObject(req, "type",       "exec");
    cJSON_AddStringToObject(req, "id",         req_id);
    cJSON_AddStringToObject(req, "token",      opts->token);
    cJSON_AddStringToObject(req, "command",    command);
    cJSON_AddNumberToObject(req, "timeout_ms", (double)opts->timeout_ms);
    if (opts->detach) {
        cJSON_AddTrueToObject(req, "detach");
        /* Only a -T typed on this command line bounds the job. Otherwise the
         * daemon's job default applies, which is measured in hours — a build
         * must not inherit the 30 s the wire conversation is tuned for. */
        if (opts->timeout_explicit)
            cJSON_AddNumberToObject(req, "job_timeout_ms", (double)opts->timeout_ms);
    }
    if (opts->max_stdout > 0)
        cJSON_AddNumberToObject(req, "max_stdout_bytes", (double)opts->max_stdout);
    if (opts->max_stderr > 0)
        cJSON_AddNumberToObject(req, "max_stderr_bytes", (double)opts->max_stderr);
    if (opts->output_to_file)
        cJSON_AddTrueToObject(req, "output_to_file");
    if (opts->workdir[0])
        cJSON_AddStringToObject(req, "workdir", opts->workdir);
    if (opts->env_count > 0) {
        cJSON *env_obj = cJSON_CreateObject();
        for (int i = 0; i < opts->env_count; i++) {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", opts->env_pairs[i]);
            char *eq = strchr(tmp, '=');
            if (eq) { *eq = '\0'; cJSON_AddStringToObject(env_obj, tmp, eq + 1); }
        }
        cJSON_AddItemToObject(req, "env", env_obj);
    }

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(fd);
        fprintf(stderr, "Failed to send request\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); fprintf(stderr, "No response\n"); return EXIT_PROTO_ERR; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }

    cJSON *j;
    const char *status = "", *node = "", *error_msg = NULL;
    const char *stdout_str = NULL, *stderr_str = NULL;
    const char *stdout_file = NULL, *stderr_file = NULL;
    int exit_code = 0, stdout_trunc = 0, stderr_trunc = 0;
    double duration = 0, stdout_orig = 0;

    j = cJSON_GetObjectItem(resp, "status");    if (cJSON_IsString(j)) status = j->valuestring;
    j = cJSON_GetObjectItem(resp, "node");      if (cJSON_IsString(j)) node   = j->valuestring;
    j = cJSON_GetObjectItem(resp, "exit_code"); if (cJSON_IsNumber(j)) exit_code = j->valueint;
    j = cJSON_GetObjectItem(resp, "duration_ms"); if (cJSON_IsNumber(j)) duration = j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stdout_truncated"); stdout_trunc = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(resp, "stderr_truncated"); stderr_trunc = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(resp, "stdout_original_bytes"); if (cJSON_IsNumber(j)) stdout_orig = j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stdout");      if (cJSON_IsString(j)) stdout_str  = j->valuestring;
    j = cJSON_GetObjectItem(resp, "stderr");      if (cJSON_IsString(j)) stderr_str  = j->valuestring;
    j = cJSON_GetObjectItem(resp, "stdout_file"); if (cJSON_IsString(j)) stdout_file = j->valuestring;
    j = cJSON_GetObjectItem(resp, "stderr_file"); if (cJSON_IsString(j)) stderr_file = j->valuestring;
    j = cJSON_GetObjectItem(resp, "error_msg");   if (cJSON_IsString(j)) error_msg   = j->valuestring;

    /* The daemon caps -T at its own max_timeout_ms, and used to do it in
     * silence: ask for ten minutes against a daemon capped at five and the
     * command died at five reporting a plain timeout, with nothing to suggest
     * the request had been overruled. Same fail-silent class as F1 and F2. */
    double t_req = 0, t_eff = 0;
    j = cJSON_GetObjectItem(resp, "timeout_requested_ms"); if (cJSON_IsNumber(j)) t_req = j->valuedouble;
    j = cJSON_GetObjectItem(resp, "timeout_effective_ms"); if (cJSON_IsNumber(j)) t_eff = j->valuedouble;
    if (t_req > 0 && t_eff > 0 && t_req > t_eff)
        fprintf(stderr,
                "[%s] warning: requested timeout %.0fms exceeds this daemon's "
                "max_timeout_ms; %.0fms was used\n", node, t_req, t_eff);

    int ret = EXIT_OK;
    if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n"); ret = EXIT_AUTH_FAILED;
    } else if (strcmp(status, "ok") && strcmp(status, "timeout")) {
        /* Anything that is not "ok" or "timeout" is a failure. This used to
         * test only for status=="error", so the daemon's *other* rejection
         * status — "bad_request", which it sends for a missing or malformed
         * command — fell through to the success branch and was reported to the
         * caller as `exit=0`. An unrecognised future status now also fails
         * closed rather than being read as success. */
        fprintf(stderr, "[%s] %s: %s\n", node, status,
                error_msg ? error_msg : "unknown");
        ret = EXIT_BAD_REQUEST;
    } else if (!strcmp(status, "timeout")) {
        fprintf(stderr, "[%s] timeout after %.0fms\n", node, duration);
        /* Whatever the command managed to produce before it was killed is
         * already in the response — the daemon buffers it either way. Printing
         * it costs nothing and is often the only record of how far a long build
         * got, which previously had to be recovered by redirecting to a file
         * inside the command itself. */
        if (stdout_file || stderr_file) {
            if (stdout_file) fprintf(stderr, "[%s] partial stdout:%s\n", node, stdout_file);
            if (stderr_file) fprintf(stderr, "[%s] partial stderr:%s\n", node, stderr_file);
        } else {
            if (stdout_str && *stdout_str) fputs(stdout_str, stdout);
            if (stderr_str && *stderr_str) fputs(stderr_str, stderr);
        }
        ret = EXIT_TIMEOUT;
    } else {
        /* Pass the remote status through verbatim (see the exit-code comment at
         * the top of this file). Guard the reserved block and the daemon's
         * negative sentinels so they can never be forged by a remote status. */
        if (exit_code < 0) {
            ret = EXIT_HOSTLINK_ERR;
        } else if (exit_code == EXIT_TIMEOUT || exit_code == EXIT_HOSTLINK_ERR) {
            /* a genuine remote 124/125 — report it, but say so, since these two
             * values otherwise mean "hostlink failed" */
            fprintf(stderr, "[%s] note: remote exited %d, which collides with "
                            "hostlink's reserved range\n", node, exit_code);
            ret = exit_code;
        } else {
            ret = exit_code > 255 ? 255 : exit_code;
        }
        /* detached — hand back the job id, which is the whole point: the
         * caller can follow, wait on, or cancel what it just started. The id
         * goes to STDOUT so it can be captured (`id=$(hl -D ...)`) while the
         * human-readable line stays on stderr. */
        j = cJSON_GetObjectItem(resp, "detached");
        if (cJSON_IsTrue(j)) {
            cJSON *jid = cJSON_GetObjectItem(resp, "job_id");
            if (cJSON_IsString(jid) && jid->valuestring[0]) {
                printf("%s\n", jid->valuestring);
                fprintf(stderr, "[%s] detached - job %s "
                                "(job status/tail/wait/cancel %s)\n",
                        node, jid->valuestring, jid->valuestring);
            } else {
                fprintf(stderr, "[%s] detached - launched in background "
                                "(no job id: daemon predates 1.6.0)\n", node);
            }
        } else if (stdout_file || stderr_file) {
            fprintf(stderr, "[%s] exit=%d time=%.0fms", node, exit_code, duration);
            if (stdout_file) fprintf(stderr, " stdout:%s", stdout_file);
            if (stderr_file) fprintf(stderr, " stderr:%s", stderr_file);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "[%s] exit=%d time=%.0fms", node, exit_code, duration);
            if (stdout_trunc)
                fprintf(stderr, " stdout:truncated(%.0f total bytes)", stdout_orig);
            if (stderr_trunc)
                fprintf(stderr, " stderr:truncated");
            fprintf(stderr, "\n");
            if (stdout_str && *stdout_str) fputs(stdout_str, stdout);
            if (stderr_str && *stderr_str) fputs(stderr_str, stderr);
        }
    }
    cJSON_Delete(resp);
    return ret;
}

/*
 * cmd_put: transfer a local file to the remote host.
 * Reads the local file, base64-encodes it, sends as a "put" message.
 * Usage: hostlink-cli [opts] put <local_path> <remote_path>
 */
static int cmd_put(cli_opts_t *opts, const char *local_path, const char *remote_path) {
    /* Read local file */
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        fprintf(stderr, "put: cannot open local file %s: %s\n",
                local_path, strerror(errno));
        return EXIT_CLIENT_ERR;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return EXIT_CLIENT_ERR; }

    unsigned char *file_data = malloc((size_t)file_size + 1);
    if (!file_data) {
        fclose(f);
        fprintf(stderr, "put: out of memory\n");
        return EXIT_CLIENT_ERR;
    }
    size_t nread = fread(file_data, 1, (size_t)file_size, f);
    fclose(f);
    if ((long)nread != file_size) {
        free(file_data);
        fprintf(stderr, "put: read error on %s\n", local_path);
        return EXIT_CLIENT_ERR;
    }

    /* Base64 encode */
    char *b64 = hl_b64_encode(file_data, nread);
    free(file_data);
    if (!b64) {
        fprintf(stderr, "put: base64 encode failed (OOM)\n");
        return EXIT_CLIENT_ERR;
    }

    /* Connect and send */
    int fd = open_connection(opts);
    if (fd < 0) { free(b64); return EXIT_CONN_FAILED; }

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "put");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);
    cJSON_AddStringToObject(req, "content", b64);
    free(b64);

    if (opts->put_mode_val > 0)
        cJSON_AddNumberToObject(req, "mode", opts->put_mode_val);
    if (opts->put_mkdir)
        cJSON_AddTrueToObject(req, "mkdir");

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(fd);
        fprintf(stderr, "put: failed to send request\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); return EXIT_PROTO_ERR; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }

    cJSON *j;
    const char *status = "", *node = "", *error_msg = NULL;
    double bytes_written = 0;
    j = cJSON_GetObjectItem(resp, "status");        if (cJSON_IsString(j)) status       = j->valuestring;
    j = cJSON_GetObjectItem(resp, "node");          if (cJSON_IsString(j)) node         = j->valuestring;
    j = cJSON_GetObjectItem(resp, "error_msg");     if (cJSON_IsString(j)) error_msg    = j->valuestring;
    j = cJSON_GetObjectItem(resp, "bytes_written"); if (cJSON_IsNumber(j)) bytes_written = j->valuedouble;

    int ret = EXIT_OK;
    if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n"); ret = EXIT_AUTH_FAILED;
    } else if (!strcmp(status, "error") || !strcmp(status, "bad_request")) {
        fprintf(stderr, "[%s] put error: %s\n", node, error_msg ? error_msg : "unknown");
        ret = EXIT_REMOTE_ERR;
    } else {
        fprintf(stderr, "[%s] put ok: %s (%.0f bytes)\n", node, remote_path, bytes_written);
    }
    cJSON_Delete(resp);
    return ret;
}


/*
 * cmd_get: retrieve a file from the remote host to a local path.
 * Usage: hostlink-cli [opts] get <remote_path> <local_path>
 */
static int cmd_get(cli_opts_t *opts, const char *remote_path, const char *local_path) {
    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "get");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(fd);
        fprintf(stderr, "get: failed to send request\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); return EXIT_PROTO_ERR; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    /* For get, always process the response (decode + write file) first,
     * then print JSON if requested.  Unlike exec/put where -j replaces
     * normal output, get must write the local file regardless. */
    cJSON *j;
    const char *status = "", *node = "", *error_msg = NULL, *content = NULL;
    double size_bytes = 0;
    j = cJSON_GetObjectItem(resp, "status");    if (cJSON_IsString(j)) status    = j->valuestring;
    j = cJSON_GetObjectItem(resp, "node");      if (cJSON_IsString(j)) node      = j->valuestring;
    j = cJSON_GetObjectItem(resp, "error_msg"); if (cJSON_IsString(j)) error_msg = j->valuestring;
    j = cJSON_GetObjectItem(resp, "content");   if (cJSON_IsString(j)) content   = j->valuestring;
    j = cJSON_GetObjectItem(resp, "size");      if (cJSON_IsNumber(j)) size_bytes = j->valuedouble;

    int ret = EXIT_OK;
    if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n"); ret = EXIT_AUTH_FAILED;
    } else if (!strcmp(status, "error") || !strcmp(status, "bad_request")) {
        fprintf(stderr, "[%s] get error: %s\n", node, error_msg ? error_msg : "unknown");
        ret = EXIT_REMOTE_ERR;
    } else if (!strcmp(status, "ok")) {
        if (!content) {
            fprintf(stderr, "get: response missing content field\n");
            cJSON_Delete(resp);
            return EXIT_PROTO_ERR;
        }
        /* decode base64 */
        size_t b64_len = strlen(content);
        size_t max_decoded = hl_b64_decoded_len(content, b64_len) + 4;
        unsigned char *data = malloc(max_decoded);
        if (!data) {
            fprintf(stderr, "get: out of memory\n");
            cJSON_Delete(resp);
            return EXIT_CLIENT_ERR;
        }
        ssize_t data_len = hl_b64_decode(content, b64_len, data, max_decoded);
        if (data_len < 0) {
            free(data);
            fprintf(stderr, "get: invalid base64 in response\n");
            cJSON_Delete(resp);
            return EXIT_PROTO_ERR;
        }
        /* write to local file */
        int wfd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd < 0) {
            free(data);
            fprintf(stderr, "get: cannot write local file %s: %s\n",
                    local_path, strerror(errno));
            cJSON_Delete(resp);
            return EXIT_CLIENT_ERR;
        }
        ssize_t written = 0;
        while (written < data_len) {
            ssize_t w = write(wfd, data + written, (size_t)(data_len - written));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(wfd);
                free(data);
                fprintf(stderr, "get: write error: %s\n", strerror(errno));
                cJSON_Delete(resp);
                return EXIT_CLIENT_ERR;
            }
            written += w;
        }
        close(wfd);
        free(data);
        if (!opts->json_output)
            fprintf(stderr, "[%s] get ok: %s -> %s (%.0f bytes)\n",
                    node, remote_path, local_path, size_bytes);
    } else {
        fprintf(stderr, "get: unexpected status: %s\n", status);
        ret = EXIT_PROTO_ERR;
    }

    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
    }
    cJSON_Delete(resp);
    return ret;
}

/* ── Streaming get/put ─────────────────────────────────────────────────────
 *
 * For files that don't fit in a single 128 MiB frame (90 MiB raw cap on the
 * legacy path due to base64 inflation). Used for tensor weights and other
 * multi-GB transfers between the container, host, and Spark.
 *
 * See server.c "Streaming get/put" comment block for the wire protocol.
 * Daemon-side both flows run inside forked I/O workers (commit 1).
 *
 * Client guarantees: end-to-end SHA-256 verified before reporting success.
 * Bounded memory: one HL_STREAM_CHUNK in flight regardless of file size.
 */
#define HL_STREAM_CHUNK   (4u * 1024u * 1024u)
#define HL_STREAM_AUTO_THRESHOLD  (90u * 1024u * 1024u)  /* legacy get/put cap */
#define HL_PATH_MAX               4096  /* composed root + relative path, both directions */

/* Headroom for the local free-space check before a get. We don't want to
 * fill the partition to the byte; reserve a small fixed buffer so the user
 * doesn't end up with a wedged disk on a tight squeeze. */
#define HL_GET_FREE_SPACE_HEADROOM  (16ull * 1024 * 1024)

/* Result of a get_stat probe. For directories, `files` and `count` are owned
 * by the caller (free with get_stat_free). For files, both are NULL/0. */
typedef struct {
    int       isdir;
    uint64_t  size;            /* file size, or sum of files if isdir */
    int       truncated;       /* daemon hit HL_GET_STAT_MAX_FILES */
    char      err[256];        /* populated on truncation, used as warning */
    /* Directory listing: parallel arrays so we don't allocate a struct per
     * entry. Each path is a malloc'd string relative to the requested root. */
    char    **paths;
    uint64_t *sizes;
    size_t    count;
} get_stat_t;

static void get_stat_free(get_stat_t *gs) {
    if (!gs) return;
    if (gs->paths) {
        for (size_t i = 0; i < gs->count; i++) free(gs->paths[i]);
        free(gs->paths);
    }
    free(gs->sizes);
    gs->paths = NULL; gs->sizes = NULL; gs->count = 0;
}

/*
 * Free space on the far side, for the destination of a put.
 *
 * Returns EXIT_OK and sets *free_bytes when the daemon answers. Any other
 * outcome — an older daemon that does not know the request type, an
 * unstattable path — is reported as a failure the CALLER IS EXPECTED TO
 * IGNORE, falling back to the pre-1.6 behaviour of just trying the transfer.
 * A capability probe must never be the thing that stops a put from happening.
 */
static int query_stat_fs(cli_opts_t *opts, const char *remote_path,
                         uint64_t *free_bytes) {
    *free_bytes = 0;

    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "stat_fs");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);
    int sent = frame_send_json(fd, req);
    cJSON_Delete(req);
    if (sent != 0) { close(fd); return EXIT_CONN_FAILED; }

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); return EXIT_PROTO_ERR; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    const char *status = "";
    cJSON *j = cJSON_GetObjectItem(resp, "status");
    if (cJSON_IsString(j)) status = j->valuestring;
    int rc = EXIT_REMOTE_ERR;
    if (!strcmp(status, "ok")) {
        j = cJSON_GetObjectItem(resp, "free_bytes");
        if (cJSON_IsNumber(j)) { *free_bytes = (uint64_t)j->valuedouble; rc = EXIT_OK; }
    }
    cJSON_Delete(resp);
    return rc;
}

/* `df <remote_path>` — the free-space probe, surfaced. Mostly it exists so the
 * put pre-check is observable (and testable) rather than a hidden branch, but
 * "how much room is left over there" is a question worth being able to ask. */
static int cmd_df(cli_opts_t *opts, const char *remote_path) {
    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "stat_fs");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);
    int sent = frame_send_json(fd, req);
    cJSON_Delete(req);
    if (sent != 0) { close(fd); return EXIT_CONN_FAILED; }

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); return EXIT_PROTO_ERR; }
    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    const char *status = "", *node = "", *resolved = "", *emsg = NULL;
    double freeb = 0, totalb = 0;
    cJSON *j;
    j = cJSON_GetObjectItem(resp, "status");      if (cJSON_IsString(j)) status   = j->valuestring;
    j = cJSON_GetObjectItem(resp, "node");        if (cJSON_IsString(j)) node     = j->valuestring;
    j = cJSON_GetObjectItem(resp, "resolved");    if (cJSON_IsString(j)) resolved = j->valuestring;
    j = cJSON_GetObjectItem(resp, "error_msg");   if (cJSON_IsString(j)) emsg     = j->valuestring;
    j = cJSON_GetObjectItem(resp, "free_bytes");  if (cJSON_IsNumber(j)) freeb    = j->valuedouble;
    j = cJSON_GetObjectItem(resp, "total_bytes"); if (cJSON_IsNumber(j)) totalb   = j->valuedouble;

    int rc = EXIT_OK;
    if (strcmp(status, "ok")) {
        fprintf(stderr, "df: %s%s%s\n", status, emsg ? ": " : "", emsg ? emsg : "");
        rc = EXIT_REMOTE_ERR;
    } else if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
    } else {
        printf("%.0f free of %.0f bytes on %s [%s]", freeb, totalb, resolved, node);
        if (strcmp(resolved, remote_path))
            printf("  (nearest existing ancestor of %s)", remote_path);
        printf("\n");
    }
    cJSON_Delete(resp);
    return rc;
}

/* Probe the remote daemon for size/type information about `remote_path`.
 * On success returns 0 and populates *out (caller must get_stat_free).
 * On error returns one of EXIT_* and writes a human message to stderr.
 *
 * One round-trip; cheap on Unix sockets, ~1ms over 10GbE. */
static int query_get_stat(cli_opts_t *opts, const char *remote_path,
                          get_stat_t *out) {
    memset(out, 0, sizeof(*out));

    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "get_stat");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(fd);
        fprintf(stderr, "get: failed to send stat request\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); return EXIT_PROTO_ERR; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) return EXIT_PROTO_ERR;

    cJSON *j;
    const char *status = "", *node = "", *err_msg = NULL;
    j = cJSON_GetObjectItem(resp, "status");    if (cJSON_IsString(j)) status  = j->valuestring;
    j = cJSON_GetObjectItem(resp, "node");      if (cJSON_IsString(j)) node    = j->valuestring;
    j = cJSON_GetObjectItem(resp, "error_msg"); if (cJSON_IsString(j)) err_msg = j->valuestring;

    if (!strcmp(status, "auth_failed")) {
        if (opts->json_output) {
            char *s = cJSON_PrintUnformatted(resp);
            if (s) { printf("%s\n", s); free(s); }
        }
        fprintf(stderr, "Authentication failed\n");
        cJSON_Delete(resp); return EXIT_AUTH_FAILED;
    }
    if (strcmp(status, "ok") != 0) {
        /* Preserve the legacy `-j get` contract: the daemon's error JSON
         * lands on stdout so callers can parse it. */
        if (opts->json_output) {
            char *s = cJSON_PrintUnformatted(resp);
            if (s) { printf("%s\n", s); free(s); }
        }
        fprintf(stderr, "[%s] get error: %s\n",
                node, err_msg ? err_msg : status);
        cJSON_Delete(resp); return EXIT_REMOTE_ERR;
    }

    j = cJSON_GetObjectItem(resp, "isdir"); out->isdir = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(resp, "size");
    if (cJSON_IsNumber(j)) out->size = (uint64_t)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "truncated"); out->truncated = cJSON_IsTrue(j);
    if (out->truncated && err_msg)
        snprintf(out->err, sizeof(out->err), "%s", err_msg);

    if (out->isdir) {
        cJSON *files = cJSON_GetObjectItem(resp, "files");
        if (cJSON_IsArray(files)) {
            int sz = cJSON_GetArraySize(files);
            if (sz > 0) {
                out->paths = calloc((size_t)sz, sizeof(char *));
                out->sizes = calloc((size_t)sz, sizeof(uint64_t));
                if (!out->paths || !out->sizes) {
                    get_stat_free(out);
                    cJSON_Delete(resp);
                    fprintf(stderr, "get: out of memory parsing stat response\n");
                    return EXIT_CLIENT_ERR;
                }
            }
            for (int i = 0; i < sz; i++) {
                cJSON *e = cJSON_GetArrayItem(files, i);
                cJSON *pj = cJSON_GetObjectItem(e, "path");
                cJSON *sj = cJSON_GetObjectItem(e, "size");
                if (!cJSON_IsString(pj) || !cJSON_IsNumber(sj)) continue;
                out->paths[out->count] = strdup(pj->valuestring);
                out->sizes[out->count] = (uint64_t)sj->valuedouble;
                out->count++;
            }
        }
    }

    cJSON_Delete(resp);
    return EXIT_OK;
}

/* Recursively mkdir -p `path`. Returns 0 on success.
 * On failure, sets errno and returns -1. */
static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Find the deepest existing directory in the path of `local_path`. We can't
 * statvfs a path that doesn't exist, but we can statvfs its closest extant
 * ancestor — same filesystem, same free-space answer. */
static int find_extant_ancestor(const char *local_path, char *out, size_t out_sz) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", local_path);
    while (1) {
        struct stat st;
        if (stat(buf, &st) == 0) { snprintf(out, out_sz, "%s", buf); return 0; }
        char *slash = strrchr(buf, '/');
        if (!slash) { snprintf(out, out_sz, "."); return 0; }
        if (slash == buf) { snprintf(out, out_sz, "/"); return 0; }
        *slash = '\0';
    }
}

/* Verify the local filesystem at `local_path` has at least `need` bytes free
 * (plus headroom). If not, print a clear message and return EXIT_CLIENT_ERR.
 *
 * We use f_bavail (blocks available to non-root) rather than f_bfree, which
 * matches what users see from `df` and avoids surprises when the partition
 * has root-reserve set (ext4 reserves 5% by default). */
static int check_free_space(const char *local_path, uint64_t need) {
    char ancestor[1024];
    if (find_extant_ancestor(local_path, ancestor, sizeof(ancestor)) < 0) {
        fprintf(stderr, "get: cannot resolve filesystem for %s: %s\n",
                local_path, strerror(errno));
        return EXIT_CLIENT_ERR;
    }
    struct statvfs vfs;
    if (statvfs(ancestor, &vfs) < 0) {
        fprintf(stderr, "get: statvfs(%s) failed: %s\n",
                ancestor, strerror(errno));
        return EXIT_CLIENT_ERR;
    }
    uint64_t avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    uint64_t want  = need + HL_GET_FREE_SPACE_HEADROOM;
    if (avail < want) {
        fprintf(stderr,
                "get: insufficient disk space on %s: need %llu bytes "
                "(%.2f MiB) plus %llu MiB headroom, only %llu bytes "
                "(%.2f MiB) available\n",
                ancestor,
                (unsigned long long)need,
                (double)need / (1024.0 * 1024.0),
                (unsigned long long)(HL_GET_FREE_SPACE_HEADROOM / (1024 * 1024)),
                (unsigned long long)avail,
                (double)avail / (1024.0 * 1024.0));
        return EXIT_CLIENT_ERR;
    }
    return EXIT_OK;
}

static int cmd_get_stream(cli_opts_t *opts, const char *remote_path, const char *local_path) {
    int fd = open_connection(opts);
    if (fd < 0) return EXIT_CONN_FAILED;

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "get_stream");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(fd);
        fprintf(stderr, "get_stream: failed to send request\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    /* Receive header */
    char *hdr_buf = NULL;
    ssize_t hn = frame_recv(fd, &hdr_buf);
    if (hn <= 0) { free(hdr_buf); close(fd); return EXIT_PROTO_ERR; }
    cJSON *hdr = cJSON_Parse(hdr_buf);
    free(hdr_buf);
    if (!hdr) { close(fd); return EXIT_PROTO_ERR; }

    /* Copy out string fields before cJSON_Delete frees them — they're
     * referenced later in error/success messages. */
    char        node[64]    = "";
    char        err_msg[256] = "";
    const char *status      = "";
    double      size_d      = 0;
    cJSON *jx;
    jx = cJSON_GetObjectItem(hdr, "status");
    if (cJSON_IsString(jx)) status = jx->valuestring;
    jx = cJSON_GetObjectItem(hdr, "node");
    if (cJSON_IsString(jx)) snprintf(node, sizeof(node), "%s", jx->valuestring);
    jx = cJSON_GetObjectItem(hdr, "size");
    if (cJSON_IsNumber(jx)) size_d = jx->valuedouble;
    jx = cJSON_GetObjectItem(hdr, "error_msg");
    if (cJSON_IsString(jx)) snprintf(err_msg, sizeof(err_msg), "%s", jx->valuestring);

    if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n");
        cJSON_Delete(hdr); close(fd); return EXIT_AUTH_FAILED;
    }
    if (strcmp(status, "ok") != 0) {
        char status_copy[64];
        snprintf(status_copy, sizeof(status_copy), "%s", status);
        cJSON_Delete(hdr); close(fd);
        fprintf(stderr, "[%s] get_stream error: %s\n",
                node, err_msg[0] ? err_msg : status_copy);
        return EXIT_REMOTE_ERR;
    }
    cJSON_Delete(hdr);

    uint64_t expected = (uint64_t)size_d;

    /* Open local destination */
    int wfd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        fprintf(stderr, "get_stream: cannot write local file %s: %s\n",
                local_path, strerror(errno));
        close(fd);
        return EXIT_CLIENT_ERR;
    }

    sha256_ctx_t hctx;
    sha256_init(&hctx);
    uint64_t total = 0;

    /* Receive raw chunks until we've got the whole file */
    while (total < expected) {
        char *chunk = NULL;
        ssize_t n = frame_recv(fd, &chunk);
        if (n <= 0) {
            free(chunk);
            fprintf(stderr, "get_stream: chunk recv failed at %llu/%llu bytes\n",
                    (unsigned long long)total, (unsigned long long)expected);
            close(wfd); unlink(local_path); close(fd);
            return EXIT_PROTO_ERR;
        }
        if (write_all(wfd, chunk, (size_t)n) != 0) {
            fprintf(stderr, "get_stream: local write failed: %s\n", strerror(errno));
            free(chunk); close(wfd); unlink(local_path); close(fd);
            return EXIT_CLIENT_ERR;
        }
        sha256_update(&hctx, chunk, (size_t)n);
        total += (uint64_t)n;
        free(chunk);
    }
    close(wfd);

    uint8_t digest[SHA256_DIGEST_LEN];
    char    local_hex[SHA256_HEX_LEN];
    sha256_final(&hctx, digest);
    sha256_hex(digest, local_hex);

    /* Receive trailer with daemon's claimed sha256 */
    char *tail_buf = NULL;
    ssize_t tn = frame_recv(fd, &tail_buf);
    close(fd);
    if (tn <= 0 || !tail_buf) {
        free(tail_buf); unlink(local_path);
        fprintf(stderr, "get_stream: trailer recv failed\n");
        return EXIT_PROTO_ERR;
    }
    cJSON *tail = cJSON_Parse(tail_buf);
    free(tail_buf);
    const char *server_sha = "";
    if (tail) {
        cJSON *sj = cJSON_GetObjectItem(tail, "sha256");
        if (cJSON_IsString(sj)) server_sha = sj->valuestring;
    }
    if (server_sha[0] == '\0' || strcmp(server_sha, local_hex) != 0) {
        fprintf(stderr, "get_stream: SHA-256 mismatch (server=%s local=%s) — "
                        "file discarded\n",
                server_sha[0] ? server_sha : "(none)", local_hex);
        if (tail) cJSON_Delete(tail);
        unlink(local_path);
        return EXIT_PROTO_ERR;
    }
    if (tail) cJSON_Delete(tail);

    if (!opts->json_output)
        fprintf(stderr, "[%s] get_stream ok: %s -> %s (%llu bytes, sha256=%.16s...)\n",
                node, remote_path, local_path,
                (unsigned long long)total, local_hex);
    else
        printf("{\"status\":\"ok\",\"node\":\"%s\",\"size\":%llu,\"sha256\":\"%s\"}\n",
               node, (unsigned long long)total, local_hex);

    return EXIT_OK;
}

/* cmd_get_smart: the transparent `get` entry point (v1.4+).
 *
 * One round-trip stat tells us:
 *   1. Is the remote a file or a directory?
 *   2. What's the total size? (sum of files for a directory)
 *
 * From there we:
 *   - Refuse early if the local filesystem doesn't have room (statvfs).
 *   - Pick legacy or streaming per-file based on size (90 MiB threshold).
 *   - For directories, mkdir the destination and pull each file into its
 *     relative slot (cp-style: `get /remote/foo /tmp/bar` puts files at
 *     /tmp/bar/<rel>). Aborts on the first failure — partial files stay so
 *     the user can see how far it got.
 *
 * Honors --stream (force streaming for the file path) for users who want
 * the SHA-256 verification on small files too. */
static int cmd_get_smart(cli_opts_t *opts, const char *remote_path,
                         const char *local_path) {
    get_stat_t gs;
    int rc = query_get_stat(opts, remote_path, &gs);
    if (rc != EXIT_OK) return rc;

    if (gs.truncated) {
        fprintf(stderr, "get: %s\n",
                gs.err[0] ? gs.err : "remote listing truncated");
        get_stat_free(&gs);
        return EXIT_REMOTE_ERR;
    }

    /* Free-space check uses the destination's filesystem. For a dir we look
     * at local_path itself (which we'll create); for a file we look at its
     * parent. find_extant_ancestor walks up until it hits something that
     * exists, so passing either is safe. */
    rc = check_free_space(local_path, gs.size);
    if (rc != EXIT_OK) { get_stat_free(&gs); return rc; }

    if (!gs.isdir) {
        get_stat_free(&gs);
        int use_stream = opts->stream || gs.size > HL_STREAM_AUTO_THRESHOLD;
        return use_stream
             ? cmd_get_stream(opts, remote_path, local_path)
             : cmd_get       (opts, remote_path, local_path);
    }

    /* Directory mode. Refuse to clobber an existing file at local_path. */
    struct stat lst;
    if (stat(local_path, &lst) == 0 && !S_ISDIR(lst.st_mode)) {
        fprintf(stderr,
                "get: destination %s exists and is not a directory\n",
                local_path);
        get_stat_free(&gs);
        return EXIT_CLIENT_ERR;
    }
    if (mkdir_p(local_path, 0755) < 0) {
        fprintf(stderr, "get: cannot create %s: %s\n",
                local_path, strerror(errno));
        get_stat_free(&gs);
        return EXIT_CLIENT_ERR;
    }

    if (!opts->json_output)
        fprintf(stderr, "get: %zu files, %llu bytes total, dest=%s\n",
                gs.count, (unsigned long long)gs.size, local_path);

    int dir_rc = EXIT_OK;
    for (size_t i = 0; i < gs.count; i++) {
        char remote_full[HL_PATH_MAX];
        char local_full[HL_PATH_MAX];
        /* A silently truncated path is a path to the wrong file — check. */
        int rn = snprintf(remote_full, sizeof(remote_full), "%s/%s",
                          remote_path, gs.paths[i]);
        int ln = snprintf(local_full,  sizeof(local_full),  "%s/%s",
                          local_path, gs.paths[i]);
        if (rn < 0 || (size_t)rn >= sizeof(remote_full) ||
            ln < 0 || (size_t)ln >= sizeof(local_full)) {
            fprintf(stderr,
                    "get: path too long (limit %d bytes), stopping at: %s/%s\n",
                    HL_PATH_MAX - 1, local_path, gs.paths[i]);
            dir_rc = EXIT_CLIENT_ERR;
            break;
        }

        /* mkdir for the file's parent. dirname() may modify its arg, so
         * work on a copy. */
        char parent_buf[HL_PATH_MAX];
        snprintf(parent_buf, sizeof(parent_buf), "%s", local_full);
        char *parent = dirname(parent_buf);
        if (parent && strcmp(parent, ".") != 0 && strcmp(parent, "/") != 0) {
            if (mkdir_p(parent, 0755) < 0) {
                fprintf(stderr, "get: cannot create %s: %s\n",
                        parent, strerror(errno));
                dir_rc = EXIT_CLIENT_ERR;
                break;
            }
        }

        int use_stream = opts->stream || gs.sizes[i] > HL_STREAM_AUTO_THRESHOLD;
        int file_rc = use_stream
                    ? cmd_get_stream(opts, remote_full, local_full)
                    : cmd_get       (opts, remote_full, local_full);
        if (file_rc != EXIT_OK) {
            fprintf(stderr,
                    "get: aborting after failure on %s (%zu of %zu done)\n",
                    gs.paths[i], i, gs.count);
            dir_rc = file_rc;
            break;
        }
    }

    get_stat_free(&gs);
    return dir_rc;
}

/* `sha_out`, when non-NULL, receives the digest the DAEMON confirmed after
 * writing the file — the raw material for the directory-put manifest (F5).
 * `quiet` suppresses the per-file success line, which is noise once there is a
 * manifest summarising the whole tree. */
static int cmd_put_stream_ex(cli_opts_t *opts, const char *local_path,
                             const char *remote_path, char *sha_out, int quiet) {
    /* Stat local file for size */
    struct stat st;
    if (stat(local_path, &st) < 0) {
        fprintf(stderr, "put_stream: cannot stat %s: %s\n", local_path, strerror(errno));
        return EXIT_CLIENT_ERR;
    }
    int rfd = open(local_path, O_RDONLY);
    if (rfd < 0) {
        fprintf(stderr, "put_stream: cannot open %s: %s\n", local_path, strerror(errno));
        return EXIT_CLIENT_ERR;
    }

    int fd = open_connection(opts);
    if (fd < 0) { close(rfd); return EXIT_CONN_FAILED; }

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "put_stream");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "path",    remote_path);
    cJSON_AddNumberToObject(req, "size",    (double)st.st_size);
    if (opts->put_mode_val > 0)
        cJSON_AddNumberToObject(req, "mode",  (double)opts->put_mode_val);
    if (opts->put_mkdir)
        cJSON_AddBoolToObject(req, "mkdir", 1);

    if (frame_send_json(fd, req) != 0) {
        cJSON_Delete(req); close(rfd); close(fd);
        fprintf(stderr, "put_stream: failed to send request\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(req);

    /* Receive ready/error response */
    char *ready_buf = NULL;
    ssize_t rn = frame_recv(fd, &ready_buf);
    if (rn <= 0) { free(ready_buf); close(rfd); close(fd); return EXIT_PROTO_ERR; }
    cJSON *ready = cJSON_Parse(ready_buf);
    free(ready_buf);
    if (!ready) { close(rfd); close(fd); return EXIT_PROTO_ERR; }

    /* Copy out string fields before cJSON_Delete frees them. */
    char        node[64]    = "";
    char        err_msg[256] = "";
    const char *status      = "";
    cJSON *jx;
    jx = cJSON_GetObjectItem(ready, "status");
    if (cJSON_IsString(jx)) status = jx->valuestring;
    jx = cJSON_GetObjectItem(ready, "node");
    if (cJSON_IsString(jx)) snprintf(node, sizeof(node), "%s", jx->valuestring);
    jx = cJSON_GetObjectItem(ready, "error_msg");
    if (cJSON_IsString(jx)) snprintf(err_msg, sizeof(err_msg), "%s", jx->valuestring);

    if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n");
        cJSON_Delete(ready); close(rfd); close(fd); return EXIT_AUTH_FAILED;
    }
    if (strcmp(status, "ok") != 0) {
        char status_copy[64];
        snprintf(status_copy, sizeof(status_copy), "%s", status);
        cJSON_Delete(ready); close(rfd); close(fd);
        fprintf(stderr, "[%s] put_stream error: %s\n",
                node, err_msg[0] ? err_msg : status_copy);
        return EXIT_REMOTE_ERR;
    }
    cJSON_Delete(ready);

    /* Stream chunks */
    sha256_ctx_t hctx;
    sha256_init(&hctx);

    uint8_t *buf = malloc(HL_STREAM_CHUNK);
    if (!buf) { close(rfd); close(fd); return EXIT_CLIENT_ERR; }

    uint64_t total = 0;
    while (total < (uint64_t)st.st_size) {
        size_t want = (uint64_t)st.st_size - total;
        if (want > HL_STREAM_CHUNK) want = HL_STREAM_CHUNK;

        ssize_t got = 0;
        while ((size_t)got < want) {
            ssize_t n = read(rfd, buf + got, want - (size_t)got);
            if (n < 0) {
                if (errno == EINTR) continue;
                free(buf); close(rfd); close(fd);
                fprintf(stderr, "put_stream: local read error: %s\n", strerror(errno));
                return EXIT_CLIENT_ERR;
            }
            if (n == 0) break;
            got += n;
        }
        if (got == 0) break;

        if (frame_send(fd, (const char *)buf, (size_t)got) != 0) {
            free(buf); close(rfd); close(fd);
            fprintf(stderr, "put_stream: chunk send failed\n");
            return EXIT_CONN_FAILED;
        }
        sha256_update(&hctx, buf, (size_t)got);
        total += (uint64_t)got;
    }

    free(buf);
    close(rfd);

    uint8_t digest[SHA256_DIGEST_LEN];
    char    local_hex[SHA256_HEX_LEN];
    sha256_final(&hctx, digest);
    sha256_hex(digest, local_hex);

    /* Send trailer */
    cJSON *trailer = cJSON_CreateObject();
    cJSON_AddNumberToObject(trailer, "version",    1);
    cJSON_AddStringToObject(trailer, "id",         req_id);
    cJSON_AddStringToObject(trailer, "sha256",     local_hex);
    cJSON_AddBoolToObject  (trailer, "stream_end", 1);
    if (frame_send_json(fd, trailer) != 0) {
        cJSON_Delete(trailer); close(fd);
        fprintf(stderr, "put_stream: trailer send failed\n");
        return EXIT_CONN_FAILED;
    }
    cJSON_Delete(trailer);

    /* Receive final daemon ack */
    char *resp_buf = NULL;
    ssize_t rrn = frame_recv(fd, &resp_buf);
    close(fd);
    if (rrn <= 0 || !resp_buf) { free(resp_buf); return EXIT_PROTO_ERR; }
    cJSON *resp = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!resp) return EXIT_PROTO_ERR;

    int  ret = EXIT_OK;
    char resp_status[32]  = "";
    char resp_msg[256]    = "";
    char resp_sha[SHA256_HEX_LEN] = "";
    jx = cJSON_GetObjectItem(resp, "status");
    if (cJSON_IsString(jx)) snprintf(resp_status, sizeof(resp_status), "%s", jx->valuestring);
    jx = cJSON_GetObjectItem(resp, "error_msg");
    if (cJSON_IsString(jx)) snprintf(resp_msg, sizeof(resp_msg), "%s", jx->valuestring);
    jx = cJSON_GetObjectItem(resp, "sha256");
    if (cJSON_IsString(jx)) snprintf(resp_sha, sizeof(resp_sha), "%s", jx->valuestring);
    char *resp_json_str = NULL;
    if (opts->json_output) resp_json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (strcmp(resp_status, "ok") != 0) {
        fprintf(stderr, "[%s] put_stream error: %s\n",
                node, resp_msg[0] ? resp_msg : resp_status);
        ret = EXIT_REMOTE_ERR;
    } else if (opts->json_output) {
        if (resp_json_str) printf("%s\n", resp_json_str);
    } else if (!quiet) {
        fprintf(stderr, "[%s] put_stream ok: %s -> %s (%llu bytes, sha256=%.16s...)\n",
                node, local_path, remote_path,
                (unsigned long long)total, resp_sha);
    }
    if (sha_out && ret == EXIT_OK) snprintf(sha_out, SHA256_HEX_LEN, "%s", resp_sha);
    free(resp_json_str);
    return ret;
}

static int cmd_put_stream(cli_opts_t *opts, const char *local_path,
                          const char *remote_path) {
    return cmd_put_stream_ex(opts, local_path, remote_path, NULL, 0);
}

/* ── Directory put (v1.5.1) ───────────────────────────────────────────────
 *
 * `get` has handled directories transparently since 1.4.0; `put` was strictly
 * one file, so shipping a tree meant a hand-written `find | while read` loop
 * plus pre-creating every nested subdirectory by hand. (Syncing this very
 * repo's 22 source files to Spark for the 1.5.0 build was exactly that loop.)
 *
 * The walk mirrors the daemon's `get_stat` walk deliberately, so that a put
 * and a get of the same tree agree on what "the tree" is:
 *   - nftw with FTW_PHYS: symlinks are NOT followed (no loops, no surprise
 *     copies of whatever they point at) and are not themselves transferred.
 *   - regular files only; directories, symlinks, sockets, devices are skipped.
 *   - the same HL_*_MAX_FILES ceiling, so neither side can be walked forever.
 *   - relative paths computed by stripping the root prefix.
 *
 * Consequence worth knowing (shared with `get`): a tree's *empty* directories
 * are not recreated, because only files are transferred.
 */
#define HL_PUT_MAX_FILES  100000   /* mirrors HL_GET_STAT_MAX_FILES */

typedef struct {
    char     *rel;
    uint64_t  size;
} put_entry_t;

/* nftw takes no user-data pointer, so the callback works through these. The
 * client is one process per invocation, so this is contained. */
static put_entry_t  *g_put_files;
static size_t        g_put_count, g_put_cap;
static uint64_t      g_put_total;
static size_t        g_put_root_len;
static const char   *g_put_root;
static int           g_put_capped, g_put_oom, g_put_skipped_nonreg;
static char * const *g_put_excludes;
static int           g_put_nexcludes;

/* True if `rel` should be excluded. A pattern matches when it matches the
 * basename, the whole relative path, or any leading directory component —
 * so `--exclude .cache` prunes the whole `.cache` subtree, which is the case
 * that motivated
 * the flag (HuggingFace's --local-dir leaves a .cache/ nobody wants shipped). */
static int put_excluded(const char *rel) {
    if (g_put_nexcludes == 0) return 0;

    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;

    for (int i = 0; i < g_put_nexcludes; i++) {
        const char *pat = g_put_excludes[i];
        if (fnmatch(pat, base, 0) == 0) return 1;
        if (fnmatch(pat, rel,  0) == 0) return 1;

        /* leading directory components */
        const char *slash = rel;
        while ((slash = strchr(slash, '/')) != NULL) {
            size_t complen = (size_t)(slash - rel);
            char comp[HL_PATH_MAX];
            if (complen < sizeof(comp)) {
                memcpy(comp, rel, complen);
                comp[complen] = '\0';
                if (fnmatch(pat, comp, 0) == 0) return 1;
                const char *cbase = strrchr(comp, '/');
                if (cbase && fnmatch(pat, cbase + 1, 0) == 0) return 1;
            }
            slash++;
        }
    }
    return 0;
}

static int put_walk_cb(const char *fpath, const struct stat *sb,
                       int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (typeflag != FTW_F || !S_ISREG(sb->st_mode)) {
        /* FTW_SL (symlink) and specials land here; count them so we can say
         * so afterwards rather than silently shipping a partial tree. */
        if (typeflag == FTW_SL || (typeflag == FTW_F && !S_ISREG(sb->st_mode)))
            g_put_skipped_nonreg++;
        return 0;
    }
    if (g_put_count >= HL_PUT_MAX_FILES) { g_put_capped = 1; return 1; }

    const char *rel = fpath;
    if (g_put_root_len > 0 && strncmp(fpath, g_put_root, g_put_root_len) == 0) {
        rel = fpath + g_put_root_len;
        if (*rel == '/') rel++;
    }
    if (!*rel) return 0;
    if (put_excluded(rel)) return 0;

    if (g_put_count == g_put_cap) {
        size_t newcap = g_put_cap ? g_put_cap * 2 : 64;
        put_entry_t *nf = realloc(g_put_files, newcap * sizeof(*nf));
        if (!nf) { g_put_oom = 1; return 1; }
        g_put_files = nf;
        g_put_cap   = newcap;
    }
    char *dup = strdup(rel);
    if (!dup) { g_put_oom = 1; return 1; }
    g_put_files[g_put_count].rel  = dup;
    g_put_files[g_put_count].size = (uint64_t)sb->st_size;
    g_put_count++;
    g_put_total += (uint64_t)sb->st_size;
    return 0;
}

static void put_list_free(void) {
    for (size_t i = 0; i < g_put_count; i++) free(g_put_files[i].rel);
    free(g_put_files);
    g_put_files = NULL;
    g_put_count = g_put_cap = 0;
}

/* cmd_put_dir: transfer a local directory, cp-style — `put /l/foo /r/bar`
 * places files at /r/bar/<rel>, matching `get`'s directory semantics.
 * Aborts on the first failure, leaving what already transferred in place so
 * the caller can see how far it got (again, as `get` does). */
static int cmd_put_dir(cli_opts_t *opts, const char *local_path,
                       const char *remote_path) {
    /* Refuse to scatter a tree underneath an existing remote *file*. A stat
     * that fails most likely means "not there yet", which is fine — we let the
     * per-file put report it. Only a successful stat saying "regular file" is
     * unambiguously wrong. */
    get_stat_t rs;
    if (query_get_stat(opts, remote_path, &rs) == EXIT_OK) {
        int remote_is_file = !rs.isdir;
        get_stat_free(&rs);
        if (remote_is_file) {
            fprintf(stderr,
                    "put: remote %s exists and is not a directory\n",
                    remote_path);
            return EXIT_CLIENT_ERR;
        }
    }

    char root[HL_PATH_MAX];
    snprintf(root, sizeof(root), "%s", local_path);
    size_t rlen = strlen(root);
    while (rlen > 1 && root[rlen - 1] == '/') root[--rlen] = '\0';

    const char *exc[HL_MAX_EXCLUDES];
    for (int i = 0; i < opts->exclude_count; i++) exc[i] = opts->excludes[i];
    g_put_excludes  = (char * const *)exc;
    g_put_nexcludes = opts->exclude_count;

    g_put_files = NULL;
    g_put_count = g_put_cap = 0;
    g_put_total = 0;
    g_put_root  = root;
    g_put_root_len = rlen;
    g_put_capped = g_put_oom = g_put_skipped_nonreg = 0;

    if (nftw(root, put_walk_cb, 32, FTW_PHYS) < 0 && !g_put_capped && !g_put_oom) {
        fprintf(stderr, "put: cannot walk %s: %s\n", root, strerror(errno));
        put_list_free();
        return EXIT_CLIENT_ERR;
    }
    if (g_put_oom) {
        fprintf(stderr, "put: out of memory listing %s\n", root);
        put_list_free();
        return EXIT_CLIENT_ERR;
    }
    if (g_put_capped) {
        fprintf(stderr, "put: %s holds more than %d files; refusing to walk further\n",
                root, HL_PUT_MAX_FILES);
        put_list_free();
        return EXIT_CLIENT_ERR;
    }
    if (g_put_count == 0) {
        fprintf(stderr, "put: %s contains no regular files to transfer%s\n", root,
                g_put_nexcludes ? " (after --exclude)" : "");
        put_list_free();
        return EXIT_OK;
    }

    if (!opts->json_output) {
        fprintf(stderr, "put: %zu files, %llu bytes total, dest=%s\n",
                g_put_count, (unsigned long long)g_put_total, remote_path);
        if (g_put_skipped_nonreg)
            fprintf(stderr, "put: skipped %d symlink/special entries (not followed)\n",
                    g_put_skipped_nonreg);
    }

    /* Remote free space (the asymmetry left open in 1.5.1).
     *
     * `get` has always checked the LOCAL filesystem before pulling; `put` had
     * no equivalent because the daemon could not be asked. It can now, so ask
     * — before the first byte rather than on whichever file runs the far side
     * out of room, gigabytes in. A daemon that does not answer (older than
     * 1.6.0, or a path it cannot stat) just means no check: the transfer
     * proceeds exactly as it did before. */
    uint64_t remote_free = 0;
    if (query_stat_fs(opts, remote_path, &remote_free) == EXIT_OK && remote_free > 0) {
        if (g_put_total + HL_GET_FREE_SPACE_HEADROOM > remote_free) {
            fprintf(stderr,
                    "put: not enough space on the remote filesystem for %s\n"
                    "     need %llu bytes (+%llu headroom), %llu available\n",
                    remote_path, (unsigned long long)g_put_total,
                    (unsigned long long)HL_GET_FREE_SPACE_HEADROOM,
                    (unsigned long long)remote_free);
            put_list_free();
            return EXIT_CLIENT_ERR;
        }
    }

    /* The daemon creates missing parents when mkdir is set, and it is the only
     * thing that can — so a directory put always implies it. */
    int saved_mkdir = opts->put_mkdir;
    opts->put_mkdir = 1;

    int dir_rc = EXIT_OK;
    size_t done = 0;
    size_t verified = 0;
    for (size_t i = 0; i < g_put_count; i++) {
        char local_full[HL_PATH_MAX], remote_full[HL_PATH_MAX];
        /* snprintf truncates silently, and a truncated path is a path to the
         * WRONG file — the one class of bug this release exists to remove.
         * Check both and say so plainly. */
        int ln = snprintf(local_full,  sizeof(local_full),  "%s/%s", root, g_put_files[i].rel);
        int rn = snprintf(remote_full, sizeof(remote_full), "%s/%s", remote_path, g_put_files[i].rel);
        if (ln < 0 || (size_t)ln >= sizeof(local_full) ||
            rn < 0 || (size_t)rn >= sizeof(remote_full)) {
            fprintf(stderr,
                    "put: path too long (limit %d bytes), stopping at: %s/%s\n"
                    "     %zu of %zu files transferred\n",
                    HL_PATH_MAX - 1, remote_path, g_put_files[i].rel, done, g_put_count);
            dir_rc = EXIT_CLIENT_ERR;
            break;
        }

        /* --verify forces every file down the streaming path, which is the
         * only one that hashes: the daemon writes the file, hashes what it
         * wrote, and the client compares. A "verified" manifest built from the
         * legacy base64 path would be a manifest of digests nobody checked. */
        int use_stream = opts->stream || opts->verify ||
                         g_put_files[i].size > HL_STREAM_AUTO_THRESHOLD;
        char sha[SHA256_HEX_LEN] = "";
        int file_rc = use_stream
                    ? cmd_put_stream_ex(opts, local_full, remote_full,
                                        opts->verify ? sha : NULL, opts->verify)
                    : cmd_put          (opts, local_full, remote_full);
        if (file_rc == EXIT_OK && opts->verify && sha[0]) {
            /* sha256sum's own format, so the manifest is worth keeping:
             *   hl put ./tree /remote/tree --verify > manifest.txt */
            printf("%s  %s\n", sha, g_put_files[i].rel);
            verified++;
        }
        if (file_rc != EXIT_OK) {
            fprintf(stderr,
                    "put: failed on %s (%zu of %zu transferred); stopping\n",
                    g_put_files[i].rel, done, g_put_count);
            dir_rc = file_rc;
            break;
        }
        done++;
    }

    opts->put_mkdir = saved_mkdir;

    if (opts->verify) {
        fflush(stdout);
        /* The count is the whole point: "107 of 107 verified" is an answer,
         * where a stream of per-file digests is only evidence. A short count
         * means the loop stopped early, and the failure was already reported
         * above. */
        fprintf(stderr, "put: verify manifest — %zu of %zu files sha256-verified "
                        "by the remote%s\n",
                verified, g_put_count,
                verified == g_put_count ? "" : "  ** INCOMPLETE **");
    }
    if (dir_rc == EXIT_OK && !opts->json_output)
        fprintf(stderr, "put: %zu files, %llu bytes -> %s\n",
                done, (unsigned long long)g_put_total, remote_path);

    put_list_free();
    return dir_rc;
}

/* ── Jobs ─────────────────────────────────────────────────────────────────
 *
 * `-D` used to be a one-way door: the command went off into the background and
 * nothing came back — not an id, not the output, not the exit status. The
 * documented workaround was to redirect to a file inside the command and poll
 * it yourself, which is precisely the friction F6 was filed about.
 *
 * Now `-D` returns a job id and these verbs read the spool the daemon keeps:
 *
 *   job list                 what has run lately, newest first
 *   job status <id>          state, exit code, spool sizes
 *   job tail <id> [-f]       output so far, or follow to completion
 *   job wait <id>            block until it ends, exit with ITS status
 *   job cancel <id>          signal the job's process group
 *
 * `wait` and `-f` poll from the client rather than blocking the daemon. A
 * server-side wait would tie up a forked worker for the life of the job, and
 * with max_concurrent_io defaulting to 4, four waiters would shut every
 * transfer out of the daemon. Polling costs one small round trip per interval,
 * cannot exhaust anything, and survives a daemon restart mid-wait.
 */
#define HL_JOB_POLL_MS_DEFAULT 1000

typedef struct {
    char      state[32];
    int       done;
    int       exit_code;
    long long created_at, ended_at;
    long      duration_ms;
    long long stdout_bytes, stderr_bytes;
    long long stdout_original, stderr_original;
    int       stdout_truncated, stderr_truncated;
    char      command[8192];
    char      error_msg[256];
    char      node[64];
} job_status_t;

static void job_fill(cJSON *resp, job_status_t *js) {
    cJSON *j;
    memset(js, 0, sizeof(*js));
    js->exit_code = -1;
    j = cJSON_GetObjectItem(resp, "state");     if (cJSON_IsString(j)) snprintf(js->state, sizeof(js->state), "%s", j->valuestring);
    j = cJSON_GetObjectItem(resp, "node");      if (cJSON_IsString(j)) snprintf(js->node, sizeof(js->node), "%s", j->valuestring);
    j = cJSON_GetObjectItem(resp, "command");   if (cJSON_IsString(j)) snprintf(js->command, sizeof(js->command), "%s", j->valuestring);
    j = cJSON_GetObjectItem(resp, "error_msg"); if (cJSON_IsString(j)) snprintf(js->error_msg, sizeof(js->error_msg), "%s", j->valuestring);
    j = cJSON_GetObjectItem(resp, "done");      js->done = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(resp, "exit_code");    if (cJSON_IsNumber(j)) js->exit_code    = j->valueint;
    j = cJSON_GetObjectItem(resp, "created_at");   if (cJSON_IsNumber(j)) js->created_at   = (long long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "ended_at");     if (cJSON_IsNumber(j)) js->ended_at     = (long long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "duration_ms");  if (cJSON_IsNumber(j)) js->duration_ms  = (long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stdout_bytes"); if (cJSON_IsNumber(j)) js->stdout_bytes = (long long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stderr_bytes"); if (cJSON_IsNumber(j)) js->stderr_bytes = (long long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stdout_original_bytes"); if (cJSON_IsNumber(j)) js->stdout_original = (long long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stderr_original_bytes"); if (cJSON_IsNumber(j)) js->stderr_original = (long long)j->valuedouble;
    j = cJSON_GetObjectItem(resp, "stdout_truncated"); js->stdout_truncated = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(resp, "stderr_truncated"); js->stderr_truncated = cJSON_IsTrue(j);
}

/* One job request/response round trip. Returns the parsed response (caller
 * frees) or NULL, with *rc set to an EXIT_* value. */
static cJSON *job_request(cli_opts_t *opts, const char *action, const char *job_id,
                          const char *stream, long long offset, long long max_bytes,
                          const char *signame, int *rc) {
    *rc = EXIT_OK;
    int fd = open_connection(opts);
    if (fd < 0) { *rc = EXIT_CONN_FAILED; return NULL; }

    char req_id[64];
    make_request_id(req_id, sizeof(req_id));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "version", 1);
    cJSON_AddStringToObject(req, "type",    "job");
    cJSON_AddStringToObject(req, "id",      req_id);
    cJSON_AddStringToObject(req, "token",   opts->token);
    cJSON_AddStringToObject(req, "action",  action);
    if (job_id)  cJSON_AddStringToObject(req, "job_id", job_id);
    if (stream)  cJSON_AddStringToObject(req, "stream", stream);
    if (signame) cJSON_AddStringToObject(req, "signal", signame);
    if (offset > 0)    cJSON_AddNumberToObject(req, "offset",    (double)offset);
    if (max_bytes > 0) cJSON_AddNumberToObject(req, "max_bytes", (double)max_bytes);

    int sent = frame_send_json(fd, req);
    cJSON_Delete(req);
    if (sent != 0) {
        close(fd);
        fprintf(stderr, "job: failed to send request\n");
        *rc = EXIT_CONN_FAILED;
        return NULL;
    }

    char *payload = NULL;
    ssize_t n = frame_recv(fd, &payload);
    close(fd);
    if (n <= 0) { free(payload); *rc = EXIT_PROTO_ERR; return NULL; }

    cJSON *resp = cJSON_Parse(payload);
    free(payload);
    if (!resp) { *rc = EXIT_PROTO_ERR; return NULL; }

    const char *status = "", *emsg = NULL;
    cJSON *j = cJSON_GetObjectItem(resp, "status");    if (cJSON_IsString(j)) status = j->valuestring;
    j = cJSON_GetObjectItem(resp, "error_msg");        if (cJSON_IsString(j)) emsg   = j->valuestring;
    if (strcmp(status, "ok") != 0) {
        /* An old daemon does not know the type at all and says so; name that
         * case rather than leaving the caller with "bad_request". */
        if (!strcmp(status, "bad_request") && emsg && strstr(emsg, "unknown message type"))
            fprintf(stderr, "job: this daemon predates the job API (hostlink < 1.6.0)\n");
        else
            fprintf(stderr, "job: %s%s%s\n", status, emsg ? ": " : "", emsg ? emsg : "");
        /* `status` points into the cJSON tree, so decide before freeing it —
         * reading it afterwards is a use-after-free, and it read as garbage
         * often enough to turn "no such job" into a transport error. */
        *rc = !strcmp(status, "not_found") ? EXIT_REMOTE_ERR : EXIT_HOSTLINK_ERR;
        cJSON_Delete(resp);
        return NULL;
    }
    return resp;
}

/* Terminal state → process exit status, on the same principle as exec: the
 * caller should be able to write `hl job wait $id && deploy`. */
static int job_exit_status(const job_status_t *js) {
    if (!strcmp(js->state, "exited")) {
        if (js->exit_code < 0) return EXIT_HOSTLINK_ERR;
        return js->exit_code > 255 ? 255 : js->exit_code;
    }
    if (!strcmp(js->state, "timeout")) return EXIT_TIMEOUT;
    return EXIT_HOSTLINK_ERR;    /* error, lost */
}

static void job_print_status(const job_status_t *js, const char *id) {
    fprintf(stderr, "[%s] job %s %s", js->node, id, js->state);
    if (js->done && !strcmp(js->state, "exited"))
        fprintf(stderr, " exit=%d", js->exit_code);
    if (js->duration_ms) fprintf(stderr, " time=%ldms", js->duration_ms);
    fprintf(stderr, " out=%lldB err=%lldB",
            js->stdout_bytes, js->stderr_bytes);
    if (js->stdout_truncated)
        fprintf(stderr, " stdout:CAPPED(%lld produced)", js->stdout_original);
    if (js->stderr_truncated)
        fprintf(stderr, " stderr:CAPPED(%lld produced)", js->stderr_original);
    fprintf(stderr, "\n");
    if (js->error_msg[0]) fprintf(stderr, "[%s] %s\n", js->node, js->error_msg);
}

static int cmd_job_status(cli_opts_t *opts, const char *id) {
    int rc;
    cJSON *resp = job_request(opts, "status", id, NULL, 0, 0, NULL, &rc);
    if (!resp) return rc;
    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }
    job_status_t js;
    job_fill(resp, &js);
    cJSON_Delete(resp);
    if (js.command[0]) fprintf(stderr, "[%s] cmd: %s\n", js.node, js.command);
    job_print_status(&js, id);
    return js.done ? job_exit_status(&js) : EXIT_OK;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Print one stream from `offset` onward. Returns the new offset, or -1. */
static long long job_drain(cli_opts_t *opts, const char *id, const char *stream,
                           long long offset, FILE *out, job_status_t *js, int *rc) {
    for (;;) {
        cJSON *resp = job_request(opts, "tail", id, stream, offset, 0, NULL, rc);
        if (!resp) return -1;
        job_fill(resp, js);
        cJSON *j = cJSON_GetObjectItem(resp, "data");
        long long next = offset;
        cJSON *nj = cJSON_GetObjectItem(resp, "next_offset");
        if (cJSON_IsNumber(nj)) next = (long long)nj->valuedouble;
        if (cJSON_IsString(j) && j->valuestring[0]) fputs(j->valuestring, out);
        cJSON *bj = cJSON_GetObjectItem(resp, "binary");
        if (cJSON_IsTrue(bj))
            fprintf(stderr, "[%s] note: output contains NUL bytes; "
                            "this text view stops at the first one per chunk\n", js->node);
        cJSON_Delete(resp);
        int progressed = (next > offset);
        offset = next;
        if (!progressed) return offset;   /* caught up with the writer */
    }
}

static int cmd_job_tail(cli_opts_t *opts, const char *id, const char *stream,
                        int follow, int interval_ms) {
    /* -j is a single request/response: the caller wants the frame, including
     * next_offset so it can page for itself. Following is a human mode. */
    if (opts->json_output) {
        int rc;
        cJSON *resp = job_request(opts, "tail", id, stream, 0, 0, NULL, &rc);
        if (!resp) return rc;
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }
    long long off = 0;
    job_status_t js;
    memset(&js, 0, sizeof(js));
    int rc = EXIT_OK;
    for (;;) {
        off = job_drain(opts, id, stream, off, stdout, &js, &rc);
        if (off < 0) return rc;
        fflush(stdout);
        if (!follow || js.done) break;
        sleep_ms(interval_ms);
    }
    if (!opts->json_output) job_print_status(&js, id);
    return js.done ? job_exit_status(&js) : EXIT_OK;
}

static int cmd_job_wait(cli_opts_t *opts, const char *id, int interval_ms,
                        long long max_wait_ms, int show_output) {
    long long waited = 0;
    job_status_t js;
    memset(&js, 0, sizeof(js));
    for (;;) {
        int rc;
        cJSON *resp = job_request(opts, "status", id, NULL, 0, 0, NULL, &rc);
        if (!resp) return rc;
        job_fill(resp, &js);
        cJSON_Delete(resp);
        if (js.done) break;
        if (max_wait_ms > 0 && waited >= max_wait_ms) {
            fprintf(stderr,
                    "[%s] job %s still %s after %lldms — giving up the wait "
                    "(the job itself keeps running)\n",
                    js.node, id, js.state, waited);
            return EXIT_TIMEOUT;
        }
        sleep_ms(interval_ms);
        waited += interval_ms;
    }
    if (show_output) {
        int rc = EXIT_OK;
        long long off = job_drain(opts, id, "stdout", 0, stdout, &js, &rc);
        if (off >= 0) job_drain(opts, id, "stderr", 0, stderr, &js, &rc);
        fflush(stdout);
    }
    if (opts->json_output) {
        int rc;
        cJSON *resp = job_request(opts, "status", id, NULL, 0, 0, NULL, &rc);
        if (resp) {
            char *s = cJSON_PrintUnformatted(resp);
            if (s) { printf("%s\n", s); free(s); }
            cJSON_Delete(resp);
        }
    } else {
        job_print_status(&js, id);
    }
    return job_exit_status(&js);
}

static int cmd_job_cancel(cli_opts_t *opts, const char *id, int hard) {
    int rc;
    cJSON *resp = job_request(opts, "cancel", id, NULL, 0, 0,
                              hard ? "KILL" : "TERM", &rc);
    if (!resp) return rc;
    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }
    cJSON *sj = cJSON_GetObjectItem(resp, "signalled");
    int ok = cJSON_IsTrue(sj);
    job_status_t js;
    job_fill(resp, &js);
    cJSON_Delete(resp);
    fprintf(stderr, "[%s] job %s: %s\n", js.node, id,
            ok ? (hard ? "SIGKILL sent to the process group"
                       : "SIGTERM sent to the process group")
               : (js.error_msg[0] ? js.error_msg : "not signalled"));
    return ok ? EXIT_OK : EXIT_REMOTE_ERR;
}

static int cmd_job_list(cli_opts_t *opts) {
    int rc;
    cJSON *resp = job_request(opts, "list", NULL, NULL, 0, 0, NULL, &rc);
    if (!resp) return rc;
    if (opts->json_output) {
        char *s = cJSON_PrintUnformatted(resp);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(resp);
        return EXIT_OK;
    }
    cJSON *arr = cJSON_GetObjectItem(resp, "jobs");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (n == 0) fprintf(stderr, "no jobs\n");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        job_status_t js;
        job_fill(o, &js);
        cJSON *idj = cJSON_GetObjectItem(o, "job_id");
        const char *id = cJSON_IsString(idj) ? idj->valuestring : "?";
        /* One line per job, command last so the columns stay aligned when it
         * is long. */
        char cmd[81];
        size_t clen = strlen(js.command);
        if (clen > sizeof(cmd) - 1) clen = sizeof(cmd) - 1;
        memcpy(cmd, js.command, clen);
        cmd[clen] = '\0';
        for (char *p = cmd; *p; p++) if (*p == '\n') *p = ' ';
        printf("%-12s  %-8s  exit=%-4d  %6ldms  out=%-9lld  %s\n",
               id, js.state, js.done ? js.exit_code : -1,
               js.duration_ms, js.stdout_bytes, cmd);
    }
    cJSON_Delete(resp);
    return EXIT_OK;
}

/* `job <action> [<id>] [flags]` — flags accepted in any position, unknown ones
 * refused rather than ignored (the F4 rule, applied to the new surface). */
static int dispatch_job(cli_opts_t *opts, int argc, char *argv[], int from) {
    const char *action = NULL, *id = NULL;
    const char *stream = "stdout";
    int follow = 0, hard = 0, show_output = 0;
    int interval_ms = HL_JOB_POLL_MS_DEFAULT;
    long long max_wait_ms = 0;   /* 0 = wait as long as it takes */

    for (int i = from; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--stderr"))                             stream = "stderr";
        else if (!strcmp(a, "--stdout"))                        stream = "stdout";
        else if (!strcmp(a, "-f") || !strcmp(a, "--follow"))    follow = 1;
        else if (!strcmp(a, "--kill"))                          hard = 1;
        else if (!strcmp(a, "-o") || !strcmp(a, "--output"))    show_output = 1;
        else if (!strcmp(a, "--interval")) {
            if (i + 1 >= argc) { fprintf(stderr, "job: --interval requires ms\n"); return EXIT_CLIENT_ERR; }
            interval_ms = atoi(argv[++i]);
            if (interval_ms < 50) interval_ms = 50;
        } else if (!strcmp(a, "--wait-timeout")) {
            if (i + 1 >= argc) { fprintf(stderr, "job: --wait-timeout requires ms\n"); return EXIT_CLIENT_ERR; }
            max_wait_ms = atoll(argv[++i]);
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr,
                    "job: unknown flag '%s'\n"
                    "  job flags: --stderr, -f/--follow, --interval <ms>,\n"
                    "             --wait-timeout <ms>, -o/--output, --kill\n", a);
            return EXIT_CLIENT_ERR;
        } else if (!action) action = a;
        else if (!id)       id     = a;
        else {
            fprintf(stderr, "job: unexpected argument '%s'\n", a);
            return EXIT_CLIENT_ERR;
        }
    }

    if (!action) {
        fprintf(stderr, "job requires an action: list | status | tail | wait | cancel\n");
        return EXIT_CLIENT_ERR;
    }
    if (!strcmp(action, "list")) return cmd_job_list(opts);

    if (!id) {
        fprintf(stderr, "job %s requires a job id\n", action);
        return EXIT_CLIENT_ERR;
    }
    if (!strcmp(action, "status")) return cmd_job_status(opts, id);
    if (!strcmp(action, "tail"))   return cmd_job_tail(opts, id, stream, follow, interval_ms);
    if (!strcmp(action, "wait"))   return cmd_job_wait(opts, id, interval_ms, max_wait_ms, show_output);
    if (!strcmp(action, "cancel")) return cmd_job_cancel(opts, id, hard);

    fprintf(stderr, "job: unknown action '%s' "
                    "(list | status | tail | wait | cancel)\n", action);
    return EXIT_CLIENT_ERR;
}

static int cmd_targets(cli_opts_t *opts, int do_ping) {
    const char *tf = find_targets_file(opts->targets_file);
    if (!tf) { fprintf(stderr, "No targets config found\n"); return EXIT_CLIENT_ERR; }
    target_entry_t *targets = targets_load(tf);
    if (!targets) { fprintf(stderr, "Failed to load targets\n"); return EXIT_CLIENT_ERR; }

    if (opts->json_output) {
        cJSON *arr = cJSON_CreateArray();
        for (target_entry_t *t = targets; t; t = t->next) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name",      t->name);
            cJSON_AddStringToObject(obj, "transport", t->transport);
            if (!strcmp(t->transport, "unix"))
                cJSON_AddStringToObject(obj, "socket", t->socket);
            else {
                cJSON_AddStringToObject(obj, "address", t->address);
                cJSON_AddNumberToObject(obj, "port",    t->port);
            }
            cJSON_AddItemToArray(arr, obj);
        }
        char *s = cJSON_PrintUnformatted(arr);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(arr);
    } else {
        for (target_entry_t *t = targets; t; t = t->next) {
            if (!strcmp(t->transport, "unix"))
                printf("%-20s unix  %s", t->name, t->socket);
            else
                printf("%-20s tcp   %s:%d", t->name, t->address, t->port);
            if (do_ping) {
                cli_opts_t p;
                memcpy(&p, opts, sizeof(p));
                snprintf(p.target,      sizeof(p.target),      "%s", t->name);
                snprintf(p.socket_path, sizeof(p.socket_path), "%s", t->socket);
                snprintf(p.address,     sizeof(p.address),     "%s", t->address);
                snprintf(p.token,       sizeof(p.token),       "%s", t->token);
                p.port = t->port;
                p.connect_timeout_ms = 2000;
                int cfd = open_connection(&p);
                if (cfd < 0) printf("  unreachable");
                else { close(cfd); printf("  ok"); }
            }
            printf("\n");
        }
    }
    targets_free(targets);
    return EXIT_OK;
}

/*
 * Join argv[from..argc-1] with single spaces into a newly allocated string.
 *
 * `exec` used to run only argv[optind] and silently discard the rest, so
 * `hostlink-cli ... exec stat -c%s /path` ran a bare `stat`, and
 * `exec ls /some/dir` listed `/`. That produced a false "all files MISSING"
 * report after a transfer that had in fact fully succeeded. The wrappers
 * (bin/hl, bin/hl-spark) were immune only because they pre-join into one token,
 * so join-of-one here is the identity and they are unaffected.
 *
 * The daemon shell-parses the joined string, so — exactly as with ssh — an
 * argument containing spaces must be quoted by the caller to survive.
 */
static char *join_argv(int argc, char *argv[], int from) {
    size_t total = 1;
    for (int i = from; i < argc; i++) total += strlen(argv[i]) + 1;

    char *out = malloc(total);
    if (!out) return NULL;

    size_t pos = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) out[pos++] = ' ';
        size_t len = strlen(argv[i]);
        memcpy(out + pos, argv[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return out;
}

/*
 * Split the arguments of a two-path subcommand (put/get and their -stream
 * variants) into exactly two positionals plus any transfer flags, accepting the
 * flags in ANY position.
 *
 * getopt is invoked with a leading '+' so it stops at the first non-option —
 * the subcommand itself. Everything after it was therefore read positionally,
 * so both of these silently did the wrong thing:
 *     put <local> <remote> --mkdir     -> --mkdir ignored, never sent
 *     put --mkdir <local> <remote>     -> "--mkdir" taken as the local path
 * The daemon implements nested mkdir correctly; it was simply never asked.
 * Top-level puts appeared to work only because their parents already existed.
 *
 * Returns 0 on success, -1 on an unknown flag or the wrong number of paths —
 * it never ignores a token it did not understand.
 */
static int parse_two_path_args(cli_opts_t *opts, int argc, char *argv[], int from,
                               const char *subcmd, const char *p1_name,
                               const char *p2_name,
                               const char **out_p1, const char **out_p2) {
    const char *pos[2] = { NULL, NULL };
    int npos = 0;

    for (int i = from; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--mkdir")) {
            opts->put_mkdir = 1;
        } else if (!strcmp(a, "--stream")) {
            opts->stream = 1;
        } else if (!strcmp(a, "--verify")) {
            opts->verify = 1;
        } else if (!strcmp(a, "--mode")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --mode requires an octal argument\n", subcmd);
                return -1;
            }
            opts->put_mode_val = (int)strtol(argv[++i], NULL, 8);
        } else if (!strncmp(a, "--mode=", 7)) {
            opts->put_mode_val = (int)strtol(a + 7, NULL, 8);
        } else if (!strcmp(a, "--exclude")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --exclude requires a pattern\n", subcmd);
                return -1;
            }
            if (opts->exclude_count >= HL_MAX_EXCLUDES) {
                fprintf(stderr, "%s: too many --exclude patterns (max %d)\n",
                        subcmd, HL_MAX_EXCLUDES);
                return -1;
            }
            snprintf(opts->excludes[opts->exclude_count++],
                     sizeof(opts->excludes[0]), "%s", argv[++i]);
        } else if (!strncmp(a, "--exclude=", 10)) {
            if (opts->exclude_count >= HL_MAX_EXCLUDES) {
                fprintf(stderr, "%s: too many --exclude patterns (max %d)\n",
                        subcmd, HL_MAX_EXCLUDES);
                return -1;
            }
            snprintf(opts->excludes[opts->exclude_count++],
                     sizeof(opts->excludes[0]), "%s", a + 10);
        } else if (!strcmp(a, "-r") || !strcmp(a, "-R") ||
                   !strcmp(a, "--recursive")) {
            /* Fail loud rather than accept a flag that implies we might NOT
             * recurse without it. Directories are always handled. */
            fprintf(stderr,
                    "%s: %s is not needed — directories are transferred "
                    "automatically, as with `get`.\n", subcmd, a);
            return -1;
        } else if (a[0] == '-' && a[1] != '\0' && strcmp(a, "-")) {
            fprintf(stderr,
                    "%s: unknown flag '%s'\n"
                    "  transfer flags are: --mkdir, --mode <oct>, --stream, --verify,\n"
                    "                      --exclude <pattern>  (put of a directory)\n"
                    "  (connection flags such as -t/-k/-T go before the subcommand)\n",
                    subcmd, a);
            return -1;
        } else if (npos < 2) {
            pos[npos++] = a;
        } else {
            fprintf(stderr,
                    "%s: too many paths — expected <%s> <%s>, got a third: '%s'\n",
                    subcmd, p1_name, p2_name, a);
            return -1;
        }
    }

    if (npos < 2) {
        fprintf(stderr, "%s requires: <%s> <%s>\n", subcmd, p1_name, p2_name);
        return -1;
    }
    *out_p1 = pos[0];
    *out_p2 = pos[1];
    return 0;
}

int main(int argc, char *argv[]) {
    log_init(LOG_TARGET_STDERR, HL_LOG_WARN, NULL);

    cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms         = 30000;
    opts.connect_timeout_ms = 5000;
    opts.put_mode_val       = 0644;

    const char *env_token = getenv("HOSTLINK_TOKEN");
    if (env_token) snprintf(opts.token, sizeof(opts.token), "%s", env_token);

    static struct option long_opts[] = {
        {"target",         required_argument, NULL, 't'},
        {"socket",         required_argument, NULL, 's'},
        {"address",        required_argument, NULL, 'a'},
        {"token",          required_argument, NULL, 'k'},
        {"timeout",        required_argument, NULL, 'T'},
        {"connect-timeout",required_argument, NULL, 'C'},
        {"targets-file",   required_argument, NULL, 'F'},
        {"json",           no_argument,       NULL, 'j'},
        {"help",           no_argument,       NULL, 'h'},
        {"version",        no_argument,       NULL, 'V'},
        {"env",            required_argument, NULL, 'e'},
        {"workdir",        required_argument, NULL, 'w'},
        {"max-output",     required_argument, NULL, 'm'},
        {"max-stdout",     required_argument, NULL, 1001},
        {"max-stderr",     required_argument, NULL, 1002},
        {"output-to-file", no_argument,       NULL, 'O'},
        {"ping",           no_argument,       NULL, 'P'},
        {"detach",         no_argument,       NULL, 'D'},   /* NEW: fire-and-forget */
        {"mkdir",          no_argument,       NULL, 1003},  /* NEW: mkdir -p on put */
        {"mode",           required_argument, NULL, 1004},  /* NEW: file mode on put */
        {"stream",         no_argument,       NULL, 1005},  /* NEW: streaming get/put (no 90 MiB cap) */
        /* --verify is accepted both here and among the subcommand's own
         * arguments, as --mkdir/--mode/--stream/--exclude already are. Callers
         * (and wrappers) put transfer flags on either side of the subcommand;
         * accepting only one position is how F2 happened. */
        {"verify",         no_argument,       NULL, 1007},
        {"exclude",        required_argument, NULL, 1006},  /* NEW: skip glob on directory put */
        {NULL, 0, NULL, 0}
    };

    int do_ping_targets = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "+t:s:a:k:T:C:F:je:w:m:OhVPD", long_opts, NULL)) != -1) {
        switch (opt) {
            case 't': snprintf(opts.target,       sizeof(opts.target),       "%s", optarg); break;
            case 's': snprintf(opts.socket_path,  sizeof(opts.socket_path),  "%s", optarg); break;
            case 'a': {
                char *colon = strrchr(optarg, ':');
                if (colon) {
                    size_t hlen = (size_t)(colon - optarg);
                    if (hlen >= sizeof(opts.address)) hlen = sizeof(opts.address) - 1;
                    memcpy(opts.address, optarg, hlen);
                    opts.address[hlen] = '\0';
                    opts.port = atoi(colon + 1);
                } else {
                    snprintf(opts.address, sizeof(opts.address), "%s", optarg);
                    opts.port = 9876;
                }
                break;
            }
            case 'k': snprintf(opts.token,        sizeof(opts.token),        "%s", optarg); break;
            case 'T': opts.timeout_ms = atoi(optarg); opts.timeout_explicit = 1; break;
            case 'C': opts.connect_timeout_ms = atoi(optarg); break;
            case 'F': snprintf(opts.targets_file, sizeof(opts.targets_file), "%s", optarg); break;
            case 'j': opts.json_output = 1; break;
            case 'e':
                if (opts.env_count < 64)
                    snprintf(opts.env_pairs[opts.env_count++], sizeof(opts.env_pairs[0]),
                             "%s", optarg);
                break;
            case 'w': snprintf(opts.workdir, sizeof(opts.workdir), "%s", optarg); break;
            case 'm': {
                long long sz = parse_size(optarg);
                opts.max_stdout = opts.max_stderr = (sz < 0 ? atoll(optarg) : sz);
                break;
            }
            case 1001: { long long sz = parse_size(optarg); opts.max_stdout = sz < 0 ? atoll(optarg) : sz; break; }
            case 1002: { long long sz = parse_size(optarg); opts.max_stderr = sz < 0 ? atoll(optarg) : sz; break; }
            case 'O': opts.output_to_file = 1; break;
            case 'P': do_ping_targets = 1; break;
            case 'D': opts.detach = 1; break;
            case 1003: opts.put_mkdir = 1; break;
            case 1004: opts.put_mode_val = (int)strtol(optarg, NULL, 8); break;
            case 1005: opts.stream = 1; break;
            case 1007: opts.verify = 1; break;
            case 1006:
                if (opts.exclude_count >= HL_MAX_EXCLUDES) {
                    fprintf(stderr, "too many --exclude patterns (max %d)\n",
                            HL_MAX_EXCLUDES);
                    return EXIT_CLIENT_ERR;
                }
                snprintf(opts.excludes[opts.exclude_count++],
                         sizeof(opts.excludes[0]), "%s", optarg);
                break;
            case 'h':
                printf("Usage: hostlink-cli [OPTIONS] <SUBCOMMAND>\n"
                       "Subcommands:\n"
                       "  exec <cmd...>               Run a command on the remote host.\n"
                       "                              Remaining arguments are joined with\n"
                       "                              spaces; quote anything containing a\n"
                       "                              space, as with ssh.\n"
                       "  put  <local> <remote>       Transfer a file or directory (auto-\n"
                       "                              streams if > 90 MiB; directories are\n"
                       "                              walked automatically, no -r needed)\n"
                       "  get  <remote> <local>       Retrieve a file or directory (auto-streams\n"
                       "                              large files; checks local free space first)\n"
                       "  put-stream <local> <remote> Force-stream a put (sha256-verified)\n"
                       "  get-stream <remote> <local> Force-stream a get (sha256-verified)\n"
                       "  df   <remote>               Free space on the filesystem that\n"
                       "                              would hold <remote> (walks up to the\n"
                       "                              nearest existing ancestor)\n"
                       "  ping                        Check if the daemon is alive\n"
                       "  targets                     List configured targets\n"
                       "  job <action> [<id>]         Detached jobs. -D on an exec prints\n"
                       "                              the id; these read it back:\n"
                       "     job list                   recent jobs, newest first\n"
                       "     job status <id>            state, exit code, spool sizes\n"
                       "     job tail <id> [-f]         output so far; -f follows to the end\n"
                       "     job wait <id> [-o]         block until it ends, exit with ITS\n"
                       "                                status; -o then prints the output\n"
                       "     job cancel <id> [--kill]   signal the job's process group\n"
                       "   flags: --stderr  --interval <ms>  --wait-timeout <ms>\n"
                       "Options:\n"
                       "  -t <target>    Target name from targets config\n"
                       "  -s <socket>    Unix socket path\n"
                       "  -a <host:port> TCP address\n"
                       "  -k <token>     Auth token\n"
                       "  -T <ms>        Command timeout (default 30000)\n"
                       "  -C <ms>        Connect timeout (default 5000)\n"
                       "  -j             JSON output\n"
                       "  -e KEY=VAL     Set environment variable\n"
                       "  -w <dir>       Set working directory\n"
                       "  -m <size>      Max output size (e.g. 4M)\n"
                       "  -O             Write output to files instead of inline\n"
                       "  -D, --detach   Fire-and-forget: return immediately, no output\n"
                       "  --mkdir        Create parent directories on put\n"
                       "  --mode <oct>   File permissions on put (default 644)\n"
                       "  --exclude <p>  Skip paths matching glob <p> when putting a\n"
                       "                 directory. Repeatable. Matches a basename, a full\n"
                       "                 relative path, or any leading directory component,\n"
                       "                 so --exclude .cache prunes the whole .cache/ tree.\n"
                       "  --verify       Directory put: stream and sha256-verify every file,\n"
                       "                 print a sha256sum-format manifest on stdout and a\n"
                       "                 pass/fail count at the end\n"
                       "  --stream       Force streaming protocol (sha256 verified) even for\n"
                       "                 small files. `get` and `put` already auto-stream when\n"
                       "                 the file is large; this flag is for verification.\n"
                       "  --targets-file <path>  Override targets config path\n"
                       "\n"
                       "Exit status (exec):\n"
                       "  The remote command's own status, passed through verbatim, so\n"
                       "  `hostlink-cli ... exec 'test -f /x' && ...` behaves as expected.\n"
                       "  124  remote command hit the timeout and was killed\n"
                       "  125  hostlink itself failed (connect / auth / protocol / usage)\n"
                       "  put/get/ping/targets report a plain 0 = ok, 125 = failed.\n"
                       "\n"
                       "Note: the command is delivered to the remote shell through the\n"
                       "  environment, not argv, so `pgrep -f` / `pkill -f` run over\n"
                       "  hostlink no longer match hostlink's own exec shell.\n");
                return EXIT_OK;
            case 'V': printf("hostlink-cli %s\n", VERSION); return EXIT_OK;
            default:  return EXIT_CLIENT_ERR;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "No subcommand. Use: exec, put, ping, targets\n");
        return EXIT_CLIENT_ERR;
    }

    const char *subcmd = argv[optind++];
    target_entry_t *all_targets = NULL;
    if (opts.target[0] && resolve_target(&opts, &all_targets) != 0)
        return EXIT_CLIENT_ERR;

    if (opts.token[0] == '\0' && strcmp(subcmd, "targets") != 0) {
        fprintf(stderr, "No auth token. Use -k, HOSTLINK_TOKEN, or targets config.\n");
        targets_free(all_targets);
        return EXIT_CLIENT_ERR;
    }

    int rc;
    if (!strcmp(subcmd, "ping")) {
        rc = cmd_ping(&opts);
    } else if (!strcmp(subcmd, "exec")) {
        if (optind >= argc) {
            fprintf(stderr, "exec requires a command argument\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        char *command = join_argv(argc, argv, optind);
        if (!command) {
            fprintf(stderr, "out of memory building command\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_exec(&opts, command);
        free(command);
    } else if (!strcmp(subcmd, "put")) {
        const char *local_path, *remote_path;
        if (parse_two_path_args(&opts, argc, argv, optind, "put",
                                "local_path", "remote_path",
                                &local_path, &remote_path) != 0) {
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        /* Directories are handled transparently, exactly as `get` does — no
         * -r needed, and the two verbs stay symmetric. */
        struct stat lst;
        if (stat(local_path, &lst) == 0 && S_ISDIR(lst.st_mode)) {
            rc = cmd_put_dir(&opts, local_path, remote_path);
        } else {
            /* Auto-promote large puts to streaming. We can stat the local file
             * cheaply, so we know the size up-front and avoid forcing the user
             * to remember --stream for big files. */
            int use_stream = opts.stream;
            if (!use_stream && stat(local_path, &lst) == 0 &&
                (uint64_t)lst.st_size > HL_STREAM_AUTO_THRESHOLD)
                use_stream = 1;
            rc = use_stream
                 ? cmd_put_stream(&opts, local_path, remote_path)
                 : cmd_put       (&opts, local_path, remote_path);
        }
    } else if (!strcmp(subcmd, "get")) {
        const char *remote_path, *local_path;
        if (parse_two_path_args(&opts, argc, argv, optind, "get",
                                "remote_path", "local_path",
                                &remote_path, &local_path) != 0) {
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        /* Smart dispatcher: probes remote size with get_stat, checks local
         * free space, then routes to legacy or streaming per-file. Handles
         * directories transparently (cp-style: local path becomes the new
         * directory). --stream still works as an explicit override. */
        rc = cmd_get_smart(&opts, remote_path, local_path);
    } else if (!strcmp(subcmd, "get-stream") || !strcmp(subcmd, "get_stream")) {
        const char *remote_path, *local_path;
        if (parse_two_path_args(&opts, argc, argv, optind, "get-stream",
                                "remote_path", "local_path",
                                &remote_path, &local_path) != 0) {
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_get_stream(&opts, remote_path, local_path);
    } else if (!strcmp(subcmd, "put-stream") || !strcmp(subcmd, "put_stream")) {
        const char *local_path, *remote_path;
        if (parse_two_path_args(&opts, argc, argv, optind, "put-stream",
                                "local_path", "remote_path",
                                &local_path, &remote_path) != 0) {
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_put_stream(&opts, local_path, remote_path);
    } else if (!strcmp(subcmd, "df")) {
        if (optind >= argc || optind + 1 != argc) {
            fprintf(stderr, "df requires exactly one remote path\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_df(&opts, argv[optind]);
    } else if (!strcmp(subcmd, "job")) {
        rc = dispatch_job(&opts, argc, argv, optind);
    } else if (!strcmp(subcmd, "targets")) {
        rc = cmd_targets(&opts, do_ping_targets);
    } else {
        fprintf(stderr, "Unknown subcommand: %s\n", subcmd);
        rc = EXIT_CLIENT_ERR;
    }

    targets_free(all_targets);
    return rc;
}
