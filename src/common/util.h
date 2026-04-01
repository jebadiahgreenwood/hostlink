#ifndef HOSTLINK_UTIL_H
#define HOSTLINK_UTIL_H

#include <stddef.h>

/* Constant-time string comparison (prevents timing attacks on tokens) */
int ct_strcmp(const char *a, const char *b);

/* Parse a size string like "4M", "512K", "1G" into bytes.
   Returns -1 on parse error. */
long long parse_size(const char *s);

/* Safe strdup that aborts on OOM */
char *hl_strdup(const char *s);

#endif /* HOSTLINK_UTIL_H */
