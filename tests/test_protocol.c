#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "testlib.h"
#include "../src/common/protocol.h"

/* Helper: create a pipe, write data to write-end, return read-end */
static int pipe_with_data(const void *data, size_t len) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    /* Write all data */
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fds[1], (const char *)data + written, len - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    close(fds[1]);
    return fds[0];
}

static void test_encode_decode_roundtrip(void) {
    TEST("encode_decode_roundtrip");
    const char *payload = "{\"hello\":\"world\"}";
    uint8_t *frame;
    ssize_t total = frame_encode(payload, strlen(payload), &frame);
    ASSERT(total == (ssize_t)(8 + strlen(payload)));

    int rd = pipe_with_data(frame, (size_t)total);
    free(frame);
    char *out = NULL;
    ssize_t n = frame_recv(rd, &out);
    close(rd);
    ASSERT(n == (ssize_t)strlen(payload));
    ASSERT_NOTNULL(out);
    ASSERT_STR(out, payload);
    free(out);
}

static void test_magic_bytes(void) {
    TEST("magic_bytes");
    const char *payload = "test";
    uint8_t *frame;
    frame_encode(payload, strlen(payload), &frame);
    uint32_t magic;
    memcpy(&magic, frame, 4);
    ASSERT_EQ(ntohl(magic), (uint32_t)0x484C4E4B);
    free(frame);
}

static void test_bad_magic(void) {
    TEST("bad_magic");
    uint8_t bad[12];
    uint32_t bad_magic = htonl(0xDEADBEEF);
    uint32_t len_be    = htonl(4);
    memcpy(bad, &bad_magic, 4);
    memcpy(bad + 4, &len_be, 4);
    memcpy(bad + 8, "data", 4);
    int rd = pipe_with_data(bad, sizeof(bad));
    char *out = NULL;
    ssize_t n = frame_recv(rd, &out);
    close(rd);
    ASSERT_EQ(n, -2);
    free(out);
}

static void test_oversized_frame(void) {
    TEST("oversized_frame");
    uint8_t hdr[8];
    uint32_t magic_be = htonl(HL_MAGIC);
    uint32_t len_be   = htonl(HL_MAX_PAYLOAD + 1);
    memcpy(hdr, &magic_be, 4);
    memcpy(hdr + 4, &len_be, 4);
    int rd = pipe_with_data(hdr, 8);
    char *out = NULL;
    ssize_t n = frame_recv(rd, &out);
    close(rd);
    ASSERT_EQ(n, -3);
    free(out);
}

static void test_truncated_frame(void) {
    TEST("truncated_frame");
    /* Say payload is 100 bytes but only send 4 */
    uint8_t hdr[8];
    uint32_t magic_be = htonl(HL_MAGIC);
    uint32_t len_be   = htonl(100);
    memcpy(hdr, &magic_be, 4);
    memcpy(hdr + 4, &len_be, 4);
    uint8_t partial[12]; /* header + 4 bytes only */
    memcpy(partial, hdr, 8);
    memcpy(partial + 8, "abcd", 4);
    int rd = pipe_with_data(partial, sizeof(partial));
    char *out = NULL;
    ssize_t n = frame_recv(rd, &out);
    close(rd);
    ASSERT(n < 0 || n == 0); /* error or EOF */
    free(out);
}

static void test_multiple_frames(void) {
    TEST("multiple_frames");
    const char *p1 = "{\"n\":1}";
    const char *p2 = "{\"n\":2}";
    uint8_t *f1, *f2;
    ssize_t t1 = frame_encode(p1, strlen(p1), &f1);
    ssize_t t2 = frame_encode(p2, strlen(p2), &f2);

    /* Concatenate both frames */
    uint8_t *both = malloc((size_t)(t1 + t2));
    memcpy(both, f1, (size_t)t1);
    memcpy(both + t1, f2, (size_t)t2);
    free(f1); free(f2);

    int fds[2];
    pipe(fds);
    write(fds[1], both, (size_t)(t1 + t2));
    close(fds[1]);
    free(both);

    char *out1 = NULL, *out2 = NULL;
    ssize_t n1 = frame_recv(fds[0], &out1);
    ssize_t n2 = frame_recv(fds[0], &out2);
    close(fds[0]);

    ASSERT(n1 > 0); ASSERT_STR(out1, p1);
    ASSERT(n2 > 0); ASSERT_STR(out2, p2);
    free(out1); free(out2);
}

static void test_empty_payload(void) {
    TEST("empty_payload");
    uint8_t *frame;
    ssize_t total = frame_encode("", 0, &frame);
    ASSERT_EQ(total, 8);
    int rd = pipe_with_data(frame, (size_t)total);
    free(frame);
    char *out = NULL;
    ssize_t n = frame_recv(rd, &out);
    close(rd);
    ASSERT_EQ(n, 0); /* 0-length payload = EOF-like but actually 0 bytes read */
    free(out);
}

int main(void) {
    test_encode_decode_roundtrip();
    test_magic_bytes();
    test_bad_magic();
    test_oversized_frame();
    test_truncated_frame();
    test_multiple_frames();
    test_empty_payload();
    TEST_SUMMARY();
}
