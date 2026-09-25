/**
 * @file base_logging.h
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_backtrace.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** Roomy enough that a thrown error can carry its message and its full propagation trace. */
#define NYA_CRASH_MESSAGE_MAX_LENGTH 1024
#define NYA_CRASH_OBSERVER_MAX       8
#define NYA_LOG_SINK_MAX             8
#define NYA_LOG_MESSAGE_MAX_LENGTH   2048

/** A tag names what a thread is working on, such as a request id, so it is short: 32 bytes with the terminator. */
#define NYA_LOG_TAG_MAX_LENGTH 32

/**
 * Typed fields one record carries beside its message.
 *
 * A record is a message plus a handful of key/value pairs, so a sink can render the same event as a human
 * line or as one JSON object. Sixteen is more than any call site here reaches; a call that passes more is
 * refused rather than truncated, so the fields a record carries are the fields the caller wrote or none of
 * them, never a silent prefix. Fixed, so nothing on the log path allocates.
 * */
#define NYA_LOG_FIELD_MAX 16

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

// TYPES

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

typedef enum NYA_LogFieldKind NYA_LogFieldKind;
typedef struct NYA_LogField   NYA_LogField;
typedef struct NYA_LogRecord  NYA_LogRecord;

/** What a field carries. One small set, so a sink switches over it exhaustively and no field is untyped. */
enum NYA_LogFieldKind {
    NYA_LOG_FIELD_STRING,
    NYA_LOG_FIELD_INT,
    NYA_LOG_FIELD_FLOAT,
    NYA_LOG_FIELD_BOOL,
};

/**
 * One typed key/value pair on a record. A value, not a builder: a call site names its fields inline with
 * the nya_log_str / _int / _float / _bool constructors and the log macro gathers them into one array, so
 * there is no per-call-site machinery and nothing to free. The key, and a string value, are borrowed for
 * the duration of the log call exactly as `message` is; a sink that keeps them past the call copies them.
 * */
struct NYA_LogField {
    NYA_ConstCString key;
    NYA_LogFieldKind kind;

    union {
        NYA_ConstCString as_string;
        s64              as_int;
        f64              as_float;
        b8               as_bool;
    };
};

/**
 * A message plus its typed fields, handed to a record sink so the sink decides the rendering. The engine's
 * own path renders the human form itself — the header, the message, then `key=value` pairs — and that is
 * what stderr, the file and the crash ring hold. A record sink is the seam where another shape lives, such
 * as one JSON object per line for a machine; see nya_log_record_render_json.
 *
 * Every pointer here is borrowed for the duration of the call and owned by the caller, as NYA_LogSink's
 * `message` is. `tag` is the current thread's log tag, or "" when there is none.
 * */
struct NYA_LogRecord {
    NYA_LogLevel     level;
    NYA_ConstCString function;
    NYA_ConstCString file;
    u32              line;
    NYA_ConstCString tag;
    NYA_ConstCString message;

    const NYA_LogField* fields;
    u32                 field_count;

    /** True when the call named more than NYA_LOG_FIELD_MAX fields, so none of them are carried. */
    b8 fields_overflowed;
};

/** Receives every record, message and typed fields together, so the sink alone decides how it reads. */
typedef void (*NYA_LogRecordSink)(const NYA_LogRecord* record, void* user_data);

// FUNCTIONS AND MACROS

// Which level to use, and how to word it.
// clang-format off
#define nya_log_trace(format, ...) _nya_log_message(NYA_LOG_LEVEL_TRACE, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_debug(format, ...) _nya_log_message(NYA_LOG_LEVEL_DEBUG, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_info(format, ...)  _nya_log_message(NYA_LOG_LEVEL_INFO,  __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_warn(format, ...)  _nya_log_message(NYA_LOG_LEVEL_WARN,  __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_error(format, ...) _nya_log_message(NYA_LOG_LEVEL_ERROR, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_log_panic(format, ...) _nya_crash_raise(NYA_CRASH_SOURCE_PANIC, __FUNCTION__, __FILE__, __LINE__, 0, format __VA_OPT__(, __VA_ARGS__))
// clang-format on

/* Typed fields, named inline where a record is logged: nya_log_int("count", n) builds one NYA_LogField, and the nya_log_*_fields macros gather however many are named into one record. */
// clang-format off
#define nya_log_str(k, v)   ((NYA_LogField){ .key = (k), .kind = NYA_LOG_FIELD_STRING, .as_string = (v) })
#define nya_log_int(k, v)   ((NYA_LogField){ .key = (k), .kind = NYA_LOG_FIELD_INT,    .as_int    = (s64)(v) })
#define nya_log_float(k, v) ((NYA_LogField){ .key = (k), .kind = NYA_LOG_FIELD_FLOAT,  .as_float  = (f64)(v) })
#define nya_log_bool(k, v)  ((NYA_LogField){ .key = (k), .kind = NYA_LOG_FIELD_BOOL,   .as_bool   = (b8)(v) })
// clang-format on

/* Logs one record: a fixed event-name message plus the typed fields it carries, so a machine sink can key on the message and read the fields as columns; the fields are a compound literal evaluated once. */
// clang-format off
#define _nya_log_fields_at(level, message, ...)                                                                        \
    _nya_log_fields((level), __FUNCTION__, __FILE__, __LINE__, (message), (const NYA_LogField[]){ __VA_ARGS__ },        \
                    (u32)(sizeof((const NYA_LogField[]){ __VA_ARGS__ }) / sizeof(NYA_LogField)))

#define nya_log_trace_fields(message, ...) _nya_log_fields_at(NYA_LOG_LEVEL_TRACE, message, __VA_ARGS__)
#define nya_log_debug_fields(message, ...) _nya_log_fields_at(NYA_LOG_LEVEL_DEBUG, message, __VA_ARGS__)
#define nya_log_info_fields(message, ...)  _nya_log_fields_at(NYA_LOG_LEVEL_INFO,  message, __VA_ARGS__)
#define nya_log_warn_fields(message, ...)  _nya_log_fields_at(NYA_LOG_LEVEL_WARN,  message, __VA_ARGS__)
#define nya_log_error_fields(message, ...) _nya_log_fields_at(NYA_LOG_LEVEL_ERROR, message, __VA_ARGS__)
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

/**
 * Adds a record sink, notified in registration order with the message and its typed fields together, so it
 * alone decides the rendering. The human line still goes to stderr, the file and the ring whether or not a
 * record sink is registered; a record sink is where a second shape such as JSON lines lives beside it.
 * Silently ignored once full, bounded by NYA_LOG_SINK_MAX as the line sinks are.
 * */
NYA_API void nya_log_record_sink_add(NYA_LogRecordSink sink, void* user_data);

/** Removes the record sink registered with exactly this callback and user data. False when it was not there. */
NYA_API b8 nya_log_record_sink_remove(NYA_LogRecordSink sink, void* user_data);

/**
 * Renders `record` as one JSON object into `out`, terminated and never past `capacity`, returning the length
 * written. The object carries `level`, `function`, `file`, `line`, the `tag` when there is one, `message`,
 * and one member per field under its own key. This is the machine shape a JSON record sink writes a line of;
 * it allocates nothing, so it is safe on the log path and from the crash path.
 *
 * A field key that collides with a built-in member, or with another field, is written as its own member all
 * the same: this is a rendering, not a map, and a reader taking the last wins loses nothing a record sink was
 * trusted to keep whole. Overflow is bounded, not undefined: a record that does not fit is cut at the last
 * whole member and closed, so the line is always valid JSON.
 * */
NYA_API u32 nya_log_record_render_json(const NYA_LogRecord* record, OUT char* out, u64 capacity);

/**
 * Tags every line this thread logs until cleared, as `[LEVEL] [tag] function (...)`. The HTTP server
 * sets a request's id here while serving it, so every line the request causes can be found by it,
 * including the ring a crash report prints. One tag, not a stack: whoever sets it clears it. Copied, and
 * cut at NYA_LOG_TAG_MAX_LENGTH - 1 bytes.
 * */
NYA_API void nya_log_tag_set(NYA_ConstCString tag);
NYA_API void nya_log_tag_clear(void);

/** The current thread's tag, or "" when there is none. */
NYA_API NYA_ConstCString nya_log_tag_get(void) __attr_no_discard;

// RING

/* The last NYA_LOG_RING_MAX lines, kept so a crash report can print what led to it; always on and filtered no further than nya_log_level_set, since a dropped line cannot be recovered. */

/** Lines held, at most NYA_LOG_RING_MAX. */
NYA_API u32 nya_log_ring_count(void) __attr_no_discard;

/** Line `index`, oldest first. Null past the count. Points into the ring, so copy it before logging again. */
NYA_API NYA_ConstCString nya_log_ring_at(u32 index) __attr_no_discard;

/** The level line `index` was logged at. NYA_LOG_LEVEL_COUNT past the count. */
NYA_API NYA_LogLevel nya_log_ring_level_at(u32 index) __attr_no_discard;

/** Drops everything the ring holds. */
NYA_API void nya_log_ring_clear(void);

// INTERNALS

NYA_API void _nya_log_message(NYA_LogLevel level, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString format, ...)
    __attr_fmt_printf(5, 6);

/**
 * What the nya_log_*_fields macros call. `message` is plain text, not a format, since the fields carry the
 * variables. Composes the human line for stderr, the file and the ring, then hands the record to every
 * record sink. `count` past NYA_LOG_FIELD_MAX is refused, not truncated.
 * */
NYA_API void
_nya_log_fields(NYA_LogLevel level, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString message, const NYA_LogField* fields, u32 count);

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
