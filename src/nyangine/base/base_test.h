/**
 * @file base_test.h
 * */
#pragma once

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_logging.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Runs `code` with crash prevention armed and asserts that it did crash.
 *
 * ```c
 * nya_expect_crash(a_function_that_panics());
 *
 * nya_expect_crash({
 *   // code that is expected to crash
 * });
 * nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_ASSERT);
 * ```
 * */
#define nya_expect_crash(code)                                                                                                                       \
    do {                                                                                                                                             \
        jmp_buf  _nya_crash_jmp;                                                                                                                     \
        jmp_buf* _nya_crash_previous = _nya_crash_prevent_push(&_nya_crash_jmp);                                                                     \
        if (setjmp(_nya_crash_jmp) == 0) { code; }                                                                                                   \
        _nya_crash_prevent_pop(_nya_crash_previous);                                                                                                 \
        nya_assert_always(nya_crash_caught() != nullptr, "Expected a crash, but none happened.");                                                    \
    } while (0)

/**
 * The most recently prevented crash on this thread, or nullptr if none has been caught. Cleared
 * when `nya_expect_crash` arms a new frame.
 * */
NYA_API const NYA_CrashInfo* nya_crash_caught(void);

/*
 * ─────────────────────────────────────────────────────────
 * SOFT ASSERTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Records a failure and carries on, where `nya_assert` would stop the process.
 *
 * ```c
 * nya_check(got == want, "row %u: got %f, want %f", row, got, want);
 * ...
 * return nya_check_failures() == 0 ? 0 : 1;
 * ```
 * */
#define nya_check(condition, ...)                                                                                                                    \
    do {                                                                                                                                             \
        if (!(condition)) {                                                                                                                          \
            /* Capped, so a systematically broken invariant reports its shape rather than scrolling  \
             * thousands of identical lines past whatever came before it. The count stays exact.  */ \
            if (_nya_check_failure_count < NYA_CHECK_REPORT_MAX) {                                                                                   \
                printf("  FAIL: ");                                                                                                                  \
                printf(__VA_ARGS__);                                                                                                                 \
                printf("\n");                                                                                                                        \
            }                                                                                                                                        \
            _nya_check_failure_count++;                                                                                                              \
        }                                                                                                                                            \
    } while (0)

/** How many `nya_check` calls have failed. Zero means the test passed. */
#define nya_check_failures() (_nya_check_failure_count)

/** Failures printed in full before the rest are only counted. */
#define NYA_CHECK_REPORT_MAX 20

__attr_allow_unused NYA_INTERNAL u32 _nya_check_failure_count = 0;

#endif // NYA_TESTING
