#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_clock_instant.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Zeroed until something installs a source, which is the wall clock. */
NYA_INTERNAL NYA_InstantSource _NYA_INSTANT_SOURCE = { 0 };

/** Days in `month` of `year`, for a month already known to be 1 to 12. */
NYA_INTERNAL u32 _nya_date_month_length(s32 year, u32 month) __attr_no_discard;

/** Division rounding toward negative infinity, so an instant before the epoch lands on the day it is in. */
NYA_INTERNAL s64 _nya_instant_floor_div(s64 value, s64 divisor, OUT s64* out_remainder) __attr_no_discard;

/** Days since the epoch of the first and last day NYA_Date may hold, for the range checks. */
NYA_INTERNAL s64 _nya_date_days_min(void) __attr_no_discard;
NYA_INTERNAL s64 _nya_date_days_max(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * INSTANTS
 * ─────────────────────────────────────────────────────────
 */

NYA_Instant nya_instant_now(void) {
    if (_NYA_INSTANT_SOURCE.now != nullptr) return _NYA_INSTANT_SOURCE.now(_NYA_INSTANT_SOURCE.context);

    u64 ns = nya_clock_get_timestamp_ns();

    // a system clock set past 2262 is not a time this type can hold, and pretending otherwise would
    // hand every caller a negative instant.
    nya_assert(ns <= (u64)S64_MAX, "the wall clock reads past 2262");

    return (NYA_Instant){ .ns = (s64)ns };
}

void nya_instant_source_set(NYA_InstantSource source) {
    // a context with nothing to hand it to is a source installed half way.
    nya_assert(source.now != nullptr || source.context == nullptr, "an instant source has a context and no clock");

    _NYA_INSTANT_SOURCE = source;
}

NYA_InstantSource nya_instant_source(void) {
    return _NYA_INSTANT_SOURCE;
}

NYA_Instant nya_instant_add_duration(NYA_Instant instant, NYA_Duration duration) {
    NYA_Instant result = { 0 };
    b8          fits   = nya_instant_add_duration_checked(instant, duration, &result);
    nya_assert(fits, "%lld ns plus %lld ns leaves the instant range", (long long)instant.ns, (long long)duration.ns);
    return result;
}

NYA_Instant nya_instant_subtract_duration(NYA_Instant instant, NYA_Duration duration) {
    NYA_Instant result = { 0 };
    b8          fits   = nya_instant_subtract_duration_checked(instant, duration, &result);
    nya_assert(fits, "%lld ns minus %lld ns leaves the instant range", (long long)instant.ns, (long long)duration.ns);
    return result;
}

b8 nya_instant_add_duration_checked(NYA_Instant instant, NYA_Duration duration, OUT NYA_Instant* out_instant) {
    nya_assert(out_instant != nullptr);

    s64 ns = 0;
    if (__builtin_add_overflow(instant.ns, duration.ns, &ns)) return false;

    *out_instant = (NYA_Instant){ .ns = ns };
    return true;
}

b8 nya_instant_subtract_duration_checked(NYA_Instant instant, NYA_Duration duration, OUT NYA_Instant* out_instant) {
    nya_assert(out_instant != nullptr);

    s64 ns = 0;
    if (__builtin_sub_overflow(instant.ns, duration.ns, &ns)) return false;

    *out_instant = (NYA_Instant){ .ns = ns };
    return true;
}

NYA_Duration nya_duration_between(NYA_Instant from, NYA_Instant to) {
    NYA_Duration result = { 0 };
    b8           fits   = nya_duration_between_checked(from, to, &result);
    nya_assert(fits, "%lld ns and %lld ns are too far apart for a duration", (long long)from.ns, (long long)to.ns);
    return result;
}

b8 nya_duration_between_checked(NYA_Instant from, NYA_Instant to, OUT NYA_Duration* out_duration) {
    nya_assert(out_duration != nullptr);

    s64 ns = 0;
    if (__builtin_sub_overflow(to.ns, from.ns, &ns)) return false;

    *out_duration = (NYA_Duration){ .ns = ns };
    return true;
}

NYA_Duration nya_duration_from_s(s64 seconds) {
    s64 ns = 0;
    nya_assert(!__builtin_mul_overflow(seconds, NYA_NS_PER_SECOND, &ns), "%lld seconds is longer than a duration holds", (long long)seconds);
    return (NYA_Duration){ .ns = ns };
}

NYA_Duration nya_duration_from_ms(s64 milliseconds) {
    s64 ns = 0;
    nya_assert(
        !__builtin_mul_overflow(milliseconds, NYA_NS_PER_SECOND / 1'000, &ns),
        "%lld ms is longer than a duration holds",
        (long long)milliseconds
    );
    return (NYA_Duration){ .ns = ns };
}

void nya_instant_to_utc(NYA_Instant instant, OUT NYA_Date* out_date, OUT NYA_TimeOfDay* out_time) {
    nya_assert(out_date != nullptr);
    nya_assert(out_time != nullptr);

    s64 ns_of_day = 0;
    s64 days      = _nya_instant_floor_div(instant.ns, NYA_NS_PER_DAY, &ns_of_day);

    *out_date = nya_date_from_days(days);
    *out_time = (NYA_TimeOfDay){
        .hour       = (u8)(ns_of_day / NYA_NS_PER_HOUR),
        .minute     = (u8)((ns_of_day / NYA_NS_PER_MINUTE) % 60),
        .second     = (u8)((ns_of_day / NYA_NS_PER_SECOND) % 60),
        .nanosecond = (u32)(ns_of_day % NYA_NS_PER_SECOND),
    };

    nya_assert(nya_time_of_day_is_valid(*out_time));
}

b8 nya_instant_from_utc(NYA_Date date, NYA_TimeOfDay time, OUT NYA_Instant* out_instant) {
    nya_assert(out_instant != nullptr);
    nya_assert(nya_date_is_valid(date), "%d-%u-%u is not a date", date.year, date.month, date.day);
    nya_assert(nya_time_of_day_is_valid(time), "%u:%u:%u.%u is not a time of day", time.hour, time.minute, time.second, time.nanosecond);

    // wide, because the earliest instant's midnight is before the earliest instant: 1677-09-21T00:12Z is
    // in range and 1677-09-21T00:00Z is not, so the day alone may overflow where the sum does not.
    s128 ns = (s128)nya_date_to_days(date) * (s128)NYA_NS_PER_DAY + nya_time_of_day_to_duration(time).ns;
    if (ns < (s128)S64_MIN || ns > (s128)S64_MAX) return false;

    *out_instant = (NYA_Instant){ .ns = (s64)ns };
    return true;
}

/*
 * ─────────────────────────────────────────────────────────
 * DATES
 * ─────────────────────────────────────────────────────────
 */

b8 nya_date_is_valid(NYA_Date date) {
    if (date.year < NYA_DATE_YEAR_MIN || date.year > NYA_DATE_YEAR_MAX) return false;
    if (date.month < 1 || date.month > 12) return false;

    return date.day >= 1 && date.day <= _nya_date_month_length(date.year, date.month);
}

b8 nya_date_is_leap_year(s32 year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

u32 nya_date_days_in_month(NYA_Date date) {
    nya_assert(date.month >= 1 && date.month <= 12, "month %u is not a month", date.month);
    return _nya_date_month_length(date.year, date.month);
}

s64 nya_date_to_days(NYA_Date date) {
    nya_assert(nya_date_is_valid(date), "%d-%u-%u is not a date", date.year, date.month, date.day);
    return nya_clock_days_from_civil(date.year, date.month, date.day);
}

NYA_Date nya_date_from_days(s64 days) {
    s32 year  = 0;
    u32 month = 0;
    u32 day   = 0;
    nya_clock_civil_from_days(days, &year, &month, &day);

    nya_assert(month >= 1 && month <= 12 && day >= 1 && day <= 31);

    // an instant's day is always inside the date range, so only a day count from elsewhere can trip this.
    NYA_Date date = { .year = year, .month = (u8)month, .day = (u8)day };
    nya_assert(nya_date_is_valid(date), "day %lld is outside the years a date holds", (long long)days);

    return date;
}

NYA_Date nya_date_add_days(NYA_Date date, s64 days) {
    NYA_Date result = { 0 };
    b8       fits   = nya_date_add_days_checked(date, days, &result);
    nya_assert(fits, "%d-%u-%u plus %lld days leaves the date range", date.year, date.month, date.day, (long long)days);
    return result;
}

b8 nya_date_add_days_checked(NYA_Date date, s64 days, OUT NYA_Date* out_date) {
    nya_assert(out_date != nullptr);

    s64 target = 0;
    if (__builtin_add_overflow(nya_date_to_days(date), days, &target)) return false;
    if (target < _nya_date_days_min() || target > _nya_date_days_max()) return false;

    *out_date = nya_date_from_days(target);
    return true;
}

NYA_Date nya_date_add_months(NYA_Date date, s32 months) {
    NYA_Date result = { 0 };
    b8       fits   = nya_date_add_months_checked(date, months, &result);
    nya_assert(fits, "%d-%u-%u plus %d months leaves the date range", date.year, date.month, date.day, months);
    return result;
}

b8 nya_date_add_months_checked(NYA_Date date, s32 months, OUT NYA_Date* out_date) {
    nya_assert(out_date != nullptr);
    nya_assert(nya_date_is_valid(date), "%d-%u-%u is not a date", date.year, date.month, date.day);

    // counted in s64 months since year zero: an s32 of months is at most 179 million years, so this
    // cannot overflow, and the range check below is all that can fail.
    s64 index = (s64)date.year * 12 + (s64)(date.month - 1) + (s64)months;
    if (index < (s64)NYA_DATE_YEAR_MIN * 12 || index > (s64)NYA_DATE_YEAR_MAX * 12 + 11) return false;

    NYA_Date result = { .year = (s32)(index / 12), .month = (u8)(index % 12 + 1), .day = date.day };

    u32 length = _nya_date_month_length(result.year, result.month);
    if (result.day > length) result.day = (u8)length;

    nya_assert(nya_date_is_valid(result));
    nya_assert(result.day <= date.day, "clamping only ever moves the day earlier");

    *out_date = result;
    return true;
}

NYA_Date nya_date_end_of_month(NYA_Date date) {
    nya_assert(nya_date_is_valid(date), "%d-%u-%u is not a date", date.year, date.month, date.day);

    return (NYA_Date){ .year = date.year, .month = date.month, .day = (u8)_nya_date_month_length(date.year, date.month) };
}

NYA_Weekday nya_date_weekday(NYA_Date date) {
    // 1970-01-01 was a Thursday, which is three days after a Monday.
    s64 remainder = 0;
    (void)_nya_instant_floor_div(nya_date_to_days(date) + NYA_WEEKDAY_THURSDAY, NYA_WEEKDAY_COUNT, &remainder);

    nya_assert(remainder >= 0 && remainder < NYA_WEEKDAY_COUNT);
    return (NYA_Weekday)remainder;
}

NYA_IsoWeek nya_date_iso_week(NYA_Date date) {
    /*
     * The week belongs to the year its Thursday is in, and is numbered by how many Thursdays of that year
     * came before. Found through the Thursday rather than through the special cases (week 53, week 1 of
     * next year) because the special cases are exactly what the Thursday rule defines.
     */
    s64 days     = nya_date_to_days(date);
    s64 thursday = days - (s64)nya_date_weekday(date) + NYA_WEEKDAY_THURSDAY;

    // not nya_date_from_days: the Thursday of 0000-01-01's week falls in year -1, outside the date range.
    s32 year  = 0;
    u32 month = 0;
    u32 day   = 0;
    nya_clock_civil_from_days(thursday, &year, &month, &day);

    s64 week = (thursday - nya_clock_days_from_civil(year, 1, 1)) / 7 + 1;

    nya_assert(week >= 1 && week <= 53, "ISO week %lld", (long long)week);
    return (NYA_IsoWeek){ .year = year, .week = (u8)week };
}

/*
 * ─────────────────────────────────────────────────────────
 * TIMES OF DAY
 * ─────────────────────────────────────────────────────────
 */

b8 nya_time_of_day_is_valid(NYA_TimeOfDay time) {
    return time.hour < 24 && time.minute < 60 && time.second < 60 && time.nanosecond < (u32)NYA_NS_PER_SECOND;
}

NYA_Duration nya_time_of_day_to_duration(NYA_TimeOfDay time) {
    nya_assert(nya_time_of_day_is_valid(time), "%u:%u:%u.%u is not a time of day", time.hour, time.minute, time.second, time.nanosecond);

    s64 ns = (s64)time.hour * NYA_NS_PER_HOUR + (s64)time.minute * NYA_NS_PER_MINUTE + (s64)time.second * NYA_NS_PER_SECOND + (s64)time.nanosecond;

    nya_assert(ns >= 0 && ns < NYA_NS_PER_DAY);
    return (NYA_Duration){ .ns = ns };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _nya_date_month_length(s32 year, u32 month) {
    nya_assert(month >= 1 && month <= 12);

    static const u8 LENGTHS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2 && nya_date_is_leap_year(year)) return 29;
    return LENGTHS[month - 1];
}

s64 _nya_instant_floor_div(s64 value, s64 divisor, OUT s64* out_remainder) {
    nya_assert(divisor > 0);
    nya_assert(out_remainder != nullptr);

    s64 quotient  = value / divisor;
    s64 remainder = value % divisor;

    if (remainder < 0) {
        quotient  -= 1;
        remainder += divisor;
    }

    nya_assert(remainder >= 0 && remainder < divisor);
    *out_remainder = remainder;
    return quotient;
}

s64 _nya_date_days_min(void) {
    return nya_clock_days_from_civil(NYA_DATE_YEAR_MIN, 1, 1);
}

s64 _nya_date_days_max(void) {
    return nya_clock_days_from_civil(NYA_DATE_YEAR_MAX, 12, 31);
}
