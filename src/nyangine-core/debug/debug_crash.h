/**
 * @file debug_crash.h
 *
 * The crash reporter: what a player sees when the engine dies.
 *
 * ```c
 * // once, from the subsystem registry. Everything below happens by itself from then on.
 * NYA_EXPECT(nya_crash_reporter_init());
 *
 * // and, for the parts a tool or a test wants on their own:
 * static u8 report[NYA_CRASH_REPORT_MAX_BYTES];
 * u32       length = nya_crash_report_compose(info, report, sizeof(report));
 *
 * u8 path[NYA_CRASH_REPORT_PATH_MAX];
 * NYA_Error written = nya_crash_report_submit((NYA_ConstCString)report, path, sizeof(path));
 * ```
 *
 * Registers one observer on base_logging.h's crash funnel, so an assertion, a panic, a thrown error and
 * a hardware fault all produce the same report. The report carries the crash itself, the stack it came
 * from, what the build is, what the machine is, what every watched frame's locals held, and the last
 * NYA_LOG_RING_MAX log lines, which is the part that usually says what the program was actually doing.
 *
 * The watched values come from base_watch.h's per thread ring, which a function opts into with an
 * annotation; see build/pp/watch.h. The walk is part of composing the report, so it runs on the fault
 * path too, where it neither allocates nor takes a lock: the ring is fixed storage belonging to the
 * thread that crashed.
 *
 * For everything but a fault the report is shown in a window with Close, Copy and Send to developer,
 * unless nobody is there to press one: a test build or a headless app writes the file instead.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Why an SDL window of its own rather than nya_window_create and the UI module
 *
 * The engine's own UI would be the nicer answer and it is not a safe one. It draws through the SDL_GPU
 * device, and the most common thing to crash inside is that device or the code feeding it: a lost device,
 * a command buffer half recorded, a pipeline that failed to build. It also needs the frame loop, the
 * asset system for its font, the arena the frame allocator hands out, the config and the callback
 * registry, and a crash reporter that needs six subsystems alive cannot report the crash that took one
 * of them down. Worse, a fault inside the reporter hits base_logging.h's reentrancy guard and the process
 * dies immediately with nothing shown at all.
 *
 * So the window is SDL_CreateWindowAndRenderer plus SDL_RenderDebugText: the 2D renderer, which is a
 * separate backend from SDL_GPU and creates cleanly beside a broken device, and SDL's built in 8x8 font,
 * which loads no asset. It still gives a real window with three buttons and a scrollable log, and it
 * depends on nothing the engine had to have got right. If even that fails to create, the report goes to
 * a file and to stderr rather than nowhere.
 *
 * Why a hardware fault gets no window
 *
 * A fault arrives in a signal handler, on the thread that faulted. Calling into SDL from there can
 * deadlock against a lock that same thread already holds, which is not hypothetical when the fault came
 * out of the video driver, and a hang shows the player nothing at all and cannot be dismissed. So a fault
 * writes the report out and names the file on stderr; every other source, where we are on an ordinary
 * stack with the process intact, opens the window.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_logging.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/base/base_watch.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Largest report that can be composed, statically allocated once.
 *
 * The log ring is the part that grows: NYA_LOG_RING_MAX lines of up to NYA_LOG_RING_LINE_MAX bytes is
 * 64 KiB by itself, and the header, the build and platform block and a 64 frame stack trace add about
 * 12 KiB on top. The watched values are the other ring, so they are counted the same way: a full one is
 * NYA_WATCH_RING_MAX lines of a value and the name and type in front of it.
 *
 * A report that would overflow is truncated with a line saying so, never silently cut.
 * */
#define NYA_CRASH_REPORT_MAX_BYTES \
    ((NYA_LOG_RING_MAX * NYA_LOG_RING_LINE_MAX) + (NYA_WATCH_RING_MAX * (NYA_WATCH_VALUE_MAX + 64)) + (16 * 1024))

/**
 * Lines the window can index for scrolling. The report is line oriented and the ring is the bulk of it,
 * so this is both rings plus the fixed blocks around them, rounded up.
 * */
#define NYA_CRASH_REPORT_LINE_MAX (NYA_LOG_RING_MAX + NYA_WATCH_RING_MAX + 128)

/** Longest path a written report can have, terminator included. */
#define NYA_CRASH_REPORT_PATH_MAX (NYA_LOG_DIRECTORY_MAX + 64)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Registers the crash observer. Costs nothing until something crashes: no thread, no timer, no work per
 * frame, and the observer is one function pointer in a list the funnel only walks on the way out.
 *
 * Call it after the log file is open, so a report written from here lands beside the day's log.
 * */
NYA_API NYA_Error nya_crash_reporter_init(void) __attr_no_discard;

/** Removes the observer. Safe to call when it was never registered. */
NYA_API void nya_crash_reporter_deinit(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * REPORT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Renders `info` plus build, platform and log context into `buffer` as null terminated text, and returns
 * the bytes written excluding the terminator.
 *
 * Touches no allocator and no stdio, so the crash path can call it with the heap already broken. The
 * caller owns the buffer; NYA_CRASH_REPORT_MAX_BYTES is the size that never truncates.
 * */
NYA_API u32 nya_crash_report_compose(const NYA_CrashInfo* info, OUT u8* buffer, u32 capacity);

/**
 * Redacts the machine's identity from an already composed report, in place, and returns the new length.
 *
 * `home` becomes `~`, and `user` and `host` become `[user]` and `[host]`, everywhere they occur: the
 * home directory is the prefix of every source path the stack trace carries, and the user and host
 * names turn up in log lines and in paths the home prefix did not cover. `home` is replaced first and
 * `user` last, because the home directory holds the user name inside it (`/home/<user>`) and a bare-name
 * pass run first would leave that path half redacted.
 *
 * A replacement shorter than what it covers closes the gap; a longer one opens it, and the single case
 * where a full buffer cannot grow overwrites the match in place rather than leaving it, so an identity
 * is never left behind. Any of the three may be null or empty, which skips it. Touches no allocator and
 * no lock, so the crash path calls it over the same fixed buffer the report was composed into.
 *
 * nya_crash_report_compose already runs this before it returns, so the report the window shows and the
 * file "Send" writes is the scrubbed one and there is no unredacted copy anywhere. It is exposed for the
 * caller that composes a report some other way, and so a test can hold it to account against known values.
 * */
NYA_API u32 nya_crash_report_scrub(OUT u8* buffer, u32 length, u32 capacity, NYA_ConstCString home, NYA_ConstCString user, NYA_ConstCString host);

/**
 * Hands the report to the developer, and writes where it went into `out_path`.
 *
 * Today that means a file under nya_log_directory, because the engine sends nothing anywhere without
 * being asked: no endpoint is compiled in and none is reachable from here. This is the single function a
 * transport would replace, so nothing above it, the window's button included, has to know the difference.
 * */
NYA_API NYA_Error nya_crash_report_submit(NYA_ConstCString report, OUT u8* out_path, u32 path_capacity) __attr_no_discard;

/**
 * Opens the crash window on `report` and blocks until the player closes it. `info` fills the header band,
 * the report fills the scrolling pane, and both buttons hand over the report verbatim.
 *
 * Returns immediately when there is no video subsystem to open a window on, which is every headless build
 * and every test.
 * */
NYA_API void nya_crash_window_show(const NYA_CrashInfo* info, NYA_ConstCString report);
