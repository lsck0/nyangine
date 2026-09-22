/**
 * @file os_time.h
 *
 * The two clocks the operating system has, in nanoseconds and nothing else.
 *
 * ```c
 * u64 started_ns = nya_os_time_monotonic_ns();
 * work();
 * u64 took_ns = nya_os_time_monotonic_ns() - started_ns;
 * ```
 *
 * Everything else about time — milliseconds, civil dates, RFC 9110 stamps, NYA_Instant — is
 * arithmetic over these two numbers and lives in platform/clock. Here there is one system call per
 * function and no conversion, so a duration that has to be exact is not rounded twice.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Nanoseconds since the Unix epoch, from the system clock. Follows it, so it can jump in either
 * direction when the clock is set or a leap second is smeared: a duration measured with this can come
 * out negative. `CLOCK_REALTIME` on Linux, GetSystemTimePreciseAsFileTime on Windows.
 * */
NYA_API u64 nya_os_time_wall_ns(void) __attr_no_discard;

/**
 * Nanoseconds from an arbitrary zero, counting up and never back. The zero differs per boot and per
 * platform, so only differences mean anything. `CLOCK_MONOTONIC` on Linux, QueryPerformanceCounter on
 * Windows; neither counts time the machine spent suspended, which is time the program did not run.
 * */
NYA_API u64 nya_os_time_monotonic_ns(void) __attr_no_discard;
