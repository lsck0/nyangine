/**
 * The log ring a crash report reads back: nya_log_ring_count / _at / _level_at / _clear.
 * */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Whether `text` appears anywhere in `line`. */
static b8 line_contains(NYA_ConstCString line, NYA_ConstCString text) {
    return line != nullptr && strstr(line, text) != nullptr;
}

s32 main(void) {
    const NYA_LogLevel original_level = nya_log_level_get();

    nya_log_level_set(NYA_LOG_LEVEL_TRACE);
    nya_log_ring_clear();

    // TEST: an empty ring answers nothing rather than answering garbage
    nya_check(nya_log_ring_count() == 0, "a cleared ring should hold nothing, holds %u", nya_log_ring_count());
    nya_check(nya_log_ring_at(0) == nullptr, "reading past an empty ring should give null");
    nya_check(nya_log_ring_level_at(0) == NYA_LOG_LEVEL_COUNT, "the level past an empty ring should be the count");

    // TEST: lines come back oldest first, with their level and their text
    nya_log_info("ring line alpha");
    nya_log_warn("ring line beta");
    nya_log_error("ring line gamma");

    nya_check(nya_log_ring_count() == 3, "three lines should be held, %u are", nya_log_ring_count());
    nya_check(line_contains(nya_log_ring_at(0), "alpha"), "the oldest line should be alpha, is '%s'", nya_log_ring_at(0));
    nya_check(line_contains(nya_log_ring_at(1), "beta"), "the middle line should be beta, is '%s'", nya_log_ring_at(1));
    nya_check(line_contains(nya_log_ring_at(2), "gamma"), "the newest line should be gamma, is '%s'", nya_log_ring_at(2));

    nya_check(nya_log_ring_level_at(0) == NYA_LOG_LEVEL_INFO, "alpha was logged at INFO");
    nya_check(nya_log_ring_level_at(1) == NYA_LOG_LEVEL_WARN, "beta was logged at WARN");
    nya_check(nya_log_ring_level_at(2) == NYA_LOG_LEVEL_ERROR, "gamma was logged at ERROR");

    nya_check(nya_log_ring_at(3) == nullptr, "reading one past the count should give null");

    // TEST: a line below the level never reaches the ring, so filtering is not
    //       something a crash report can work around
    nya_log_level_set(NYA_LOG_LEVEL_ERROR);
    nya_log_info("ring line filtered out");
    nya_check(nya_log_ring_count() == 3, "a filtered line should not be kept, count is %u", nya_log_ring_count());
    nya_log_level_set(NYA_LOG_LEVEL_TRACE);

    // TEST: past capacity the oldest line is dropped and the count stops growing
    nya_log_ring_clear();

    const u32 overfill = NYA_LOG_RING_MAX + 17;
    for (u32 i = 0; i < overfill; i++) nya_log_info("ring fill %u", i);

    nya_check(nya_log_ring_count() == NYA_LOG_RING_MAX, "the ring should saturate at %u, holds %u", (u32)NYA_LOG_RING_MAX, nya_log_ring_count());

    {
        // The oldest surviving line is the one written NYA_LOG_RING_MAX lines before the end.
        char expected[64];
        (void)snprintf(expected, sizeof(expected), "ring fill %u", overfill - NYA_LOG_RING_MAX);
        nya_check(line_contains(nya_log_ring_at(0), expected), "the oldest line should be '%s', is '%s'", expected, nya_log_ring_at(0));

        (void)snprintf(expected, sizeof(expected), "ring fill %u", overfill - 1);
        nya_check(line_contains(nya_log_ring_at(NYA_LOG_RING_MAX - 1), expected), "the newest line should be '%s', is '%s'", expected,
                  nya_log_ring_at(NYA_LOG_RING_MAX - 1));
    }

    // Nothing beyond the capacity is readable, whatever was written.
    nya_check(nya_log_ring_at(NYA_LOG_RING_MAX) == nullptr, "reading past the capacity should give null");

    // TEST: a line longer than a slot is truncated, not written past its slot
    nya_log_ring_clear();

    {
        char long_message[NYA_LOG_RING_LINE_MAX * 2];
        for (u32 i = 0; i < sizeof(long_message) - 1; i++) long_message[i] = 'x';
        long_message[sizeof(long_message) - 1] = '\0';

        nya_log_info("%s", long_message);

        NYA_ConstCString kept = nya_log_ring_at(0);
        nya_check(kept != nullptr, "an over-long line should still be kept");
        nya_check(strlen(kept) == NYA_LOG_RING_LINE_MAX - 1, "an over-long line should be cut to %u bytes, is " FMTu64, NYA_LOG_RING_LINE_MAX - 1,
                  (u64)strlen(kept));
    }

    // TEST: the ring keeps working after a wrap that clear reset mid-cycle, which
    //       is the case the oldest-slot arithmetic gets wrong when it is wrong
    for (u32 i = 0; i < NYA_LOG_RING_MAX + (NYA_LOG_RING_MAX / 2); i++) nya_log_info("wrap %u", i);
    nya_log_ring_clear();
    nya_log_info("after the clear");

    nya_check(nya_log_ring_count() == 1, "one line should follow a clear, %u do", nya_log_ring_count());
    nya_check(line_contains(nya_log_ring_at(0), "after the clear"), "the only line should be the one after the clear, is '%s'", nya_log_ring_at(0));

    // CLEANUP
    nya_log_ring_clear();
    nya_log_level_set(original_level);

    return nya_check_failures() == 0 ? 0 : 1;
}
