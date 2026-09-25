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

/**
 * The environment variable that names where crash reports are sent, read by nya_crash_reports_flush on an
 * ordinary run — never on the crash path. Unset or empty means nowhere: reports stay the local files
 * nya_crash_report_submit wrote, and nothing leaves the machine. Kept in the environment rather than
 * compiled in so the destination is the operator's to set and a build ships pointing at nobody.
 * */
#define NYA_CRASH_REPORT_ENDPOINT_ENV "NYA_CRASH_REPORT_ENDPOINT"

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
 * Always a file under nya_log_directory: submitting persists the report where the player can find it and
 * where nya_crash_reports_flush picks it up on a later run. This runs on the crash path, a signal handler
 * included, so it touches no allocator and no network — only the raw file write below it. The network step
 * is deliberately a separate, later thing; see nya_crash_reports_flush.
 * */
NYA_API NYA_Error nya_crash_report_submit(NYA_ConstCString report, OUT u8* out_path, u32 path_capacity) __attr_no_discard;

/**
 * How a persisted crash report reaches its endpoint. POSTs `report_size` bytes of `report` (the same text
 * the file holds) to `url`, and returns NYA_OK only when the endpoint accepted it — a 2xx. Anything else,
 * a refused connection or a non-2xx, is an error, and the report is kept on disk for the next run.
 *
 * The reporter is module `debug` and does not include a network transport, so a program that wants reports
 * sent supplies one of these over whatever HTTP client it links — the curl plugin, most likely. The
 * transport owns the round trip and the timeout; this module owns the file's lifetime around it.
 * */
typedef NYA_Error (*NYA_CrashReportTransportFn)(void* userdata, NYA_ConstCString url, const u8* report, u64 report_size);

/**
 * Submits every crash report left on disk from an earlier run to the endpoint the environment names, and
 * deletes each one the endpoint accepted. Returns how many were sent.
 *
 * Off by default: with NYA_CRASH_REPORT_ENDPOINT_ENV unset or empty, or `transport` null, this does
 * nothing and every report stays the local file it already is — the engine sends nothing anywhere the
 * operator did not point it. What travels is exactly the file: a backtrace, the build id, the machine's
 * kind and the tail of the log, already scrubbed of the home directory, user and host by compose. No
 * player data, no identity.
 *
 * Meant for an ordinary later startup, not the crash path: it allocates an arena, lists a directory and
 * makes a blocking round trip per report, none of which is safe from a signal handler and none of which
 * the crashing run does. A report the endpoint refuses is left on disk to try again next time.
 * */
NYA_API u32 nya_crash_reports_flush(NYA_CrashReportTransportFn transport, void* userdata);

/**
 * Opens the crash window on `report` and blocks until the player closes it. `info` fills the header band,
 * the report fills the scrolling pane, and both buttons hand over the report verbatim.
 *
 * Returns immediately when there is no video subsystem to open a window on, which is every headless build
 * and every test.
 * */
NYA_API void nya_crash_window_show(const NYA_CrashInfo* info, NYA_ConstCString report);
