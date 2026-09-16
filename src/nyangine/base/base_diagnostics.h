/**
 * @file base_diagnostics.h
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts writing the log to `path` as well, buffered — a write syscall per line is not free on hot
 * paths. Flushed when full, on WARN or worse, on shutdown, and on the way out of a crash, so the
 * interesting lines are never still sitting in memory. Uses raw file descriptors rather than stdio
 * since the crash sink flushes this from contexts where stdio is unsafe to touch.
 * */
NYA_API NYA_Error nya_log_file_open(NYA_ConstCString path) __attr_no_discard;

/** Flushes and closes the log file. Safe to call when no file is open. */
NYA_API void nya_log_file_close(void);

/** Where the engine puts its daily log files, and how many days of them it keeps. */
#ifndef NYA_LOG_DIRECTORY
#define NYA_LOG_DIRECTORY "./logs"
#endif
#ifndef NYA_LOG_RETENTION_DAYS
#define NYA_LOG_RETENTION_DAYS 14
#endif

/**
 * Starts writing the log into `directory`, one file per day, named `YYYY-MM-DD.log`. Creates the
 * directory if needed and opens today's file in append mode, so a second run the same day continues
 * rather than truncating — the case a crash causes.
 * */
NYA_API NYA_Error nya_log_directory_open(NYA_ConstCString directory, u32 retention_days) __attr_no_discard;

/**
 * Switches to the new day's file if the UTC date has changed since the last call; cheap, and does
 * nothing until needed. Called once per frame — otherwise a long-running server would put a week
 * into whichever file happened to be open when it started.
 * */
NYA_API void nya_log_directory_roll(void);

/** Writes out whatever is buffered. Safe to call from a signal handler. */
NYA_API void nya_log_file_flush(void);

/**
 * Adds a crash observer, notified in registration order. Returns an error once
 * NYA_CRASH_OBSERVER_MAX are registered rather than dropping it quietly — a report that never gets
 * written is not something to discover after the crash.
 * */
NYA_API NYA_Error nya_crash_observer_add(NYA_CrashObserver observer, void* user_data) __attr_no_discard;

/**
 * Removes the observer registered with exactly this callback and user data. False when it was not there.
 *
 * The partner `nya_crash_observer_clear` is not: clear drops every observer, including whatever the crash
 * reporter installed at startup, so a caller taking its own overlay down with clear takes the report with
 * it. Matched on the pair, because one callback registered twice with different user data is two observers.
 * */
NYA_API b8 nya_crash_observer_remove(NYA_CrashObserver observer, void* user_data);

NYA_API void      nya_crash_observer_clear(void);
