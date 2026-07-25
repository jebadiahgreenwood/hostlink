#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include "executor.h"
#include "../common/log.h"

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Command delivery: via the environment, not argv.
 *
 * The shell used to be exec'd as `sh -c "<command>"`, which put the command
 * text into the child's /proc/<pid>/cmdline. Any pattern-matching process
 * tool run *through* hostlink then matched hostlink's own exec shell:
 *   hl "pgrep -fc 'zzz_no_such_pattern'"   ->  2
 * That is not a cosmetic wart. `hl "pkill -f <pattern>"` matched and killed
 * its own session (SIGTERM, exit 144), orphaning the process it had just
 * started. The bracket trick ([p]attern) does not help, because the pattern
 * still appears verbatim in the wrapper's command line.
 *
 * So the command travels in HOSTLINK_COMMAND and the shell runs a fixed stub.
 * The stub copies it to a shell-local variable and unsets the exported one, so
 * the command text is absent from cmdline *and* from every descendant's
 * environ. What is left in cmdline is the constant stub, which matches nothing
 * a caller would search for.
 */
#define HL_CMD_ENV   "HOSTLINK_COMMAND"
#define HL_EVAL_STUB "__hl_c=\"$HOSTLINK_COMMAND\"; unset HOSTLINK_COMMAND; eval \"$__hl_c\""

/*
 * Hand the child a clean signal environment before exec.
 *
 * The daemon blocks SIGHUP/SIGINT/SIGTERM/SIGCHLD (it consumes them through a
 * signalfd) and ignores SIGPIPE. A signal MASK survives both fork and execve,
 * and a SIG_IGN disposition survives execve too — so without this, every
 * command run over hostlink executed with those signals blocked or ignored,
 * which is not what the same command does in any ordinary shell:
 *
 *   - `yes | head -1` did not die on SIGPIPE; the producer just kept going and
 *     the pipeline reported success.
 *   - a remote command could not be terminated by its own `kill -TERM $$`.
 *   - worst, the timeout path below sends SIGTERM to the process group, which
 *     the child was blocking — so every timeout ignored the graceful stop and
 *     waited out the full 2 s escalation to SIGKILL.
 *
 * execve already resets caught signals to their default; it is specifically the
 * mask and the ignored ones we have to undo by hand.
 */
static void reset_child_signals(void) {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT,  SIG_DFL);
    signal(SIGHUP,  SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* Build environment: merge process env with requested overrides, and append
 * HOSTLINK_COMMAND. Returns the count of heap-allocated entries (which the
 * caller must pass to free_env) via *heap_out. */
static char **build_env(exec_result_t *r, int *heap_out) {
    extern char **environ;
    int base_count = 0;
    while (environ[base_count]) base_count++;

    /* +1 slot for HOSTLINK_COMMAND, +1 for the NULL terminator */
    char **env = malloc(sizeof(char *) * (size_t)(base_count + r->env_count + 2));
    if (!env) { if (heap_out) *heap_out = 0; return NULL; }

    int out = 0, heap = 0;
    for (int i = 0; i < base_count; i++) {
        int skip = 0;
        /* a caller-supplied HOSTLINK_COMMAND would be shadowed anyway, but drop
         * any inherited one so the stub can never pick up a stale value */
        if (strncmp(environ[i], HL_CMD_ENV "=", sizeof(HL_CMD_ENV)) == 0)
            skip = 1;
        for (int j = 0; !skip && j < r->env_count; j++) {
            size_t klen = strlen(r->env_keys[j]);
            if (strncmp(environ[i], r->env_keys[j], klen) == 0 &&
                environ[i][klen] == '=') {
                skip = 1; break;
            }
        }
        if (!skip) env[out++] = environ[i];
    }
    for (int j = 0; j < r->env_count; j++) {
        char *kv;
        if (asprintf(&kv, "%s=%s", r->env_keys[j], r->env_vals[j]) < 0) {
            for (int k = out - heap; k < out; k++) free(env[k]);
            free(env);
            if (heap_out) *heap_out = 0;
            return NULL;
        }
        env[out++] = kv; heap++;
    }
    {
        char *kv;
        if (asprintf(&kv, "%s=%s", HL_CMD_ENV, r->command) < 0) {
            for (int k = out - heap; k < out; k++) free(env[k]);
            free(env);
            if (heap_out) *heap_out = 0;
            return NULL;
        }
        env[out++] = kv; heap++;
    }
    env[out] = NULL;
    if (heap_out) *heap_out = heap;
    return env;
}

static void free_env(char **env, int heap_count) {
    if (!env) return;
    int total = 0;
    while (env[total]) total++;
    for (int i = total - heap_count; i < total; i++)
        free(env[i]);
    free(env);
}

/* ── Detach path: double-fork so grandchild is owned by init ─────────── */
static void executor_run_detach(exec_result_t *r) {
    long long start_ms = now_ms();

    if (r->workdir[0] != '\0' && access(r->workdir, X_OK) != 0) {
        snprintf(r->error_msg, sizeof(r->error_msg),
                 "workdir not accessible: %s", strerror(errno));
        r->exec_error = 1;
        r->duration_ms = 0;
        return;
    }

    int env_heap = 0;
    char **env = build_env(r, &env_heap);
    if (!env) {
        /* Without the environment the stub has no command to eval — fail loud
         * rather than silently running nothing. */
        snprintf(r->error_msg, sizeof(r->error_msg), "out of memory building environment");
        r->exec_error = 1;
        r->duration_ms = 0;
        return;
    }

    /* First fork */
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(r->error_msg, sizeof(r->error_msg), "fork: %s", strerror(errno));
        r->exec_error = 1;
        free_env(env, env_heap);
        r->duration_ms = 0;
        return;
    }

    if (pid == 0) {
        /* Intermediate child — create new session, second fork, then exit.
           Grandchild is reparented to init; we never wait on it. */
        setsid();

        pid_t gpid = fork();
        if (gpid < 0) _exit(1);
        if (gpid > 0) _exit(0); /* intermediate exits immediately */

        /* Grandchild: fully detached */
        int null_rd = open("/dev/null", O_RDONLY);
        int null_wr = open("/dev/null", O_WRONLY);
        if (null_rd >= 0) { dup2(null_rd, STDIN_FILENO);  close(null_rd); }
        if (null_wr >= 0) { dup2(null_wr, STDOUT_FILENO); dup2(null_wr, STDERR_FILENO); close(null_wr); }
        for (int fd2 = 3; fd2 < 1024; fd2++) close(fd2);
        if (r->workdir[0] != '\0') { int _cd = chdir(r->workdir); (void)_cd; }

        reset_child_signals();
        char *argv_child[4] = { r->shell, "-c", (char *)HL_EVAL_STUB, NULL };
        execve(r->shell, argv_child, env);
        _exit(126);
    }

    /* Parent: wait on intermediate only (it exits quickly) */
    free_env(env, env_heap);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    r->exit_code = 0; /* detached — no exit code to report */
    r->duration_ms = (long)(now_ms() - start_ms);
}

/* ── Normal (synchronous) execution path ─────────────────────────────── */
void executor_run(exec_result_t *r) {
    r->stdout_buf = NULL;
    r->stderr_buf = NULL;
    r->stdout_len = 0;
    r->stderr_len = 0;
    r->stdout_original_bytes = 0;
    r->stderr_original_bytes = 0;
    r->stdout_truncated = 0;
    r->stderr_truncated = 0;
    r->stdout_file[0] = '\0';
    r->stderr_file[0] = '\0';
    r->exit_code = -1;
    r->timed_out = 0;
    r->exec_error = 0;
    r->error_msg[0] = '\0';

    /* Detach mode: double-fork, return immediately */
    if (r->detach) {
        executor_run_detach(r);
        return;
    }

    long long start_ms = now_ms();

    /* Validate workdir */
    if (r->workdir[0] != '\0') {
        if (access(r->workdir, X_OK) != 0) {
            snprintf(r->error_msg, sizeof(r->error_msg),
                     "workdir not accessible: %s", strerror(errno));
            r->exec_error = 1;
            r->duration_ms = 0;
            return;
        }
    }

    /* Create output files if file mode */
    int out_fd = -1, err_fd = -1;
    if (r->output_to_file) {
        struct stat st;
        if (stat(r->output_tmpdir, &st) != 0) {
            if (mkdir(r->output_tmpdir, 0750) != 0 && errno != EEXIST) {
                snprintf(r->error_msg, sizeof(r->error_msg),
                         "cannot create output_tmpdir: %s", strerror(errno));
                r->exec_error = 1;
                r->duration_ms = 0;
                return;
            }
        }
        snprintf(r->stdout_file, sizeof(r->stdout_file),
                 "%s/hl_%s_stdout", r->output_tmpdir, r->request_id);
        snprintf(r->stderr_file, sizeof(r->stderr_file),
                 "%s/hl_%s_stderr", r->output_tmpdir, r->request_id);
        out_fd = open(r->stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        err_fd = open(r->stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0 || err_fd < 0) {
            snprintf(r->error_msg, sizeof(r->error_msg),
                     "cannot create output files: %s", strerror(errno));
            if (out_fd >= 0) close(out_fd);
            if (err_fd >= 0) close(err_fd);
            r->exec_error = 1;
            r->duration_ms = 0;
            return;
        }
    }

    /* Create pipes */
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        snprintf(r->error_msg, sizeof(r->error_msg), "pipe: %s", strerror(errno));
        r->exec_error = 1;
        if (out_fd >= 0) close(out_fd);
        if (err_fd >= 0) close(err_fd);
        r->duration_ms = 0;
        return;
    }

    int env_heap = 0;
    char **env = build_env(r, &env_heap);
    if (!env) {
        snprintf(r->error_msg, sizeof(r->error_msg), "out of memory building environment");
        r->exec_error = 1;
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        if (out_fd >= 0) close(out_fd);
        if (err_fd >= 0) close(err_fd);
        r->duration_ms = 0;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(r->error_msg, sizeof(r->error_msg), "fork: %s", strerror(errno));
        r->exec_error = 1;
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        if (out_fd >= 0) close(out_fd);
        if (err_fd >= 0) close(err_fd);
        free_env(env, env_heap);
        r->duration_ms = 0;
        return;
    }

    if (pid == 0) {
        setpgrp();
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        for (int fd2 = 3; fd2 < 1024; fd2++) close(fd2);
        if (r->workdir[0] != '\0') {
            if (chdir(r->workdir) != 0) {
                fprintf(stderr, "chdir: %s\n", strerror(errno));
                _exit(126);
            }
        }
        reset_child_signals();
        char *argv_child[4];
        argv_child[0] = r->shell;
        argv_child[1] = "-c";
        argv_child[2] = (char *)HL_EVAL_STUB;
        argv_child[3] = NULL;
        execve(r->shell, argv_child, env);
        fprintf(stderr, "execve failed: %s\n", strerror(errno));
        _exit(126);
    }

    /* Parent */
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
    free_env(env, env_heap);

    make_nonblocking(stdout_pipe[0]);
    make_nonblocking(stderr_pipe[0]);

    int out_write_fd = -1, err_write_fd = -1;
    if (r->output_to_file) {
        out_write_fd = open(r->stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        err_write_fd = open(r->stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLHUP;
    ev.data.fd = stdout_pipe[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, stdout_pipe[0], &ev);
    ev.data.fd = stderr_pipe[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, stderr_pipe[0], &ev);

    int stdout_done = 0, stderr_done = 0;
    long long deadline = now_ms() + r->timeout_ms;
    size_t stdout_cap = 0, stderr_cap = 0;

    while (!stdout_done || !stderr_done) {
        long long remaining = deadline - now_ms();
        if (remaining <= 0) {
            kill(-pid, SIGTERM);
            for (int w = 0; w < 20; w++) {
                usleep(100000);
                if (waitpid(pid, NULL, WNOHANG) == pid) { pid = 0; break; }
            }
            if (pid != 0) kill(-pid, SIGKILL);
            r->timed_out = 1;
            break;
        }
        struct epoll_event events[2];
        int nev = epoll_wait(epfd, events, 2,
                             (int)(remaining > 1000 ? 1000 : remaining));
        if (nev < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < nev; i++) {
            int efd = events[i].data.fd;
            int is_stdout = (efd == stdout_pipe[0]);
            char rbuf[65536];
            while (1) {
                ssize_t nr = read(efd, rbuf, sizeof(rbuf));
                if (nr < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (is_stdout) stdout_done = 1; else stderr_done = 1;
                    break;
                }
                if (nr == 0) {
                    if (is_stdout) stdout_done = 1; else stderr_done = 1;
                    break;
                }
                long long *orig = is_stdout
                    ? &r->stdout_original_bytes : &r->stderr_original_bytes;
                *orig += nr;

                if (r->output_to_file) {
                    int wfd = is_stdout ? out_write_fd : err_write_fd;
                    if (wfd >= 0) { ssize_t wn = write(wfd, rbuf, (size_t)nr); (void)wn; }
                } else {
                    long long limit  = is_stdout ? r->max_stdout_bytes : r->max_stderr_bytes;
                    char    **buf    = is_stdout ? &r->stdout_buf : &r->stderr_buf;
                    size_t  *blen    = is_stdout ? &r->stdout_len : &r->stderr_len;
                    size_t  *bcap    = is_stdout ? &stdout_cap    : &stderr_cap;
                    int     *trunc   = is_stdout ? &r->stdout_truncated : &r->stderr_truncated;

                    if (*blen < (size_t)limit) {
                        size_t can_add = (size_t)limit - *blen;
                        size_t to_add  = ((size_t)nr < can_add) ? (size_t)nr : can_add;
                        if (*blen + to_add > *bcap) {
                            size_t newcap = *bcap == 0 ? 4096 : *bcap * 2;
                            while (newcap < *blen + to_add) newcap *= 2;
                            if (newcap > (size_t)limit) newcap = (size_t)limit;
                            char *newbuf = realloc(*buf, newcap + 1);
                            if (!newbuf) break;
                            *buf  = newbuf;
                            *bcap = newcap;
                        }
                        memcpy(*buf + *blen, rbuf, to_add);
                        *blen += to_add;
                        (*buf)[*blen] = '\0';
                        if ((long long)*blen >= limit) *trunc = 1;
                    } else {
                        *trunc = 1;
                    }
                }
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                if (is_stdout) stdout_done = 1; else stderr_done = 1;
            }
        }
    }

    close(epfd);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    if (out_write_fd >= 0) close(out_write_fd);
    if (err_write_fd >= 0) close(err_write_fd);

    int wstatus = 0;
    pid_t wpid  = 0;
    if (pid != 0) {
        do { wpid = waitpid(pid, &wstatus, 0); } while (wpid < 0 && errno == EINTR);
    }

    if (r->timed_out) {
        r->exit_code = -2;
    } else if (wpid == pid) {
        if (WIFEXITED(wstatus))        r->exit_code = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus)) r->exit_code = 128 + (int)WTERMSIG(wstatus);
        else                           r->exit_code = -1;
    }

    if (r->output_to_file) {
        if (r->stdout_file[0]) { int fd = open(r->stdout_file, O_WRONLY|O_CREAT, 0600); if (fd>=0) close(fd); }
        if (r->stderr_file[0]) { int fd = open(r->stderr_file, O_WRONLY|O_CREAT, 0600); if (fd>=0) close(fd); }
    }

    r->duration_ms = (long)(now_ms() - start_ms);
}

void executor_free(exec_result_t *r) {
    free(r->stdout_buf); r->stdout_buf = NULL;
    free(r->stderr_buf); r->stderr_buf = NULL;
}
