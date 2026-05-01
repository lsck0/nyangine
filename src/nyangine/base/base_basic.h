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
#include <fenv.h>
#include <immintrin.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <memory.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
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

#if defined(DEBUG) && DEBUG
#define NYA_DEBUG 1
#else
#define NYA_DEBUG 0
#endif

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

typedef enum {
    NYA_MODE_DEBUG,
    NYA_MODE_RELEASE,
    NYA_MODE_COUNT,
#if NYA_DEBUG
    NYA_MODE_CURRENT = NYA_MODE_DEBUG,
#else
    NYA_MODE_CURRENT = NYA_MODE_RELEASE,
#endif
} NYA_Mode;

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

#if defined(__has_feature) && __has_feature(address_sanitizer)
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

#ifdef __cplusplus
#define NYA_EXTERN extern "C"
#else
#define NYA_EXTERN extern
#endif

#if COMPILER_CLANG || COMPILER_GCC
#define NYA_INTERNAL __attribute__((visibility("hidden"))) static
#else
#define NYA_INTERNAL static
#endif

#if OS_WINDOWS
#define NYA_API __declspec(dllexport)
#else
#define NYA_API __attribute__((visibility("default")))
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
