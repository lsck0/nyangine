/**
 * @file build.h
 *
 * Everything the build system offers, in the order the pieces depend on each other.
 *
 * toolchain.h comes first because it decides which host is doing the building, and every rule below
 * names the tools it defines. flags.h is next for the same reason: the vendors and the asset
 * pipeline both spell their compile flags with its macros.
 *
 * Mirrors nyangine.h, so build.c reads the same way main.c does: one header for the declarations,
 * one .c for the translation units behind them.
 * */
#pragma once

#include "nyangine/nyangine.h"

// Which host is doing the building decides the tool names every rule uses.
#include "build/toolchain.h"
/**/
#include "build/flags.h"
/**/
#include "build/asset/asset.h"
#include "build/hooks/hooks.h"
#include "build/test.h"
#include "build/vendor/vendor.h"

/*
 * ─────────────────────────────────────────────────────────
 * PROJECT BUILD RULES
 * ─────────────────────────────────────────────────────────
 */

/*
 * The asset rules, declared here and defined in build.c.
 *
 * Every per host project rule below depends on them, and build.c is where they live because they
 * are the same on every host. Tentative definitions rather than a header of their own: NYA_INTERNAL
 * is static, so naming one here reserves it and build.c's initialiser is what defines it.
 * */
NYA_INTERNAL NYA_BuildRule build_shaders;
NYA_INTERNAL NYA_BuildRule index_assets;
NYA_INTERNAL NYA_BuildRule bundle_assets;

// Which host is doing the building decides how each target is produced, so the rules live in a
// directory per host rather than behind conditionals inside one file.
#if OS_WINDOWS
#include "build/on_windows/build_linux.h"
#include "build/on_windows/build_windows.h"
#else
#include "build/on_linux/build_linux.h"
#include "build/on_linux/build_windows.h"
#endif

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
 *
 * Neither of the per target flag sets fits one of those. FLAGS_LINUX_X86_64 is an ELF rpath, and
 * FLAGS_WINDOWS_X86_64 carries -Wl,-subsystem,windows, which would turn a command line tool into a
 * GUI process with nowhere to print. So the host flags are spelled out here rather than borrowed.
 *
 * BUILD_TOOL_BINARY in particular has to be exactly what the running executable is called:
 * nya_rebuild_yourself backs up argv[0], compiles over it, then exec's it again. Name the output
 * anything else and the compile writes a file nobody runs while the stale binary re-execs itself,
 * which looks like a build system that silently ignores its own source changes.
 */
#if OS_WINDOWS

#define BUILD_TOOL_BINARY "build.exe"

/*
 * No sanitizers on a Windows host. -fsanitize=leak has no Windows implementation at all, and asan
 * under mingw is not usable the way it is on Linux — the same reason the Windows project rules skip
 * it. lld because mold is a Linux linker, and lld is what this host's other rules already name.
 * */
#define FLAGS_HOST_NATIVE       "-fuse-ld=lld"
#define BACKTRACE_A_HOST        BACKTRACE_A_WINDOWS_X86_64
#define BACKTRACE_INCLUDES_HOST BACKTRACE_INCLUDES_WINDOWS_X86_64

#else

#define BUILD_TOOL_BINARY "build"

#define FLAGS_HOST_NATIVE       FLAGS_DEBUG_LINUX_X86_64, FLAGS_SANITIZE, FLAGS_LINUX_X86_64
#define BACKTRACE_A_HOST        BACKTRACE_A_LINUX_X86_64
#define BACKTRACE_INCLUDES_HOST BACKTRACE_INCLUDES_LINUX_X86_64

#endif
