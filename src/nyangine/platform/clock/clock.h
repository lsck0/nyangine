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

NYA_API u64 nya_clock_get_timestamp_s(void);
NYA_API u64 nya_clock_get_timestamp_ms(void);
NYA_API u64 nya_clock_get_timestamp_µs(void);
NYA_API u64 nya_clock_get_timestamp_ns(void);
