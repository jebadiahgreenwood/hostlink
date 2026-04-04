#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <grp.h>
#include <pwd.h>
#include "server.h"
#include "../common/config.h"
#include "../common/log.h"

#define VERSION "1.0.0"
#define DEFAULT_CONFIG "/etc/hostlink/hostlink.conf"
#define PID_FILE "/run/hostlink/hostlink.pid"

static void usage(void) {
    printf("Usage: hostlinkd [OPTIONS]\n"
           "  -c, --config <path>  Config file (default: " DEFAULT_CONFIG ")\n"
           "  -f, --foreground     Run in foreground\n"
           "  -v, --verbose        Increase verbosity (repeatable)\n"
           "  -h, --help           Show help\n"
           "  -V, --version        Show version\n");
}

static int setup_unix_socket(const char *path, int mode, const char *group) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket(AF_UNIX)"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* sun_path is 108 bytes on Linux; validate before copy */
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Unix socket path too long: %s\n", path);
        close(fd); return -1;
    }
    memcpy(addr.sun_path, path, plen + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind(unix)"); close(fd); return -1;
    }
    chmod(path, (mode_t)mode);
    if (group && group[0]) {
        struct group *gr = getgrnam(group);
        if (gr) { if (chown(path, (uid_t)-1, gr->gr_gid) != 0) log_warn("chown failed on %s", path); }
        else log_warn("group '%s' not found, skipping chown", group);
    }
    if (listen(fd, 64) != 0) { perror("listen(unix)"); close(fd); return -1; }
    return fd;
}

static int setup_tcp_socket(const char *bind_addr, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket(TCP)"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address: %s\n", bind_addr);
        close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind(tcp)"); close(fd); return -1;
    }
    if (listen(fd, 64) != 0) { perror("listen(tcp)"); close(fd); return -1; }
    return fd;
}

static void mkdir_parent(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *p = strrchr(tmp, '/');
    if (!p || p == tmp) return;
    *p = '\0';
    /* mkdir -p equivalent: walk and create each component */
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0755);
            *q = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_pidfile(const char *path) {
    mkdir_parent(path);
    FILE *f = fopen(path, "w");
    if (!f) { log_warn("cannot write PID file %s: %s", path, strerror(errno)); return; }
    fprintf(f, "%d\n", getpid());
    fclose(f);
}

/* Check if another instance is already running via PID file.
 * Returns 1 if a live process exists, 0 if safe to start.
 * Removes stale PID files (process dead but file remains). */
static int check_existing_instance(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;  /* no PID file — safe to start */
    pid_t existing_pid = 0;
    fscanf(f, "%d", &existing_pid);
    fclose(f);
    if (existing_pid <= 0) { unlink(path); return 0; }
    /* Check if the process is alive by sending signal 0 */
    if (kill(existing_pid, 0) == 0) {
        /* Process exists — check it's actually hostlinkd, not a recycled PID */
        char proc_comm[256] = {0};
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)existing_pid);
        FILE *cf = fopen(comm_path, "r");
        if (cf) { fscanf(cf, "%255s", proc_comm); fclose(cf); }
        if (strstr(proc_comm, "hostlinkd") != NULL) {
            fprintf(stderr, "ERROR: hostlinkd is already running (pid %d).\n"
                            "       Use 'kill %d' or 'systemctl restart hostlinkd' to restart.\n",
                    (int)existing_pid, (int)existing_pid);
            return 1;
        }
    }
    /* Stale PID file — process is dead */
    unlink(path);
    return 0;
}

static void drop_privileges(const char *username) {
    if (!username || username[0] == '\0') return;
    if (getuid() != 0) return;
    struct passwd *pw = getpwnam(username);
    if (!pw) { log_warn("user '%s' not found, not dropping privileges", username); return; }
    if (setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
        log_error("failed to drop privileges to %s: %s", username, strerror(errno));
        exit(1);
    }
    log_info("dropped privileges to %s (uid=%d)", username, (int)pw->pw_uid);
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    if (setsid() < 0) { perror("setsid"); exit(1); }

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    umask(0);
    if (chdir("/") != 0) { perror("chdir"); exit(1); }

    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        if (null > 2) close(null);
    }
}

int main(int argc, char *argv[]) {
    const char *config_path = DEFAULT_CONFIG;
    int foreground = 0;
    int verbosity  = 0;

    static struct option long_opts[] = {
        {"config",     required_argument, NULL, 'c'},
        {"foreground", no_argument,       NULL, 'f'},
        {"verbose",    no_argument,       NULL, 'v'},
        {"help",       no_argument,       NULL, 'h'},
        {"version",    no_argument,       NULL, 'V'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:fvhV", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'f': foreground  = 1;      break;
            case 'v': verbosity++;           break;
            case 'h': usage(); return 0;
            case 'V': printf("hostlinkd %s\n", VERSION); return 0;
            default:  usage(); return 1;
        }
    }

    daemon_config_t cfg;
    if (daemon_config_load(config_path, &cfg) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", config_path);
        return 1;
    }

    log_level_t log_lvl = HL_LOG_INFO;
    if (!strcmp(cfg.log_level, "debug"))      log_lvl = HL_LOG_DEBUG;
    else if (!strcmp(cfg.log_level, "warn"))  log_lvl = HL_LOG_WARN;
    else if (!strcmp(cfg.log_level, "error")) log_lvl = HL_LOG_ERROR;
    if (verbosity > 0) {
        int l = (int)log_lvl - verbosity;
        log_lvl = (log_level_t)(l < 0 ? 0 : l);
    }

    log_target_t log_tgt = LOG_TARGET_STDERR;
    if (!strcmp(cfg.log_target, "syslog"))          log_tgt = LOG_TARGET_SYSLOG;
    else if (strcmp(cfg.log_target, "stderr") != 0) log_tgt = LOG_TARGET_FILE;

    const char *log_file = (log_tgt == LOG_TARGET_FILE) ? cfg.log_target : NULL;
    log_init(log_tgt, log_lvl, log_file);

    /* Refuse to start if another instance is already running */
    if (check_existing_instance(PID_FILE) != 0) { return 1; }

    int unix_fd = -1, tcp_fd = -1;
    if (cfg.unix_enabled) {
        unix_fd = setup_unix_socket(cfg.unix_path, cfg.unix_mode, cfg.unix_group);
        if (unix_fd < 0) { log_error("Failed to create Unix socket"); return 1; }
        log_info("Unix socket: %s", cfg.unix_path);
    }
    if (cfg.tcp_enabled) {
        tcp_fd = setup_tcp_socket(cfg.tcp_bind, cfg.tcp_port);
        if (tcp_fd < 0) { log_error("Failed to create TCP socket"); return 1; }
        log_info("TCP socket: %s:%d", cfg.tcp_bind, cfg.tcp_port);
    }
    if (unix_fd < 0 && tcp_fd < 0) {
        log_error("No transports enabled"); return 1;
    }

    drop_privileges(cfg.run_as_user);

    if (!foreground) daemonize();

    write_pidfile(PID_FILE);

    int rc = server_run(&cfg, unix_fd, tcp_fd, config_path);

    if (cfg.unix_enabled) unlink(cfg.unix_path);
    unlink(PID_FILE);
    log_close();
    return rc;
}
