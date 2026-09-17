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
 * Also writes the log to `path`, buffered to avoid a syscall per line. Flushed when full, on WARN or
 * worse, on shutdown and on crash. Uses raw file descriptors because the crash sink flushes from
 * contexts where stdio is unsafe.
 * */
NYA_API NYA_Error nya_log_file_open(NYA_ConstCString path) __attr_no_discard;

/** Flushes and closes the log file. Safe to call when no file is open. */
NYA_API void nya_log_file_close(void);

/**
 * How many days of daily log files the engine keeps. They go to `logs` under the app's user data directory
 * (see nya_save_application), since an install directory is often not writable; -DNYA_LOG_DIRECTORY=\"./logs\"
 * pins them elsewhere.
 * */
#ifndef NYA_LOG_RETENTION_DAYS
#define NYA_LOG_RETENTION_DAYS 14
#endif

/**
 * Writes the log into `directory`, one `YYYY-MM-DD.log` per day. Creates the directory and appends to
 * today's file, so a restart after a crash continues it.
 * */
NYA_API NYA_Error nya_log_directory_open(NYA_ConstCString directory, u32 retention_days) __attr_no_discard;

/**
 * Switches to the new day's file when the UTC date has changed. Cheap. Call once per frame, or a long
 * running server logs a week into one file.
 * */
NYA_API void nya_log_directory_roll(void);

/** Writes out whatever is buffered. Safe to call from a signal handler. */
NYA_API void nya_log_file_flush(void);

/**
 * Adds a crash observer, notified in registration order. Fails once NYA_CRASH_OBSERVER_MAX are
 * registered, so a missing report is found before the crash.
 * */
NYA_API NYA_Error nya_crash_observer_add(NYA_CrashObserver observer, void* user_data) __attr_no_discard;

/**
 * Removes the observer registered with exactly this callback and user data. False when absent.
 *
 * The partner of add, unlike `nya_crash_observer_clear`, which also drops the crash reporter's own
 * observer. Matched on the pair, since one callback with two user data values is two observers.
 * */
NYA_API b8 nya_crash_observer_remove(NYA_CrashObserver observer, void* user_data);

NYA_API void      nya_crash_observer_clear(void);
