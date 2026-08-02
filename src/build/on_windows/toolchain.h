/**
 * @file toolchain.h
 *
 * Which tools a Windows host uses to produce each target.
 *
 * Everything here answers "what is doing the building", never "what is being built". Keeping it in
 * one file per host is what lets the vendor rules and the project rules stay declarative: a rule
 * names a toolchain macro and never has to ask which machine it is running on.
 *
 * Producing Windows binaries from here is native. Producing Linux binaries is not routinely
 * possible, see on_windows/build_linux.h.
 *
 * The autotools based vendors (libbacktrace, sqlite) need `sh` and `make`, which on Windows means
 * an msys2 install on PATH.
 * */
#pragma once

// clang-format off

/** Native, so there is no target triple. */
#define FLAGS_TARGET_WINDOWS_X86_64

/** Cross compiling to Linux. Needs a glibc sysroot supplied out of band. */
#define FLAGS_TARGET_LINUX_X86_64

/** mold is a Linux host linker, so a Windows host has to fall back to lld. */
#define FLAGS_DEBUG_LINUX_X86_64_ON_WINDOWS "-fuse-ld=lld", "-rdynamic"

/** Native resource compiler. */
#define WINDRES "windres"

/** Native, so the ordinary compiler and archiver. */
#define NYA_WINDOWS_CC CC
#define NYA_WINDOWS_AR "ar"

/** Not cross compiling, so LuaJIT's host compiler is simply the compiler. */
#define NYA_LUAJIT_CROSS

/** Not cross compiling, so autotools must not be told a host. */
#define NYA_AUTOTOOLS_WINDOWS_HOST

/** Native, so cmake needs no toolchain redirection. */
#define NYA_CMAKE_WINDOWS_TOOLCHAIN "-DCMAKE_BUILD_TYPE=Release"

// clang-format on
