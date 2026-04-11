#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include "server.h"
#include "executor.h"
#include "../common/protocol.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/util.h"
#include "../common/cjson/cJSON.h"

#define MAX_EVENTS 64

static daemon_config_t *g_cfg        = NULL;
static volatile int     g_running    = 1;
static volatile int     g_reload     = 0;
static long long         g_start_time = 0;

/*
 * g_active_children tracks how many worker children are currently running.
 * It is incremented BEFORE fork() in the parent (so the busy check is
 * accurate even when multiple connections arrive before any child exits),
 * and decremented when SIGCHLD fires via waitpid() in the event loop.
 */
static int g_active_children = 0;
#define MAX_WORKERS 256
static pid_t g_worker_pids[MAX_WORKERS];
static int   g_worker_count = 0;

static void track_worker(pid_t pid) {
    if (g_worker_count < MAX_WORKERS)
        g_worker_pids[g_worker_count++] = pid;
}

static void untrack_worker(pid_t pid) {
    for (int i = 0; i < g_worker_count; i++) {
        if (g_worker_pids[i] == pid) {
            g_worker_pids[i] = g_worker_pids[--g_worker_count];
            return;
        }
    }
}

static void kill_all_workers(int sig) {
    for (int i = 0; i < g_worker_count; i++)
        kill(g_worker_pids[i], sig);
}

static long long now_s(void) { return (long long)time(NULL); }

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void send_error(int fd, const char *req_id, const char *status,
                        const char *msg) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "version", 1);
    cJSON_AddStringToObject(obj, "id",        req_id ? req_id : "");
    cJSON_AddStringToObject(obj, "node",      g_cfg->node_name);
    cJSON_AddStringToObject(obj, "status",    status);
    cJSON_AddStringToObject(obj, "error_msg", msg);
    frame_send_json(fd, obj);
    cJSON_Delete(obj);
}

static void handle_ping(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *id_j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(id_j)) req_id = id_j->valuestring;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "version",  1);
    cJSON_AddStringToObject(obj, "id",       req_id);
    cJSON_AddStringToObject(obj, "node",     g_cfg->node_name);
    cJSON_AddStringToObject(obj, "status",   "ok");
    cJSON_AddNumberToObject(obj, "uptime_s", (double)(now_s() - g_start_time));
    frame_send_json(fd, obj);
    cJSON_Delete(obj);
}

/*
 * handle_exec: runs in a forked worker child.
 * The parent has already incremented g_active_children and will decrement
 * it via SIGCHLD/waitpid when the child exits.
 */
static void handle_exec(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;

    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    cJSON *cmd_j = cJSON_GetObjectItem(req, "command");
    if (!cJSON_IsString(cmd_j) || cmd_j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "command is required");
        return;
    }

    exec_result_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.request_id, sizeof(r.request_id), "%s", req_id);
    snprintf(r.command,    sizeof(r.command),    "%s", cmd_j->valuestring);
    snprintf(r.shell,      sizeof(r.shell),      "%s", g_cfg->shell);
    snprintf(r.output_tmpdir, sizeof(r.output_tmpdir), "%s", g_cfg->output_tmpdir);

    j = cJSON_GetObjectItem(req, "workdir");
    if (cJSON_IsString(j)) snprintf(r.workdir, sizeof(r.workdir), "%s", j->valuestring);

    j = cJSON_GetObjectItem(req, "timeout_ms");
    r.timeout_ms = (cJSON_IsNumber(j) && j->valueint > 0)
                   ? j->valueint : g_cfg->default_timeout_ms;
    if (r.timeout_ms > g_cfg->max_timeout_ms) r.timeout_ms = g_cfg->max_timeout_ms;

    long long eff_max = g_cfg->default_max_output_bytes;
    j = cJSON_GetObjectItem(req, "max_stdout_bytes");
    if (cJSON_IsNumber(j) && j->valuedouble > 0)
        eff_max = (long long)j->valuedouble;
    if (g_cfg->max_output_bytes > 0 && eff_max > g_cfg->max_output_bytes)
        eff_max = g_cfg->max_output_bytes;
    r.max_stdout_bytes = eff_max;

    eff_max = g_cfg->default_max_output_bytes;
    j = cJSON_GetObjectItem(req, "max_stderr_bytes");
    if (cJSON_IsNumber(j) && j->valuedouble > 0)
        eff_max = (long long)j->valuedouble;
    if (g_cfg->max_output_bytes > 0 && eff_max > g_cfg->max_output_bytes)
        eff_max = g_cfg->max_output_bytes;
    r.max_stderr_bytes = eff_max;

    j = cJSON_GetObjectItem(req, "output_to_file");
    r.output_to_file = cJSON_IsTrue(j) ? 1 : 0;

    j = cJSON_GetObjectItem(req, "env");
    if (cJSON_IsObject(j)) {
        cJSON *kv;
        cJSON_ArrayForEach(kv, j) {
            if (r.env_count < 256 && cJSON_IsString(kv)) {
                r.env_keys[r.env_count] = kv->string;
                r.env_vals[r.env_count] = kv->valuestring;
                r.env_count++;
            }
        }
    }

    executor_run(&r);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version", 1);
    cJSON_AddStringToObject(resp, "id",   req_id);
    cJSON_AddStringToObject(resp, "node", g_cfg->node_name);

    if (r.exec_error) {
        cJSON_AddStringToObject(resp, "status",    "error");
        cJSON_AddStringToObject(resp, "error_msg", r.error_msg);
        cJSON_AddNumberToObject(resp, "exit_code", -1);
    } else if (r.timed_out) {
        cJSON_AddStringToObject(resp, "status",    "timeout");
        cJSON_AddNumberToObject(resp, "exit_code", -2);
    } else {
        cJSON_AddStringToObject(resp, "status",    "ok");
        cJSON_AddNumberToObject(resp, "exit_code", r.exit_code);
    }

    if (r.output_to_file) {
        cJSON_AddStringToObject(resp, "stdout_file", r.stdout_file);
        cJSON_AddStringToObject(resp, "stderr_file", r.stderr_file);
    } else {
        cJSON_AddStringToObject(resp, "stdout", r.stdout_buf ? r.stdout_buf : "");
        cJSON_AddStringToObject(resp, "stderr", r.stderr_buf ? r.stderr_buf : "");
    }
    cJSON_AddBoolToObject(resp,   "stdout_truncated",        r.stdout_truncated);
    cJSON_AddBoolToObject(resp,   "stderr_truncated",        r.stderr_truncated);
    cJSON_AddNumberToObject(resp, "stdout_original_bytes",   (double)r.stdout_original_bytes);
    cJSON_AddNumberToObject(resp, "stderr_original_bytes",   (double)r.stderr_original_bytes);
    cJSON_AddNumberToObject(resp, "duration_ms",             (double)r.duration_ms);

    frame_send_json(fd, resp);
    cJSON_Delete(resp);
    executor_free(&r);
}


/*
 * dispatch_connection: accept + route.
 *
 * For exec requests we want true concurrency: fork a worker child that
 * handles the request, while the parent immediately returns to epoll.
 * g_active_children is incremented in the parent BEFORE fork so that
 * subsequent connections see the correct count even before any child exits.
 *
 * For ping we handle inline (cheap, non-blocking).
 */
static void dispatch_connection(int client_fd) {
    /* Peek at the request type without fully parsing, to decide fork vs inline.
     * Simple approach: always fork for exec, handle ping inline.
     * We do a full parse in handle_client in both paths — that's fine. */

    /* Read the frame first to decide */
    char *payload = NULL;
    ssize_t n = frame_recv(client_fd, &payload);
    if (n <= 0) {
        if (n == -2) log_warn("client: bad magic, closing");
        if (n == -3) log_warn("client: oversized frame, closing");
        free(payload);
        close(client_fd);
        return;
    }

    cJSON *req = cJSON_Parse(payload);
    free(payload);
    if (!req) {
        send_error(client_fd, "", "bad_request", "invalid JSON");
        close(client_fd);
        return;
    }

    /* Auth check in parent — fail fast without forking */
    cJSON *token_j = cJSON_GetObjectItem(req, "token");
    if (!cJSON_IsString(token_j) ||
        ct_strcmp(token_j->valuestring, g_cfg->auth_token) != 0) {
        send_error(client_fd, "", "auth_failed", "authentication failed");
        cJSON_Delete(req);
        close(client_fd);
        return;
    }

    cJSON *type_j = cJSON_GetObjectItem(req, "type");
    const char *type = cJSON_IsString(type_j) ? type_j->valuestring : "";

    if (!strcmp(type, "ping")) {
        /* Inline — cheap */
        handle_ping(client_fd, req);
        cJSON_Delete(req);
        close(client_fd);
        return;
    }

    if (!strcmp(type, "exec")) {
        /* Check concurrency limit BEFORE forking */
        cJSON *id_j  = cJSON_GetObjectItem(req, "id");
        const char *req_id = cJSON_IsString(id_j) ? id_j->valuestring : "";

        if (g_active_children >= g_cfg->max_concurrent) {
            send_error(client_fd, req_id, "error",
                       "server busy, max concurrent commands reached");
            cJSON_Delete(req);
            close(client_fd);
            return;
        }

        /* Increment before fork — parent sees accurate count immediately */
        g_active_children++;

        pid_t pid = fork();
        if (pid < 0) {
            g_active_children--;
            send_error(client_fd, req_id, "error", "fork failed");
            cJSON_Delete(req);
            close(client_fd);
            return;
        }

        if (pid == 0) {
            /* Worker child */
            /* Close listen fds inherited from parent */
            /* (They're CLOEXEC so exec'd children don't get them, but
               we close explicitly in the worker to be tidy.) */
            handle_exec(client_fd, req);
            cJSON_Delete(req);
            close(client_fd);
            _exit(0);
        }

        /* Parent: worker is running, return to epoll */
        track_worker(pid);
        cJSON_Delete(req);
        close(client_fd);
        log_debug("forked worker pid=%d active=%d", (int)pid, g_active_children);
        return;
    }

    send_error(client_fd, "", "bad_request", "unknown message type");
    cJSON_Delete(req);
    close(client_fd);
}

int server_run(daemon_config_t *cfg, int unix_fd, int tcp_fd,
               const char *config_path) {
    g_cfg        = cfg;
    g_start_time = now_s();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) { log_error("signalfd: %s", strerror(errno)); return -1; }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { log_error("epoll_create1: %s", strerror(errno)); return -1; }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    if (unix_fd >= 0) {
        make_nonblocking(unix_fd);
        ev.data.fd = unix_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, unix_fd, &ev);
    }
    if (tcp_fd >= 0) {
        make_nonblocking(tcp_fd);
        ev.data.fd = tcp_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, tcp_fd, &ev);
    }

    log_info("hostlinkd started, node=%s", cfg->node_name);

    while (g_running) {
        struct epoll_event events[MAX_EVENTS];
        int nev = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nev < 0) {
            if (errno == EINTR) continue;
            log_error("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nev; i++) {
            int efd = events[i].data.fd;

            if (efd == sfd) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        log_info("received signal %u, shutting down", si.ssi_signo);
                        g_running = 0;
                    } else if (si.ssi_signo == SIGHUP) {
                        log_info("received SIGHUP, reloading config");
                        g_reload = 1;
                    } else if (si.ssi_signo == SIGCHLD) {
                        int status;
                        pid_t wpid;
                        while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
                            untrack_worker(wpid);
                            if (g_active_children > 0) g_active_children--;
                            log_debug("worker pid=%d exited active=%d",
                                      (int)wpid, g_active_children);
                        }
                    }
                }
            } else if (efd == unix_fd || efd == tcp_fd) {
                while (1) {
                    int client_fd = accept(efd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                            log_warn("accept: %s", strerror(errno));
                        break;
                    }
                    log_debug("accepted connection on %s",
                              efd == unix_fd ? "unix" : "tcp");
                    dispatch_connection(client_fd);
                    /* client_fd closed inside dispatch_connection */
                }
            }
        }

        if (g_reload) {
            g_reload = 0;
            if (config_path) {
                daemon_config_t new_cfg;
                if (daemon_config_load(config_path, &new_cfg) == 0) {
                    memcpy(g_cfg->auth_token, new_cfg.auth_token, sizeof(g_cfg->auth_token));
                    g_cfg->max_concurrent           = new_cfg.max_concurrent;
                    g_cfg->default_timeout_ms       = new_cfg.default_timeout_ms;
                    g_cfg->max_timeout_ms           = new_cfg.max_timeout_ms;
                    g_cfg->default_max_output_bytes = new_cfg.default_max_output_bytes;
                    g_cfg->max_output_bytes         = new_cfg.max_output_bytes;
                    log_level_t new_lvl = HL_LOG_INFO;
                    if (!strcmp(new_cfg.log_level, "debug"))      new_lvl = HL_LOG_DEBUG;
                    else if (!strcmp(new_cfg.log_level, "warn"))  new_lvl = HL_LOG_WARN;
                    else if (!strcmp(new_cfg.log_level, "error")) new_lvl = HL_LOG_ERROR;
                    log_set_level(new_lvl);
                    memcpy(g_cfg->log_level, new_cfg.log_level, sizeof(g_cfg->log_level));
                    log_info("config reloaded from %s", config_path);
                } else {
                    log_warn("config reload failed — keeping existing config");
                }
            }
        }
    }

    /* Graceful shutdown: send SIGTERM to all worker children, then wait up to
       2s for them to finish, then SIGKILL any stragglers. */
    if (g_active_children > 0) {
        log_info("sending SIGTERM to %d in-flight workers", g_active_children);
        kill_all_workers(SIGTERM);  /* kill tracked worker children by PID */
        /* Wait up to 2s */
        int waited = 0;
        while (g_active_children > 0 && waited < 20) {
            struct timespec ts = {0, 100000000L}; /* 100ms */
            nanosleep(&ts, NULL);
            int status;
            pid_t wpid;
            while ((wpid = waitpid(-1, &status, WNOHANG)) > 0)
                if (g_active_children > 0) g_active_children--;
            waited++;
        }
        if (g_active_children > 0) {
            log_warn("killing %d remaining workers", g_active_children);
            kill_all_workers(SIGKILL);
            while (waitpid(-1, NULL, 0) > 0) {}
        }
    }

    close(sfd);
    close(epfd);
    log_info("hostlinkd stopped");
    return 0;
}
