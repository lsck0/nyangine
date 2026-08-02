/**
 * @file base_test.h
 *
 * Crash prevention for tests, and nothing else.
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

#endif // NYA_TESTING
