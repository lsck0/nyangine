/**
 * @file base_backtrace.h
 *
 * Stack capture and hardware fault interception.
 *
 * Backed by libbacktrace on Linux and Windows. Platforms without a backend (WASM, unknown) get a
 * null implementation that captures nothing, so every call site stays valid.
 *
 * `nya_backtrace_init` must run before anything else in the process. It creates the symbolization
 * state, warms it, installs an alternate signal stack and hooks the fault signals. Every fault it
 * catches is forwarded to the central crash sink in base_logging.h.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_BACKTRACE_DEPTH_MAX 64

/**
 * True when a real symbolizing backend is compiled in.
 *
 * Degrades on purpose: if libbacktrace has not been built yet the engine still compiles and runs,
 * it just captures nothing. Define NYA_NO_BACKTRACE to force the null backend.
 * */
// Spelled 1/0 rather than true/false: base_basic.h defines those as ((b8)1) and ((b8)0), which a
// preprocessor #if cannot evaluate.
//
// The NYA_NO_BACKTRACE term was documented above but missing from this condition, so defining the
// macro did nothing at all and the only way to get the null backend was to not have built
// libbacktrace. That is the opposite of an override: it made the fallback reachable by accident and
// unreachable on purpose.
#if (OS_LINUX || OS_WINDOWS) && __has_include("backtrace.h") && !defined(NYA_NO_BACKTRACE)
#define NYA_BACKTRACE_SUPPORTED 1
#else
#define NYA_BACKTRACE_SUPPORTED 0
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_BacktraceFrame NYA_BacktraceFrame;
typedef struct NYA_Backtrace      NYA_Backtrace;

/**
 * A single resolved stack frame.
 *
 * `function` and `file` point into libbacktrace's own debug info mapping, which lives as long as
 * the process does. They are never owned by the frame and must not be freed.
 * */
struct NYA_BacktraceFrame {
    u64              address;
    NYA_ConstCString function;
    NYA_ConstCString file;
    u32              line;
};

/**
 * A captured stack. Roughly 1.5 KiB, so always pass it by pointer.
 * */
struct NYA_Backtrace {
    u32                count;
    NYA_BacktraceFrame frames[NYA_BACKTRACE_DEPTH_MAX];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Prepares symbolization and installs the fault handlers. Call this first, before any other engine
 * subsystem, and before spawning threads. Calling it twice is a no-op.
 * */
NYA_API void nya_backtrace_init(void);

/**
 * Restores the default fault handlers. Symbolization state is intentionally kept, since a crash
 * during shutdown should still produce a readable trace.
 * */
NYA_API void nya_backtrace_deinit(void);

/**
 * Walks the current stack into `out_backtrace`.
 *
 * `skip` drops that many innermost frames so the listing starts at the interesting call site
 * rather than inside the capture machinery itself. The caller of this function is frame 0.
 * */
NYA_API void nya_backtrace_capture(OUT NYA_Backtrace* out_backtrace, u32 skip);

/**
 * Renders a captured stack into `buffer` as newline terminated text. Always null terminates when
 * `capacity` is non zero. Returns the number of bytes written, excluding the terminator.
 *
 * Touches no allocator and no stdio, so it is safe to call from a signal handler.
 * */
NYA_API u32 nya_backtrace_format(const NYA_Backtrace* backtrace, OUT u8* buffer, u32 capacity);
