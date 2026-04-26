/* sha256.h — minimal streaming SHA-256.
 *
 * Public domain, derived from the FIPS 180-4 reference. No external
 * crypto deps — keeps hostlinkd build lean (no libssl link).
 *
 * Streaming API (init / update / final) so we can hash multi-GB transfers
 * without buffering the whole payload.
 *
 * Usage:
 *     sha256_ctx_t c;
 *     sha256_init(&c);
 *     while ((n = read(fd, buf, sizeof buf)) > 0)
 *         sha256_update(&c, buf, (size_t)n);
 *     uint8_t digest[32];
 *     sha256_final(&c, digest);
 *     char hex[65];
 *     sha256_hex(digest, hex);   // NUL-terminated 64-char lowercase hex
 */
#ifndef HOSTLINK_SHA256_H
#define HOSTLINK_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN  32
#define SHA256_HEX_LEN     65   /* 64 chars + NUL */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx_t;

void sha256_init  (sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final (sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]);

/* Helper: write 64-char lowercase hex + NUL into out (must be >= 65 bytes). */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_LEN], char out[SHA256_HEX_LEN]);

#endif /* HOSTLINK_SHA256_H */
