/**
 * The crash reporter's report: nya_crash_report_compose and nya_crash_report_submit, and the window
 * that shows it.
 *
 * Most of what a test can hold to account is the text, which is also exactly what the window shows and
 * what both of its buttons hand over. The window itself is opened twice: once with no video subsystem,
 * where it must decline rather than fail, and once for real, where it must come up, draw and go away
 * again when it is dismissed. That second one is what caught SDL_RENDER being off in the vendored SDL,
 * which had left the window unable to open in any build ever shipped; see vendor_sdl.h.
 *
 * And once more in a child process that really crashes with the reporter registered, which must write
 * the report and exit: nobody is at a test to dismiss a window, and test_agent hung in one for hours.
 * */

// after the engine, which is what decides how SDL is configured; see the fuzz targets for the same rule.
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_render.h"
#include "SDL3/SDL_thread.h"
#include "SDL3/SDL_timer.h"
#include "SDL3/SDL_video.h"

#define TEST_DIRECTORY "./.test_crash_reports"

/** Long enough for the window to have come up and drawn at least once. */
#define DISMISS_DELAY_MS 300

/**
 * How many dismissals are sent before the test stops trying, bounding it at six seconds.
 *
 * The window waits for an input and there is nobody at a CI runner to give it one, so a dismissal that
 * never arrived would hang the whole suite rather than fail it. One is enough in practice; the rest are
 * there so that a dropped or filtered event costs a slow test instead of a stuck one.
 * */
#define DISMISS_ATTEMPTS_MAX 20

/** A crash that reaches the window hangs until this, and fails there instead of stopping the suite. */
#define CHILD_DEADLINE_S 10

/** Cleared once nya_crash_window_show has returned, so the dismisser stops knocking. */
static atomic_bool window_open = false;

/** Dismisses the crash window from outside it, the way a person clicking the close box would. */
static int SDLCALL dismiss_after_a_moment(void* user_data) {
    nya_unused(user_data);

    for (u32 attempt = 0; attempt < DISMISS_ATTEMPTS_MAX && atomic_load(&window_open); attempt++) {
        SDL_Delay(DISMISS_DELAY_MS);
        (void)SDL_PushEvent(&(SDL_Event){ .type = SDL_EVENT_QUIT });
    }

    return 0;
}

static u8 report[NYA_CRASH_REPORT_MAX_BYTES];

static b8 contains(NYA_ConstCString text) {
    return strstr((const char*)report, text) != nullptr;
}

/** A crash as the funnel would hand it to an observer, minus the process actually dying. */
static NYA_CrashInfo crash_of(NYA_CrashSource source, NYA_ConstCString message) {
    NYA_CrashInfo info = {
        .source   = source,
        .function = "test_crash_function",
        .file     = "tests/nyangine/debug/test_crash_report.c",
        .line     = 4242,
    };
    (void)snprintf((char*)info.message, sizeof(info.message), "%s", message);

    return info;
}

/** Crashes the way a test does, with a video subsystem up so the window could open if it were asked to. */
static void child_crash_unattended(void) {
    nya_test_deadline_start("the crashing child", CHILD_DEADLINE_S);

    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
    (void)SDL_Init(SDL_INIT_VIDEO);

    // a directory of its own: the parent still holds today's log open in TEST_DIRECTORY, and Windows
    // refuses a second process that file.
    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY "/child", 14));
    NYA_EXPECT(nya_crash_reporter_init());

    nya_assert(false, "the child crashes on purpose");
}

s32 main(s32 argc, NYA_CString argv[]) {
    if (argc == 2 && nya_string_equals(argv[1], "--crash")) child_crash_unattended();

    if (nya_filesystem_exists(TEST_DIRECTORY)) NYA_EXPECT(nya_filesystem_delete_recursive(TEST_DIRECTORY));

    const NYA_LogLevel original_level = nya_log_level_get();
    nya_log_level_set(NYA_LOG_LEVEL_TRACE);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the report carries the crash itself, where it came from, and the blocks
    //       a bug report is triaged from
    // ─────────────────────────────────────────────────────────────────────────────
    nya_log_ring_clear();
    nya_log_info("a line from before the crash, marker ZZTOP");

    NYA_CrashInfo assertion = crash_of(NYA_CRASH_SOURCE_ASSERT, "widget != nullptr, the widget was null");
    nya_backtrace_capture(&assertion.backtrace, 0);

    u32 length = nya_crash_report_compose(&assertion, report, sizeof(report));

    nya_check(length > 0, "a report should not be empty");
    nya_check(length == strlen((const char*)report), "the reported length %u should match the string, which is " FMTu64, length,
              (u64)strlen((const char*)report));

    nya_check(contains("ASSERTION FAILED"), "the report should name the crash source");
    nya_check(contains("the widget was null"), "the report should carry the assertion message");
    nya_check(contains("test_crash_function"), "the report should name the function");
    nya_check(contains("test_crash_report.c:4242"), "the report should carry file and line");

    nya_check(contains("\nBuild\n"), "the report should have a build block");
    nya_check(contains(NYA_VERSION), "the build block should carry the version");
    nya_check(contains(NYA_BUILD_COMMIT), "the build block should carry the commit");
    nya_check(contains(NYA_EXECUTION_MODE_NAME_MAP[NYA_EXECUTION_MODE_CURRENT]), "the build block should name the build kind");

    nya_check(contains("\nPlatform\n"), "the report should have a platform block");
    nya_check(contains("  cpu "), "the platform block should name the processor");

    /*
     * Which system, and what this process was holding. "Linux" in a bug report is not an answer, and
     * how much RAM the machine has does not say whether this program was the one that ran out.
     */
    nya_check(contains("  system "), "the platform block should name the distribution");
    nya_check(contains("  kernel "), "the platform block should name the kernel");
    nya_check(contains("  ram used "), "the platform block should carry the process's resident memory");

    nya_check(contains("\nStack trace\n"), "the report should have a stack trace block");
    nya_check(contains("ZZTOP"), "the report should carry the log lines from before the crash");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a fault and a thrown error each say the extra thing they know
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_CrashInfo fault      = crash_of(NYA_CRASH_SOURCE_FAULT, "Fault, signal 11");
        fault.signal             = 11;
        fault.fault_address      = 0xDEAD'BEEF;
        fault.fault_path         = true;

        (void)nya_crash_report_compose(&fault, report, sizeof(report));
        nya_check(contains("FAULT"), "a fault report should name the source");
        nya_check(contains("signal 11"), "a fault report should carry the signal");
        nya_check(contains("0xdeadbeef"), "a fault report should carry the faulting address");
    }
    {
        NYA_CrashInfo thrown = crash_of(NYA_CRASH_SOURCE_ERROR, "the file was not there");
        thrown.error_kind    = NYA_ERROR_NOT_FOUND;

        (void)nya_crash_report_compose(&thrown, report, sizeof(report));
        nya_check(contains("ERROR THROWN"), "a thrown error report should name the source");
        nya_check(contains("NOT_FOUND"), "a thrown error report should name the error kind");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a buffer too small truncates and says so, rather than overrunning or
    //       ending mid sentence as though the program had simply stopped
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u8        small[512] = { 0 };
        const u32 written    = nya_crash_report_compose(&assertion, small, sizeof(small));

        nya_check(written < sizeof(small), "a truncated report must stay inside its buffer, wrote %u of " FMTu64, written, (u64)sizeof(small));
        nya_check(written == strlen((const char*)small), "a truncated report must still be terminated where it says it ends");
        nya_check(strstr((const char*)small, "[report truncated]") != nullptr, "a truncated report must say that it was truncated");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: submitting with nowhere to write fails rather than inventing a path
    // ─────────────────────────────────────────────────────────────────────────────
    NYA_EXPECT(nya_log_directory_open(nullptr, 0));
    {
        u8              path[NYA_CRASH_REPORT_PATH_MAX] = { 0 };
        const NYA_Error submitted                       = nya_crash_report_submit("report body", path, sizeof(path));

        nya_check(!submitted.ok, "submitting with no log directory should fail");
        nya_check(submitted.kind == NYA_ERROR_NOT_FOUND, "the failure should be NOT_FOUND, is %s", NYA_ERRORKIND_NAME_MAP[submitted.kind]);
        nya_check(path[0] == '\0', "a failed submit should leave no path behind");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: submitting writes the whole report under the log directory and names
    //       the file it wrote
    // ─────────────────────────────────────────────────────────────────────────────
    NYA_EXPECT(nya_log_directory_open(TEST_DIRECTORY, 14));

    length = nya_crash_report_compose(&assertion, report, sizeof(report));
    {
        u8 path[NYA_CRASH_REPORT_PATH_MAX] = { 0 };
        NYA_EXPECT(nya_crash_report_submit((NYA_ConstCString)report, path, sizeof(path)));

        nya_check(path[0] != '\0', "a successful submit should name the file it wrote");
        nya_check(nya_filesystem_exists((NYA_ConstCString)path), "the named file '%s' should exist", (const char*)path);
        nya_check(strstr((const char*)path, TEST_DIRECTORY) != nullptr, "the report should land under the log directory, went to '%s'",
                  (const char*)path);

        NYA_Arena*  arena   = nya_arena_create(.name = "test_crash_report");
        defer       nya_arena_destroy(arena);
        NYA_String* written = nya_string_create(arena);
        NYA_EXPECT(nya_file_read((NYA_ConstCString)path, written));

        nya_check(written->length == length, "the file should hold the whole report, %u bytes against " FMTu64, length, (u64)written->length);
        nya_check(nya_memcmp(written->items, report, length) == 0, "the file should hold the report verbatim");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the window declines to open where there is no video subsystem, rather
    //       than failing. A headless build and a test are both that case.
    // ─────────────────────────────────────────────────────────────────────────────
    nya_crash_window_show(&assertion, (NYA_ConstCString)report);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: and opens, draws and closes where there is one. The quit comes from a
    //       thread because the window blocks until it is dismissed, which is what
    //       it is supposed to do.
    // ─────────────────────────────────────────────────────────────────────────────
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        nya_log_warn("No video subsystem here, skipping the crash window: %s", SDL_GetError());
    } else {
        /*
         * Probed rather than assumed, and skipped rather than failed. The window draws through
         * SDL_Renderer and the only render drivers this build of SDL carries want a real display, so a
         * machine with none is one this test cannot run on rather than a reporter that is broken. It has
         * to be a probe and not a try: where the window cannot open, nya_crash_window_show falls through
         * to a modal message box, and a test that opens one waits for a person who is not there.
         */
        SDL_Window*   probe          = nullptr;
        SDL_Renderer* probe_renderer = nullptr;

        if (!SDL_CreateWindowAndRenderer("probe", 64, 64, 0, &probe, &probe_renderer)) {
            nya_log_warn("No render driver here, skipping the crash window: %s", SDL_GetError());
        } else {
            SDL_DestroyRenderer(probe_renderer);
            SDL_DestroyWindow(probe);

            atomic_store(&window_open, true);

            SDL_Thread* dismiss = SDL_CreateThread(dismiss_after_a_moment, "dismiss_crash_window", nullptr);
            nya_check(dismiss != nullptr, "the dismissing thread should start: %s", SDL_GetError());

            if (dismiss != nullptr) {
                nya_crash_window_show(&assertion, (NYA_ConstCString)report);

                atomic_store(&window_open, false);
                SDL_WaitThread(dismiss, nullptr);
            }
        }

        SDL_Quit();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the observer registers once and comes back out again
    // ─────────────────────────────────────────────────────────────────────────────
    NYA_EXPECT(nya_crash_reporter_init());
    NYA_EXPECT(nya_crash_reporter_init()); // idempotent, so a hot reload does not register a second
    nya_crash_reporter_deinit();
    nya_crash_reporter_deinit(); // and the teardown takes anything, including nothing

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a test that crashes writes the report and exits, rather than waiting
    //       in a window for a click nobody will make
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_crash_report_child");
        defer      nya_arena_destroy(arena);

        NYA_Command child = {
            .arena     = arena,
            .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
            .program   = argv[0],
            .arguments = { "--crash", nullptr },
        };
        NYA_EXPECT(nya_command_run(&child));

        nya_check(child.exit_code != 0, "the child crashed, so it fails, got exit code %d", child.exit_code);
        nya_check(!nya_string_contains(child.stderr_content, "[DEADLINE]"), "it exited by itself rather than at its deadline");
        nya_check(nya_string_contains(child.stderr_content, "Crash report written to " TEST_DIRECTORY), "and named the report it wrote");

        if (nya_check_failures() > 0) (void)fprintf(stderr, "child stderr:\n%.*s\n", (int)child.stderr_content->length, child.stderr_content->items);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // CLEANUP
    // ─────────────────────────────────────────────────────────────────────────────
    NYA_EXPECT(nya_log_directory_open(nullptr, 0));
    NYA_EXPECT(nya_filesystem_delete_recursive(TEST_DIRECTORY));

    nya_log_level_set(original_level);

    return nya_check_failures() == 0 ? 0 : 1;
}
