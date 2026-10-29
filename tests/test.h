/* SPDX-License-Identifier: ISC */
/* Minimal host test harness: no deps beyond stdio/string. */
#ifndef QCA6174_TEST_H
#define QCA6174_TEST_H

#include <stdio.h>
#include <string.h>

/* Counters live in the runner TU (define with TEST_GLOBALS there). */
extern int g_tests_run;
extern int g_tests_failed;
extern int g_checks;

#define TEST_GLOBALS int g_tests_run = 0; int g_tests_failed = 0; int g_checks = 0;

#define CHECK(cond) do {                                               \
    g_checks++;                                                        \
    if (!(cond)) {                                                     \
        g_tests_failed++;                                              \
        fprintf(stderr, "  FAIL %s:%d: CHECK(%s)\n",                   \
                __FILE__, __LINE__, #cond);                            \
    }                                                                  \
} while (0)

#define CHECK_EQ_U(a, b) do {                                          \
    g_checks++;                                                        \
    unsigned long long _a = (unsigned long long)(a);                   \
    unsigned long long _b = (unsigned long long)(b);                   \
    if (_a != _b) {                                                    \
        g_tests_failed++;                                              \
        fprintf(stderr, "  FAIL %s:%d: %s (%llu) != %s (%llu)\n",      \
                __FILE__, __LINE__, #a, _a, #b, _b);                   \
    }                                                                  \
} while (0)

#define CHECK_STR_EQ(a, alen, lit) do {                                \
    g_checks++;                                                        \
    size_t _n = strlen(lit);                                           \
    if ((size_t)(alen) != _n || memcmp((a), (lit), _n) != 0) {         \
        g_tests_failed++;                                              \
        fprintf(stderr, "  FAIL %s:%d: string != \"%s\"\n",            \
                __FILE__, __LINE__, lit);                              \
    }                                                                  \
} while (0)

#define RUN(fn) do {                                                   \
    int before = g_tests_failed;                                       \
    g_tests_run++;                                                     \
    printf("[ RUN  ] %s\n", #fn);                                      \
    fn();                                                              \
    if (g_tests_failed == before)                                      \
        printf("[  OK  ] %s\n", #fn);                                  \
    else                                                               \
        printf("[ FAIL ] %s\n", #fn);                                  \
} while (0)

static inline int test_summary(void)
{
    printf("----\n%d test(s), %d check(s), %d failure(s)\n",
           g_tests_run, g_checks, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}

#endif /* QCA6174_TEST_H */
