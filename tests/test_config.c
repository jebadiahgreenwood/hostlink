#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testlib.h"
#include "../src/common/config.h"

static void test_daemon_valid(void) {
    TEST("daemon_config_valid");
    daemon_config_t cfg;
    int rc = daemon_config_load("tests/fixtures/valid.conf", &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(cfg.node_name, "testnode");
    ASSERT_STR(cfg.auth_token, "abc123");
    ASSERT_EQ(cfg.unix_enabled, 1);
    ASSERT_EQ(cfg.tcp_port, 9876);
    ASSERT_EQ(cfg.max_concurrent, 4);
}

static void test_daemon_missing_token(void) {
    TEST("daemon_config_missing_token");
    daemon_config_t cfg;
    int rc = daemon_config_load("tests/fixtures/no_token.conf", &cfg);
    ASSERT_EQ(rc, -1);
}

static void test_daemon_missing_file(void) {
    TEST("daemon_config_missing_file");
    daemon_config_t cfg;
    int rc = daemon_config_load("/nonexistent/path.conf", &cfg);
    ASSERT_EQ(rc, -1);
}

static void test_daemon_empty_file(void) {
    TEST("daemon_config_empty_file");
    daemon_config_t cfg;
    /* Empty file has no auth_token, should fail */
    int rc = daemon_config_load("tests/fixtures/empty.conf", &cfg);
    ASSERT_EQ(rc, -1);
}

static void test_daemon_comments(void) {
    TEST("daemon_config_comments");
    daemon_config_t cfg;
    int rc = daemon_config_load("tests/fixtures/with_comments.conf", &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_STR(cfg.node_name, "commented");
    ASSERT_STR(cfg.auth_token, "tok999");
}

static void test_targets_valid(void) {
    TEST("targets_valid");
    target_entry_t *t = targets_load("tests/fixtures/targets.conf");
    ASSERT_NOTNULL(t);
    target_entry_t *desktop = targets_find(t, "desktop");
    ASSERT_NOTNULL(desktop);
    ASSERT_STR(desktop->transport, "unix");
    ASSERT_STR(desktop->socket, "/run/hostlink/hostlink.sock");
    target_entry_t *dgx = targets_find(t, "dgx-spark");
    ASSERT_NOTNULL(dgx);
    ASSERT_STR(dgx->transport, "tcp");
    ASSERT_EQ(dgx->port, 9876);
    targets_free(t);
}

static void test_targets_missing_file(void) {
    TEST("targets_missing_file");
    target_entry_t *t = targets_load("/nonexistent/targets.conf");
    ASSERT_NULL(t);
}

static void test_util_ct_strcmp(void) {
    TEST("ct_strcmp");
    /* Need to include util.h manually */
    extern int ct_strcmp(const char *, const char *);
    ASSERT_EQ(ct_strcmp("abc", "abc"), 0);
    ASSERT(ct_strcmp("abc", "xyz") != 0);
    ASSERT(ct_strcmp("abc", "ab") != 0);
    ASSERT(ct_strcmp("", "") == 0);
}

static void test_util_parse_size(void) {
    TEST("parse_size");
    extern long long parse_size(const char *);
    ASSERT_EQ(parse_size("1024"), 1024LL);
    ASSERT_EQ(parse_size("4K"),   4096LL);
    ASSERT_EQ(parse_size("1M"),   1048576LL);
    ASSERT_EQ(parse_size("1G"),   1073741824LL);
    ASSERT_EQ(parse_size("bad"),  -1LL);
    ASSERT_EQ(parse_size(""),     -1LL);
}

int main(void) {
    test_daemon_valid();
    test_daemon_missing_token();
    test_daemon_missing_file();
    test_daemon_empty_file();
    test_daemon_comments();
    test_targets_valid();
    test_targets_missing_file();
    test_util_ct_strcmp();
    test_util_parse_size();
    TEST_SUMMARY();
}
