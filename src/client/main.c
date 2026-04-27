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
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "connection.h"
#include "../common/protocol.h"
#include "../common/config.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/sha256.h"
#include "../common/cjson/cJSON.h"

#define VERSION "1.4.0"

#define EXIT_OK           0
#define EXIT_REMOTE_ERR   1
#define EXIT_CONN_FAILED  2
#define EXIT_AUTH_FAILED  3
#define EXIT_BAD_REQUEST  4
#define EXIT_TIMEOUT      5
#define EXIT_PROTO_ERR    6
#define EXIT_CLIENT_ERR   7

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
    char  put_mode[4];      /* "644" etc — unused, kept for mode parsing */
    int   put_mkdir;        /* --mkdir: create parent dirs on put */
    int   put_mode_val;     /* octal file mode for put */
    int   stream;           /* --stream: force streaming mode for get/put */
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
    if (opts->detach)
        cJSON_AddTrueToObject(req, "detach");
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

    int ret = EXIT_OK;
    if (!strcmp(status, "auth_failed")) {
        fprintf(stderr, "Authentication failed\n"); ret = EXIT_AUTH_FAILED;
    } else if (!strcmp(status, "error")) {
        fprintf(stderr, "[%s] error: %s\n", node, error_msg ? error_msg : "unknown");
        ret = EXIT_BAD_REQUEST;
    } else if (!strcmp(status, "timeout")) {
        fprintf(stderr, "[%s] timeout after %.0fms\n", node, duration);
        ret = EXIT_TIMEOUT;
    } else {
        if (exit_code != 0) ret = EXIT_REMOTE_ERR;
        /* detached — just print a brief ack */
        j = cJSON_GetObjectItem(resp, "detached");
        if (cJSON_IsTrue(j)) {
            fprintf(stderr, "[%s] detached - launched in background\n", node);
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
        char remote_full[1024];
        char local_full[1024];
        snprintf(remote_full, sizeof(remote_full), "%s/%s",
                 remote_path, gs.paths[i]);
        snprintf(local_full,  sizeof(local_full),  "%s/%s",
                 local_path, gs.paths[i]);

        /* mkdir for the file's parent. dirname() may modify its arg, so
         * work on a copy. */
        char parent_buf[1024];
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

static int cmd_put_stream(cli_opts_t *opts, const char *local_path, const char *remote_path) {
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
    } else {
        fprintf(stderr, "[%s] put_stream ok: %s -> %s (%llu bytes, sha256=%.16s...)\n",
                node, local_path, remote_path,
                (unsigned long long)total, resp_sha);
    }
    free(resp_json_str);
    return ret;
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
            case 'T': opts.timeout_ms         = atoi(optarg); break;
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
            case 'h':
                printf("Usage: hostlink-cli [OPTIONS] <SUBCOMMAND>\n"
                       "Subcommands:\n"
                       "  exec <cmd>                  Run a command on the remote host\n"
                       "  put  <local> <remote>       Transfer a file (auto-streams if > 90 MiB)\n"
                       "  get  <remote> <local>       Retrieve a file or directory (auto-streams\n"
                       "                              large files; checks local free space first)\n"
                       "  put-stream <local> <remote> Force-stream a put (sha256-verified)\n"
                       "  get-stream <remote> <local> Force-stream a get (sha256-verified)\n"
                       "  ping                        Check if the daemon is alive\n"
                       "  targets                     List configured targets\n"
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
                       "  --stream       Force streaming protocol (sha256 verified) even for\n"
                       "                 small files. `get` and `put` already auto-stream when\n"
                       "                 the file is large; this flag is for verification.\n"
                       "  --targets-file <path>  Override targets config path\n");
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
        rc = cmd_exec(&opts, argv[optind]);
    } else if (!strcmp(subcmd, "put")) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "put requires: <local_path> <remote_path>\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        const char *local_path = argv[optind];
        /* Auto-promote large puts to streaming. We can stat the local file
         * cheaply, so we know the size up-front and avoid forcing the user
         * to remember --stream for big files. */
        int use_stream = opts.stream;
        if (!use_stream) {
            struct stat lst;
            if (stat(local_path, &lst) == 0 &&
                (uint64_t)lst.st_size > HL_STREAM_AUTO_THRESHOLD)
                use_stream = 1;
        }
        rc = use_stream
             ? cmd_put_stream(&opts, local_path, argv[optind + 1])
             : cmd_put       (&opts, local_path, argv[optind + 1]);
    } else if (!strcmp(subcmd, "get")) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "get requires: <remote_path> <local_path>\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        /* Smart dispatcher: probes remote size with get_stat, checks local
         * free space, then routes to legacy or streaming per-file. Handles
         * directories transparently (cp-style: local path becomes the new
         * directory). --stream still works as an explicit override. */
        rc = cmd_get_smart(&opts, argv[optind], argv[optind + 1]);
    } else if (!strcmp(subcmd, "get-stream") || !strcmp(subcmd, "get_stream")) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "get-stream requires: <remote_path> <local_path>\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_get_stream(&opts, argv[optind], argv[optind + 1]);
    } else if (!strcmp(subcmd, "put-stream") || !strcmp(subcmd, "put_stream")) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "put-stream requires: <local_path> <remote_path>\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_put_stream(&opts, argv[optind], argv[optind + 1]);
    } else if (!strcmp(subcmd, "targets")) {
        rc = cmd_targets(&opts, do_ping_targets);
    } else {
        fprintf(stderr, "Unknown subcommand: %s\n", subcmd);
        rc = EXIT_CLIENT_ERR;
    }

    targets_free(all_targets);
    return rc;
}
