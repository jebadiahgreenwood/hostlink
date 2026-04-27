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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ftw.h>
#include "server.h"
#include "executor.h"
#include "../common/protocol.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/util.h"
#include "../common/sha256.h"
#include "../common/cjson/cJSON.h"

#define MAX_EVENTS 64
#define HL_GET_MAX_FILE_SIZE (90u * 1024u * 1024u)

static daemon_config_t *g_cfg        = NULL;
static volatile int     g_running    = 1;
static volatile int     g_reload     = 0;
static long long         g_start_time = 0;

/* Worker tracking — independent budgets for exec vs I/O workers.
 *
 * Why two counters: get/put used to run inline on the main event loop, which
 * head-of-line blocked everything else for the duration of the transfer.
 * Now each get/put forks like exec, but with its OWN concurrency budget so
 * a flurry of exec calls can't starve transfers and vice versa.
 *
 * SIGCHLD must decrement the right counter, so each tracked PID carries
 * its worker_type. Single-threaded epoll loop means the array doesn't
 * need locking. */
typedef enum {
    WORKER_EXEC = 1,    /* forked exec worker — bounded by max_concurrent    */
    WORKER_IO   = 2,    /* forked get/put worker — bounded by max_concurrent_io */
} worker_type_t;

typedef struct {
    pid_t         pid;
    worker_type_t type;
} worker_entry_t;

static int            g_active_children = 0;  /* WORKER_EXEC count */
static int            g_active_io       = 0;  /* WORKER_IO count   */
#define MAX_WORKERS 256
static worker_entry_t g_workers[MAX_WORKERS];
static int            g_worker_count = 0;

static void track_worker(pid_t pid, worker_type_t type) {
    if (g_worker_count < MAX_WORKERS) {
        g_workers[g_worker_count].pid  = pid;
        g_workers[g_worker_count].type = type;
        g_worker_count++;
    }
}

/* Remove the entry for `pid`. Returns its type (0 if not found). */
static worker_type_t untrack_worker(pid_t pid) {
    for (int i = 0; i < g_worker_count; i++) {
        if (g_workers[i].pid == pid) {
            worker_type_t t = g_workers[i].type;
            g_workers[i] = g_workers[--g_worker_count];
            return t;
        }
    }
    return (worker_type_t)0;
}

static void kill_all_workers(int sig) {
    for (int i = 0; i < g_worker_count; i++)
        kill(g_workers[i].pid, sig);
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
 * handle_put: write a file to the host from base64-encoded content.
 * Request fields:
 *   path    (string) — absolute destination path on the host
 *   content (string) — base64-encoded file bytes
 *   mode    (number, optional) — file permissions, default 0644
 *   mkdir   (bool, optional)   — create parent directories if needed
 */
static void handle_put(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    /* path */
    j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(j) || j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "put: path is required");
        return;
    }
    const char *dest_path = j->valuestring;

    /* content (base64) */
    j = cJSON_GetObjectItem(req, "content");
    if (!cJSON_IsString(j)) {
        send_error(fd, req_id, "bad_request", "put: content (base64) is required");
        return;
    }
    const char *b64 = j->valuestring;
    size_t b64_len  = strlen(b64);

    /* mode */
    mode_t file_mode = 0644;
    j = cJSON_GetObjectItem(req, "mode");
    if (cJSON_IsNumber(j) && j->valueint > 0)
        file_mode = (mode_t)j->valueint;

    /* mkdir */
    int do_mkdir = 0;
    j = cJSON_GetObjectItem(req, "mkdir");
    if (cJSON_IsTrue(j)) do_mkdir = 1;

    /* Decode base64 */
    size_t max_decoded = hl_b64_decoded_len(b64, b64_len) + 4;
    unsigned char *data = malloc(max_decoded);
    if (!data) {
        send_error(fd, req_id, "error", "put: out of memory");
        return;
    }
    ssize_t data_len = hl_b64_decode(b64, b64_len, data, max_decoded);
    if (data_len < 0) {
        free(data);
        send_error(fd, req_id, "bad_request", "put: invalid base64 content");
        return;
    }

    /* mkdir -p on parent if requested */
    if (do_mkdir) {
        char *path_copy = strdup(dest_path);
        if (path_copy) {
            char *slash = strrchr(path_copy, '/');
            if (slash && slash != path_copy) {
                *slash = '\0';
                /* Walk and create each component */
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s", path_copy);
                for (char *p = tmp + 1; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(tmp, 0755);
                        *p = '/';
                    }
                }
                mkdir(tmp, 0755);
            }
            free(path_copy);
        }
    }

    /* Write file */
    int wfd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, file_mode);
    if (wfd < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "put: cannot open %s: %s", dest_path, strerror(errno));
        free(data);
        send_error(fd, req_id, "error", msg);
        return;
    }

    ssize_t written = 0;
    while (written < data_len) {
        ssize_t n = write(wfd, data + written, (size_t)(data_len - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(wfd);
            free(data);
            char msg[256];
            snprintf(msg, sizeof(msg), "put: write error: %s", strerror(errno));
            send_error(fd, req_id, "error", msg);
            return;
        }
        written += n;
    }
    close(wfd);
    free(data);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version",       1);
    cJSON_AddStringToObject(resp, "id",            req_id);
    cJSON_AddStringToObject(resp, "node",          g_cfg->node_name);
    cJSON_AddStringToObject(resp, "status",        "ok");
    cJSON_AddStringToObject(resp, "path",          dest_path);
    cJSON_AddNumberToObject(resp, "bytes_written", (double)written);
    frame_send_json(fd, resp);
    cJSON_Delete(resp);
}


/*
 * handle_get: read a file from the host and return base64-encoded content.
 * Request fields:
 *   path (string) - absolute path of the file to retrieve
 */
static void handle_get(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    /* path */
    j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(j) || j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "get: path is required");
        return;
    }
    const char *src_path = j->valuestring;

    /* stat the file */
    struct stat st;
    if (stat(src_path, &st) < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "get: cannot open %s: %s", src_path, strerror(errno));
        send_error(fd, req_id, "error", msg);
        return;
    }

    /* size check */
    if ((size_t)st.st_size > HL_GET_MAX_FILE_SIZE) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "get: file too large (%zu bytes, max %u)",
                 (size_t)st.st_size, HL_GET_MAX_FILE_SIZE);
        send_error(fd, req_id, "error", msg);
        return;
    }

    size_t file_size = (size_t)st.st_size;

    /* allocate read buffer */
    unsigned char *buf = malloc(file_size + 1);
    if (!buf) {
        send_error(fd, req_id, "error", "get: out of memory");
        return;
    }

    int rfd = open(src_path, O_RDONLY);
    if (rfd < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "get: cannot open %s: %s", src_path, strerror(errno));
        free(buf);
        send_error(fd, req_id, "error", msg);
        return;
    }

    /* read entire file */
    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t n = read(rfd, buf + total_read, file_size - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(rfd);
            free(buf);
            char msg[256];
            snprintf(msg, sizeof(msg), "get: read error: %s", strerror(errno));
            send_error(fd, req_id, "error", msg);
            return;
        }
        if (n == 0) break;
        total_read += (size_t)n;
    }
    close(rfd);

    /* base64 encode */
    char *b64 = hl_b64_encode(buf, total_read);
    free(buf);
    if (!b64) {
        send_error(fd, req_id, "error", "get: base64 encode failed");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version", 1);
    cJSON_AddStringToObject(resp, "id",      req_id);
    cJSON_AddStringToObject(resp, "node",    g_cfg->node_name);
    cJSON_AddStringToObject(resp, "status",  "ok");
    cJSON_AddStringToObject(resp, "path",    src_path);
    cJSON_AddNumberToObject(resp, "size",    (double)total_read);
    cJSON_AddStringToObject(resp, "content", b64);
    free(b64);
    frame_send_json(fd, resp);
    cJSON_Delete(resp);
}

/* ── get_stat ─────────────────────────────────────────────────────────────
 *
 * Probe used by the client `get` dispatcher to learn what's at a remote path
 * before deciding how to fetch it. Returns:
 *   - isdir = false: { size }            for a single regular file
 *   - isdir = true:  { size, files: [{path, size}, ...] }
 *     where each entry's path is relative to the requested root.
 *
 * Walks with FTW_PHYS so symlinks (file or dir) are reported as their link
 * type and skipped — never followed — to avoid loops and surprise pulls.
 * Sockets, devices, and FIFOs are skipped silently.
 *
 * Caps:
 *   - HL_GET_STAT_MAX_FILES entries per request — the response would otherwise
 *     blow past HL_MAX_PAYLOAD on huge trees, and the client UX of pulling a
 *     million tiny files is bad anyway. Caller gets `truncated=true` in the
 *     response and an error_msg; they can narrow the path and retry.
 *
 * Wire protocol:
 *   C→D  {type:"get_stat", id, token, path}
 *   D→C  {status:"ok", id, node, path, isdir, size, [files], [truncated]}
 *        OR error JSON
 *
 * Runs in a forked I/O worker (same dispatch as get/put) so a slow walk of
 * a network filesystem can't stall the main event loop. */
#define HL_GET_STAT_MAX_FILES  100000

/* nftw has no user-data argument, so the walk callback reads/writes these.
 * Safe because each request runs in its own forked I/O worker: globals are
 * private to that child process. */
static cJSON      *g_walk_files;
static const char *g_walk_root;
static size_t      g_walk_root_len;
static uint64_t    g_walk_total;
static int         g_walk_count;
static int         g_walk_capped;

static int get_stat_walk_cb(const char *fpath, const struct stat *sb,
                            int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (typeflag != FTW_F) return 0;            /* dirs, symlinks, special: skip */
    if (!S_ISREG(sb->st_mode)) return 0;
    if (g_walk_count >= HL_GET_STAT_MAX_FILES) {
        g_walk_capped = 1;
        return 1;                                /* abort walk */
    }
    const char *rel = fpath;
    if (g_walk_root_len > 0 &&
        strncmp(fpath, g_walk_root, g_walk_root_len) == 0) {
        rel = fpath + g_walk_root_len;
        if (*rel == '/') rel++;
    }
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "path", rel);
    cJSON_AddNumberToObject(entry, "size", (double)sb->st_size);
    cJSON_AddItemToArray(g_walk_files, entry);
    g_walk_count++;
    g_walk_total += (uint64_t)sb->st_size;
    return 0;
}

static void handle_get_stat(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(j) || j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "get_stat: path is required");
        return;
    }
    const char *src_path = j->valuestring;

    /* lstat first so we can refuse to traverse a symlinked root — same
     * principle as FTW_PHYS below: never silently follow links. */
    struct stat st;
    if (lstat(src_path, &st) < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "get_stat: cannot stat %s: %s",
                 src_path, strerror(errno));
        send_error(fd, req_id, "error", msg);
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version", 1);
    cJSON_AddStringToObject(resp, "id",      req_id);
    cJSON_AddStringToObject(resp, "node",    g_cfg->node_name);
    cJSON_AddStringToObject(resp, "status",  "ok");
    cJSON_AddStringToObject(resp, "path",    src_path);

    if (S_ISREG(st.st_mode)) {
        cJSON_AddBoolToObject  (resp, "isdir", 0);
        cJSON_AddNumberToObject(resp, "size",  (double)st.st_size);
    } else if (S_ISDIR(st.st_mode)) {
        cJSON *files = cJSON_CreateArray();
        g_walk_files    = files;
        g_walk_root     = src_path;
        g_walk_root_len = strlen(src_path);
        /* Strip trailing slash so the relative-path math doesn't double up. */
        while (g_walk_root_len > 1 && src_path[g_walk_root_len - 1] == '/')
            g_walk_root_len--;
        g_walk_total  = 0;
        g_walk_count  = 0;
        g_walk_capped = 0;

        /* FTW_PHYS: don't follow symlinks. 32 fds is plenty and well under
         * the daemon's open-file limit. */
        int rc = nftw(src_path, get_stat_walk_cb, 32, FTW_PHYS);
        if (rc < 0 && !g_walk_capped) {
            cJSON_Delete(files);
            cJSON_Delete(resp);
            char msg[300];
            snprintf(msg, sizeof(msg), "get_stat: walk failed at %s: %s",
                     src_path, strerror(errno));
            send_error(fd, req_id, "error", msg);
            return;
        }

        cJSON_AddBoolToObject  (resp, "isdir", 1);
        cJSON_AddNumberToObject(resp, "size",  (double)g_walk_total);
        cJSON_AddItemToObject  (resp, "files", files);
        if (g_walk_capped) {
            cJSON_AddBoolToObject  (resp, "truncated", 1);
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "directory contains more than %d files; narrow the path",
                     HL_GET_STAT_MAX_FILES);
            cJSON_AddStringToObject(resp, "error_msg", msg);
            /* Status stays "ok" so the client can render the partial listing,
             * but it must check truncated before acting on `files`. */
        }
    } else {
        cJSON_Delete(resp);
        send_error(fd, req_id, "error",
                   "get_stat: path is neither regular file nor directory");
        return;
    }

    frame_send_json(fd, resp);
    cJSON_Delete(resp);
}

/* ── Streaming get/put ─────────────────────────────────────────────────────
 *
 * Wire protocol (forward-compatible with legacy get/put):
 *
 *   get_stream:
 *     C→D  {type:"get_stream", id, token, path}                     (1 frame, JSON)
 *     D→C  {status:"ok", id, node, path, size, stream:true}         (1 frame, JSON)
 *          OR error JSON, then close
 *     D→C  raw bytes                                                (N frames, binary)
 *          chunked at HL_STREAM_CHUNK; final chunk may be smaller
 *     D→C  {status:"ok", id, sha256:"<hex>", stream_end:true}       (1 frame, JSON)
 *
 *   put_stream:
 *     C→D  {type:"put_stream", id, token, path, size, mode?, mkdir?} (1 frame, JSON)
 *     D→C  {status:"ok", id, node, ready:true}                      (1 frame, JSON)
 *          OR error JSON, then close
 *     C→D  raw bytes                                                (N frames, binary)
 *     C→D  {sha256:"<hex>", stream_end:true}                        (1 frame, JSON)
 *     D→C  {status:"ok", id, node, sha256:"<hex>", size}            (1 frame, JSON)
 *          (status="error" if sha256 mismatch or short write)
 *
 * Notes:
 *   - Both flows run inside forked I/O workers (commit 1 fork-per-IO).
 *   - No size cap beyond available disk + connection lifetime. Tested at 1 GB+.
 *   - SHA-256 verifies integrity end-to-end. A 4 MiB chunk size keeps memory
 *     bounded (one chunk in flight), well under HL_MAX_PAYLOAD = 128 MiB. */
#define HL_STREAM_CHUNK  (4u * 1024u * 1024u)   /* 4 MiB per binary frame */

static void handle_get_stream(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(j) || j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "get_stream: path is required");
        return;
    }
    const char *src_path = j->valuestring;

    struct stat st;
    if (stat(src_path, &st) < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "get_stream: cannot open %s: %s", src_path, strerror(errno));
        send_error(fd, req_id, "error", msg);
        return;
    }

    int rfd = open(src_path, O_RDONLY);
    if (rfd < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "get_stream: cannot open %s: %s", src_path, strerror(errno));
        send_error(fd, req_id, "error", msg);
        return;
    }

    cJSON *hdr = cJSON_CreateObject();
    cJSON_AddNumberToObject(hdr, "version", 1);
    cJSON_AddStringToObject(hdr, "id",      req_id);
    cJSON_AddStringToObject(hdr, "node",    g_cfg->node_name);
    cJSON_AddStringToObject(hdr, "status",  "ok");
    cJSON_AddStringToObject(hdr, "path",    src_path);
    cJSON_AddNumberToObject(hdr, "size",    (double)st.st_size);
    cJSON_AddBoolToObject  (hdr, "stream",  1);
    if (frame_send_json(fd, hdr) != 0) { cJSON_Delete(hdr); close(rfd); return; }
    cJSON_Delete(hdr);

    sha256_ctx_t hctx;
    sha256_init(&hctx);

    uint8_t *buf = malloc(HL_STREAM_CHUNK);
    if (!buf) { log_error("get_stream: out of memory"); close(rfd); return; }

    size_t total_sent = 0;
    while ((size_t)st.st_size - total_sent > 0) {
        size_t want = (size_t)st.st_size - total_sent;
        if (want > HL_STREAM_CHUNK) want = HL_STREAM_CHUNK;

        ssize_t got = 0;
        while ((size_t)got < want) {
            ssize_t n = read(rfd, buf + got, want - (size_t)got);
            if (n < 0) {
                if (errno == EINTR) continue;
                log_warn("get_stream: read error: %s", strerror(errno));
                free(buf); close(rfd); return;
            }
            if (n == 0) break;   /* unexpected EOF — file shrunk under us */
            got += n;
        }
        if (got == 0) {
            log_warn("get_stream: EOF before expected size (sent=%zu of %lld)",
                     total_sent, (long long)st.st_size);
            break;
        }

        if (frame_send(fd, (const char *)buf, (size_t)got) != 0) {
            log_warn("get_stream: frame_send failed (peer disconnected?)");
            free(buf); close(rfd); return;
        }
        sha256_update(&hctx, buf, (size_t)got);
        total_sent += (size_t)got;
    }

    free(buf);
    close(rfd);

    uint8_t digest[SHA256_DIGEST_LEN];
    char    hex[SHA256_HEX_LEN];
    sha256_final(&hctx, digest);
    sha256_hex(digest, hex);

    cJSON *tail = cJSON_CreateObject();
    cJSON_AddNumberToObject(tail, "version",    1);
    cJSON_AddStringToObject(tail, "id",         req_id);
    cJSON_AddStringToObject(tail, "node",       g_cfg->node_name);
    cJSON_AddStringToObject(tail, "status",     "ok");
    cJSON_AddStringToObject(tail, "sha256",     hex);
    cJSON_AddNumberToObject(tail, "size",       (double)total_sent);
    cJSON_AddBoolToObject  (tail, "stream_end", 1);
    frame_send_json(fd, tail);
    cJSON_Delete(tail);
}

static void handle_put_stream(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(j) || j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "put_stream: path is required");
        return;
    }
    const char *dest_path = j->valuestring;

    j = cJSON_GetObjectItem(req, "size");
    if (!cJSON_IsNumber(j) || j->valuedouble < 0) {
        send_error(fd, req_id, "bad_request", "put_stream: size is required");
        return;
    }
    uint64_t size_bytes = (uint64_t)j->valuedouble;

    mode_t file_mode = 0644;
    j = cJSON_GetObjectItem(req, "mode");
    if (cJSON_IsNumber(j) && j->valueint > 0) file_mode = (mode_t)j->valueint;

    int do_mkdir = 0;
    j = cJSON_GetObjectItem(req, "mkdir");
    if (cJSON_IsTrue(j)) do_mkdir = 1;

    if (do_mkdir) {
        char *path_copy = strdup(dest_path);
        if (path_copy) {
            char *slash = strrchr(path_copy, '/');
            if (slash && slash != path_copy) {
                *slash = '\0';
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s", path_copy);
                for (char *p = tmp + 1; *p; p++) {
                    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
                }
                mkdir(tmp, 0755);
            }
            free(path_copy);
        }
    }

    int wfd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, file_mode);
    if (wfd < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "put_stream: cannot open %s: %s", dest_path, strerror(errno));
        send_error(fd, req_id, "error", msg);
        return;
    }

    cJSON *ready = cJSON_CreateObject();
    cJSON_AddNumberToObject(ready, "version", 1);
    cJSON_AddStringToObject(ready, "id",      req_id);
    cJSON_AddStringToObject(ready, "node",    g_cfg->node_name);
    cJSON_AddStringToObject(ready, "status",  "ok");
    cJSON_AddBoolToObject  (ready, "ready",   1);
    if (frame_send_json(fd, ready) != 0) {
        cJSON_Delete(ready); close(wfd); unlink(dest_path); return;
    }
    cJSON_Delete(ready);

    sha256_ctx_t hctx;
    sha256_init(&hctx);
    uint64_t total_recv = 0;

    while (total_recv < size_bytes) {
        char *chunk = NULL;
        ssize_t n = frame_recv(fd, &chunk);
        if (n <= 0) {
            free(chunk);
            log_warn("put_stream: chunk recv failed (n=%zd) at %llu/%llu",
                     n, (unsigned long long)total_recv, (unsigned long long)size_bytes);
            close(wfd); unlink(dest_path); return;
        }
        if ((uint64_t)n + total_recv > size_bytes) {
            log_warn("put_stream: chunk overshoots advertised size");
            free(chunk);
            close(wfd); unlink(dest_path); return;
        }
        if (write_all(wfd, chunk, (size_t)n) != 0) {
            log_warn("put_stream: write error: %s", strerror(errno));
            free(chunk); close(wfd); unlink(dest_path); return;
        }
        sha256_update(&hctx, chunk, (size_t)n);
        total_recv += (uint64_t)n;
        free(chunk);
    }
    close(wfd);

    char *trailer_buf = NULL;
    ssize_t tn = frame_recv(fd, &trailer_buf);
    if (tn <= 0 || !trailer_buf) {
        free(trailer_buf);
        send_error(fd, req_id, "error", "put_stream: missing trailer");
        unlink(dest_path);
        return;
    }
    cJSON *trailer = cJSON_Parse(trailer_buf);
    free(trailer_buf);
    const char *client_sha = "";
    if (trailer) {
        cJSON *sj = cJSON_GetObjectItem(trailer, "sha256");
        if (cJSON_IsString(sj)) client_sha = sj->valuestring;
    }

    uint8_t digest[SHA256_DIGEST_LEN];
    char    hex[SHA256_HEX_LEN];
    sha256_final(&hctx, digest);
    sha256_hex(digest, hex);

    int sha_ok = (client_sha[0] != '\0' && strcmp(client_sha, hex) == 0);
    if (!sha_ok) {
        log_warn("put_stream: sha256 mismatch (client=%s daemon=%s)",
                 client_sha[0] ? client_sha : "(none)", hex);
        unlink(dest_path);
    }
    if (trailer) cJSON_Delete(trailer);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version", 1);
    cJSON_AddStringToObject(resp, "id",      req_id);
    cJSON_AddStringToObject(resp, "node",    g_cfg->node_name);
    cJSON_AddStringToObject(resp, "status",  sha_ok ? "ok" : "error");
    cJSON_AddStringToObject(resp, "path",    dest_path);
    cJSON_AddNumberToObject(resp, "size",    (double)total_recv);
    cJSON_AddStringToObject(resp, "sha256",  hex);
    if (!sha_ok)
        cJSON_AddStringToObject(resp, "error_msg",
            "put_stream: sha256 mismatch — file discarded");
    frame_send_json(fd, resp);
    cJSON_Delete(resp);
}

/*
 * handle_exec: runs in a forked worker child.
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
    snprintf(r.request_id,    sizeof(r.request_id),    "%s", req_id);
    snprintf(r.command,       sizeof(r.command),        "%s", cmd_j->valuestring);
    snprintf(r.shell,         sizeof(r.shell),          "%s", g_cfg->shell);
    snprintf(r.output_tmpdir, sizeof(r.output_tmpdir),  "%s", g_cfg->output_tmpdir);

    j = cJSON_GetObjectItem(req, "workdir");
    if (cJSON_IsString(j)) snprintf(r.workdir, sizeof(r.workdir), "%s", j->valuestring);

    j = cJSON_GetObjectItem(req, "timeout_ms");
    r.timeout_ms = (cJSON_IsNumber(j) && j->valueint > 0)
                   ? j->valueint : g_cfg->default_timeout_ms;
    if (r.timeout_ms > g_cfg->max_timeout_ms) r.timeout_ms = g_cfg->max_timeout_ms;

    /* detach flag */
    j = cJSON_GetObjectItem(req, "detach");
    r.detach = cJSON_IsTrue(j) ? 1 : 0;

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

    if (r.detach) {
        cJSON_AddTrueToObject(resp, "detached");
    } else if (r.output_to_file) {
        cJSON_AddStringToObject(resp, "stdout_file", r.stdout_file);
        cJSON_AddStringToObject(resp, "stderr_file", r.stderr_file);
    } else {
        cJSON_AddStringToObject(resp, "stdout", r.stdout_buf ? r.stdout_buf : "");
        cJSON_AddStringToObject(resp, "stderr", r.stderr_buf ? r.stderr_buf : "");
    }
    cJSON_AddBoolToObject(resp,   "stdout_truncated",      r.stdout_truncated);
    cJSON_AddBoolToObject(resp,   "stderr_truncated",      r.stderr_truncated);
    cJSON_AddNumberToObject(resp, "stdout_original_bytes", (double)r.stdout_original_bytes);
    cJSON_AddNumberToObject(resp, "stderr_original_bytes", (double)r.stderr_original_bytes);
    cJSON_AddNumberToObject(resp, "duration_ms",           (double)r.duration_ms);

    frame_send_json(fd, resp);
    cJSON_Delete(resp);
    executor_free(&r);
}

static void dispatch_connection(int client_fd) {
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
        handle_ping(client_fd, req);
        cJSON_Delete(req);
        close(client_fd);
        return;
    }

    /* get/put: fork an I/O worker so the main event loop stays responsive.
     * Bounded by max_concurrent_io independently of max_concurrent (exec).
     *
     * Pre-refactor these ran inline on the event loop, head-of-line blocking
     * every other request for the duration of the transfer. With multiple
     * agents calling hl-get/hl-put, that meant a single multi-MiB transfer
     * froze the whole daemon. Forking matches the exec model. */
    if (!strcmp(type, "put")        || !strcmp(type, "get") ||
        !strcmp(type, "put_stream") || !strcmp(type, "get_stream") ||
        !strcmp(type, "get_stat")) {
        cJSON *id_j = cJSON_GetObjectItem(req, "id");
        const char *req_id = cJSON_IsString(id_j) ? id_j->valuestring : "";

        if (g_active_io >= g_cfg->max_concurrent_io) {
            send_error(client_fd, req_id, "error",
                       "server busy, max concurrent I/O transfers reached");
            cJSON_Delete(req);
            close(client_fd);
            return;
        }

        g_active_io++;
        pid_t pid = fork();
        if (pid < 0) {
            g_active_io--;
            send_error(client_fd, req_id, "error", "fork failed");
            cJSON_Delete(req);
            close(client_fd);
            return;
        }

        if (pid == 0) {
            /* Child: handle the transfer, then exit. The handler closes
             * client_fd implicitly via _exit (close-on-exit semantics).
             * We don't run cJSON_Delete here because _exit reclaims memory. */
            if      (!strcmp(type, "put"))        handle_put       (client_fd, req);
            else if (!strcmp(type, "get"))        handle_get       (client_fd, req);
            else if (!strcmp(type, "put_stream")) handle_put_stream(client_fd, req);
            else if (!strcmp(type, "get_stream")) handle_get_stream(client_fd, req);
            else                                  handle_get_stat  (client_fd, req);
            _exit(0);
        }

        /* Parent: track child, drop the fd, return to epoll. */
        track_worker(pid, WORKER_IO);
        cJSON_Delete(req);
        close(client_fd);
        log_debug("forked I/O worker pid=%d type=%s active_io=%d",
                  (int)pid, type, g_active_io);
        return;
    }

    if (!strcmp(type, "exec")) {
        cJSON *id_j = cJSON_GetObjectItem(req, "id");
        const char *req_id = cJSON_IsString(id_j) ? id_j->valuestring : "";

        /* detach: lightweight double-fork, no output, no concurrency slot needed */
        cJSON *detach_j = cJSON_GetObjectItem(req, "detach");
        if (cJSON_IsTrue(detach_j)) {
            handle_exec(client_fd, req);
            cJSON_Delete(req);
            close(client_fd);
            return;
        }

        if (g_active_children >= g_cfg->max_concurrent) {
            send_error(client_fd, req_id, "error",
                       "server busy, max concurrent commands reached");
            cJSON_Delete(req);
            close(client_fd);
            return;
        }

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
            handle_exec(client_fd, req);
            cJSON_Delete(req);
            close(client_fd);
            _exit(0);
        }

        track_worker(pid, WORKER_EXEC);
        cJSON_Delete(req);
        close(client_fd);
        log_debug("forked exec worker pid=%d active=%d", (int)pid, g_active_children);
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
                            worker_type_t t = untrack_worker(wpid);
                            if (t == WORKER_EXEC && g_active_children > 0) {
                                g_active_children--;
                                log_debug("exec worker pid=%d exited active=%d",
                                          (int)wpid, g_active_children);
                            } else if (t == WORKER_IO && g_active_io > 0) {
                                g_active_io--;
                                log_debug("I/O worker pid=%d exited active_io=%d",
                                          (int)wpid, g_active_io);
                            } else {
                                /* Untracked PID — likely a detached double-fork's
                                 * intermediate child, or a worker exiting after
                                 * we already decremented in fork-failure path. */
                                log_debug("untracked child pid=%d reaped", (int)wpid);
                            }
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
                    g_cfg->max_concurrent_io        = new_cfg.max_concurrent_io;
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

    if (g_worker_count > 0) {
        log_info("sending SIGTERM to %d in-flight workers (exec=%d io=%d)",
                 g_worker_count, g_active_children, g_active_io);
        kill_all_workers(SIGTERM);
        int waited = 0;
        while (g_worker_count > 0 && waited < 20) {
            struct timespec ts = {0, 100000000L};
            nanosleep(&ts, NULL);
            int status;
            pid_t wpid;
            while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
                worker_type_t t = untrack_worker(wpid);
                if (t == WORKER_EXEC && g_active_children > 0) g_active_children--;
                else if (t == WORKER_IO && g_active_io > 0)    g_active_io--;
            }
            waited++;
        }
        if (g_worker_count > 0) {
            log_warn("killing %d remaining workers", g_worker_count);
            kill_all_workers(SIGKILL);
            while (waitpid(-1, NULL, 0) > 0) {}
        }
    }

    close(sfd);
    close(epfd);
    log_info("hostlinkd stopped");
    return 0;
}
