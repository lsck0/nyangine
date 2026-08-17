/**
 * @file base_test.h
 *
 * What a test needs from the engine and a shipped build must not have: crash prevention, and a soft
 * assertion that counts rather than aborts.
 *
 * A test that deliberately trips an assertion, a panic or a thrown error needs the process to
 * survive so it can assert on what happened. `nya_expect_crash` arms exactly that, and
 * `nya_crash_caught` hands back the resulting NYA_CrashInfo.
 *
 * The whole facility is compiled in only when NYA_TESTING is defined, which is set by the test
 * build rule and by nothing else. In every other build the prevention branch does not exist in the
 * binary at all. That is deliberate: shipping code must never be able to arm this and carry on
 * running on state that is already known to be broken.
 *
 * Hardware faults are not preventable. See §2.4.2 of the design notes: siglongjmp out of a SIGSEGV
 * leaves the faulting instruction's state undefined and strands any lock the faulting thread held.
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
 * Nestable: the previously armed frame is saved and restored, so a helper with its own
 * `nya_expect_crash` no longer clobbers an enclosing one.
 *
 * Usage:
 *
 * ```c
 * nya_expect_crash(a_function_that_panics());
 *
 * nya_expect_crash({
 *   // code that is expected to crash
 * });
 * nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_ASSERT);
 * ```
 *
 * Locals written inside `code` and read afterwards must be declared `volatile`, otherwise their
 * values are indeterminate after the longjmp.
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
 * For the tests that check one invariant across thousands of cases — a gradient against every op, a
 * genome against every generation — where the first broken case is the least interesting fact
 * available. `nya_assert` stops there; this counts, prints, and lets the run finish so the shape of
 * the failure is visible.
 *
 * Usage:
 *
 * ```c
 * nya_check(got == want, "row %u: got %f, want %f", row, got, want);
 * ...
 * return nya_check_failures() == 0 ? 0 : 1;
 * ```
 *
 * Returned as a zero-or-one rather than as the count: an exit status is eight bits, so a test that
 * failed exactly 256 times would report success.
 *
 * A file scope counter rather than anything shared, which is exactly right here: every test is a
 * unity build of one translation unit, so each binary gets its own. Three tests had each grown a
 * private copy of this macro and its counter before it was worth having once.
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
