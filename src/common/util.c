#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "util.h"

int ct_strcmp(const char *a, const char *b) {
    if (!a || !b) return 1;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    /* Compare lengths without short-circuiting */
    volatile int diff = 0;
    /* Use the longer length so both are fully scanned */
    size_t maxlen = la > lb ? la : lb;
    for (size_t i = 0; i < maxlen; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (ca ^ cb);
    }
    return diff != 0;
}

long long parse_size(const char *s) {
    if (!s || !*s) return -1;
    char *end;
    long long val = strtoll(s, &end, 10);
    if (val < 0) return -1;
    if (*end == '\0') return val;
    char suffix = (char)toupper((unsigned char)*end);
    if (*(end + 1) != '\0') return -1; /* trailing garbage */
    switch (suffix) {
        case 'K': return val * 1024LL;
        case 'M': return val * 1024LL * 1024LL;
        case 'G': return val * 1024LL * 1024LL * 1024LL;
        default:  return -1;
    }
}

char *hl_strdup(const char *s) {
    if (!s) return NULL;
    char *d = strdup(s);
    if (!d) { fprintf(stderr, "hl_strdup: out of memory\n"); abort(); }
    return d;
}

/* ── Base64 (RFC 4648, standard alphabet, no line wrapping) ─────────── */

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Decode table: -1 = invalid, -2 = padding '=' */
static const signed char B64_DEC[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x00-0x0f */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x10-0x1f */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, /* 0x20-0x2f */
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1, /* 0x30-0x3f */
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* 0x40-0x4f */
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, /* 0x50-0x5f */
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* 0x60-0x6f */
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, /* 0x70-0x7f */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x80-0x8f */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

char *hl_b64_encode(const unsigned char *src, size_t src_len) {
    size_t out_len = ((src_len + 2) / 3) * 4 + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i + 2 < src_len) {
        unsigned int v = ((unsigned int)src[i] << 16)
                       | ((unsigned int)src[i+1] << 8)
                       | (unsigned int)src[i+2];
        out[j++] = B64_ENC[(v >> 18) & 0x3f];
        out[j++] = B64_ENC[(v >> 12) & 0x3f];
        out[j++] = B64_ENC[(v >>  6) & 0x3f];
        out[j++] = B64_ENC[(v      ) & 0x3f];
        i += 3;
    }
    if (i < src_len) {
        unsigned int v = (unsigned int)src[i] << 16;
        if (i + 1 < src_len) v |= (unsigned int)src[i+1] << 8;
        out[j++] = B64_ENC[(v >> 18) & 0x3f];
        out[j++] = B64_ENC[(v >> 12) & 0x3f];
        out[j++] = (i + 1 < src_len) ? B64_ENC[(v >> 6) & 0x3f] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

size_t hl_b64_decoded_len(const char *b64, size_t b64_len) {
    if (b64_len == 0) return 0;
    size_t padding = 0;
    if (b64_len >= 1 && b64[b64_len-1] == '=') padding++;
    if (b64_len >= 2 && b64[b64_len-2] == '=') padding++;
    return (b64_len / 4) * 3 - padding;
}

ssize_t hl_b64_decode(const char *src, size_t src_len,
                      unsigned char *out_buf, size_t out_cap) {
    if (src_len % 4 != 0) return -1;
    size_t out_len = hl_b64_decoded_len(src, src_len);
    if (out_len > out_cap) return -1;

    size_t i = 0, j = 0;
    while (i < src_len) {
        signed char a = B64_DEC[(unsigned char)src[i]];
        signed char b = B64_DEC[(unsigned char)src[i+1]];
        signed char c = B64_DEC[(unsigned char)src[i+2]];
        signed char d = B64_DEC[(unsigned char)src[i+3]];

        if (a < 0 || b < 0) return -1;
        out_buf[j++] = (unsigned char)((a << 2) | (b >> 4));

        if (c == -2) { /* padding */
            if (d != -2) return -1;
            break;
        }
        if (c < 0) return -1;
        out_buf[j++] = (unsigned char)((b << 4) | (c >> 2));

        if (d == -2) break;
        if (d < 0) return -1;
        out_buf[j++] = (unsigned char)((c << 6) | d);

        i += 4;
    }
    return (ssize_t)j;
}
