/**
 * @file toolchain.h
 * */
#pragma once

// clang-format off

/** Native, so there is no target triple. Expands to nothing at all, comma included. */
#define FLAGS_TARGET_WINDOWS_X86_64

/** Native resource compiler. */
#define WINDRES "windres"

/** Native, so the ordinary compiler and archiver. */
#define NYA_WINDOWS_CC CC
#define NYA_WINDOWS_AR "ar"

/** Not cross compiling, so LuaJIT's host compiler is the compiler. Named, since its Makefile defaults to gcc. */
#define NYA_LUAJIT_CROSS CC_MAKE,

/** Not cross compiling, so autotools must not be told a host. */
#define NYA_AUTOTOOLS_WINDOWS_HOST

/**
 * How an autotools `configure` is invoked, through an explicit shell.
 * */
#define NYA_CONFIGURE_PROGRAM      "sh"
#define NYA_CONFIGURE_LEADING_ARGS "../configure",

/** Native, so cmake needs no toolchain redirection. */
#define NYA_CMAKE_WINDOWS_TOOLCHAIN "-DCMAKE_BUILD_TYPE=Release"

// clang-format on
