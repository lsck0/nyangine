/**
 * Instants, durations and dates: clock_instant.h. Known edges as tables, the calendar's laws as property
 * tests, and the seam the simulation installs its clock through.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Cases per law. The calendar repeats every 400 years, so this reaches every shape of year several times. */
#define CASES 4000

/** Fixed, so the suite is the same run every time. */
#define SEED 0x636C6F636B696E73ULL

/** How far a law moves a date, in days. Wide enough to cross centuries, narrow enough to stay in range. */
#define DAYS_SPAN_MAX 200'000

/** How far a law moves a date, in months. */
#define MONTHS_SPAN_MAX 2'400

/* HELPERS */

static b8 date_equals(NYA_Date a, NYA_Date b) {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

static NYA_Date date(s32 year, u8 month, u8 day) {
    NYA_Date result = { .year = year, .month = month, .day = day };
    nya_assert(nya_date_is_valid(result), "%d-%u-%u is not a date", year, month, day);
    return result;
}

/** A date anywhere in the range, uniform over days rather than biased small like draw_below. */
static NYA_Date draw_date(NYA_Property* property) {
    s64 first = nya_date_to_days(date(NYA_DATE_YEAR_MIN, 1, 1));
    s64 last  = nya_date_to_days(date(NYA_DATE_YEAR_MAX, 12, 31));

    return nya_date_from_days(first + (s64)(nya_property_draw_u64(property) % (u64)(last - first + 1)));
}

/** A signed count below `limit` in size, biased small so a shrunk case moves the date as little as it can. */
static s64 draw_signed(NYA_Property* property, u64 limit) {
    s64 magnitude = (s64)nya_property_draw_below(property, limit);
    return nya_property_draw_bool(property, 50) ? -magnitude : magnitude;
}

static NYA_Instant fixed_instant(void* context) {
    return *(const NYA_Instant*)context;
}

/* LAWS */

/** A date is the day count it converts to, and back. */
static b8 law_days_round_trip(NYA_Property* property) {
    NYA_Date start = draw_date(property);
    NYA_Date again = nya_date_from_days(nya_date_to_days(start));

    nya_property_note(property, "%d-%02u-%02u", start.year, start.month, start.day);
    return date_equals(start, again);
}

/** Adding n days and then subtracting n days is the identity, and the day count moves by exactly n. */
static b8 law_add_days_inverts(NYA_Property* property) {
    NYA_Date start = draw_date(property);
    s64      days  = draw_signed(property, DAYS_SPAN_MAX);

    NYA_Date moved = { 0 };
    if (!nya_date_add_days_checked(start, days, &moved)) return true;

    NYA_Date back = { 0 };
    if (!nya_date_add_days_checked(moved, -days, &back)) {
        nya_property_note(property, "%d-%02u-%02u %+lld days could not come back", start.year, start.month, start.day, (long long)days);
        return false;
    }

    nya_property_note(property, "%d-%02u-%02u %+lld days", start.year, start.month, start.day, (long long)days);
    return date_equals(start, back) && nya_date_to_days(moved) - nya_date_to_days(start) == days;
}

/** The weekday advances by n mod 7 when the date advances by n days. */
static b8 law_weekday_advances(NYA_Property* property) {
    NYA_Date start = draw_date(property);
    s64      days  = draw_signed(property, DAYS_SPAN_MAX);

    NYA_Date moved = { 0 };
    if (!nya_date_add_days_checked(start, days, &moved)) return true;

    s64 expected = ((s64)nya_date_weekday(start) + days % NYA_WEEKDAY_COUNT + NYA_WEEKDAY_COUNT) % NYA_WEEKDAY_COUNT;

    nya_property_note(property, "%d-%02u-%02u %+lld days", start.year, start.month, start.day, (long long)days);
    return (s64)nya_date_weekday(moved) == expected;
}

/**
 * Adding months lands in the month n later, keeps the day unless the month is too short, and then
 * clamps to that month's last day. Invertible whenever no clamp happened.
 * */
static b8 law_add_months_clamps(NYA_Property* property) {
    NYA_Date start  = draw_date(property);
    s32      months = (s32)draw_signed(property, MONTHS_SPAN_MAX);

    NYA_Date moved = { 0 };
    if (!nya_date_add_months_checked(start, months, &moved)) return true;

    nya_property_note(property, "%d-%02u-%02u %+d months", start.year, start.month, start.day, months);

    s64 index_before = (s64)start.year * 12 + start.month - 1;
    s64 index_after  = (s64)moved.year * 12 + moved.month - 1;
    if (index_after - index_before != months) return false;

    if (start.day <= nya_date_days_in_month(moved)) {
        if (moved.day != start.day) return false;

        NYA_Date back = nya_date_add_months(moved, -months);
        return date_equals(back, start);
    }

    return moved.day == nya_date_days_in_month(moved) && date_equals(moved, nya_date_end_of_month(moved));
}

/** Two consecutive days share an ISO week unless the second is a Monday, which starts the next one. */
static b8 law_iso_week_turns_on_monday(NYA_Property* property) {
    NYA_Date start = draw_date(property);

    NYA_Date next = { 0 };
    if (!nya_date_add_days_checked(start, 1, &next)) return true;

    NYA_IsoWeek before = nya_date_iso_week(start);
    NYA_IsoWeek after  = nya_date_iso_week(next);

    nya_property_note(property, "%d-%02u-%02u is %d-W%02u", start.year, start.month, start.day, before.year, before.week);

    if (before.week < 1 || before.week > 53) return false;

    b8 same = before.year == after.year && before.week == after.week;
    if (nya_date_weekday(next) != NYA_WEEKDAY_MONDAY) return same;

    b8 following = (after.year == before.year && after.week == before.week + 1) || (after.year == before.year + 1 && after.week == 1);
    return following;
}

/** Every instant is a date and a time of day, and those name the same instant. */
static b8 law_instant_utc_round_trip(NYA_Property* property) {
    NYA_Instant instant = { .ns = (s64)nya_property_draw_u64(property) };

    NYA_Date      day  = { 0 };
    NYA_TimeOfDay time = { 0 };
    nya_instant_to_utc(instant, &day, &time);

    NYA_Instant again = { 0 };
    b8          fits  = nya_instant_from_utc(day, time, &again);

    nya_property_note(property, "%lld ns", (long long)instant.ns);
    return fits && again.ns == instant.ns;
}

/** instant + d - d is instant, and the duration between them is d, whenever the sum is an instant. */
static b8 law_duration_inverts(NYA_Property* property) {
    NYA_Instant  instant  = { .ns = (s64)nya_property_draw_u64(property) };
    NYA_Duration duration = { .ns = (s64)nya_property_draw_u64(property) };

    NYA_Instant later = { 0 };
    if (!nya_instant_add_duration_checked(instant, duration, &later)) return true;

    NYA_Instant  back    = nya_instant_subtract_duration(later, duration);
    NYA_Duration between = { 0 };
    b8           fits    = nya_duration_between_checked(instant, later, &between);

    nya_property_note(property, "%lld ns plus %lld ns", (long long)instant.ns, (long long)duration.ns);
    return back.ns == instant.ns && fits && between.ns == duration.ns;
}

/* SIMULATION */

typedef struct {
    NYA_Instant seen[8];
    u64         clock_ns[8];
    u32         count;
} SimulatedDates;

static void action_read_the_date(NYA_SimulationRun* run) {
    SimulatedDates* dates = run->user_data;
    if (dates->count < nya_carray_length(dates->seen)) {
        dates->seen[dates->count]     = nya_instant_now();
        dates->clock_ns[dates->count] = nya_simulation_now_ns(run);
        dates->count++;
    }

    nya_simulation_advance(run, run->time_step_ns);
}

s32 main(void) {
    u32 failures = 0;

    // TEST: the edges of the instant range, as dates
    printf("TEST: instants as UTC dates and times\n");
    {
        struct {
            s64      ns;
            NYA_Date date;
            u8       hour, minute, second;
            u32      nanosecond;
        } cases[] = {
            { 0,                                 { 1970, 1, 1 },   0,  0,  0,  0           },
            { -1,                                { 1969, 12, 31 }, 23, 59, 59, 999'999'999 },
            { 784'111'777LL * NYA_NS_PER_SECOND, { 1994, 11, 6 },  8,  49, 37, 0           },
            { S64_MAX,                           { 2262, 4, 11 },  23, 47, 16, 854'775'807 },
            { S64_MIN,                           { 1677, 9, 21 },  0,  12, 43, 145'224'192 },
        };

        for (u32 i = 0; i < nya_carray_length(cases); i++) {
            NYA_Date      day  = { 0 };
            NYA_TimeOfDay time = { 0 };
            nya_instant_to_utc((NYA_Instant){ .ns = cases[i].ns }, &day, &time);

            nya_assert(date_equals(day, cases[i].date), "case %u: %d-%u-%u", i, day.year, day.month, day.day);
            nya_assert(time.hour == cases[i].hour && time.minute == cases[i].minute && time.second == cases[i].second, "case %u", i);
            nya_assert(time.nanosecond == cases[i].nanosecond, "case %u: %u ns", i, time.nanosecond);

            NYA_Instant again = { 0 };
            nya_assert(nya_instant_from_utc(day, time, &again) && again.ns == cases[i].ns, "case %u does not come back", i);
        }

        // a date can be outside the instants: 1600 is a year and not a moment this type holds.
        NYA_Instant untouched = { .ns = 42 };
        nya_assert(!nya_instant_from_utc(date(1600, 1, 1), (NYA_TimeOfDay){ 0 }, &untouched));
        nya_assert(untouched.ns == 42, "a refused conversion wrote its output anyway");
    }
    printf("  PASSED\n");

    // TEST: arithmetic, and where it stops
    printf("TEST: instant and duration arithmetic\n");
    {
        NYA_Instant epoch  = { 0 };
        NYA_Instant minute = nya_instant_add_duration(epoch, nya_duration_from_s(60));
        nya_assert(minute.ns == NYA_NS_PER_MINUTE);
        nya_assert(nya_duration_between(epoch, minute).ns == NYA_NS_PER_MINUTE);
        nya_assert(nya_duration_between(minute, epoch).ns == -NYA_NS_PER_MINUTE, "a duration backwards is negative, not a wrap");
        nya_assert(nya_instant_subtract_duration(minute, nya_duration_from_ms(60'000)).ns == 0);

        // overflow is refused, and the output is left alone.
        NYA_Instant  last   = { .ns = S64_MAX };
        NYA_Instant  first  = { .ns = S64_MIN };
        NYA_Instant  result = { .ns = 7 };
        NYA_Duration apart  = { .ns = 7 };
        nya_assert(!nya_instant_add_duration_checked(last, (NYA_Duration){ .ns = 1 }, &result));
        nya_assert(!nya_instant_subtract_duration_checked(first, (NYA_Duration){ .ns = 1 }, &result));
        nya_assert(!nya_duration_between_checked(first, last, &apart), "585 years apart has no duration");
        nya_assert(result.ns == 7 && apart.ns == 7);

        nya_assert(nya_instant_subtract_duration_checked(last, (NYA_Duration){ .ns = 1 }, &result) && result.ns == S64_MAX - 1);
    }
    printf("  PASSED\n");

    // TEST: the calendar's edge cases
    printf("TEST: calendar arithmetic\n");
    {
        nya_assert(nya_date_is_leap_year(2000) && nya_date_is_leap_year(2024) && nya_date_is_leap_year(0));
        nya_assert(!nya_date_is_leap_year(1900) && !nya_date_is_leap_year(2023) && !nya_date_is_leap_year(2100));

        nya_assert(nya_date_days_in_month(date(2024, 2, 1)) == 29);
        nya_assert(nya_date_days_in_month(date(2023, 2, 1)) == 28);
        nya_assert(nya_date_days_in_month(date(2023, 4, 1)) == 30);

        nya_assert(!nya_date_is_valid((NYA_Date){ .year = 2023, .month = 2, .day = 29 }));
        nya_assert(!nya_date_is_valid((NYA_Date){ .year = 2023, .month = 13, .day = 1 }));
        nya_assert(!nya_date_is_valid((NYA_Date){ .year = 10'000, .month = 1, .day = 1 }));
        nya_assert(!nya_date_is_valid((NYA_Date){ 0 }), "a zeroed date is not a date");

        nya_assert(!nya_time_of_day_is_valid((NYA_TimeOfDay){ .hour = 24 }));
        nya_assert(!nya_time_of_day_is_valid((NYA_TimeOfDay){ .second = 60 }), "no leap second in a time of day");
        nya_assert(!nya_time_of_day_is_valid((NYA_TimeOfDay){ .nanosecond = 1'000'000'000 }));

        nya_assert(date_equals(nya_date_end_of_month(date(2024, 2, 10)), date(2024, 2, 29)));
        nya_assert(date_equals(nya_date_end_of_month(date(2023, 12, 1)), date(2023, 12, 31)));

        // the month's end clamps rather than spilling into the next month.
        nya_assert(date_equals(nya_date_add_months(date(2024, 1, 31), 1), date(2024, 2, 29)));
        nya_assert(date_equals(nya_date_add_months(date(2023, 1, 31), 1), date(2023, 2, 28)));
        nya_assert(date_equals(nya_date_add_months(date(2023, 3, 31), -1), date(2023, 2, 28)));
        nya_assert(date_equals(nya_date_add_months(date(2023, 11, 15), 3), date(2024, 2, 15)));
        nya_assert(date_equals(nya_date_add_months(date(2024, 2, 15), -14), date(2022, 12, 15)));

        nya_assert(date_equals(nya_date_add_days(date(2023, 12, 31), 1), date(2024, 1, 1)));
        nya_assert(date_equals(nya_date_add_days(date(2024, 3, 1), -1), date(2024, 2, 29)));

        NYA_Date untouched = date(2000, 1, 1);
        nya_assert(!nya_date_add_months_checked(date(9'999, 12, 1), 1, &untouched), "year 10000 has no four digit spelling");
        nya_assert(!nya_date_add_months_checked(date(0, 1, 1), -1, &untouched));
        nya_assert(!nya_date_add_days_checked(date(9'999, 12, 31), 1, &untouched));
        nya_assert(!nya_date_add_days_checked(date(0, 1, 1), S64_MIN, &untouched), "an overflowing day count is refused, not wrapped");
        nya_assert(date_equals(untouched, date(2000, 1, 1)));

        nya_assert(nya_date_weekday(date(1970, 1, 1)) == NYA_WEEKDAY_THURSDAY);
        nya_assert(nya_date_weekday(date(2000, 1, 1)) == NYA_WEEKDAY_SATURDAY);
        nya_assert(nya_date_weekday(date(1994, 11, 6)) == NYA_WEEKDAY_SUNDAY);
        nya_assert(nya_date_weekday(date(1969, 12, 31)) == NYA_WEEKDAY_WEDNESDAY, "a date before the epoch floors, not truncates");

        struct {
            NYA_Date    date;
            NYA_IsoWeek week;
        } weeks[] = {
            { { 2021, 1, 3 },   { 2020, 53 } },
            { { 2024, 12, 30 }, { 2025, 1 }  },
            { { 2026, 1, 1 },   { 2026, 1 }  },
            { { 2027, 1, 1 },   { 2026, 53 } },
            { { 2008, 12, 29 }, { 2009, 1 }  },
            { { 2010, 1, 3 },   { 2009, 53 } },
            { { 2026, 9, 22 },  { 2026, 39 } },
        };

        for (u32 i = 0; i < nya_carray_length(weeks); i++) {
            NYA_IsoWeek week = nya_date_iso_week(weeks[i].date);
            nya_assert(week.year == weeks[i].week.year && week.week == weeks[i].week.week, "case %u: %d-W%02u", i, week.year, week.week);
        }
    }
    printf("  PASSED\n");

    // TEST: the laws
    printf("TEST: calendar and instant laws\n");
    {
        failures += nya_property_check("a date round trips through its day count", CASES, SEED, law_days_round_trip);
        failures += nya_property_check("adding n days then -n days is the identity", CASES, SEED, law_add_days_inverts);
        failures += nya_property_check("the weekday advances by n mod 7", CASES, SEED, law_weekday_advances);
        failures += nya_property_check("adding months keeps the day or clamps it", CASES, SEED, law_add_months_clamps);
        failures += nya_property_check("the ISO week turns over on a Monday", CASES, SEED, law_iso_week_turns_on_monday);
        failures += nya_property_check("an instant round trips through UTC", CASES, SEED, law_instant_utc_round_trip);
        failures += nya_property_check("a duration added and taken off is the identity", CASES, SEED, law_duration_inverts);
    }
    if (failures == 0) printf("  PASSED\n");

    // TEST: where nya_instant_now reads
    printf("TEST: the instant source\n");
    {
        NYA_InstantSource wall = nya_instant_source();
        nya_assert(wall.now == nullptr, "nothing installed a source, so it is the wall clock");

        // the wall clock is past 2020, and agrees with the raw timestamp it is made from.
        NYA_Instant before = { .ns = (s64)nya_clock_get_timestamp_ns() };
        NYA_Instant now    = nya_instant_now();
        nya_assert(now.ns >= before.ns && now.ns > 1'577'836'800LL * NYA_NS_PER_SECOND);

        NYA_Instant pinned = { .ns = 784'111'777LL * NYA_NS_PER_SECOND };
        nya_instant_source_set((NYA_InstantSource){ .now = fixed_instant, .context = &pinned });
        nya_assert(nya_instant_now().ns == pinned.ns);

        nya_instant_source_set(wall);
        nya_assert(nya_instant_now().ns >= now.ns, "the wall clock is back");

        // a simulation runs on its own clock and puts the wall clock back after.
        SimulatedDates     dates = { 0 };
        NYA_SimulationRun* run   = nya_simulation_create(.seed = SEED, .step_count = 5, .user_data = &dates);
        nya_simulation_action_add(run, "read the date", 1, action_read_the_date);
        nya_assert(nya_simulation_run(run) == 0);
        nya_simulation_destroy(run);

        nya_assert(dates.count == 5);
        for (u32 i = 0; i < dates.count; i++) {
            nya_assert(dates.seen[i].ns == NYA_SIMULATION_INSTANT_ORIGIN_NS + (s64)dates.clock_ns[i], "step %u read a date off another clock", i);
        }
        nya_assert(nya_instant_source().now == nullptr, "the simulation left its clock installed");

        NYA_Date      first    = { 0 };
        NYA_TimeOfDay midnight = { 0 };
        nya_instant_to_utc(dates.seen[0], &first, &midnight);
        nya_assert(date_equals(first, date(2000, 1, 1)), "a simulation starts at its origin");
    }
    printf("  PASSED\n");

    return failures == 0 ? 0 : 1;
}
