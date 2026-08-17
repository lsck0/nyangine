/**
 * @file toolchain.h
 *
 * Which tools a Windows host uses to produce each target.
 *
 * Everything here answers "what is doing the building", never "what is being built". Keeping it in
 * one file per host is what lets the vendor rules and the project rules stay declarative: a rule
 * names a toolchain macro and never has to ask which machine it is running on.
 *
 * Producing Windows binaries from here is native, and it is the only thing this host produces. See
 * build.h for why there is no Linux target here and no counterpart to the Linux host's mingw-w64
 * macros below.
 *
 * The autotools based vendors (libbacktrace, sqlite) need `sh` and `make`, which on Windows means
 * an msys2 install on PATH.
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

/** Not cross compiling, so LuaJIT's host compiler is simply the compiler. */
#define NYA_LUAJIT_CROSS

/** Not cross compiling, so autotools must not be told a host. */
#define NYA_AUTOTOOLS_WINDOWS_HOST

/**
 * How an autotools `configure` is invoked, through an explicit shell.
 *
 * `configure` is a shell script, and CreateProcess cannot run one: a shebang is a kernel convention
 * that Windows does not have, so spawning it directly fails with "failed to create process for
 * '../configure'" and nothing about the message says the file is a script.
 *
 * Only visible on a cold vendor cache, which is why it survived: a CI run that restores the cache
 * never configures anything, and the miss is what surfaced it.
 * */
#define NYA_CONFIGURE_PROGRAM      "sh"
#define NYA_CONFIGURE_LEADING_ARGS "../configure",

/** Native, so cmake needs no toolchain redirection. */
#define NYA_CMAKE_WINDOWS_TOOLCHAIN "-DCMAKE_BUILD_TYPE=Release"

// clang-format on
