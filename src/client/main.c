#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "connection.h"
#include "../common/protocol.h"
#include "../common/config.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/cjson/cJSON.h"

#define VERSION "1.2.0"

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
            case 'h':
                printf("Usage: hostlink-cli [OPTIONS] <SUBCOMMAND>\n"
                       "Subcommands:\n"
                       "  exec <cmd>                  Run a command on the remote host\n"
                       "  put  <local> <remote>       Transfer a file to the remote host\n"
                       "  get  <remote> <local>       Retrieve a file from the remote host\n"
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
        rc = cmd_put(&opts, argv[optind], argv[optind + 1]);
    } else if (!strcmp(subcmd, "get")) {
        if (optind + 1 >= argc) {
            fprintf(stderr, "get requires: <remote_path> <local_path>\n");
            targets_free(all_targets); return EXIT_CLIENT_ERR;
        }
        rc = cmd_get(&opts, argv[optind], argv[optind + 1]);
    } else if (!strcmp(subcmd, "targets")) {
        rc = cmd_targets(&opts, do_ping_targets);
    } else {
        fprintf(stderr, "Unknown subcommand: %s\n", subcmd);
        rc = EXIT_CLIENT_ERR;
    }

    targets_free(all_targets);
    return rc;
}
