#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include "connection.h"
#include "../common/protocol.h"
#include "../common/config.h"
#include "../common/util.h"
#include "../common/log.h"
#include "../common/cjson/cJSON.h"

#define VERSION "1.0.0"

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
        if (stdout_file || stderr_file) {
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
        {NULL, 0, NULL, 0}
    };

    int do_ping_targets = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "+t:s:a:k:T:C:F:je:w:m:OhVP", long_opts, NULL)) != -1) {
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
            case 'h':
                printf("Usage: hostlink-cli [OPTIONS] <SUBCOMMAND>\n"
                       "Subcommands: exec <cmd>, ping, targets\n"
                       "Options: -t <target> -s <socket> -a <host:port> -k <token>\n"
                       "         -T <ms> -C <ms> -j -e KEY=VAL -w <dir> -m <size> -O\n"
                       "         --targets-file <path>\n");
                return EXIT_OK;
            case 'V': printf("hostlink-cli %s\n", VERSION); return EXIT_OK;
            default:  return EXIT_CLIENT_ERR;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "No subcommand. Use: exec, ping, targets\n");
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
    } else if (!strcmp(subcmd, "targets")) {
        rc = cmd_targets(&opts, do_ping_targets);
    } else {
        fprintf(stderr, "Unknown subcommand: %s\n", subcmd);
        rc = EXIT_CLIENT_ERR;
    }

    targets_free(all_targets);
    return rc;
}
