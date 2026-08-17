/**
 * @file host.h
 *
 * The machine doing the building, as the rest of the build system sees it: which project rules
 * target it, what those rules call their output, and how anything compiled to run here right now is
 * built.
 *
 * Separate from build.h so that rebuild.h, misc.h and cli.c can include what they use. These macros
 * were spelled out in build.h itself, which meant every file naming one of them compiled only in
 * build.h's include order and reported undeclared identifiers when opened on its own — the same
 * problem vendor_common.h was split out to solve. build.h is now purely an umbrella.
 *
 * Which host is building decides how each target is produced, so the per host rule files are
 * included here rather than named by anyone else.
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
 *
 * A Windows host builds Windows targets only, which is why there is no Linux rule file to include
 * on that side. Targeting Linux from Windows needs a glibc sysroot *and* the X11, Wayland, xkbcommon,
 * ALSA and D-Bus development headers SDL configures against, and nothing on a Windows machine
 * supplies the second half of that. A Linux host targeting Windows has no equivalent problem —
 * mingw-w64 ships the whole win32 surface — which is why that direction is supported and this one
 * is not. Build Linux binaries on Linux, or in WSL2, where the ordinary native path applies.
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
