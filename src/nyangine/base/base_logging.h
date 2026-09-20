/**
 * @file base_logging.h
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

/**
 * Lines the ring holds, and how much of each it keeps.
 *
 * The ring is what a crash report prints as "what the program was doing", so it is sized by how far back
 * that has to reach rather than by how much a log file holds. A frame that loads a level logs a few dozen
 * lines, so 256 covers the seconds before a crash without covering a whole session; 256 bytes per line
 * takes the header (`[LEVEL] function (file:line): `) plus a sentence, which is every line the engine
 * emits bar a formatted stack trace, and those are rendered by the crash path itself.
 *
 * Statically allocated at 256 * 264 bytes, about 66 KiB, so there is no allocation on the log path and
 * none on the crash path either.
 * */
#ifndef NYA_LOG_RING_MAX
#define NYA_LOG_RING_MAX 256
#endif

#ifndef NYA_LOG_RING_LINE_MAX
#define NYA_LOG_RING_LINE_MAX 256
#endif

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
    /** An explicit nya_log_panic. */
    NYA_CRASH_SOURCE_PANIC,
    /** An NYA_Error that reached NYA_THROW or NYA_EXPECT. */
    NYA_CRASH_SOURCE_ERROR,
    /** A hardware fault: SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT or the Windows equivalent. */
    NYA_CRASH_SOURCE_FAULT,

    NYA_CRASH_SOURCE_COUNT,
};

/** What a crash calls itself, in a log line and at the top of a crash report. */
__attr_allow_unused static NYA_ConstCString NYA_CRASH_SOURCE_NAME_MAP[NYA_CRASH_SOURCE_COUNT] = {
    [NYA_CRASH_SOURCE_ASSERT] = "ASSERTION FAILED",
    [NYA_CRASH_SOURCE_PANIC]  = "PANIC",
    [NYA_CRASH_SOURCE_ERROR]  = "ERROR THROWN",
    [NYA_CRASH_SOURCE_FAULT]  = "FAULT",
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
     * */
    b8 fault_path;

    u8            message[NYA_CRASH_MESSAGE_MAX_LENGTH];
    NYA_Backtrace backtrace;
};

/**
 * Receives every crash, whatever its source. Registered observers are notified in registration
 * order, just before the process dies.
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
 */
// clang-format off
#define nya_log_trace(format, ...) _nya_log_message(NYA_LOG_LEVEL_TRACE, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_debug(format, ...) _nya_log_message(NYA_LOG_LEVEL_DEBUG, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_info(format, ...)  _nya_log_message(NYA_LOG_LEVEL_INFO,  __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_warn(format, ...)  _nya_log_message(NYA_LOG_LEVEL_WARN,  __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_error(format, ...) _nya_log_message(NYA_LOG_LEVEL_ERROR, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_panic(format, ...) _nya_crash_raise(NYA_CRASH_SOURCE_PANIC, __FUNCTION__, __FILE__, __LINE__, 0, format __VA_OPT__(, __VA_ARGS__))
// clang-format on

NYA_API NYA_LogLevel nya_log_level_get(void);
NYA_API void         nya_log_level_set(NYA_LogLevel level);

/** Adds a log sink. Sinks are notified in registration order. Silently ignored once full. */
NYA_API void nya_log_sink_add(NYA_LogSink sink, void* user_data);

/**
 * Removes the sink registered with exactly this callback and user data. False when it was not there.
 *
 * The partner `nya_log_sink_clear` is not: clear drops *everyone's* sink, including the engine's own file
 * sink, so a caller taking its console window down with clear takes the log file with it. Matching on the
 * pair rather than the callback alone, because one callback registered twice with different user data is
 * two sinks.
 * */
NYA_API b8 nya_log_sink_remove(NYA_LogSink sink, void* user_data);

NYA_API void nya_log_sink_clear(void);

/*
 * ─────────────────────────────────────────────────────────
 * RING
 * ─────────────────────────────────────────────────────────
 */

/*
 * The last NYA_LOG_RING_MAX lines, kept so that whoever reports a crash can print what led to it. Always
 * on and never filtered further than nya_log_level_set already filters: a line the ring did not keep is a
 * line nobody can get back once the process is gone.
 *
 * ```c
 * for (u32 i = 0; i < nya_log_ring_count(); i++) printf("%s\n", nya_log_ring_at(i));
 * ```
 */

/** Lines held, at most NYA_LOG_RING_MAX. */
NYA_API u32 nya_log_ring_count(void) __attr_no_discard;

/** Line `index`, oldest first. Null past the count. Points into the ring, so copy it before logging again. */
NYA_API NYA_ConstCString nya_log_ring_at(u32 index) __attr_no_discard;

/** The level line `index` was logged at. NYA_LOG_LEVEL_COUNT past the count. */
NYA_API NYA_LogLevel nya_log_ring_level_at(u32 index) __attr_no_discard;

/** Drops everything the ring holds. */
NYA_API void nya_log_ring_clear(void);

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
