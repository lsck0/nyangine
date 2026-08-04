/**
 * @file flags.h
 *
 * Project names, output paths and compiler flag sets shared by every build rule.
 *
 * Flags are split three ways:
 * - portable ones that apply everywhere (CFLAGS, WARNINGS, FLAGS_DEBUG, FLAGS_RELEASE),
 * - per target ones for the platform being produced (FLAGS_LINUX_X86_64, FLAGS_WINDOWS_X86_64),
 * - per host ones, in on_linux/toolchain.h and on_windows/toolchain.h, for the tools themselves.
 *
 * Nothing about a third party dependency belongs here. Include paths, cflags and link flags for
 * those live on their NYA_VendorRule, so adding a dependency never widens this file.
 * */
#pragma once

// clang-format off

#define PROJECT_NAME "gnyame"
#define VERSION      "0.0.0"

#define BINARY_SOURCE_PATH "./src/main.c"
#define DLL_SOURCE_PATH    "./src/gnyame/gnyame.c"

#define LINUX_X86_64_DEBUG_BINARY   PROJECT_NAME ".debug"
#define LINUX_X86_64_DEBUG_DLL      PROJECT_NAME ".debug.so"
#define LINUX_X86_64_DEV_BINARY     PROJECT_NAME ".dev"
#define LINUX_X86_64_DEV_DLL        PROJECT_NAME ".dev.so"
#define LINUX_X86_64_BINARY         PROJECT_NAME "." VERSION ".linux-x86_64"
#define LINUX_X86_64_TEST_BINARY    PROJECT_NAME ".test"
#define WINDOWS_X86_64_DEBUG_BINARY PROJECT_NAME ".debug.exe"
#define WINDOWS_X86_64_DEBUG_DLL    PROJECT_NAME ".debug.dll"
#define WINDOWS_X86_64_DEBUG_IMPLIB PROJECT_NAME ".debug.lib"
#define WINDOWS_X86_64_DEV_BINARY   PROJECT_NAME ".dev.exe"
#define WINDOWS_X86_64_DEV_DLL      PROJECT_NAME ".dev.dll"
#define WINDOWS_X86_64_DEV_IMPLIB   PROJECT_NAME ".dev.lib"
#define WINDOWS_X86_64_BINARY       PROJECT_NAME "." VERSION ".windows-x86_64.exe"

// CC and NPROCS come from build/vendor/vendor.h, which needs them for the vendor rules.
#define CFLAGS        "-std=c2y", "-mavx", "-mavx2", "-fdefer-ts", "-fenable-matrix", "-ggdb"
#define WARNINGS      "-Wall", "-Wextra", "-Wstrict-prototypes", "-Wswitch", "-Wswitch-default", "-Wimplicit-fallthrough", "-Wno-gnu", "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro"
// Only the project's own paths. Everything a third party dependency needs lives on its
// NYA_VendorRule instead, so this does not grow as dependencies are added.
#define INCLUDE_PATHS "-I./", "-I./src/"
#define LINKER_FLAGS  "-lm", "-pthread"

/*
 * Every mode names its NYA_EXECUTION_MODE explicitly. The macro defaults to 0, so leaving it off
 * does not mean "unset", it means "debug" — which is how release binaries ended up compiling the
 * hot reload entry point and skipping their own integrity check.
 *
 * Assets are not a backend choice any more. The filesystem is always available; NYA_ASSET_PREFER_BLOB adds
 * the baked copy in front of it and NYA_ASSET_HOT_RELOAD watches whatever still comes off disk. A
 * mode naming neither reads assets from disk and does not watch them, which is what a test wants.
 */
#define FLAGS_DEBUG     "-DNYA_EXECUTION_MODE=0", "-DDEBUG=true", "-O0", "-DNYA_ASSET_HOT_RELOAD"

// Developer: hot reload like debug, but optimized and without sanitizers. For actually playing the
// game while iterating on it, where debug is too slow to feel right.
#define FLAGS_DEVELOPER "-DNYA_EXECUTION_MODE=1", "-O2", "-DNYA_ASSET_HOT_RELOAD"

// Test: assertions live, nya_expect_crash compiled in so a test can survive a deliberate panic, and
// headless so it needs no GPU. Statically linked game code, no hot reload DLL to find.
#define FLAGS_TEST      "-DNYA_EXECUTION_MODE=4", "-O0", "-DNYA_TESTING", "-DNYA_HEADLESS"

// The build system is a host tool. It needs base, math, platform and serde and nothing that opens
// a window, so core and renderer are compiled out rather than linked and left unused. Without this
// the tool would need SDL on the link line to build SDL, which is a bootstrap it cannot satisfy.
#define FLAGS_BUILD_TOOL "-DNYA_NO_SDL"

// Runs the engine with the drawing compiled out. Everything else still runs, so a test exercises
// the real frame loop; there is just no GPU device to create, which is what CI cannot provide.
#define FLAGS_HEADLESS "-DNYA_HEADLESS"
#define FLAGS_DLL      "-fPIC", "-shared"
#define FLAGS_SANITIZE "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls", "-fno-sanitize-recover=all", "-fsanitize=address,leak,undefined,signed-integer-overflow,unsigned-integer-overflow,shift,float-cast-overflow,float-divide-by-zero,pointer-overflow"

// -g1 is deliberate: without line tables libbacktrace can only print raw addresses, so a crash
// report from a shipped build would be unreadable. Note that the debug sections are covered by the
// integrity CRC, so a shipped binary must never be stripped after hook_insert_integrity_hash.
// -DNYA_EXECUTION_MODE=2 is load bearing, not decoration. NYA_DEBUG is (NYA_EXECUTION_MODE == 0)
// and the macro defaults to 0, so without this a release binary compiles main.c's *hot reload* entry
// point and goes looking for gnyame.debug.so, and nya_integrity_assert early returns because it
// believes it is a debug build. Both were true until this was added.
#define FLAGS_RELEASE  "-O3", "-flto", "-fPIE", "-fuse-ld=lld", "-g1", "-DNYA_EXECUTION_MODE=2", "-DNYA_ASSET_PREFER_BLOB", "-D_FORTIFY_SOURCE=2", "-fcf-protection=full", "-fstack-protector-strong", "-fno-omit-frame-pointer"

// Only what the target itself needs. Library paths and -l flags belong to the vendors.
// Steam is release plus the Steam runtime. Same deploy shape, different execution mode, so
// NYA_STEAM can gate overlay and achievements without a second set of build rules.
#define FLAGS_STEAM "-DNYA_EXECUTION_MODE=3"

#define FLAGS_LINUX_X86_64   "-Wl,-rpath,$ORIGIN"
#define FLAGS_WINDOWS_X86_64 "-Wl,-subsystem,windows", "-static"

// mold is Linux only, and -rdynamic is what lets the hot reloaded game DLL resolve engine symbols
// out of the executable via dlopen(nullptr).
#define FLAGS_DEBUG_LINUX_X86_64   "-fuse-ld=mold", "-rdynamic"
// The PE equivalent: the executable exports its symbols and produces an import library, which the
// game DLL then links against. Windows has no dlopen(nullptr) to fall back on.
// The import library name is part of the flag set because it is per artifact: a dev exe emitting
// the debug implib would have the dev DLL resolving its engine symbols against the wrong
// executable, which links cleanly and then misbehaves at runtime.
#define FLAGS_HOTRELOAD_WINDOWS_X86_64 "-fuse-ld=lld", "-Wl,--export-all-symbols"
#define FLAGS_DEBUG_WINDOWS_X86_64     FLAGS_HOTRELOAD_WINDOWS_X86_64, "-Wl,--out-implib," WINDOWS_X86_64_DEBUG_IMPLIB
#define FLAGS_DEV_WINDOWS_X86_64       FLAGS_HOTRELOAD_WINDOWS_X86_64, "-Wl,--out-implib," WINDOWS_X86_64_DEV_IMPLIB

// clang-format on
