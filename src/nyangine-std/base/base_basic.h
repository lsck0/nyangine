/**
 * @file base_basic.h
 *
 * Most basic includes, macros, and (compile-time) mode/platform/compiler detection.
 * */
#pragma once

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <complex.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#if !defined(__wasm__) && !defined(__EMSCRIPTEN__)
// x86 SIMD intrinsics, x86-only: wasm has no <immintrin.h> and does not build the SIMD paths, so it is guarded off there while the native x86 build keeps this include exactly.
#include <immintrin.h>
#endif
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <memory.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stddefer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Before <tgmath.h>, and only on Windows.
#if defined(_WIN32) || defined(__CYGWIN__)
// mingw-w64 before 12 (Ubuntu 24.04 ships 11) pulls in clang's deprecated <mm3dnow.h>, whose #warning fails the build under -Werror.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-W#warnings"
#include <intrin.h>
#pragma clang diagnostic pop
#endif

#include <tgmath.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

// The only engine header here, and it has no includes of its own: the name maps below are file-scope tables every translation unit copies, so they need __attr_allow_unused.
#include "nyangine-std/base/base_attributes.h"

// DEBUG AND VERSION

#ifdef VERSION
#define NYA_VERSION VERSION
#else
#define NYA_VERSION "unknown"
#endif

/**
 * The commit the binary was built from, with `-dirty` appended when the tree had uncommitted changes.
 * Injected by hook_add_build_info_flag; "unknown" for anything built outside the build system, which
 * includes the bootstrap compile of the build tool itself.
 * */
#ifndef NYA_BUILD_COMMIT
#define NYA_BUILD_COMMIT "unknown"
#endif

#ifndef NYA_EXECUTION_MODE
#define NYA_EXECUTION_MODE 0
#endif

// The five execution modes, and what each is for.
#define NYA_DEBUG     (NYA_EXECUTION_MODE == 0)
#define NYA_DEVELOPER (NYA_EXECUTION_MODE == 1)
#define NYA_RELEASE   (NYA_EXECUTION_MODE == 2)
#define NYA_STEAM     (NYA_EXECUTION_MODE == 3)
#define NYA_TEST      (NYA_EXECUTION_MODE == 4)

/** Built to be worked on: hot reloading, filesystem assets, diagnostics that cost something. */
#define NYA_DEVELOPMENT_BUILD (NYA_DEBUG || NYA_DEVELOPER)

/** Built to be handed to someone else: bundled assets, integrity checked, no reload machinery. */
#define NYA_SHIPPING_BUILD (NYA_RELEASE || NYA_STEAM)

#ifdef NYA_NO_ASSERT
#error "NYA_NO_ASSERT is not supported: assertions stay enabled in every execution mode, shipping included."
#endif

/* Whether the game is loaded from a hot-swappable shared library; overridable so a test can compile the reload machinery in a mode that would otherwise leave it out. */
#ifndef NYA_CODE_HOT_RELOAD
#define NYA_CODE_HOT_RELOAD NYA_DEVELOPMENT_BUILD
#endif

/* Terminal: the engine draws into a grid of character cells instead of a swapchain; implies headless (no GPU, swapchain or 3D), the terminal backend replacing 2D drawing. See terminal.h. */
#ifdef NYA_TERMINAL
#define NYA_TERMINAL_ENABLED 1
#else
#define NYA_TERMINAL_ENABLED 0
#endif

// Headless: the engine runs, but nothing is drawn.
#if defined(NYA_HEADLESS) || NYA_TERMINAL_ENABLED
#define NYA_HEADLESS_ENABLED 1
#else
#define NYA_HEADLESS_ENABLED 0
#endif

typedef enum {
    NYA_EXECUTION_MODE_DEBUG     = 0,
    NYA_EXECUTION_MODE_DEVELOPER = 1,
    NYA_EXECUTION_MODE_RELEASE   = 2,
    NYA_EXECUTION_MODE_STEAM     = 3,

    /**
     * Built to be run by the test harness.
     * */
    NYA_EXECUTION_MODE_TEST = 4,

    NYA_EXECUTION_MODE_COUNT,
    NYA_EXECUTION_MODE_CURRENT = NYA_EXECUTION_MODE,
} NYA_ExecutionMode;
static_assert(NYA_EXECUTION_MODE_CURRENT < NYA_EXECUTION_MODE_COUNT, "Invalid execution mode");

/** What a build calls itself in a log line, a window title or a crash report. */
__attr_allow_unused static const char* const NYA_EXECUTION_MODE_NAME_MAP[NYA_EXECUTION_MODE_COUNT] = {
    [NYA_EXECUTION_MODE_DEBUG]     = "debug",
    [NYA_EXECUTION_MODE_DEVELOPER] = "dev",
    [NYA_EXECUTION_MODE_RELEASE]   = "release",
    [NYA_EXECUTION_MODE_STEAM]     = "steam",
    [NYA_EXECUTION_MODE_TEST]      = "test",
};

// COMPILER DETECTION

#ifdef _MSC_VER
#define COMPILER_MSVC 1
#elifdef __clang__
#define COMPILER_CLANG 1
#elifdef __GNUC__
#define COMPILER_GCC 1
#else
#define COMPILER_UNKNOWN 1
#endif

typedef enum {
    NYA_COMPILER_NULL,
    NYA_COMPILER_MSVC,
    NYA_COMPILER_CLANG,
    NYA_COMPILER_GCC,
    NYA_COMPILER_COUNT,
#if COMPILER_MSVC
    NYA_COMPILER_CURRENT = NYA_COMPILER_MSVC,
#elif COMPILER_CLANG
    NYA_COMPILER_CURRENT = NYA_COMPILER_CLANG,
#elif COMPILER_GCC
    NYA_COMPILER_CURRENT = NYA_COMPILER_GCC,
#else
    NYA_COMPILER_CURRENT = NYA_COMPILER_NULL,
#endif
} NYA_Compiler;

// PLATFORM DETECTION

#ifdef __wasm__
#define OS_WASM 1
#elif defined(_WIN32) || defined(__CYGWIN__)
#define OS_WINDOWS 1
#elifdef __linux__
#define OS_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define OS_MAC 1
#else
#define OS_UNKNOWN 1
#endif

typedef enum {
    NYA_OS_NULL,
    NYA_OS_WASM,
    NYA_OS_WINDOWS,
    NYA_OS_LINUX,
    NYA_OS_MAC,
    NYA_OS_COUNT,
#if OS_WASM
    NYA_OS_CURRENT = NYA_OS_WASM,
#elif OS_WINDOWS
    NYA_OS_CURRENT = NYA_OS_WINDOWS,
#elif OS_LINUX
    NYA_OS_CURRENT = NYA_OS_LINUX,
#elif OS_MAC
    NYA_OS_CURRENT = NYA_OS_MAC,
#else
    NYA_OS_CURRENT = NYA_OS_NULL,
#endif
} NYA_OperatingSystem;

// ARCHITECTURE DETECTION

#ifdef __wasm32__
#define ARCH_WASM32 1
#elifdef __wasm64__
#define ARCH_WASM64 1
#elif defined(_M_IX86) || defined(__i386__)
#define ARCH_X86 1
#elif defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
#define ARCH_X86_64 1
#elif defined(_M_ARM) || defined(__arm__)
#define ARCH_ARM32 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define ARCH_ARM64 1
#else
#define ARCH_UNKNOWN 1
#endif

typedef enum {
    NYA_ARCH_NULL,
    NYA_ARCH_WASM32,
    NYA_ARCH_WASM64,
    NYA_ARCH_X86,
    NYA_ARCH_X86_64,
    NYA_ARCH_ARM32,
    NYA_ARCH_ARM64,
    NYA_ARCH_COUNT,
#if ARCH_WASM32
    NYA_ARCH_CURRENT = NYA_ARCH_WASM32,
#elif ARCH_WASM64
    NYA_ARCH_CURRENT = NYA_ARCH_WASM64,
#elif ARCH_X86
    NYA_ARCH_CURRENT = NYA_ARCH_X86,
#elif ARCH_X86_64
    NYA_ARCH_CURRENT = NYA_ARCH_X86_64,
#elif ARCH_ARM32
    NYA_ARCH_CURRENT = NYA_ARCH_ARM32,
#elif ARCH_ARM64
    NYA_ARCH_CURRENT = NYA_ARCH_ARM64,
#else
    NYA_ARCH_CURRENT = NYA_ARCH_NULL,
#endif
} NYA_Architecture;

// ADDRESS SANITIZATION

// The header is checked separately from the feature: a toolchain may do -fsanitize=address yet ship its runtime headers uninstalled, so without the header the manual poisoning compiles out and ASan still works.
#if defined(__has_feature) && __has_feature(address_sanitizer) && __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#define ASAN_ENABLED                            true
#define ASAN_PADDING                            64 // bytes
#define asan_poison_memory_region(addr, size)   __asan_poison_memory_region(addr, size)
#define asan_unpoison_memory_region(addr, size) __asan_unpoison_memory_region(addr, size)
static_assert(ASAN_PADDING >= 0);
#else
#define ASAN_ENABLED                            false
#define ASAN_PADDING                            0 // bytes
#define asan_poison_memory_region(addr, size)   ((void)(addr), (void)(size))
#define asan_unpoison_memory_region(addr, size) ((void)(addr), (void)(size))
static_assert(ASAN_PADDING >= 0);
#endif // defined(__has_feature) && __has_feature(address_sanitizer)

// VISIBILITY AND LINKAGE

#if COMPILER_CLANG || COMPILER_GCC
#define NYA_INTERNAL __attribute__((visibility("hidden"))) static
#else
#define NYA_INTERNAL static
#endif

/**
 * Internal, except in a build that reloads code, where the name has to be findable.
 *
 * A callback registered with nya_callback is re-resolved by name after a code reload, and dlsym and
 * GetProcAddress cannot find a static or hidden symbol: a function the engine registers that way is
 * visible where a reload can happen and internal where one cannot. Use it for nothing else, since an
 * exported symbol is a link time GC root. See core_callback.h and main.c's update_callback_pointers.
 * */
#if NYA_CODE_HOT_RELOAD
#define NYA_INTERNAL_CALLBACK
#else
#define NYA_INTERNAL_CALLBACK NYA_INTERNAL
#endif

#ifdef __cplusplus
#define NYA_EXTERN extern "C"
#else
#define NYA_EXTERN extern
#endif

// Exported only where a hot-reloaded game DLL links against the executable: an export is a GC root, so a release exe exporting the whole API would keep every function the game never calls.
#if OS_WINDOWS && NYA_DEVELOPMENT_BUILD
#define NYA_API __declspec(dllexport) NYA_EXTERN
#elif OS_WINDOWS
#define NYA_API NYA_EXTERN
#else
#define NYA_API __attribute__((visibility("default"))) NYA_EXTERN
#endif

// OTHER CODEBASE MACROS

#define atomic       _Atomic
#define thread_local _Thread_local
#define true         ((b8)1)
#define false        ((b8)0)

#define OUT

// The impl nonsense is such that it concatenates not the symbol but the value after the preprocessor replaces the symbol.
#define CONCAT(a, b)       _CONCAT_IMPL(a, b)
#define _CONCAT_IMPL(a, b) a##b
