#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// FUNCTIONS AND MACROS

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

// WALL CLOCK

/**
 * Time since the Unix epoch. Follows the system clock, so it can jump in either direction.
 * */
NYA_API u64 nya_clock_get_timestamp_s(void);
NYA_API u64 nya_clock_get_timestamp_ms(void);
NYA_API u64 nya_clock_get_timestamp_µs(void);
NYA_API u64 nya_clock_get_timestamp_ns(void);

// MONOTONIC

/**
 * Time since an unspecified fixed point, guaranteed never to go backwards.
 * */
NYA_API u64 nya_clock_get_monotonic_ms(void);
NYA_API u64 nya_clock_get_monotonic_µs(void);
NYA_API u64 nya_clock_get_monotonic_ns(void);

// CIVIL TIME

#define NYA_CLOCK_SECONDS_PER_DAY 86'400ULL

/** Longest string nya_clock_format_utc produces, terminator included. */
#define NYA_CLOCK_FORMAT_MAX_LENGTH 32

/**
 * How nya_clock_format_utc spells a moment.
 * */
typedef enum {
    /** `2026-09-20 14:03:11 UTC`, for a person to read. */
    NYA_CLOCK_FORMAT_READABLE,

    /** `2026-09-20-140311`, safe as a path component on every target. */
    NYA_CLOCK_FORMAT_FILENAME,

    NYA_CLOCK_FORMAT_COUNT,
} NYA_ClockFormat;

/**
 * The proleptic Gregorian date a day count since the Unix epoch falls on, and back again. The pair is
 * exact for every day either can represent, which a round trip property test relies on.
 * */
NYA_API void nya_clock_civil_from_days(s64 days, OUT s32* out_year, OUT u32* out_month, OUT u32* out_day);
NYA_API s64  nya_clock_days_from_civil(s32 year, u32 month, u32 day) __attr_no_discard;

/**
 * Writes `timestamp_s` into `buffer` as UTC, null terminated, and returns the bytes written excluding
 * the terminator. Touches no allocator, no stdio and no timezone database, so the crash path can call
 * it from a signal handler.
 * */
NYA_API u32 nya_clock_format_utc(u64 timestamp_s, NYA_ClockFormat format, OUT u8* buffer, u32 capacity);
