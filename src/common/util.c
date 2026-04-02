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
