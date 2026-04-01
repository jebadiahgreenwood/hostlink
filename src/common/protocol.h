#ifndef HOSTLINK_PROTOCOL_H
#define HOSTLINK_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "../common/cjson/cJSON.h"

#define HL_MAGIC        0x484C4E4Bu  /* "HLNK" */
#define HL_MAX_PAYLOAD  (128u * 1024u * 1024u)  /* 128 MiB */
#define HL_HEADER_SIZE  8  /* 4 bytes magic + 4 bytes len */

/* Frame encode: allocates *out (caller must free). Returns total bytes or -1. */
ssize_t frame_encode(const char *payload, size_t payload_len,
                     uint8_t **out);

/* Frame decode from a stream fd. Reads exactly one frame.
   *payload_out is malloc'd; caller must free.
   Returns payload length, 0 on EOF, -1 on error (sets errno),
   -2 on bad magic, -3 on oversized frame. */
ssize_t frame_recv(int fd, char **payload_out);

/* Send a full frame to fd. Returns 0 on success, -1 on error. */
int frame_send(int fd, const char *payload, size_t payload_len);

/* Convenience: send a JSON object as a frame */
int frame_send_json(int fd, cJSON *obj);

/* Read all bytes from fd into a buffer. Handles partial reads.
   Returns bytes read, 0 on EOF, -1 on error. */
ssize_t read_all(int fd, void *buf, size_t len);

/* Write all bytes to fd. Returns 0 on success, -1 on error. */
int write_all(int fd, const void *buf, size_t len);

#endif /* HOSTLINK_PROTOCOL_H */
