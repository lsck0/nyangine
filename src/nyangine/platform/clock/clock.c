#include "nyangine/base/base.h"
#include "nyangine/os/os_time.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Both clocks are one os call in nanoseconds, and every unit below is that number divided. This used
 * to be two files of seven functions, each doing its own conversion from the platform's own epoch and
 * its own tick rate, which is seven chances for the two targets to disagree about the same moment.
 */

u64 nya_clock_get_timestamp_s(void) {
    return nya_os_time_wall_ns() / 1'000'000'000ULL;
}

u64 nya_clock_get_timestamp_ms(void) {
    return nya_os_time_wall_ns() / 1'000'000ULL;
}

u64 nya_clock_get_timestamp_µs(void) {
    return nya_os_time_wall_ns() / 1'000ULL;
}

u64 nya_clock_get_timestamp_ns(void) {
    return nya_os_time_wall_ns();
}

u64 nya_clock_get_monotonic_ms(void) {
    return nya_os_time_monotonic_ns() / 1'000'000ULL;
}

u64 nya_clock_get_monotonic_µs(void) {
    return nya_os_time_monotonic_ns() / 1'000ULL;
}

u64 nya_clock_get_monotonic_ns(void) {
    return nya_os_time_monotonic_ns();
}

/*
 * Civil date from a day count, after Howard Hinnant's chrono algorithms. Not localtime_r or gmtime_r:
 * those touch the timezone database and, on glibc, a lock, and the crash path formats a timestamp from
 * a signal handler.
 */
void nya_clock_civil_from_days(s64 days, OUT s32* out_year, OUT u32* out_month, OUT u32* out_day) {
    nya_assert(out_year != nullptr);
    nya_assert(out_month != nullptr);
    nya_assert(out_day != nullptr);

    days += 719'468;

    const s64 era         = (days >= 0 ? days : days - 146'096) / 146'097;
    const u64 day_of_era  = (u64)(days - era * 146'097);
    const u64 year_of_era = (day_of_era - day_of_era / 1'460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    const s64 year        = (s64)year_of_era + era * 400;
    const u64 day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const u64 month_prime = (5 * day_of_year + 2) / 153;
    const u64 day         = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const u64 month       = month_prime < 10 ? month_prime + 3 : month_prime - 9;

    *out_year  = (s32)(year + (month <= 2 ? 1 : 0));
    *out_month = (u32)month;
    *out_day   = (u32)day;
}

s64 nya_clock_days_from_civil(s32 year, u32 month, u32 day) {
    nya_assert(month >= 1 && month <= 12, "month %u is not a month", month);
    nya_assert(day >= 1 && day <= 31, "day %u is not a day of the month", day);

    // The inverse of nya_clock_civil_from_days, same era shift.
    const s64 shifted     = year - (month <= 2 ? 1 : 0);
    const s64 era         = (shifted >= 0 ? shifted : shifted - 399) / 400;
    const u64 year_of_era = (u64)(shifted - era * 400);
    const u64 day_of_year = (u64)((153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1);
    const u64 day_of_era  = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

    return era * 146'097 + (s64)day_of_era - 719'468;
}

u32 nya_clock_format_utc(u64 timestamp_s, NYA_ClockFormat format, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(format < NYA_CLOCK_FORMAT_COUNT);

    if (capacity == 0) return 0;

    const u64 seconds_of_day = timestamp_s % NYA_CLOCK_SECONDS_PER_DAY;

    s32 year  = 0;
    u32 month = 0;
    u32 day   = 0;
    nya_clock_civil_from_days((s64)(timestamp_s / NYA_CLOCK_SECONDS_PER_DAY), &year, &month, &day);

    const u32 hour   = (u32)(seconds_of_day / 3'600);
    const u32 minute = (u32)((seconds_of_day / 60) % 60);
    const u32 second = (u32)(seconds_of_day % 60);

    s32 written = 0;
    switch (format) {
        case NYA_CLOCK_FORMAT_READABLE:
            written = snprintf((char*)buffer, capacity, "%04d-%02u-%02u %02u:%02u:%02u UTC", year, month, day, hour, minute, second);
            break;

        case NYA_CLOCK_FORMAT_FILENAME:
            written = snprintf((char*)buffer, capacity, "%04d-%02u-%02u-%02u%02u%02u", year, month, day, hour, minute, second);
            break;

        default: nya_unreachable();
    }
    static_assert(NYA_CLOCK_FORMAT_COUNT == 2, "Unhandled NYA_ClockFormat enum value.");

    if (written <= 0) {
        buffer[0] = '\0';
        return 0;
    }

    return (u32)written < capacity ? (u32)written : capacity - 1;
}
