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
#include <immintrin.h>
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
#include <tgmath.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DEBUG AND VERSION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#ifdef VERSION
#define NYA_VERSION VERSION
#else
#define NYA_VERSION "unknown"
#endif

#ifdef GIT_COMMIT
#define NYA_GIT_COMMIT GIT_COMMIT
#else
#define NYA_GIT_COMMIT "unknown"
#endif

#ifndef NYA_EXECUTION_MODE
#define NYA_EXECUTION_MODE 0
#endif

/*
 * The five execution modes, and what each is for.
 *
 * | mode      | for                 | hot reload | sanitizers | arena debug | assets     |
 * |-----------|---------------------|------------|------------|-------------|------------|
 * | DEBUG     | finding bugs        | yes        | yes        | yes         | filesystem |
 * | DEVELOPER | playing while build |
 * |           | ing it              | yes        | no         | no          | filesystem |
 * | RELEASE   | shipping            | no         | no         | no          | blob       |
 * | STEAM     | shipping on Steam   | no         | no         | no          | blob       |
 * | TEST      | the test harness    | no         | yes        | yes         | filesystem |
 *
 * DEBUG and DEVELOPER are both development builds and both hot reload; DEBUG additionally carries
 * ASan and the arena's own memory debugging, which is what makes it slow enough to want DEVELOPER.
 *
 * RELEASE and STEAM are both deploy ready and differ only in that STEAM expects the Steam runtime
 * and links the Steam SDK.
 *
 * **Assertions are enabled in every mode, including the shipping ones.** A wrong assumption in a
 * player's hands is worth catching loudly rather than continuing into undefined behaviour, and the
 * crash sink turns it into a report. Nothing in the build defines NYA_NO_ASSERT; the static assert
 * below keeps it that way.
 */
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

/*
 * Whether the game is loaded from a shared library that can be swapped while it runs.
 *
 * Debug and developer builds both want it; release and steam must not have it, since it costs an
 * indirection on every entry point and means shipping the game as a loose DLL beside the exe.
 * main.c keys its entry point off this rather than off NYA_DEBUG, which is what previously made
 * developer builds silently fall into the release path.
 */
#define NYA_CODE_HOT_RELOAD NYA_DEVELOPMENT_BUILD

/*
 * Headless: the engine runs, but nothing is drawn.
 *
 * Everything else stays live — events, jobs, assets, the simulation and its barriers — so a test
 * exercises the same code paths it would in a real frame. Only the GPU work is skipped, which is
 * what makes this usable on a CI machine that has no device to create and no display to present to.
 *
 * Distinct from NYA_NO_SDL, which compiles the renderer and core out entirely for host tools. This
 * keeps them compiled and callable, and makes the drawing a no-op.
 *
 * Spelled 1/0 rather than true/false because `true` is `((b8)1)` here, which #if cannot evaluate.
 */
#ifdef NYA_HEADLESS
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
     *
     * Distinct from DEBUG because a test is not an interactive session: it compiles in
     * nya_expect_crash so a deliberate assertion can be survived, and it is the mode that pairs
     * with NYA_HEADLESS on a machine with no GPU.
     * */
    NYA_EXECUTION_MODE_TEST = 4,

    NYA_EXECUTION_MODE_COUNT,
    NYA_EXECUTION_MODE_CURRENT = NYA_EXECUTION_MODE,
} NYA_ExecutionMode;
static_assert(NYA_EXECUTION_MODE_CURRENT < NYA_EXECUTION_MODE_COUNT, "Invalid execution mode");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * COMPILER DETECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if defined(_MSC_VER)
#define COMPILER_MSVC 1
#elif defined(__clang__)
#define COMPILER_CLANG 1
#elif defined(__GNUC__)
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PLATFORM DETECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if defined(__wasm__)
#define OS_WASM 1
#elif defined(_WIN32) || defined(__CYGWIN__)
#define OS_WINDOWS 1
#elif defined(__linux__)
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ARCHITECTURE DETECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if defined(__wasm32__)
#define ARCH_WASM32 1
#elif defined(__wasm64__)
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ADDRESS SANITIZATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// The header is checked for separately from the feature. A toolchain can be perfectly capable of
// -fsanitize=address while shipping the runtime headers in a package nobody installed, which is the
// normal state of a CI container. Without the header the manual poisoning below is compiled out and
// ASan still catches everything it finds on its own, which is a far better outcome than refusing to
// build.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VISIBILITY AND LINKAGE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if COMPILER_CLANG || COMPILER_GCC
#define NYA_INTERNAL __attribute__((visibility("hidden"))) static
#else
#define NYA_INTERNAL static
#endif

#ifdef __cplusplus
#define NYA_EXTERN extern "C"
#else
#define NYA_EXTERN extern
#endif

#if OS_WINDOWS
#define NYA_API __declspec(dllexport) NYA_EXTERN
#else
#define NYA_API __attribute__((visibility("default"))) NYA_EXTERN
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * OTHER CODEBASE MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define atomic       _Atomic
#define thread_local _Thread_local
#define true         ((b8)1)
#define false        ((b8)0)

#define OUT

// the impl non-sense is such that not the symbol is concatenated
// but the value after the preprocessor replaces the symbol
#define CONCAT(a, b)       _CONCAT_IMPL(a, b)
#define _CONCAT_IMPL(a, b) a##b
