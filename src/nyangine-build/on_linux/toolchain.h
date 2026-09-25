/**
 * @file toolchain.h
 * */
#pragma once

// clang-format off

/* Cross compiling to Windows, so clang has to be pointed at the target explicitly. */
/**
 * mingw-w64 headers before 12 (Ubuntu's) define __cpuidex in every translation unit that includes intrin.h, which
 * clang's own cpuid.h redefines static: a compile error in C++ and a duplicate symbol at link in C. Marking it
 * defined leaves clang's static inline as the only one.
 * */
#define NYA_MINGW_INTRINSICS "-D__INTRINSIC_DEFINED___cpuidex"

#define FLAGS_TARGET_WINDOWS_X86_64 "--target=x86_64-w64-mingw32", NYA_MINGW_INTRINSICS,

/* There is no FLAGS_TARGET_LINUX_X86_64: Linux is only built natively, and the Windows host does not target it (see build.h). */

/** The only resource compiler that exists on a Linux host. */
#define WINDRES "x86_64-w64-mingw32-windres"

/** Compilers for Makefile based vendors targeting Windows. clang for the reason on NYA_CMAKE_WINDOWS_TOOLCHAIN. */
// the compiler's own name, not the cache in front of it: this one is spelled into a `CC=` a Makefile
// reads, and the cache reaches those through NYA_CC_MAKE instead.
#define NYA_WINDOWS_CC CC " --target=x86_64-w64-mingw32 " NYA_MINGW_INTRINSICS
#define NYA_WINDOWS_AR "x86_64-w64-mingw32-ar"

/** LuaJIT builds a host side code generator first, so HOST_CC must stay the host compiler. */
#define NYA_LUAJIT_CROSS "HOST_CC=cc -m64", "CROSS=x86_64-w64-mingw32-",

/** Autotools needs telling it is cross compiling. */
#define NYA_AUTOTOOLS_WINDOWS_HOST "--host=x86_64-w64-mingw32",

/**
 * How an autotools `configure` is invoked. See the Windows counterpart for why this is host specific.
 *
 * Directly here: `configure` opens with `#! /bin/sh`, and execve honours that.
 * */
#define NYA_CONFIGURE_PROGRAM      "../configure"
#define NYA_CONFIGURE_LEADING_ARGS

/**
 * Points cmake at the mingw-w64 sysroot for vendors targeting Windows, compiled by clang like the engine.
 *
 * Not mingw gcc: its -ffunction-sections sections are not COMDAT, and a PE link only collects COMDAT sections, so
 * nothing it built ever left the Windows executable.
 * */
#define NYA_CMAKE_WINDOWS_TOOLCHAIN                     \
    "-DCMAKE_SYSTEM_NAME=Windows",                      \
    "-DCMAKE_C_COMPILER=" CC,                           \
    "-DCMAKE_C_COMPILER_TARGET=x86_64-w64-mingw32",     \
    "-DCMAKE_CXX_COMPILER=" CC "++",                    \
    "-DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-mingw32",   \
    /* after NYA_CMAKE_OPTIMIZE on every command line, so these release flags are the ones cmake keeps. */ \
    "-DCMAKE_C_FLAGS_RELEASE=" NYA_VENDOR_OPTIMIZE " " NYA_MINGW_INTRINSICS, \
    "-DCMAKE_CXX_FLAGS_RELEASE=" NYA_VENDOR_OPTIMIZE " " NYA_MINGW_INTRINSICS, \
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld",            \
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",         \
    "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres",   \
    "-DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32",   \
    /* ONLY, not BOTH: with BOTH, a cross build that calls find_package can pick up the host's   */ \
    /* /usr/include and mix glibc headers into a mingw compile, which fails on conflicting       */ \
    /* ssize_t/time_t/uintptr_t rather than anything obvious.                                    */ \
    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",         \
    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",         \
    /* BOTH for packages: the vendored SDL3 lives outside the mingw sysroot, so find_package has  */ \
    /* to be allowed to look at CMAKE_PREFIX_PATH. Its config hands back absolute include dirs,   */ \
    /* which are used directly and so are not filtered by the INCLUDE mode above.                 */ \
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH",         \
    "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"

// clang-format on
