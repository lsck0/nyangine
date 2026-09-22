/**
 * A test that hangs fails within its deadline instead of stopping the suite.
 *
 * The hang runs in a child: this binary starts itself again with `--hang`, and the child arms a one
 * second deadline and never returns. What is checked is what a person reading a CI log would need: the
 * child fails, it fails near the deadline rather than at the runner's timeout, it names itself, and on
 * Linux the report is the stuck thread's own backtrace.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_timer.h"

#define CHILD_DEADLINE_S 1

/** The whole of a failed run: the deadline, the stuck thread reporting, and the process going down. */
#define CHILD_RUN_MAX_MS ((CHILD_DEADLINE_S * 1000) + _NYA_TEST_DEADLINE_REPORT_GRACE_MS)

/** Spins rather than sleeps: a busy loop never returns to anything that could notice a stop by itself. */
__attribute__((noinline)) static void child_hang_forever(void) {
    volatile u64 spins = 0;
    for (;;) spins++;
}

s32 main(s32 argc, NYA_CString argv[]) {
    if (argc == 2 && nya_string_equals(argv[1], "--hang")) {
        nya_test_deadline_start("the hanging child", CHILD_DEADLINE_S);
        child_hang_forever();
    }

    NYA_Arena* arena = nya_arena_create(.name = "test_deadline");
    defer nya_arena_destroy(arena);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a hang fails at its deadline, names itself, and reports where it was
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Command child = {
            .arena     = arena,
            .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
            .program   = argv[0],
            .arguments = { "--hang", nullptr },
        };
        NYA_EXPECT(nya_command_run(&child));

        nya_check(child.exit_code != 0, "a hung child fails, got exit code %d", child.exit_code);
        nya_check(child.execution_time_ms >= (u64)CHILD_DEADLINE_S * 1000,"and not before its deadline, took " FMTu64 " ms", child.execution_time_ms);
        nya_check(child.execution_time_ms < CHILD_RUN_MAX_MS, "but soon after it, took " FMTu64 " ms", child.execution_time_ms);
        nya_check(nya_string_contains(child.stderr_content, "[DEADLINE] the hanging child is still running after 1 s"), "the failure names the test");

#if OS_LINUX
        nya_check(nya_string_contains(child.stderr_content, "Fault, signal"), "the stuck thread went down through the crash path");
        nya_check(nya_string_contains(child.stderr_content, "child_hang_forever"), "and its backtrace is the stuck thread's, not the watchdog's");
        nya_check(!nya_string_contains(child.stderr_content, "did not report"), "the stuck thread reported within the grace");
#endif

        if (nya_check_failures() > 0) (void)fprintf(stderr, "child stderr:\n%.*s\n", (int)child.stderr_content->length, child.stderr_content->items);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a deadline stopped in time never fires
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_test_deadline_start("test_deadline", CHILD_DEADLINE_S);
        nya_test_deadline_stop();

        // past where it would have fired. Reaching the check at all is the test.
        SDL_Delay((CHILD_DEADLINE_S * 1000) + 500);
        nya_check(true, "still running after the stopped deadline would have passed");

        // stopping twice, and stopping nothing, are both no-ops.
        nya_test_deadline_stop();

        printf("  PASSED\n");
    }

    nya_log_info("PASSED: test_deadline");

    return nya_check_failures() == 0 ? 0 : 1;
}
