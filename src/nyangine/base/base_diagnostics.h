/**
 * @file base_diagnostics.h
 *
 * Where diagnostics go: the log file, and the observers notified on a crash.
 *
 * Separate from base_logging.h because these report failure with NYA_Error, and base_error.h sits
 * *above* base_logging.h in the include order — the crash sink has to be usable by the error type
 * itself, so it cannot name it. Everything here is called during setup rather than on the crash
 * path, so it is free to depend on the richer type.
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
 * Starts writing the log to `path` as well, buffered.
 *
 * Buffered because logging is on hot paths and a write syscall per line is not free. The buffer is
 * flushed when it fills, whenever a WARN or worse is logged, on shutdown, and on the way out of a
 * crash, so the interesting lines are never the ones still sitting in memory.
 *
 * Uses raw file descriptors rather than stdio: the crash sink flushes this from contexts where
 * stdio is not safe to touch.
 *
 * Returns false if the file could not be opened. Passing nullptr disables file logging.
 * */
NYA_API NYA_Error nya_log_file_open(NYA_ConstCString path) __attr_no_discard;

/** Flushes and closes the log file. Safe to call when no file is open. */
NYA_API void nya_log_file_close(void);

/** Writes out whatever is buffered. Safe to call from a signal handler. */
NYA_API void nya_log_file_flush(void);

/**
 * Adds a crash observer. Notified in registration order.
 *
 * Returns an error once NYA_CRASH_OBSERVER_MAX are registered rather than dropping the
 * registration quietly: an observer that was never installed means a crash report that never gets
 * written, and that is not something to discover after the crash.
 * */
NYA_API NYA_Error nya_crash_observer_add(NYA_CrashObserver observer, void* user_data) __attr_no_discard;
NYA_API void      nya_crash_observer_clear(void);
