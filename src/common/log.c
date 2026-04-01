#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include "log.h"

static log_target_t g_target    = LOG_TARGET_STDERR;
static log_level_t  g_min_level = HL_LOG_INFO;
static FILE        *g_file      = NULL;

static const char *level_str(log_level_t l) {
    switch (l) {
        case HL_LOG_DEBUG: return "DEBUG";
        case HL_LOG_INFO:  return "INFO";
        case HL_LOG_WARN:  return "WARN";
        case HL_LOG_ERROR: return "ERROR";
        default:           return "?";
    }
}

static int level_to_syslog_prio(log_level_t l) {
    switch (l) {
        case HL_LOG_DEBUG: return LOG_DEBUG;
        case HL_LOG_INFO:  return LOG_INFO;
        case HL_LOG_WARN:  return LOG_WARNING;
        case HL_LOG_ERROR: return LOG_ERR;
        default:           return LOG_INFO;
    }
}

void log_init(log_target_t target, log_level_t min_level, const char *file_path) {
    g_target    = target;
    g_min_level = min_level;
    if (target == LOG_TARGET_SYSLOG) {
        openlog("hostlinkd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    } else if (target == LOG_TARGET_FILE && file_path) {
        g_file = fopen(file_path, "a");
        if (!g_file) {
            fprintf(stderr, "log_init: cannot open log file %s\n", file_path);
            g_target = LOG_TARGET_STDERR;
        }
    }
}

void log_close(void) {
    if (g_target == LOG_TARGET_SYSLOG) closelog();
    if (g_file) { fclose(g_file); g_file = NULL; }
}

void log_set_level(log_level_t level) {
    g_min_level = level;
}

void log_msg(log_level_t level, const char *fmt, ...) {
    if (level < g_min_level) return;
    va_list ap;
    va_start(ap, fmt);
    if (g_target == LOG_TARGET_SYSLOG) {
        /* vsyslog is available with _GNU_SOURCE */
        vsyslog(level_to_syslog_prio(level), fmt, ap);
    } else {
        FILE *out = g_file ? g_file : stderr;
        time_t now = time(NULL);
        struct tm tm_buf;
        gmtime_r(&now, &tm_buf);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        fprintf(out, "[%s] [%s] ", ts, level_str(level));
        vfprintf(out, fmt, ap);
        fprintf(out, "\n");
        fflush(out);
    }
    va_end(ap);
}
