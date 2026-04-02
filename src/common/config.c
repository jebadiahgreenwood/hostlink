#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "config.h"
#include "log.h"

/* Strip leading/trailing whitespace in-place */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* Generic INI parser: calls cb(section, key, value, userdata) for each kv pair */
typedef void (*ini_cb_t)(const char *section, const char *key,
                          const char *value, void *userdata);

static int ini_parse(const char *path, ini_cb_t cb, void *userdata) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("config: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    char line[1024];
    char section[64] = "";
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) { log_warn("config:%d: malformed section", lineno); continue; }
            *end = '\0';
            snprintf(section, sizeof(section), "%s", p + 1);
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) { log_warn("config:%d: no '=' found", lineno); continue; }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        cb(section, key, val, userdata);
    }
    fclose(f);
    return 0;
}

/* ---- Daemon config ---- */
void daemon_config_defaults(daemon_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->node_name, sizeof(cfg->node_name), "%s", "hostlink");
    snprintf(cfg->unix_path, sizeof(cfg->unix_path), "%s", "/run/hostlink/hostlink.sock");
    cfg->unix_mode     = 0660;
    snprintf(cfg->unix_group, sizeof(cfg->unix_group), "%s", "hostlink");
    cfg->unix_enabled  = 1;
    cfg->tcp_enabled   = 0;
    snprintf(cfg->tcp_bind, sizeof(cfg->tcp_bind), "%s", "127.0.0.1");
    cfg->tcp_port      = 9876;
    cfg->max_concurrent = 8;
    cfg->default_timeout_ms = 30000;
    cfg->max_timeout_ms     = 300000;
    snprintf(cfg->shell, sizeof(cfg->shell), "%s", "/bin/sh");
    cfg->default_max_output_bytes = 4194304LL;
    cfg->max_output_bytes         = 67108864LL;
    snprintf(cfg->output_tmpdir, sizeof(cfg->output_tmpdir), "%s", "/run/hostlink/output");
    snprintf(cfg->log_target, sizeof(cfg->log_target), "%s", "stderr");
    snprintf(cfg->log_level, sizeof(cfg->log_level), "%s", "info");
}

static void daemon_cfg_cb(const char *section, const char *key,
                           const char *value, void *userdata) {
    (void)section;
    daemon_config_t *cfg = (daemon_config_t *)userdata;
#define STR(field) if (!strcmp(key, #field)) { snprintf(cfg->field, sizeof(cfg->field), "%s", value); return; }
#define INT(field) if (!strcmp(key, #field)) { cfg->field = atoi(value); return; }
    STR(node_name) STR(auth_token) STR(unix_path) STR(unix_group)
    STR(tcp_bind) STR(shell) STR(output_tmpdir) STR(log_target) STR(log_level)
    STR(run_as_user)
    INT(unix_enabled) INT(tcp_enabled) INT(tcp_port)
    INT(max_concurrent) INT(default_timeout_ms) INT(max_timeout_ms)
    if (!strcmp(key, "unix_mode")) {
        cfg->unix_mode = (int)strtol(value, NULL, 8); return;
    }
    if (!strcmp(key, "default_max_output_bytes")) {
        cfg->default_max_output_bytes = atoll(value); return;
    }
    if (!strcmp(key, "max_output_bytes")) {
        cfg->max_output_bytes = atoll(value); return;
    }
#undef STR
#undef INT
}

int daemon_config_load(const char *path, daemon_config_t *cfg) {
    daemon_config_defaults(cfg);
    if (ini_parse(path, daemon_cfg_cb, cfg) != 0) return -1;
    if (cfg->auth_token[0] == '\0') {
        log_error("config: auth_token is required");
        return -1;
    }
    return 0;
}

/* ---- Targets config ---- */
static void targets_cb(const char *section, const char *key,
                        const char *value, void *userdata) {
    target_entry_t **head_ptr = (target_entry_t **)userdata;
    if (!section || section[0] == '\0') return;

    /* Find or create entry for this section */
    target_entry_t *e = *head_ptr;
    while (e) {
        if (!strcmp(e->name, section)) break;
        e = e->next;
    }
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (!e) return;
        snprintf(e->name, sizeof(e->name), "%s", section);
        /* default port */
        e->port = 9876;
        /* prepend */
        e->next = *head_ptr;
        *head_ptr = e;
    }

    if (!strcmp(key, "transport")) snprintf(e->transport, sizeof(e->transport), "%s", value);
    else if (!strcmp(key, "socket"))  snprintf(e->socket,    sizeof(e->socket),    "%s", value);
    else if (!strcmp(key, "address")) snprintf(e->address,   sizeof(e->address),   "%s", value);
    else if (!strcmp(key, "port"))    e->port = atoi(value);
    else if (!strcmp(key, "token"))   snprintf(e->token,     sizeof(e->token),     "%s", value);
}

/* Reverse a linked list */
static target_entry_t *list_reverse(target_entry_t *head) {
    target_entry_t *prev = NULL, *cur = head, *next;
    while (cur) { next = cur->next; cur->next = prev; prev = cur; cur = next; }
    return prev;
}

target_entry_t *targets_load(const char *path) {
    target_entry_t *head = NULL;
    if (ini_parse(path, targets_cb, &head) != 0) {
        targets_free(head);
        return NULL;
    }
    return list_reverse(head);
}

target_entry_t *targets_find(target_entry_t *head, const char *name) {
    while (head) {
        if (!strcmp(head->name, name)) return head;
        head = head->next;
    }
    return NULL;
}

void targets_free(target_entry_t *head) {
    while (head) {
        target_entry_t *next = head->next;
        free(head);
        head = next;
    }
}
