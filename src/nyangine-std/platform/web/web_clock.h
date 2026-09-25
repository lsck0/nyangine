/**
 * @file web_clock.h
 *
 * The browser's two clocks, in milliseconds, for the client-side (CSR) path.
 *
 * ```c
 * u64 started_ms = nya_web_clock_monotonic_ms();
 * frame();
 * u64 took_ms = nya_web_clock_monotonic_ms() - started_ms;
 * ```
 *
 * This is the web sibling of os/os_time.h. A wasm module has no operating system clock of its own; the
 * two numbers it can read are the page's `performance.now()` (a monotonic count of milliseconds since
 * the document loaded, never running backward) and `Date.now()` (wall-clock milliseconds since the Unix
 * epoch, which follows the user's system clock and can jump). Both come back through emscripten's JS
 * bridge rather than a system call.
 *
 * Milliseconds, not the nanoseconds os_time answers in: the browser's own clocks are millisecond
 * quantities (`performance.now`'s sub-millisecond fraction is clamped by every engine for timing-attack
 * reasons), so there is no finer number to report and rounding to nanoseconds would only invent digits.
 *
 * Off wasm this is not a no-op: it answers from os/os_time so the same call compiles and runs in the
 * native tree, which is what a test and a shared timing path need. There it is `nya_os_time`'s
 * nanoseconds divided down to milliseconds.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Milliseconds from an arbitrary zero, counting up and never back — the browser's `performance.now()`,
 * floored to a whole millisecond. The zero is the moment the page began loading, so only differences
 * mean anything, exactly as with os/os_time's monotonic clock. Off wasm, `nya_os_time_monotonic_ns`
 * divided to milliseconds.
 * */
NYA_API u64 nya_web_clock_monotonic_ms(void) __attr_no_discard;

/**
 * Milliseconds since the Unix epoch, from the browser's `Date.now()`. Follows the user's system clock,
 * so it can jump in either direction and a duration measured with it can come out negative — the wall
 * clock's standing caveat. Off wasm, `nya_os_time_wall_ns` divided to milliseconds.
 * */
NYA_API u64 nya_web_clock_wall_ms(void) __attr_no_discard;
