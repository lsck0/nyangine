/**
 * @file host.h
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/flags.h"
#include "build/toolchain.h"
#include "build/vendor/vendor.h"

/*
 * ─────────────────────────────────────────────────────────
 * PROJECT BUILD RULES
 * ─────────────────────────────────────────────────────────
 */

/*
 * A rule file lives in a host directory only when it is genuinely per host, which turned out to be
 * just the Linux one. The Windows rules were duplicated into both directories and were identical;
 * see build_windows.h.
 */
#if !OS_WINDOWS
#include "build/on_linux/build_linux.h"
#endif

// Not per host: the native and cross compiled Windows rules were identical, so there is one copy.
// See build_windows.h.
#include "build/build_windows.h"

/*
 * ─────────────────────────────────────────────────────────
 * HOST TARGETS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Running always targets the host. Cross compiling to Windows from Linux is a build time
 * convenience; there is nothing sensible to do with the resulting .exe here, so `run` picks the
 * native artifact and the rules it names are selected by host rather than exposed as a choice.
 */

#if OS_WINDOWS
#define HOST_DEBUG_BINARY   WINDOWS_X86_64_DEBUG_BINARY
#define HOST_DEV_BINARY     WINDOWS_X86_64_DEV_BINARY
#define HOST_RELEASE_BINARY WINDOWS_X86_64_BINARY
#define host_build_debug    build_project_debug_windows
#define host_build_dev      build_project_dev_windows
#define host_build_release  build_project_windows_x86_64
#else
#define HOST_DEBUG_BINARY   LINUX_X86_64_DEBUG_BINARY
#define HOST_DEV_BINARY     LINUX_X86_64_DEV_BINARY
#define HOST_RELEASE_BINARY LINUX_X86_64_BINARY
#define host_build_debug    build_project_debug_linux
#define host_build_dev      build_project_dev_linux
#define host_build_release  build_project_linux_x86_64
#endif

/** Sanitizer configuration shared by everything that runs an instrumented binary. */
#define SANITIZER_ENVIRONMENT                                                                                                                        \
    "ASAN_OPTIONS=suppressions=./.sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1",                                       \
        "LSAN_OPTIONS=suppressions=./.sanitizers/lsan.supp", "TSAN_OPTIONS=suppressions=./.sanitizers/tsan.supp",                                    \
        "UBSAN_OPTIONS=suppressions=./.sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1"

/*
 * ─────────────────────────────────────────────────────────
 * HOST NATIVE ARTIFACTS
 * ─────────────────────────────────────────────────────────
 */

/*
 * For the two things built to run on this machine right now rather than to be shipped anywhere: the
 * build tool, which compiles itself, and the test binary, which the test runner then executes.
 */
#if OS_WINDOWS

#define BUILD_TOOL_BINARY "build.exe"

/**
 * lz4 for the build tool itself, which compresses the asset blob. See nya_asset_bundle.
 *
 * The tool links the host's own archive because it runs here, unlike every other use of lz4 in this
 * tree, which links the archive for the target being shipped.
 * */
#define FLAGS_HOST_LZ4 "-I./vendor/lz4/lib/", LZ4_A_WIN

/*
 * No sanitizers on a Windows host. -fsanitize=leak has no Windows implementation at all, and asan
 * under mingw is not usable the way it is on Linux — the same reason the Windows project rules skip
 * it. lld because mold is a Linux linker, and lld is what this host's other rules already name.
 * */
#define FLAGS_HOST_NATIVE       "-fuse-ld=lld"

/** The same host flags without the sanitizers. See the Linux definition for why this exists. */
#define FLAGS_HOST_NATIVE_BENCH "-fuse-ld=lld"

#define BACKTRACE_A_HOST        BACKTRACE_A_WINDOWS_X86_64
#define BACKTRACE_INCLUDES_HOST BACKTRACE_INCLUDES_WINDOWS_X86_64

#else

#define BUILD_TOOL_BINARY "build"

/** See the Windows definition. */
#define FLAGS_HOST_LZ4 "-I./vendor/lz4/lib/", LZ4_A_LIN

#define FLAGS_HOST_NATIVE       FLAGS_DEBUG_LINUX_X86_64, FLAGS_SANITIZE, FLAGS_LINUX_X86_64

/**
 * The same, minus FLAGS_SANITIZE. What a benchmark is built with.
 * */
#define FLAGS_HOST_NATIVE_BENCH FLAGS_DEBUG_LINUX_X86_64, FLAGS_LINUX_X86_64

#define BACKTRACE_A_HOST        BACKTRACE_A_LINUX_X86_64
#define BACKTRACE_INCLUDES_HOST BACKTRACE_INCLUDES_LINUX_X86_64

#endif
