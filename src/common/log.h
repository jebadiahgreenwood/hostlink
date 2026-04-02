#ifndef HOSTLINK_LOG_H
#define HOSTLINK_LOG_H

/* Prefixed HL_ to avoid clashing with syslog.h LOG_DEBUG / LOG_INFO macros */
typedef enum {
    HL_LOG_DEBUG = 0,
    HL_LOG_INFO  = 1,
    HL_LOG_WARN  = 2,
    HL_LOG_ERROR = 3
} log_level_t;

typedef enum {
    LOG_TARGET_STDERR = 0,
    LOG_TARGET_SYSLOG = 1,
    LOG_TARGET_FILE   = 2
} log_target_t;

void log_init(log_target_t target, log_level_t min_level, const char *file_path);
void log_close(void);
void log_set_level(log_level_t level);
void log_msg(log_level_t level, const char *fmt, ...);

#define log_debug(...) log_msg(HL_LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(HL_LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_msg(HL_LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_msg(HL_LOG_ERROR, __VA_ARGS__)

#endif /* HOSTLINK_LOG_H */
