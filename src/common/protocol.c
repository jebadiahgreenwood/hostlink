#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "log.h"

ssize_t read_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char *)buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return (ssize_t)total; /* EOF */
        total += (size_t)n;
    }
    return (ssize_t)total;
}

int write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char *)buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

ssize_t frame_encode(const char *payload, size_t payload_len, uint8_t **out) {
    if (payload_len > HL_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t total = HL_HEADER_SIZE + payload_len;
    *out = malloc(total);
    if (!*out) return -1;
    uint32_t magic_be = htonl(HL_MAGIC);
    uint32_t len_be   = htonl((uint32_t)payload_len);
    memcpy(*out,               &magic_be, 4);
    memcpy(*out + 4,           &len_be,   4);
    memcpy(*out + HL_HEADER_SIZE, payload, payload_len);
    return (ssize_t)total;
}

ssize_t frame_recv(int fd, char **payload_out) {
    *payload_out = NULL;
    uint8_t header[HL_HEADER_SIZE];
    ssize_t n = read_all(fd, header, HL_HEADER_SIZE);
    if (n == 0) return 0; /* EOF */
    if (n < (ssize_t)HL_HEADER_SIZE) {
        log_warn("frame_recv: short header read (%zd)", n);
        return -1;
    }
    uint32_t magic, payload_len;
    memcpy(&magic,       header,     4);
    memcpy(&payload_len, header + 4, 4);
    magic       = ntohl(magic);
    payload_len = ntohl(payload_len);
    if (magic != HL_MAGIC) {
        log_warn("frame_recv: bad magic 0x%08X", magic);
        return -2;
    }
    if (payload_len > HL_MAX_PAYLOAD) {
        log_warn("frame_recv: oversized frame %u bytes", payload_len);
        return -3;
    }
    char *buf = malloc(payload_len + 1);
    if (!buf) return -1;
    if (payload_len > 0) {
        n = read_all(fd, buf, payload_len);
        if (n < (ssize_t)payload_len) {
            free(buf);
            log_warn("frame_recv: short payload read");
            return -1;
        }
    }
    buf[payload_len] = '\0';
    *payload_out = buf;
    return (ssize_t)payload_len;
}

int frame_send(int fd, const char *payload, size_t payload_len) {
    uint8_t *frame;
    ssize_t total = frame_encode(payload, payload_len, &frame);
    if (total < 0) return -1;
    int rc = write_all(fd, frame, (size_t)total);
    free(frame);
    return rc;
}

int frame_send_json(int fd, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return -1;
    int rc = frame_send(fd, s, strlen(s));
    free(s);
    return rc;
}
