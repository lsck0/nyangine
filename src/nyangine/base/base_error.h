/**
 * @file base_error.h
 *
 * Error handling and missing values. The convention is:
 * - Functions that can fail or not succeed return `NYA_Error` and use out-parameters for other return values.
 * - Functions that would return a simple success bool return either NYA_OK or NYA_NOT_OK.
 *
 * An error carries two different traces, and they answer different questions:
 * - the *error trace* is the propagation chain, one frame pushed per NYA_TRY. It shows how the
 *   error bubbled up, costs almost nothing, and works in release without debug info.
 * - the *stack trace* is where the failure physically happened, captured by libbacktrace when the
 *   error is created. Deep and precise, but it needs symbols and is only captured in debug and
 *   developer builds.
 *
 * `NYA_Error` is returned by value, which is fine for I/O shaped calls and wrong for hot loops. Hot
 * paths that can fail should return `b8` or a `Maybe` instead.
 *
 * Its size depends on NYA_ERROR_CAPTURE_STACK: 2520 bytes in debug and developer builds, 464 in
 * shipping ones, where the stack trace is compiled out rather than left empty. Note that a success
 * costs the same as a failure — NYA_OK is a compound literal of the entire struct, so returning it
 * zeroes all of that, and NYA_TRY copies it again into the caller's frame.
 *
 * Both figures are eight bytes larger than they were before `ok` was added, not one: it sits between
 * two four byte fields, so it brings three bytes of padding with it, and pushing `message` along by
 * four costs another four to realign the `error_trace` array behind it. Worth knowing before adding
 * another small field — there is room for three more bools beside this one for free.
 * */

#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_backtrace.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_template.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_ERROR_MESSAGE_MAX_LENGTH 192
#define NYA_ERROR_TRACE_MAX          8

/**
 * Whether creating an error also captures a stack trace. Unwinding is far too expensive to do on
 * every failed file open in a shipping build, so it is on only where it earns its cost.
 * */
#if NYA_EXECUTION_MODE == 0 || NYA_EXECUTION_MODE == 1
#define NYA_ERROR_CAPTURE_STACK 1
#else
#define NYA_ERROR_CAPTURE_STACK 0
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ErrorKind NYA_ErrorKind;
typedef struct NYA_Error   NYA_Error;

enum NYA_ErrorKind {
    NYA_ERROR_NONE,

    NYA_ERROR_NOT_OK,
    NYA_ERROR_NOT_FOUND,
    NYA_ERROR_PERMISSION_DENIED,
    NYA_ERROR_ALREADY_EXISTS,
    NYA_ERROR_INVALID_ARGUMENT,
    NYA_ERROR_OUT_OF_MEMORY,
    NYA_ERROR_IO,
    NYA_ERROR_TIMEOUT,
    NYA_ERROR_NOT_SUPPORTED,
    NYA_ERROR_CORRUPT,
    NYA_ERROR_PARSE,

    NYA_ERROR_COUNT,
};

struct NYA_Error {
    /**
     * What went wrong. **The authoritative field**; `ok` is derived from it.
     * */
    NYA_ErrorKind kind;

    /**
     * `kind == NYA_ERROR_NONE`, precomputed, so success reads as `if (!result.ok)`.
     *
     * Exactly the same question as comparing `kind` against NYA_ERROR_NONE, and both spellings stay
     * correct — this exists because the comparison is the overwhelmingly common thing to want and
     * spelling it out at every call site buries the interesting half of the line.
     *
     * **Never set this by hand.** It is redundant state, and the only thing keeping it honest is
     * that every NYA_Error in the tree is born in one of three places: NYA_OK, NYA_NOT_OK and
     * _nya_error_create. An error assembled with a designated initializer would come out `.ok =
     * false` regardless of its kind, because that is what zero means — which is why there is no
     * fourth place, and why `kind` rather than this is what the formatting and throwing paths read.
     * */
    b8 ok;

    /** How many NYA_TRY frames the error has bubbled through. Saturates at NYA_ERROR_TRACE_MAX. */
    u32 error_trace_count;

    u8 message[NYA_ERROR_MESSAGE_MAX_LENGTH];

    /** Propagation chain, innermost first. `address` is unused here. */
    NYA_BacktraceFrame error_trace[NYA_ERROR_TRACE_MAX];

#if NYA_ERROR_CAPTURE_STACK
    /**
     * Where the error was created.
     *
     * Compiled out entirely rather than merely left empty when the capture is off, because it is
     * 2056 of the struct's bytes and every NYA_Error is returned by value. A shipping build was
     * moving that field on every single error return, and zeroing it on every *success* return too,
     * since NYA_OK is a compound literal of the whole struct — for a field guaranteed to stay zero
     * in that mode.
     *
     * This is what makes the size note at the top of the file true: 2512 bytes with the capture on,
     * 456 without. It had said "around half a kilobyte" unconditionally, which described neither.
     *
     * The mode is uniform across an executable and the game DLL it loads — both come from the same
     * flag set, and only debug and developer builds hot reload at all — so the two never disagree
     * about this struct's layout.
     * */
    NYA_Backtrace stack_trace;
#endif // NYA_ERROR_CAPTURE_STACK
};

__attr_allow_unused static NYA_ConstCString NYA_ERRORKIND_NAME_MAP[NYA_ERROR_COUNT] = {
    [NYA_ERROR_NONE]              = "NONE",
    [NYA_ERROR_NOT_OK]            = "NOT_OK",
    [NYA_ERROR_NOT_FOUND]         = "NOT_FOUND",
    [NYA_ERROR_PERMISSION_DENIED] = "PERMISSION_DENIED",
    [NYA_ERROR_ALREADY_EXISTS]    = "ALREADY_EXISTS",
    [NYA_ERROR_INVALID_ARGUMENT]  = "INVALID_ARGUMENT",
    [NYA_ERROR_OUT_OF_MEMORY]     = "OUT_OF_MEMORY",
    [NYA_ERROR_IO]                = "IO",
    [NYA_ERROR_TIMEOUT]           = "TIMEOUT",
    [NYA_ERROR_NOT_SUPPORTED]     = "NOT_SUPPORTED",
    [NYA_ERROR_CORRUPT]           = "CORRUPT",
    [NYA_ERROR_PARSE]             = "PARSE",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * MISSING VALUES
 * ─────────────────────────────────────────────────────────
 */

/** Name of the optional type derived for `type`, e.g. NYA_Maybeᐸu64ᐳ. */
#define _nya_derive_maybe_name(type) nya_template(NYA_Maybe, type)

#define nya_derive_maybe(type)                                                                                                                       \
    typedef struct {                                                                                                                                 \
        b8   has_value;                                                                                                                              \
        type value;                                                                                                                                  \
    } _nya_derive_maybe_name(type)

#define nya_none(type)      ((_nya_derive_maybe_name(type)){ .has_value = false })
#define nya_some(type, val) ((_nya_derive_maybe_name(type)){ .has_value = true, .value = (val) })

nya_derive_maybe(b8);
nya_derive_maybe(b16);
nya_derive_maybe(b32);
nya_derive_maybe(b64);
nya_derive_maybe(b128);
nya_derive_maybe(u8);
nya_derive_maybe(u16);
nya_derive_maybe(u32);
nya_derive_maybe(u64);
nya_derive_maybe(u128);
nya_derive_maybe(s8);
nya_derive_maybe(s16);
nya_derive_maybe(s32);
nya_derive_maybe(s64);
nya_derive_maybe(s128);
nya_derive_maybe(f16);
nya_derive_maybe(f32);
nya_derive_maybe(f64);
nya_derive_maybe(f128);

/*
 * ─────────────────────────────────────────────────────────
 * ERROR HANDLING
 * ─────────────────────────────────────────────────────────
 */

// Kept as compound literals so both stay usable wherever an initializer is expected.
//
// `ok` is spelled out in both rather than left to the zero value, because it is derived from `kind`
// and the two must not disagree. See the field's note in NYA_Error.
#define NYA_OK     ((NYA_Error){ .kind = NYA_ERROR_NONE, .ok = true })
#define NYA_NOT_OK ((NYA_Error){ .kind = NYA_ERROR_NOT_OK, .ok = false })

/**
 * Usage:
 *
 * ```c
 * nya_error(NYA_ERROR_NOT_FOUND)
 * nya_error(NYA_ERROR_NOT_FOUND, message)
 * nya_error(NYA_ERROR_NOT_FOUND, format, ...) with a max. of 10 format arguments
 * ```
 * */
#define nya_error(...) _nya_error(__VA_ARGS__)

/** Reports `error` through the central crash sink and terminates. */
#define NYA_THROW(error) _nya_error_throw((error), __FUNCTION__, __FILE__, __LINE__, "")

/**
 * Propagates a failure to the caller, recording one error trace frame on the way out.
 *
 * Only valid inside a function that itself returns NYA_Error. Evaluates `expr` exactly once, and
 * returns only when it actually failed.
 * */
#define NYA_TRY(expr)                                                                                                                                \
    do {                                                                                                                                             \
        NYA_Error _nya_try_error = (expr);                                                                                                           \
        if (_nya_try_error.kind != NYA_ERROR_NONE) {                                                                                                 \
            _nya_error_push_frame(&_nya_try_error, __FUNCTION__, __FILE__, __LINE__);                                                                \
            return _nya_try_error;                                                                                                                   \
        }                                                                                                                                            \
    } while (0)

/**
 * Same as NYA_TRY, but for functions that do not return NYA_Error: returns `value` instead.
 * */
#define NYA_TRY_OR(expr, value)                                                                                                                      \
    do {                                                                                                                                             \
        NYA_Error _nya_try_error = (expr);                                                                                                           \
        if (_nya_try_error.kind != NYA_ERROR_NONE) {                                                                                                 \
            _nya_error_push_frame(&_nya_try_error, __FUNCTION__, __FILE__, __LINE__);                                                                \
            return (value);                                                                                                                          \
        }                                                                                                                                            \
    } while (0)

/**
 * Unwrap or die. Use where a failure means the program has no sensible way to continue.
 *
 * Usage:
 *
 * ```c
 * NYA_EXPECT(expr)
 * NYA_EXPECT(expr, message)
 * NYA_EXPECT(expr, format, ...) with a max. of 10 format arguments
 * ```
 * */
#define NYA_EXPECT(...)                                                                                                                                                                                    \
    _NYA_PICK_EXPECT(__VA_ARGS__, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT3, _NYA_EXPECT2, _NYA_EXPECT1)( \
        __VA_ARGS__                                                                                                                                                                                        \
    )

NYA_API NYA_Error nya_error_from_errno(void) __attr_no_discard;

/** Renders the propagation chain and the captured stack into `buffer`. Always null terminates. */
NYA_API u32 nya_error_format(const NYA_Error* error, OUT u8* buffer, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off
#define _nya_error(...)  _NYA_PICK_ERR(__VA_ARGS__, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR3, _NYA_ERROR2, _NYA_ERROR1)(__VA_ARGS__)
#define _NYA_PICK_ERR(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, NAME, ...) NAME
#define _NYA_ERROR1(code)              _nya_error_create(code, "")
#define _NYA_ERROR2(code, message)     _nya_error_create(code, "%s", message)
#define _NYA_ERROR3(code, format, ...) _nya_error_create(code, format, __VA_ARGS__)
// clang-format on

#define _NYA_PICK_EXPECT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, NAME, ...) NAME

#define _NYA_EXPECT1(expr)                                                                                                                           \
    do {                                                                                                                                             \
        NYA_Error _nya_expect_error = (expr);                                                                                                        \
        if (_nya_expect_error.kind != NYA_ERROR_NONE) _nya_error_throw(_nya_expect_error, __FUNCTION__, __FILE__, __LINE__, "");                     \
    } while (0)

#define _NYA_EXPECT2(expr, message)                                                                                                                  \
    do {                                                                                                                                             \
        NYA_Error _nya_expect_error = (expr);                                                                                                        \
        if (_nya_expect_error.kind != NYA_ERROR_NONE) _nya_error_throw(_nya_expect_error, __FUNCTION__, __FILE__, __LINE__, "%s", message);          \
    } while (0)

#define _NYA_EXPECT3(expr, format, ...)                                                                                                              \
    do {                                                                                                                                             \
        NYA_Error _nya_expect_error = (expr);                                                                                                        \
        if (_nya_expect_error.kind != NYA_ERROR_NONE) _nya_error_throw(_nya_expect_error, __FUNCTION__, __FILE__, __LINE__, format, __VA_ARGS__);    \
    } while (0)

NYA_API NYA_Error _nya_error_create(NYA_ErrorKind kind, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3) __attr_no_discard;
NYA_API void      _nya_error_throw(NYA_Error error, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString format, ...)
    __attr_fmt_printf(5, 6) __attr_noreturn;
NYA_API void _nya_error_push_frame(NYA_Error* error, NYA_ConstCString function, NYA_ConstCString file, u32 line);
