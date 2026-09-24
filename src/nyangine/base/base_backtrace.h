/**
 * @file base_backtrace.h
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

#define NYA_BACKTRACE_DEPTH_MAX 64

/**
 * True when a real symbolizing backend is compiled in. Degrades on purpose: if libbacktrace hasn't
 * been built yet the engine still compiles and runs, it just captures nothing. Define
 * NYA_NO_BACKTRACE to force the null backend.
 * */
// spelled 1/0 because base_basic.h defines true/false as ((b8)1)/((b8)0), which #if cannot evaluate.
#if (OS_LINUX || OS_WINDOWS) && __has_include("backtrace.h") && !defined(NYA_NO_BACKTRACE)
#define NYA_BACKTRACE_SUPPORTED 1
#else
#define NYA_BACKTRACE_SUPPORTED 0
#endif

// TYPES

typedef struct NYA_BacktraceFrame NYA_BacktraceFrame;
typedef struct NYA_Backtrace      NYA_Backtrace;

/**
 * A single resolved stack frame. `function` and `file` point into libbacktrace's debug info, which
 * lives as long as the process. Not owned; never free them.
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

// FUNCTIONS AND MACROS

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
 * Walks the current stack into `out_backtrace`. `skip` drops that many innermost frames so the
 * listing starts at the interesting call site rather than inside the capture machinery; the caller
 * of this function is frame 0.
 * */
NYA_API void nya_backtrace_capture(OUT NYA_Backtrace* out_backtrace, u32 skip);

/**
 * Renders a captured stack into `buffer` as newline-terminated text, null-terminating when
 * `capacity` is non-zero, and returns bytes written excluding the terminator. Touches no allocator
 * and no stdio, so it is safe to call from a signal handler.
 * */
NYA_API u32 nya_backtrace_format(const NYA_Backtrace* backtrace, OUT u8* buffer, u32 capacity);
