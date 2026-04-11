#ifndef HOSTLINK_UTIL_H
#define HOSTLINK_UTIL_H

#include <stddef.h>
#include <sys/types.h>

/* Constant-time string comparison (prevents timing attacks on tokens) */
int ct_strcmp(const char *a, const char *b);

/* Parse a size string like "4M", "512K", "1G" into bytes.
   Returns -1 on parse error. */
long long parse_size(const char *s);

/* Safe strdup that aborts on OOM */
char *hl_strdup(const char *s);

/* Base64 encode/decode (RFC 4648, no line wrapping).
   hl_b64_encode: returns malloc'd NUL-terminated string, caller must free. NULL on OOM.
   hl_b64_decoded_len: upper bound on decoded byte count for a given b64 string length.
   hl_b64_decode: returns decoded byte count, or -1 on bad input. */
char   *hl_b64_encode(const unsigned char *src, size_t src_len);
size_t  hl_b64_decoded_len(const char *b64, size_t b64_len);
ssize_t hl_b64_decode(const char *src, size_t src_len,
                      unsigned char *out_buf, size_t out_cap);

#endif /* HOSTLINK_UTIL_H */
