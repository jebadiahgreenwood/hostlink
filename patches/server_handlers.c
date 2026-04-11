/* ---- File Transfer: handle_put / handle_get ----
 *
 * These run inline (no fork) since they're I/O-bound, not CPU-bound.
 * The frame protocol already limits us to 128 MiB, which bounds memory usage.
 */

static void handle_put(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    /* Required: path, data (base64) */
    cJSON *path_j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(path_j) || path_j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "path is required");
        return;
    }
    const char *remote_path = path_j->valuestring;

    /* path must be absolute */
    if (remote_path[0] != '/') {
        send_error(fd, req_id, "bad_request", "path must be absolute");
        return;
    }

    cJSON *data_j = cJSON_GetObjectItem(req, "data");
    if (!cJSON_IsString(data_j)) {
        send_error(fd, req_id, "bad_request", "data (base64) is required");
        return;
    }

    /* Optional: mode (octal, default 0644) */
    int file_mode = 0644;
    j = cJSON_GetObjectItem(req, "mode");
    if (cJSON_IsNumber(j)) file_mode = j->valueint;

    /* Decode base64 */
    unsigned char *decoded = NULL;
    ssize_t dec_len = b64_decode(data_j->valuestring,
                                  strlen(data_j->valuestring), &decoded);
    if (dec_len < 0 || !decoded) {
        send_error(fd, req_id, "error", "base64 decode failed");
        return;
    }

    /* Atomic write: write to .tmp, then rename */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.hlput.tmp", remote_path);

    int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)file_mode);
    if (out_fd < 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "cannot create %s: %s",
                 tmp_path, strerror(errno));
        send_error(fd, req_id, "error", errbuf);
        free(decoded);
        return;
    }

    /* Write all data */
    size_t written = 0;
    while (written < (size_t)dec_len) {
        ssize_t w = write(out_fd, decoded + written, (size_t)dec_len - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "write error: %s", strerror(errno));
            close(out_fd);
            unlink(tmp_path);
            free(decoded);
            send_error(fd, req_id, "error", errbuf);
            return;
        }
        written += (size_t)w;
    }

    /* fsync for durability */
    fsync(out_fd);
    close(out_fd);
    free(decoded);

    /* Rename atomically */
    if (rename(tmp_path, remote_path) != 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "rename to %s failed: %s",
                 remote_path, strerror(errno));
        unlink(tmp_path);
        send_error(fd, req_id, "error", errbuf);
        return;
    }

    /* Success response */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version", 1);
    cJSON_AddStringToObject(resp, "id",      req_id);
    cJSON_AddStringToObject(resp, "node",    g_cfg->node_name);
    cJSON_AddStringToObject(resp, "status",  "ok");
    cJSON_AddNumberToObject(resp, "bytes_written", (double)written);
    cJSON_AddStringToObject(resp, "path",    remote_path);
    frame_send_json(fd, resp);
    cJSON_Delete(resp);
}

static void handle_get(int fd, cJSON *req) {
    const char *req_id = "";
    cJSON *j;
    j = cJSON_GetObjectItem(req, "id");
    if (cJSON_IsString(j)) req_id = j->valuestring;

    /* Required: path */
    cJSON *path_j = cJSON_GetObjectItem(req, "path");
    if (!cJSON_IsString(path_j) || path_j->valuestring[0] == '\0') {
        send_error(fd, req_id, "bad_request", "path is required");
        return;
    }
    const char *remote_path = path_j->valuestring;

    if (remote_path[0] != '/') {
        send_error(fd, req_id, "bad_request", "path must be absolute");
        return;
    }

    /* Optional: max_bytes (default: config default_max_output_bytes) */
    long long max_bytes = g_cfg->default_max_output_bytes;
    j = cJSON_GetObjectItem(req, "max_bytes");
    if (cJSON_IsNumber(j) && j->valuedouble > 0)
        max_bytes = (long long)j->valuedouble;
    /* Clamp to config max */
    if (g_cfg->max_output_bytes > 0 && max_bytes > g_cfg->max_output_bytes)
        max_bytes = g_cfg->max_output_bytes;

    /* Open and stat the file */
    int in_fd = open(remote_path, O_RDONLY);
    if (in_fd < 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "cannot open %s: %s",
                 remote_path, strerror(errno));
        send_error(fd, req_id, "error", errbuf);
        return;
    }

    struct stat st;
    if (fstat(in_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(in_fd);
        send_error(fd, req_id, "error", "not a regular file");
        return;
    }

    long long file_size = (long long)st.st_size;
    int truncated = 0;
    long long read_size = file_size;
    if (read_size > max_bytes) {
        read_size = max_bytes;
        truncated = 1;
    }

    /* Read the file */
    unsigned char *buf = malloc((size_t)read_size + 1);
    if (!buf) {
        close(in_fd);
        send_error(fd, req_id, "error", "out of memory");
        return;
    }

    size_t total_read = 0;
    while ((long long)total_read < read_size) {
        ssize_t n = read(in_fd, buf + total_read,
                         (size_t)(read_size - (long long)total_read));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(in_fd);
            send_error(fd, req_id, "error", "read error");
            return;
        }
        if (n == 0) break; /* EOF */
        total_read += (size_t)n;
    }
    close(in_fd);

    /* Base64 encode */
    char *encoded = NULL;
    ssize_t enc_len = b64_encode(buf, total_read, &encoded);
    free(buf);
    if (enc_len < 0 || !encoded) {
        send_error(fd, req_id, "error", "base64 encode failed");
        return;
    }

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "version",   1);
    cJSON_AddStringToObject(resp, "id",        req_id);
    cJSON_AddStringToObject(resp, "node",      g_cfg->node_name);
    cJSON_AddStringToObject(resp, "status",    "ok");
    cJSON_AddStringToObject(resp, "data",      encoded);
    cJSON_AddNumberToObject(resp, "size",      (double)file_size);
    cJSON_AddNumberToObject(resp, "bytes_read",(double)total_read);
    cJSON_AddBoolToObject(resp,   "truncated", truncated);
    cJSON_AddStringToObject(resp, "path",      remote_path);
    frame_send_json(fd, resp);
    cJSON_Delete(resp);
    free(encoded);
}
