#ifndef HOSTLINK_TESTLIB_H
#define HOSTLINK_TESTLIB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int  _test_pass = 0;
static int  _test_fail = 0;
static const char *_current_test = "(none)";

#define TEST(name) do { _current_test = (name); } while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s]: assertion failed: %s (line %d)\n", \
                _current_test, #cond, __LINE__); \
        _test_fail++; \
    } else { \
        _test_pass++; \
    } \
} while(0)

#define ASSERT_EQ(a, b)  ASSERT((a) == (b))
#define ASSERT_STR(a, b) ASSERT(strcmp((a), (b)) == 0)
#define ASSERT_NULL(p)   ASSERT((p) == NULL)
#define ASSERT_NOTNULL(p) ASSERT((p) != NULL)

#define TEST_SUMMARY() do { \
    printf("Results: %d passed, %d failed\n", _test_pass, _test_fail); \
    return _test_fail > 0 ? 1 : 0; \
} while(0)

#endif /* HOSTLINK_TESTLIB_H */
