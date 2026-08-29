/**
 * Daily log files and the retention sweep: nya_log_directory_open / nya_log_directory_roll.
 *
 * The sweep deletes files, so most of what is tested here is what it refuses to touch. A retention
 * pass that went by modification time rather than by name would delete whatever else happened to be
 * in the directory, and that is the failure worth having a test for.
 * */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define TEST_DIRECTORY "./.test_logs"

/** Days since the epoch, the same clock nya_log_directory_open names its file after. */
static s64 today(void) {
    return (s64)(nya_clock_get_timestamp_s() / 86'400);
}

/** Writes an empty file named for `day`, as though a run that day had left one behind. */
static void seed_log_for_day(s64 day) {
    s32 year  = 0;
    u32 month = 0;
    u32 date  = 0;
    _nya_log_civil_from_days(day, &year, &month, &date);

    char path[512];
    (void)snprintf(path, sizeof(path), TEST_DIRECTORY "/%04d-%02u-%02u.log", year, month, date);

    NYA_EXPECT(nya_file_write(path, "seeded\n"));
}

static b8 exists_for_day(s64 day) {
    s32 year  = 0;
    u32 month = 0;
    u32 date  = 0;
    _nya_log_civil_from_days(day, &year, &month, &date);

    char path[512];
    (void)snprintf(path, sizeof(path), TEST_DIRECTORY "/%04d-%02u-%02u.log", year, month, date);

    return nya_filesystem_exists(path);
}

s32 main(void) {
    if (nya_filesystem_exists(TEST_DIRECTORY)) NYA_EXPECT(nya_filesystem_delete_recursive(TEST_DIRECTORY));

    // ── The date round trip is what names every file, so it is checked first and over a wide range.
    for (s64 day = -20'000; day < 40'000; day += 7) {
        s32 year  = 0;
        u32 month = 0;
        u32 date  = 0;
        _nya_log_civil_from_days(day, &year, &month, &date);

        char name[64];
        (void)snprintf(name, sizeof(name), "%04d-%02u-%02u.log", year, month, date);

        s64 parsed = 0;
        nya_check(_nya_log_day_from_name(name, &parsed), "'%s' should parse back to a day", name);
        nya_check(parsed == day, "'%s' round tripped to %lld, not %lld", name, (long long)parsed, (long long)day);
    }

    // Day zero is the epoch. A wrong era shift still round trips, so this pins the absolute offset.
    {
        s32 year  = 0;
        u32 month = 0;
        u32 date  = 0;
        _nya_log_civil_from_days(0, &year, &month, &date);
        nya_check(year == 1970 && month == 1 && date == 1, "day 0 should be 1970-01-01, got %04d-%02u-%02u", year, month, date);
    }

    // ── Names the sweep must refuse. Each of these would be a deleted user file.
    {
        s64 ignored = 0;
        nya_check(!_nya_log_day_from_name("notes.txt", &ignored), "a non-log name must not parse");
        nya_check(!_nya_log_day_from_name("2026-08-26.log.bak", &ignored), "a suffixed name must not parse");
        nya_check(!_nya_log_day_from_name("2026-13-01.log", &ignored), "month 13 must not parse");
        nya_check(!_nya_log_day_from_name("2026-08-32.log", &ignored), "day 32 must not parse");
        nya_check(!_nya_log_day_from_name("", &ignored), "an empty name must not parse");
        nya_check(_nya_log_day_from_name("2026-08-26.log", &ignored), "a well formed name must parse");
    }

    // ── Opening creates the directory and today's file.
    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY, 14));
    nya_check(nya_filesystem_is_directory(TEST_DIRECTORY), "the log directory should have been created");
    nya_check(exists_for_day(today()), "today's log file should exist after opening");
    nya_log_file_close();

    // ── Retention keeps today and the window behind it, and drops everything older.
    const s64 now = today();
    seed_log_for_day(now - 1);
    seed_log_for_day(now - 13);
    seed_log_for_day(now - 14);
    seed_log_for_day(now - 30);
    NYA_EXPECT(nya_file_write(TEST_DIRECTORY "/notes.txt", "not a log\n"));
    NYA_EXPECT(nya_file_write(TEST_DIRECTORY "/2026-08-26.log.bak", "not a log either\n"));

    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY, 14));

    nya_check(exists_for_day(now), "today must be kept");
    nya_check(exists_for_day(now - 1), "yesterday must be kept");
    nya_check(exists_for_day(now - 13), "the oldest day inside a 14 day window must be kept");
    nya_check(!exists_for_day(now - 14), "day 14 is outside the window and must be swept");
    nya_check(!exists_for_day(now - 30), "day 30 must be swept");
    nya_check(nya_filesystem_exists(TEST_DIRECTORY "/notes.txt"), "an unrelated file must survive the sweep");
    nya_check(nya_filesystem_exists(TEST_DIRECTORY "/2026-08-26.log.bak"), "a suffixed file must survive the sweep");

    // ── Retention of zero keeps everything.
    seed_log_for_day(now - 900);
    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY, 0));
    nya_check(exists_for_day(now - 900), "retention 0 must keep everything");

    // ── Reopening the same day appends rather than truncating: a crash restart must not erase the run that crashed.
    nya_log_file_close();
    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY, 14));
    nya_log_info("first line");
    nya_log_file_flush();
    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY, 14));
    nya_log_info("second line");
    nya_log_file_close();
    {
        s32 year  = 0;
        u32 month = 0;
        u32 date  = 0;
        _nya_log_civil_from_days(now, &year, &month, &date);

        char path[512];
        (void)snprintf(path, sizeof(path), TEST_DIRECTORY "/%04d-%02u-%02u.log", year, month, date);

        NYA_Arena arena = nya_arena_create_on_stack(.name = "read_back");
        defer     nya_arena_destroy_on_stack(&arena);

        NYA_String contents = *nya_string_create(&arena);
        NYA_EXPECT(nya_file_read(path, &contents));
        NYA_CString text = nya_string_to_cstring(&arena, &contents);

        nya_check(strstr(text, "first line") != nullptr, "the line from before the reopen must survive it");
        nya_check(strstr(text, "second line") != nullptr, "the line after the reopen must be there too");
    }

    // ── A roll on the same day is a no-op, and passing nullptr turns daily logging off.
    nya_log_directory_roll();
    NYA_EXPECT(nya_log_directory_open(nullptr, 14));

    NYA_EXPECT(nya_filesystem_delete_recursive(TEST_DIRECTORY));

    return nya_check_failures() == 0 ? 0 : 1;
}
