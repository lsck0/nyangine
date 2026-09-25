/**
 * Bidi: a line of mixed direction splits into directional runs ordered left-to-right, a reduced UAX #9.
 *
 * The Hebrew below is the source's own UTF-8; each letter is two bytes, so "שלום" is eight. No font is opened:
 * this exercises the reordering, which is bytes and levels, not glyphs.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define HE "שלום" /* "shalom", four Hebrew letters, eight bytes */

/** A run covers exactly [offset, offset + length) at the expected direction. */
static b8 run_is(NYA_BidiRun run, u32 offset, u32 length, b8 rtl) {
    return run.offset == offset && run.length == length && ((run.level & 1U) != 0) == rtl;
}

s32 main(void) {
    NYA_BidiRun runs[NYA_BIDI_RUNS_MAX];

    // Pure left-to-right is one run, unchanged: the same bytes, left-to-right, in place.
    {
        u32 count = nya_text_bidi_runs("hello", 0, runs, NYA_BIDI_RUNS_MAX);

        nya_check(count == 1, "pure LTR is one run, got " FMTu32, count);
        nya_check(run_is(runs[0], 0, 5, false), "covering the whole string left-to-right");
    }

    // Pure right-to-left is one run, flagged right-to-left; the base direction came from the first strong letter.
    {
        u32 count = nya_text_bidi_runs(HE, 0, runs, NYA_BIDI_RUNS_MAX);

        nya_check(count == 1, "pure RTL is one run, got " FMTu32, count);
        nya_check(run_is(runs[0], 0, 8, true), "covering the whole string right-to-left");
    }

    // A right-to-left word inside a left-to-right line: the Latin stays first, the Hebrew run follows it, shaped RTL.
    {
        u32 count = nya_text_bidi_runs("hi " HE, 0, runs, NYA_BIDI_RUNS_MAX);

        nya_check(count == 2, "LTR with an RTL word is two runs, got " FMTu32, count);
        nya_check(run_is(runs[0], 0, 3, false), "the 'hi ' run comes first, left-to-right");
        nya_check(run_is(runs[1], 3, 8, true), "the Hebrew run follows it, right-to-left");
    }

    // A left-to-right word inside a right-to-left line: it is last in logic but reorders to the visual left.
    {
        u32 count = nya_text_bidi_runs(HE " hi", 0, runs, NYA_BIDI_RUNS_MAX);

        nya_check(count == 2, "RTL with an LTR word is two runs, got " FMTu32, count);
        nya_check(run_is(runs[0], 9, 2, false), "the 'hi' reorders to the visual left, left-to-right");
        nya_check(run_is(runs[1], 0, 9, true), "the Hebrew and its space follow, right-to-left");
        nya_check(runs[0].offset > runs[1].offset, "the visually first run is the logically last one");
    }

    // Digits inside a right-to-left line run left-to-right and sit to the visual left of the script.
    {
        u32 count = nya_text_bidi_runs(HE " 123", 0, runs, NYA_BIDI_RUNS_MAX);

        nya_check(count == 2, "RTL with digits is two runs, got " FMTu32, count);
        nya_check(run_is(runs[0], 9, 3, false), "'123' runs left-to-right on the visual left");
        nya_check(run_is(runs[1], 0, 9, true), "the Hebrew and its space follow it");
    }

    // Digits inside a left-to-right line do not split it: they read the same direction as the letters around them.
    {
        u32 count = nya_text_bidi_runs("abc 123", 0, runs, NYA_BIDI_RUNS_MAX);

        nya_check(count == 1, "LTR with digits is one run, got " FMTu32, count);
        nya_check(run_is(runs[0], 0, 7, false), "covering the whole string left-to-right");
    }

    // The degenerate inputs a caller reaches: empty, and a capacity of none.
    {
        nya_check(nya_text_bidi_runs("", 0, runs, NYA_BIDI_RUNS_MAX) == 0, "an empty line has no runs");
        nya_check(nya_text_bidi_runs("hi", 0, runs, 0) == 0, "no capacity returns no runs");
        nya_check(nya_text_bidi_runs(nullptr, 0, runs, NYA_BIDI_RUNS_MAX) == 0, "null text returns no runs");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
