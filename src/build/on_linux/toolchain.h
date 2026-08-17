/**
 * @file toolchain.h
 *
 * Which tools a Linux host uses to produce each target.
 *
 * Everything here answers "what is doing the building", never "what is being built". Keeping it in
 * one file per host is what lets the vendor rules and the project rules stay declarative: a rule
 * names a toolchain macro and never has to ask which machine it is running on.
 *
 * Producing Windows binaries from here means cross compiling with mingw-w64.
 * */
#pragma once

// clang-format off

/*
 * Cross compiling to Windows, so clang has to be pointed at the target explicitly.
 *
 * The trailing comma belongs to the macro, not to the use site, because the other host defines this
 * empty and `FLAGS, , FLAGS` is not an expression. Same convention as NYA_LUAJIT_CROSS below.
 * */
#define FLAGS_TARGET_WINDOWS_X86_64 "--target=x86_64-w64-mingw32",

/*
 * There is no FLAGS_TARGET_LINUX_X86_64. Linux is only ever built natively — the Windows host does
 * not target it, see build.h — so there is no host for which that macro would expand to anything.
 */

/** The only resource compiler that exists on a Linux host. */
#define WINDRES "x86_64-w64-mingw32-windres"

/** Compilers for Makefile based vendors targeting Windows. */
#define NYA_WINDOWS_CC "x86_64-w64-mingw32-gcc"
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

/** Points cmake at mingw-w64 for vendors targeting Windows. */
#define NYA_CMAKE_WINDOWS_TOOLCHAIN                     \
    "-DCMAKE_SYSTEM_NAME=Windows",                      \
    "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc",        \
    "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++",      \
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
