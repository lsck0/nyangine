/**
 * @file base_logging.h
 *
 * Logging and the central crash sink.
 *
 * Every abort path in the engine, an assertion, a panic, a thrown error or a hardware fault, ends
 * up in `_nya_crash_raise`. That single funnel is what makes telemetry, a crash report writer or an
 * in game crash overlay possible: register an `NYA_CrashObserver` and it sees all four.
 *
 * Observers cannot stop a crash. Preventing one is a testing facility only and lives in
 * base_test.h, compiled in exclusively when NYA_TESTING is defined.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_backtrace.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Roomy enough that a thrown error can carry its message and its full propagation trace. */
#define NYA_CRASH_MESSAGE_MAX_LENGTH 1024
#define NYA_CRASH_OBSERVER_MAX       8
#define NYA_LOG_SINK_MAX             8
#define NYA_LOG_MESSAGE_MAX_LENGTH   2048

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_LogLevel    NYA_LogLevel;
typedef enum NYA_CrashSource NYA_CrashSource;
typedef struct NYA_CrashInfo NYA_CrashInfo;

enum NYA_LogLevel {
    NYA_LOG_LEVEL_TRACE,
    NYA_LOG_LEVEL_DEBUG,
    NYA_LOG_LEVEL_INFO,
    NYA_LOG_LEVEL_WARN,
    NYA_LOG_LEVEL_ERROR,
    NYA_LOG_LEVEL_PANIC,
    NYA_LOG_LEVEL_COUNT,
};

enum NYA_CrashSource {
    /** A failed nya_assert. */
    NYA_CRASH_SOURCE_ASSERT,
    /** An explicit nya_panic. */
    NYA_CRASH_SOURCE_PANIC,
    /** An NYA_Error that reached NYA_THROW or NYA_EXPECT. */
    NYA_CRASH_SOURCE_ERROR,
    /** A hardware fault: SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT or the Windows equivalent. */
    NYA_CRASH_SOURCE_FAULT,

    NYA_CRASH_SOURCE_COUNT,
};

/**
 * Everything known about a crash, self contained and copyable. Holds no pointers into the crashing
 * frame, so it stays valid after the stack it came from is gone.
 * */
struct NYA_CrashInfo {
    NYA_CrashSource  source;
    NYA_ConstCString function;
    NYA_ConstCString file;
    u32              line;

    /** Signal number, or the Windows exception code. Only set for NYA_CRASH_SOURCE_FAULT. */
    s32 signal;
    /** Address that was touched. Only set for NYA_CRASH_SOURCE_FAULT. */
    u64 fault_address;
    /** NYA_ErrorKind as a plain integer. Only meaningful for NYA_CRASH_SOURCE_ERROR. */
    u32 error_kind;

    /**
     * True when running in async signal context, which is to say NYA_CRASH_SOURCE_FAULT.
     *
     * An observer seeing this MUST restrict itself to preallocated memory and write(2). No malloc,
     * no stdio, no windowing, no networking: the faulting thread may well hold the allocator lock,
     * and taking it again deadlocks the crash handler. Queue the report to a preopened file and
     * upload it on the next launch instead.
     * */
    b8 fault_path;

    u8            message[NYA_CRASH_MESSAGE_MAX_LENGTH];
    NYA_Backtrace backtrace;
};

/**
 * Receives every crash, whatever its source. Registered observers are notified in registration
 * order, just before the process dies.
 *
 * Returns void on purpose. Observers report, they never veto: letting production code swallow a
 * failed assertion would mean continuing on state already known to be corrupt.
 *
 * Must respect `info->fault_path`.
 * */
typedef void (*NYA_CrashObserver)(const NYA_CrashInfo* info, void* user_data);

/**
 * Receives every formatted log line. `message` is null terminated and owned by the caller.
 * */
typedef void (*NYA_LogSink)(NYA_LogLevel level, NYA_ConstCString message, u32 length, void* user_data);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Which level to use, and how to word it.
 *
 * The levels only mean something if they are applied the same way everywhere. A log where INFO
 * carries per asset chatter is a log nobody reads, and one where a hard failure arrives as a
 * warning is a log that hides the thing it was written to surface.
 *
 * - TRACE  every occurrence of something that happens constantly. Off outside deep debugging.
 * - DEBUG  per item detail: one line per asset, per entity, per request. Useful while working on
 *          that subsystem, noise otherwise.
 * - INFO   events a reader wants without asking. Subsystem boundaries, mode changes, one-per-run
 *          milestones. If it can fire more than a few times a second it is DEBUG.
 * - WARN   degraded, but continuing as designed. An optional capability is unavailable, a fallback
 *          was taken, a limit was reached. Nothing the caller asked for was lost.
 * - ERROR  an operation the caller explicitly asked for definitively failed, and the failure is
 *          being swallowed here rather than returned. If it is returned as an NYA_Error instead,
 *          do not also log it: the caller decides how loud it is.
 * - PANIC  nya_panic, which does not return.
 *
 * Message wording: capitalised, no trailing period, present tense. Error *messages* are the
 * exception and are deliberately the other way round — lowercase and unpunctuated — because
 * nya_error_format renders them as a clause after the kind, "NOT_FOUND, asset not found on disk",
 * and NYA_EXPECT context reads the same way, "while loading the shader".
 */
// clang-format off
#define nya_trace(format, ...)     _nya_log_message(NYA_LOG_LEVEL_TRACE, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_debug(format, ...)     _nya_log_message(NYA_LOG_LEVEL_DEBUG, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_info(format, ...)      _nya_log_message(NYA_LOG_LEVEL_INFO, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_warn(format, ...)      _nya_log_message(NYA_LOG_LEVEL_WARN, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_error(format, ...) _nya_log_message(NYA_LOG_LEVEL_ERROR, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_panic(format, ...)     _nya_crash_raise(NYA_CRASH_SOURCE_PANIC, __FUNCTION__, __FILE__, __LINE__, 0, format __VA_OPT__(, __VA_ARGS__))
// clang-format on

NYA_API NYA_LogLevel nya_log_level_get(void);
NYA_API void         nya_log_level_set(NYA_LogLevel level);

/** Adds a log sink. Sinks are notified in registration order. Silently ignored once full. */
NYA_API void nya_log_sink_add(NYA_LogSink sink, void* user_data);
NYA_API void nya_log_sink_clear(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API void _nya_log_message(NYA_LogLevel level, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString format, ...)
    __attr_fmt_printf(5, 6);

/**
 * The central crash sink. Never returns: it either longjmps back to a test that armed prevention,
 * or it terminates the process.
 * */
NYA_API void
_nya_crash_raise(NYA_CrashSource source, NYA_ConstCString function, NYA_ConstCString file, u32 line, u32 error_kind, NYA_ConstCString format, ...)
    __attr_fmt_printf(6, 7) __attr_noreturn;

/**
 * Same, but reports `backtrace` instead of capturing one here.
 *
 * A thrown NYA_Error already carries the stack from where it was created, which is far more useful
 * than the stack of whatever finally threw it. Pass nullptr to capture at the call site as usual.
 * */
NYA_API void _nya_crash_raise_with_backtrace(
    NYA_CrashSource      source,
    NYA_ConstCString     function,
    NYA_ConstCString     file,
    u32                  line,
    u32                  error_kind,
    const NYA_Backtrace* backtrace,
    NYA_ConstCString     format,
    ...
) __attr_fmt_printf(7, 8) __attr_noreturn;

/** Fault entry point into the same sink. Called from the signal handler and the Windows filter. */
NYA_API void _nya_crash_raise_fault(s32 signal, u64 fault_address) __attr_noreturn;

#ifdef NYA_TESTING
/** Arms crash prevention, returning the frame it replaced so it can be restored. Testing only. */
NYA_API jmp_buf* _nya_crash_prevent_push(jmp_buf* jmp);
/** Restores a previously armed frame. Testing only. */
NYA_API void     _nya_crash_prevent_pop(jmp_buf* previous);
#endif // NYA_TESTING
