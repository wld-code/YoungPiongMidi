/**
 * @file test_common.h
 * @brief Minimal, dependency-free assert-based test framework for the
 *        host-side DSP/conversion tests in this directory.
 *
 * Deliberately not a full framework (no Unity, no CMock): the code under
 * test here is a handful of pure-C functions with no ESP-IDF dependency,
 * and pulling in a test framework's own build system for that would be
 * more machinery than the problem needs (project spec: "do not generate
 * huge abstraction layers without a concrete need"). Each test_*.c file
 * is its own standalone executable - see test/Makefile.
 *
 * Usage: call TEST_CHECK_* macros from any function, then TEST_MAIN_END()
 * once at the end of main() to print a summary and set the process exit
 * code (0 = all passed, 1 = at least one failure - this is what makes
 * `make test` fail loudly in CI or on the command line).
 */
#pragma once

#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_test_checks = 0;
static int g_test_failures = 0;

#define TEST_CHECK_TRUE(cond, msg) \
    do { \
        g_test_checks++; \
        if (!(cond)) { \
            g_test_failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        } \
    } while (0)

#define TEST_CHECK_NEAR(actual, expected, tol, msg) \
    do { \
        g_test_checks++; \
        double _a = (double)(actual), _e = (double)(expected), _t = (double)(tol); \
        if (fabs(_a - _e) > _t) { \
            g_test_failures++; \
            printf("  FAIL %s:%d: %s (got %g, expected %g +/- %g)\n", \
                   __FILE__, __LINE__, msg, _a, _e, _t); \
        } \
    } while (0)

#define TEST_CHECK_EQ_INT(actual, expected, msg) \
    do { \
        g_test_checks++; \
        long _a = (long)(actual), _e = (long)(expected); \
        if (_a != _e) { \
            g_test_failures++; \
            printf("  FAIL %s:%d: %s (got %ld, expected %ld)\n", \
                   __FILE__, __LINE__, msg, _a, _e); \
        } \
    } while (0)

#define TEST_CHECK_STR_EQ(actual, expected, msg) \
    do { \
        g_test_checks++; \
        if (strcmp((actual), (expected)) != 0) { \
            g_test_failures++; \
            printf("  FAIL %s:%d: %s (got \"%s\", expected \"%s\")\n", \
                   __FILE__, __LINE__, msg, (actual), (expected)); \
        } \
    } while (0)

/** Call once, right before running each named test function's body -
 *  purely cosmetic (progress output), not required for pass/fail. */
#define TEST_CASE(name) printf("- %s\n", name)

/** Call once at the end of main(). Prints a summary and returns the
 *  process exit code to use (0 pass, 1 fail) - `return TEST_MAIN_END();`. */
static inline int test_main_end(void)
{
    printf("\n%d/%d checks passed", g_test_checks - g_test_failures, g_test_checks);
    if (g_test_failures > 0) {
        printf(" - %d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("\n");
    return 0;
}
#define TEST_MAIN_END() test_main_end()
