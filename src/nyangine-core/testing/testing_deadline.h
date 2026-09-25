/**
 * @file testing_deadline.h
 *
 * A wall clock deadline for a test process. A test whose work is bounded (so many episodes, so many
 * ticks) can still hang on a lock or a loop that never ends, and a hung test does not fail: it stops
 * the suite, and outlives whatever killed the runner above it. This makes it fail, loudly, on its own.
 *
 * Overview:
 *   nya_test_deadline_start     arms a watchdog thread that fails the process after `limit_s`
 *   nya_test_deadline_stop      disarms it; the work finished in time
 *
 * ```c
 * nya_test_deadline_start("test_agent", 60);
 * defer nya_test_deadline_stop();
 * ```
 *
 * Past the limit it names the test and the limit on stderr. On Linux it then sends SIGABRT to the
 * thread that armed it, so the crash path prints the backtrace of the thread that is stuck rather than
 * of the watchdog. Elsewhere, or if that does not end the process, it exits with a failure.
 *
 * Rejected: a timeout in the test runner. It would cover every test, but it kills from outside, so it
 * says nothing about where the test was, and a test orphaned by a killed runner would still run on.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Arms the deadline. `name` is what the failure line calls the test. One deadline at a time, armed and
 * stopped from the same thread.
 * */
NYA_API void nya_test_deadline_start(NYA_ConstCString name, u32 limit_s);

/** Disarms the deadline and joins the watchdog. Idempotent, and a no-op when nothing is armed. */
NYA_API void nya_test_deadline_stop(void);

#endif // NYA_TESTING
