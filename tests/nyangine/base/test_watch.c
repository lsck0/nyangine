/**
 * What a crash report is able to say about a value: nya_watch_value_format over every scalar the engine
 * has, the comparison assertions that print both of their operands, and the ring a watched frame
 * registers its locals into.
 *
 * The ring is held to the two things the crash path depends on: an entry leaves with the frame it
 * points into, on every path out of it, and an overflow drops the oldest rather than handing back a
 * slot that now belongs to somebody else.
 * */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** What a watched frame writes down, as the generated code writes it. See build/pp/watch.h. */
#define watch_local(frame, function, variable)                                                                                                       \
    nya_watch_record(frame, function, #variable, "typeof(" #variable ")", _nya_watch_type_of(variable), (u32)sizeof(variable), &(variable))

static u8 text[NYA_WATCH_VALUE_MAX];

/** Formats one variable the way the ring would, and returns what landed in `text`. */
#define format_of(variable)                                                                                                                          \
    (nya_watch_value_format(_nya_watch_type_of(variable), (u32)sizeof(variable), &(variable), text, (u32)sizeof(text)), (NYA_ConstCString)text)

/** Counts its calls, so an assertion's operands can be shown to be evaluated exactly once. */
static u32 reads = 0;

static u32 read_once(u32 value) {
    reads++;
    return value;
}

/** A frame with two ways out, so the exit registration is tested on the one that returns early. */
static u32 watched_frame(b8 leave_early) {
    u32 held = 41;

    const u32 frame = nya_watch_frame_begin();
    watch_local(frame, "watched_frame", held);
    defer nya_watch_frame_end(frame);

    if (leave_early) return nya_watch_count();

    held++;
    return nya_watch_count();
}

/** Registers one local and leaves it registered, which only a longjmp out of here can do. */
static void watched_frame_that_crashes(void) {
    u32 doomed = 7;

    const u32 frame = nya_watch_frame_begin();
    watch_local(frame, "watched_frame_that_crashes", doomed);
    defer nya_watch_frame_end(frame);

    nya_assert_eq(doomed, 8U);
}

s32 main(void) {
    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: every scalar the engine has is written down as itself, at its own width
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u8   byte   = 200;
        u16  word   = 65535;
        u32  count  = 4294967295U;
        u64  big    = 18446744073709551615ULL;
        u128 huge   = ((u128)1 << 100);
        s8   small  = -128;
        s32  signed_count = -2147483647 - 1;
        s64  deep   = -9007199254740993LL;
        s128 lowest = S128_MIN;

        nya_check(nya_string_equals(format_of(byte), "200"), "a u8 should print as itself, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(word), "65535"), "a u16 should print as itself, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(count), "4294967295"), "a u32 should print as itself, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(big), "18446744073709551615"), "a u64 should print as itself, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(huge), "1267650600228229401496703205376"), "a u128 should print in decimal, printed '%s'",
                  (const char*)text);

        nya_check(nya_string_equals(format_of(small), "-128"), "an s8 should keep its sign, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(signed_count), "-2147483648"), "an s32 should keep its sign, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(deep), "-9007199254740993"), "an s64 should print exactly, printed '%s'", (const char*)text);

        // The value with no positive counterpart, which is where a naive negation overflows.
        nya_check(nya_string_equals(format_of(lowest), "-170141183460469231731687303715884105728"),
                  "the most negative s128 should print, printed '%s'", (const char*)text);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: and so is everything that is not an integer
    // ─────────────────────────────────────────────────────────────────────────────
    {
        f16  half      = (f16)0.5F;
        f32  single    = 1.5F;
        f64  tiny      = 0.000001;
        f128 quadruple = 2.25L;
        bool yes       = true;
        char letter    = 'A';
        char newline   = '\n';

        nya_check(nya_string_equals(format_of(half), "0.5"), "an f16 should print, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(single), "1.5"), "an f32 should print, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(tiny), "1e-06"), "a small f64 should print in exponent form, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(quadruple), "2.25"), "an f128 should print, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(yes), "true"), "a bool should print as a word, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(letter), "'A' (65)"), "a char should print as itself and its code, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(newline), "'\\x0a'"), "an unprintable char should print as its code, printed '%s'", (const char*)text);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a pointer says where it points, and text says what it says
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_watch");
        defer      nya_arena_destroy(arena);

        NYA_ConstCString greeting = "hello";
        NYA_ConstCString nothing  = nullptr;
        NYA_String*      string   = nya_string_from(arena, "a string");
        NYA_String*      no_string = nullptr;
        void*            somewhere = arena;
        f32x2            vector    = { 1.0F, 2.0F };

        nya_check(nya_string_equals(format_of(greeting), "\"hello\""), "a C string should print its text, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(nothing), "nullptr"), "a null C string should say so, printed '%s'", (const char*)text);
        nya_check(nya_string_equals(format_of(string), "\"a string\" (8 bytes)"), "an NYA_String should print its text and its length, printed '%s'",
                  (const char*)text);
        nya_check(nya_string_equals(format_of(no_string), "nullptr"), "a null NYA_String should say so, printed '%s'", (const char*)text);

        nya_check(nya_string_starts_with(format_of(somewhere), "0x"), "a pointer should print as an address, printed '%s'", (const char*)text);

        // Not a scalar and not a pointer: there is nothing to read, so the report says where it is.
        nya_check(nya_string_starts_with(format_of(vector), "<8 bytes at 0x"), "a struct should print its size and address, printed '%s'",
                  (const char*)text);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a string longer than the report keeps is cut, and says it was
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_ConstCString long_line = "0123456789012345678901234567890123456789012345678901234567890123456789";

        nya_check(nya_string_contains(format_of(long_line), "..."), "a long string should be cut, printed '%s'", (const char*)text);
        nya_check(strlen((const char*)text) < NYA_WATCH_STRING_MAX + 8, "a cut string should be about as long as the cut, printed " FMTu64,
                  (u64)strlen((const char*)text));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a comparison that holds costs nothing and says nothing
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u32 written = 6;

        nya_assert_eq(written, 6U);
        nya_assert_ne(written, 7U);
        nya_assert_lt(written, 7U);
        nya_assert_le(written, 6U);
        nya_assert_gt(written, 5U);
        nya_assert_ge(written, 6U);

        // pointers compare as pointers, which is what the operator would have done on its own.
        NYA_ConstCString name = "written";
        nya_assert_eq(name, name);
        nya_assert_ne(name, (NYA_ConstCString) nullptr);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: one that does not names both sides and what each held
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u32 written  = 6;
        u32 expected = 8;

        nya_expect_crash(nya_assert_eq(written, expected));

        const NYA_CrashInfo* caught = nya_crash_caught();

        nya_check(caught != nullptr && caught->source == NYA_CRASH_SOURCE_ASSERT, "a failed comparison should be an assertion");
        nya_check(caught != nullptr && nya_string_contains((NYA_ConstCString)caught->message, "written == expected"),
                  "the report should carry the comparison as it was written, carried '%s'", caught != nullptr ? (const char*)caught->message : "");
        nya_check(caught != nullptr && nya_string_contains((NYA_ConstCString)caught->message, "written is 6"),
                  "the report should carry the left operand, carried '%s'", caught != nullptr ? (const char*)caught->message : "");
        nya_check(caught != nullptr && nya_string_contains((NYA_ConstCString)caught->message, "expected is 8"),
                  "the report should carry the right operand, carried '%s'", caught != nullptr ? (const char*)caught->message : "");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: and each side is evaluated once, so an operand with a side effect has
    //       the same meaning it would have had in the comparison itself
    // ─────────────────────────────────────────────────────────────────────────────
    {
        reads = 0;
        nya_assert_eq(read_once(3), 3U);
        nya_check(reads == 1, "a comparison that holds should read each side once, read %u times", reads);

        reads = 0;
        nya_expect_crash(nya_assert_eq(read_once(3), 4U));
        nya_check(reads == 1, "a comparison that fails should still read each side once, read %u times", reads);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a watched frame registers on the way in and unregisters on every way
    //       out, the early one included
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_check(nya_watch_count() == 0, "nothing should be watched before the first frame, %u was", nya_watch_count());

        nya_check(watched_frame(false) == 1, "a watched frame should hold its local while it runs");
        nya_check(nya_watch_count() == 0, "and let go of it when it returns, %u left", nya_watch_count());

        nya_check(watched_frame(true) == 1, "an early return should hold it too");
        nya_check(nya_watch_count() == 0, "and let go of it just the same, %u left", nya_watch_count());
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the entries carry the name, the type and the value, innermost last
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u32 outer = 11;
        s32 inner = -12;

        const u32 first = nya_watch_frame_begin();
        watch_local(first, "outer_frame", outer);

        const u32 second = nya_watch_frame_begin();
        watch_local(second, "inner_frame", inner);

        nya_check(nya_watch_count() == 2, "two frames should hold two entries, hold %u", nya_watch_count());
        nya_check(first != second, "two live frames should have two ids");

        const NYA_WatchEntry* entry = nya_watch_at(1);

        nya_check(entry != nullptr && nya_string_equals(entry->name, "inner"), "the last entry should be the innermost frame's");
        nya_check(entry != nullptr && nya_string_equals(entry->function, "inner_frame"), "and should name the function it came from");

        if (entry != nullptr) {
            (void)nya_watch_value_format(entry->type, entry->size, entry->address, text, (u32)sizeof(text));
            nya_check(nya_string_equals((NYA_ConstCString)text, "-12"), "and should read back the value the local holds, read '%s'",
                      (const char*)text);
        }

        nya_watch_frame_end(second);
        nya_check(nya_watch_count() == 1, "ending the inner frame should leave the outer one, left %u", nya_watch_count());

        nya_watch_frame_end(first);
        nya_check(nya_watch_count() == 0, "and ending the outer one should empty the ring, left %u", nya_watch_count());
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: past the bound the oldest entries go, and the ring says how many
    // ─────────────────────────────────────────────────────────────────────────────
    {
        const u32 overflow = 5;

        u32       values[NYA_WATCH_RING_MAX + 5] = { 0 };
        const u32 frame                          = nya_watch_frame_begin();

        for (u32 i = 0; i < nya_carray_length(values); i++) {
            values[i] = i;
            nya_watch_record(frame, "overflowing_frame", "value", "u32", NYA_WATCH_TYPE_UNSIGNED, (u32)sizeof(values[i]), &values[i]);
        }

        nya_check(nya_watch_count() == NYA_WATCH_RING_MAX, "the ring should hold its bound and no more, holds %u", nya_watch_count());
        nya_check(nya_watch_dropped() == overflow, "and should report the %u it dropped, reported %u", overflow, nya_watch_dropped());

        // The oldest went, not the newest: the entries that are left are the ones nearest the crash.
        const NYA_WatchEntry* oldest = nya_watch_at(0);
        const NYA_WatchEntry* newest = nya_watch_at(nya_watch_count() - 1);

        nya_check(oldest != nullptr && oldest->address == &values[overflow], "the oldest entry left should be the first one not dropped");
        nya_check(newest != nullptr && newest->address == &values[nya_carray_length(values) - 1], "and the newest should be the last one pushed");

        nya_watch_frame_end(frame);

        nya_check(nya_watch_count() == 0, "unwinding should empty it, left %u", nya_watch_count());
        nya_check(nya_watch_dropped() == 0, "and nothing is missing from an empty ring, reported %u", nya_watch_dropped());
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a crash that is caught rather than fatal still takes the frame's
    //       entries with it, because no defer runs on the way out of a longjmp
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_expect_crash(watched_frame_that_crashes());

        nya_check(nya_watch_count() == 0, "a prevented crash should leave nothing pointing into the frame it left, %u left", nya_watch_count());
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
