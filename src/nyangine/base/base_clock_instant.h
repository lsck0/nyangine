/**
 * @file clock_instant.h
 *
 * Moments, spans and calendar days as types of their own. A bare u64 from clock.h says nothing about
 * whether it counts seconds or nanoseconds, or whether it is a moment or the distance between two;
 * these do, and the compiler refuses to mix them.
 *
 * Overview:
 *   NYA_Instant                              a moment in UTC, nanoseconds since the Unix epoch
 *   NYA_Duration                             signed nanoseconds between two instants
 *   NYA_Date, NYA_TimeOfDay                  a proleptic Gregorian day, and a moment within one
 *
 *   nya_instant_now                          the wall clock, or whatever source is installed
 *   nya_instant_source_set / _source         where nya_instant_now reads; the simulation seam
 *   nya_instant_add_duration / _checked      instant + duration
 *   nya_instant_subtract_duration / _checked instant - duration
 *   nya_duration_between / _checked          instant - instant
 *   nya_duration_from_s / _from_ms           spans from coarser units
 *   nya_instant_to_utc / _from_utc           instant <-> (date, time of day)
 *
 *   nya_date_is_valid / _is_leap_year        predicates
 *   nya_date_days_in_month                   28 to 31
 *   nya_date_to_days / _from_days            days since 1970-01-01, the calendar's arithmetic
 *   nya_date_add_days / _checked             n days later, or earlier for negative n
 *   nya_date_add_months / _checked           n months later, the day clamped to the month's end
 *   nya_date_end_of_month                    the last day of the date's month
 *   nya_date_weekday / nya_date_iso_week     ISO 8601's weekday and week number
 *   nya_time_of_day_is_valid                 the predicate
 *   nya_time_of_day_to_duration              time since midnight
 *
 * ```c
 * NYA_Instant  now     = nya_instant_now();
 * NYA_Instant  expires = nya_instant_add_duration(now, nya_duration_from_s(15 * 60));
 *
 * NYA_Date      today = { 0 };
 * NYA_TimeOfDay clock = { 0 };
 * nya_instant_to_utc(now, &today, &clock);
 *
 * NYA_Date next_bill = nya_date_add_months(today, 1);   // 2026-01-31 becomes 2026-02-28
 * ```
 *
 * ## Why structs and not typedefs
 *
 * A C typedef is an alias: `typedef s64 NYA_Instant` accepts a duration, a count of seconds and a file
 * size without a word. A one-field struct is the only newtype C has, and it is free at runtime.
 *
 * ## Why signed nanoseconds
 *
 * Nanoseconds because the monotonic clock, the logs and the frame loop already count them, so nothing
 * converts on the way in. Signed because a moment before 1970 is an ordinary date of birth. The price is
 * the range: 1677-09-21 to 2262-04-11, which a parser reports as NYA_TIME_PARSE_OUT_OF_RANGE rather than
 * clamping. That is the case for a cookie that "never expires" in 9999; the caller decides what it means.
 *
 * Rejected: a (seconds, nanoseconds) pair. It reaches year 9999, but every comparison and subtraction
 * becomes two fields and a carry, for dates nothing in the engine stores.
 *
 * ## No leap seconds
 *
 * An instant counts the way POSIX time does: every day is 86 400 seconds. That is what every clock the
 * program can read reports, and what makes a date plus a time of day one multiplication. A leap second
 * written in text (`23:59:60`) is refused by the parsers; see clock_format.h.
 *
 * ## Overflow fails, it does not wrap or saturate
 *
 * The plain calls assert: an instant from the clock plus a span the program chose cannot leave a range
 * of five centuries, so leaving it is a bug. The `_checked` calls return false instead, for operands that
 * came from outside (a token's expiry plus a leeway, a date a user typed plus a count they typed), where
 * an assertion would be a denial of service. Saturating was rejected: it silently breaks
 * `a + d - d == a`, and a saturated expiry reads as "valid until 2262".
 *
 * ## Calendar arithmetic is on dates
 *
 * "One month later" has no meaning for an instant until someone says in which zone, and "one day later"
 * across a daylight saving change is 23 or 25 hours. So months, weeks and month ends are on NYA_Date,
 * and an instant only moves by a duration, which is exact.
 *
 * Rejected: gmtime_r and struct tm. They take the libc timezone lock, an int year, and a zero based
 * month, and they cannot be called from the crash path.
 *
 * Not here yet: time zones from the platform's database, locale aware display, and reflection knowing
 * these types. Everything here is UTC.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

#define NYA_NS_PER_SECOND 1'000'000'000LL
#define NYA_NS_PER_MINUTE (60LL * NYA_NS_PER_SECOND)
#define NYA_NS_PER_HOUR   (60LL * NYA_NS_PER_MINUTE)
#define NYA_NS_PER_DAY    (24LL * NYA_NS_PER_HOUR)

/**
 * The years a NYA_Date may hold. Four digits, because that is what RFC 3339 and RFC 9110 can write:
 * every valid date then has a text form, and a date that would need a fifth digit is refused by the
 * `_checked` arithmetic rather than printed wrong. Year zero exists, as 1 BC, in the proleptic calendar.
 * */
#define NYA_DATE_YEAR_MIN 0
#define NYA_DATE_YEAR_MAX 9'999

// TYPES

typedef struct NYA_Instant       NYA_Instant;
typedef struct NYA_Duration      NYA_Duration;
typedef struct NYA_Date          NYA_Date;
typedef struct NYA_TimeOfDay     NYA_TimeOfDay;
typedef struct NYA_IsoWeek       NYA_IsoWeek;
typedef struct NYA_InstantSource NYA_InstantSource;
typedef enum NYA_Weekday         NYA_Weekday;

/** A moment, in UTC. Compare `ns` directly; there is nothing else in it. */
struct NYA_Instant {
    s64 ns;
};

/** The signed distance between two instants. Never a moment of its own. */
struct NYA_Duration {
    s64 ns;
};

/** A day in the proleptic Gregorian calendar, with no zone. Month and day are one based. */
struct NYA_Date {
    s32 year;
    u8  month;
    u8  day;
};

/** A moment within a day. No 60th second; see the note at the top. */
struct NYA_TimeOfDay {
    u8  hour;
    u8  minute;
    u8  second;
    u32 nanosecond;
};

/** ISO 8601's week date. `year` differs from the calendar year for up to three days at either end. */
struct NYA_IsoWeek {
    s32 year;

    /** 1 to 53. */
    u8 week;
};

/** ISO 8601's numbering, Monday first. Add one for the ISO digit. */
enum NYA_Weekday {
    NYA_WEEKDAY_MONDAY,
    NYA_WEEKDAY_TUESDAY,
    NYA_WEEKDAY_WEDNESDAY,
    NYA_WEEKDAY_THURSDAY,
    NYA_WEEKDAY_FRIDAY,
    NYA_WEEKDAY_SATURDAY,
    NYA_WEEKDAY_SUNDAY,

    NYA_WEEKDAY_COUNT,
};

/**
 * Where nya_instant_now reads. Zeroed is the wall clock.
 *
 * The seam the testing harness installs a simulated clock through, so session expiry, token lifetimes
 * and retention sweeps see the same dates on every replay of a seed. Its own seam rather than
 * NYA_AppTimeSource's: that one is monotonic and lives in core, above this module, and a program with
 * no app loop (a server, a CLI tool) still reads the time.
 * */
struct NYA_InstantSource {
    /** Null means the wall clock. Must never be called before it is installed or after it is replaced. */
    NYA_Instant (*now)(void* context);

    /** Handed to `now`, never looked inside. */
    void* context;
};

// FUNCTIONS

// INSTANTS

/**
 * The current moment. The wall clock, so it can step in either direction when the system clock is set;
 * measure elapsed time with nya_clock_get_monotonic_ns, never with two of these.
 * */
NYA_API NYA_Instant nya_instant_now(void) __attr_no_discard;

/**
 * Replaces where nya_instant_now reads, and returns nothing to undo: read the old one with
 * nya_instant_source first and set it back after. A zeroed source is the wall clock.
 *
 * Not thread safe, like nya_app_time_source_set: install it before anything that reads the time runs.
 * */
NYA_API void              nya_instant_source_set(NYA_InstantSource source);
NYA_API NYA_InstantSource nya_instant_source(void) __attr_no_discard;

/** `instant` moved by `duration`. Asserts the result is an instant; see the note on overflow. */
NYA_API NYA_Instant nya_instant_add_duration(NYA_Instant instant, NYA_Duration duration) __attr_no_discard;
NYA_API NYA_Instant nya_instant_subtract_duration(NYA_Instant instant, NYA_Duration duration) __attr_no_discard;

/** The same, false and nothing written when the result would leave the range. For operands from outside. */
NYA_API b8 nya_instant_add_duration_checked(NYA_Instant instant, NYA_Duration duration, OUT NYA_Instant* out_instant) __attr_no_discard;
NYA_API b8 nya_instant_subtract_duration_checked(NYA_Instant instant, NYA_Duration duration, OUT NYA_Instant* out_instant) __attr_no_discard;

/**
 * `to - from`: positive when `to` is later. Two instants more than 292 years apart have no duration,
 * which the plain call asserts and the checked one reports.
 * */
NYA_API NYA_Duration nya_duration_between(NYA_Instant from, NYA_Instant to) __attr_no_discard;
NYA_API b8           nya_duration_between_checked(NYA_Instant from, NYA_Instant to, OUT NYA_Duration* out_duration) __attr_no_discard;

/** Spans from coarser units. Assert the span fits, which any literal a program writes does. */
NYA_API NYA_Duration nya_duration_from_s(s64 seconds) __attr_no_discard;
NYA_API NYA_Duration nya_duration_from_ms(s64 milliseconds) __attr_no_discard;

/** The UTC day and time of day `instant` falls on. Total: every instant has one. */
NYA_API void nya_instant_to_utc(NYA_Instant instant, OUT NYA_Date* out_date, OUT NYA_TimeOfDay* out_time);

/**
 * The instant a UTC day and time of day name. False when it lies outside what NYA_Instant holds, which
 * a valid date can: 1600-01-01 is a date and not an instant. Asserts both halves are valid.
 * */
NYA_API b8 nya_instant_from_utc(NYA_Date date, NYA_TimeOfDay time, OUT NYA_Instant* out_instant) __attr_no_discard;

// DATES

/** Month 1 to 12, day within that month in that year, year within NYA_DATE_YEAR_MIN and _MAX. */
NYA_API b8 nya_date_is_valid(NYA_Date date) __attr_no_discard;

/** The Gregorian rule: every fourth year, except centuries, except every fourth century. */
NYA_API b8 nya_date_is_leap_year(s32 year) __attr_no_discard;

NYA_API u32 nya_date_days_in_month(NYA_Date date) __attr_no_discard;

/**
 * Days since 1970-01-01, and back. The difference of two dates is the difference of these, and the pair
 * is exact over the whole date range, which a property test holds it to.
 * */
NYA_API s64      nya_date_to_days(NYA_Date date) __attr_no_discard;
NYA_API NYA_Date nya_date_from_days(s64 days) __attr_no_discard;

/**
 * `days` later, or earlier when negative. The plain call asserts the result is a date; the checked one
 * returns false past year 0 or 9999.
 * */
NYA_API NYA_Date nya_date_add_days(NYA_Date date, s64 days) __attr_no_discard;
NYA_API b8       nya_date_add_days_checked(NYA_Date date, s64 days, OUT NYA_Date* out_date) __attr_no_discard;

/**
 * `months` later, or earlier when negative, with the day clamped to the target month's length:
 * 2024-01-31 plus one month is 2024-02-29, and 2023-01-31 plus one is 2023-02-28.
 *
 * Clamped rather than overflowing into the next month (which is what adding to struct tm and
 * normalising does, giving March 3rd), because a monthly bill due on the 31st is due at the end of
 * February and not in March. It follows that this is not invertible: 01-31 + 1 - 1 is 01-28.
 * */
NYA_API NYA_Date nya_date_add_months(NYA_Date date, s32 months) __attr_no_discard;
NYA_API b8       nya_date_add_months_checked(NYA_Date date, s32 months, OUT NYA_Date* out_date) __attr_no_discard;

NYA_API NYA_Date nya_date_end_of_month(NYA_Date date) __attr_no_discard;

NYA_API NYA_Weekday nya_date_weekday(NYA_Date date) __attr_no_discard;

/**
 * ISO 8601's week: weeks start on Monday and week 1 is the one holding the year's first Thursday, so
 * 2021-01-03 is in week 53 of 2020 and 2024-12-30 in week 1 of 2025.
 * */
NYA_API NYA_IsoWeek nya_date_iso_week(NYA_Date date) __attr_no_discard;

// TIMES OF DAY

/** Hour below 24, minute and second below 60, nanosecond below a second. */
NYA_API b8 nya_time_of_day_is_valid(NYA_TimeOfDay time) __attr_no_discard;

/** Time since midnight. Asserts the time of day is valid. */
NYA_API NYA_Duration nya_time_of_day_to_duration(NYA_TimeOfDay time) __attr_no_discard;
