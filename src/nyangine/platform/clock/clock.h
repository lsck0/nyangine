#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_time_s_to_ms(seconds)       ((u64)(seconds) * 1'000ULL)
#define nya_time_s_to_µs(seconds)       ((u64)(seconds) * 1'000'000ULL)
#define nya_time_s_to_ns(seconds)       ((u64)(seconds) * 1'000'000'000ULL)
#define nya_time_ms_to_s(milliseconds)  ((f64)(milliseconds) / 1'000.0F)
#define nya_time_ms_to_µs(milliseconds) ((u64)(milliseconds) * 1'000ULL)
#define nya_time_ms_to_ns(milliseconds) ((u64)(milliseconds) * 1'000'000ULL)
#define nya_time_µs_to_s(microseconds)  ((f64)(microseconds) / 1'000'000.0F)
#define nya_time_µs_to_ms(microseconds) ((f64)(microseconds) / 1'000.0F)
#define nya_time_µs_to_ns(microseconds) ((u64)(microseconds) * 1'000ULL)
#define nya_time_ns_to_s(nanoseconds)   ((f64)(nanoseconds) / 1'000'000'000.0F)
#define nya_time_ns_to_ms(nanoseconds)  ((f64)(nanoseconds) / 1'000'000.0F)
#define nya_time_ns_to_µs(nanoseconds)  ((f64)(nanoseconds) / 1'000.0F)

/*
 * ─────────────────────────────────────────────────────────
 * WALL CLOCK
 * ─────────────────────────────────────────────────────────
 */

/**
 * Time since the Unix epoch. Follows the system clock, so it can jump in either direction.
 *
 * For anything that has to agree with the outside world: a UUIDv7's embedded timestamp, an event's
 * timestamp, a comparison against a file's modification time, an RNG seed.
 *
 * **Never subtract two of these to measure how long something took.** The system clock is adjusted
 * by NTP, by suspend and resume, and by the user; a backward step makes the later reading smaller
 * than the earlier one, and since these are u64 the subtraction wraps to something near 2^64 rather
 * than going negative. The engine builds with -fsanitize=unsigned-integer-overflow and
 * -fno-sanitize-recover=all, so that aborts. Use the monotonic clock below.
 * */
NYA_API u64 nya_clock_get_timestamp_s(void);
NYA_API u64 nya_clock_get_timestamp_ms(void);
NYA_API u64 nya_clock_get_timestamp_µs(void);
NYA_API u64 nya_clock_get_timestamp_ns(void);

/*
 * ─────────────────────────────────────────────────────────
 * MONOTONIC
 * ─────────────────────────────────────────────────────────
 */

/**
 * Time since an unspecified fixed point, guaranteed never to go backwards.
 *
 * The epoch is arbitrary and differs between platforms and between runs, so a single reading means
 * nothing on its own. The difference between two of them is the elapsed time, and that is the only
 * thing these are for.
 *
 * This split exists because every duration in the engine — frame timing, uptime, the profiler, the
 * NEAT evolution budget, subprocess run time — was measured by subtracting two wall clock readings.
 * That is correct until the system clock moves, and then it is a wrapped u64 that aborts a sanitized
 * build. core_asset.c's hot reload throttle even documented itself as immune to clock adjustment
 * while reading a value derived from CLOCK_REALTIME; it is now actually immune.
 *
 * CLOCK_MONOTONIC on POSIX, QueryPerformanceCounter on Windows. Neither counts time the machine
 * spent suspended, which is what a frame timer wants anyway.
 * */
NYA_API u64 nya_clock_get_monotonic_ms(void);
NYA_API u64 nya_clock_get_monotonic_µs(void);
NYA_API u64 nya_clock_get_monotonic_ns(void);
