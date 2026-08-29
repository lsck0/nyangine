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
// -mfma is not implied by -mavx2: they are separate feature bits, and the nn kernels in
// nn/nn_simd.h fall back to a separate multiply and add without it. Every part that shipped with
// AVX2 has FMA3 — both arrived with Haswell — so this widens nothing the -mavx2 above had not
// already committed to.
#define CFLAGS        "-std=c2y", "-mavx", "-mavx2", "-mfma", "-fdefer-ts", "-fenable-matrix", "-ggdb"
#define WARNINGS      "-Wall", "-Wextra", "-Wstrict-prototypes", "-Wswitch", "-Wswitch-default", "-Wimplicit-fallthrough", "-Wno-gnu", "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro"
// Only the project's own paths. Everything a third party dependency needs lives on its
// NYA_VendorRule instead, so this does not grow as dependencies are added.
#define INCLUDE_PATHS "-I./", "-I./src/"
#define LINKER_FLAGS  "-lm", "-pthread"

/*
 * Which optional plugins the *project* compiles. See src/nyangine/plugins/plugins.h.
 *
 * On the project rules rather than in CFLAGS, because CFLAGS is also what the build tool compiles
 * itself with — and the build tool is what builds libcurl and libsqlite3, so on a fresh checkout it
 * cannot link against either. Naming them here keeps that asymmetry in one visible place instead of
 * as a negative flag the bootstrap has to remember.
 *
 * The matching vendors must be in the target's NYA_PROJECT_VENDORS_* list, or the plugin compiles
 * and then fails to link.
 */
#define FLAGS_PLUGINS "-DNYA_PLUGIN_CURL", "-DNYA_PLUGIN_SQLITE", "-DNYA_PLUGIN_DISCORD", "-DNYA_PLUGIN_LUA"

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

/**
 * Source based coverage instrumentation, for `./build run coverage`.
 *
 * Both flags are needed and both go on the one clang invocation: -fprofile-instr-generate links the
 * runtime that writes the raw profile, and -fcoverage-mapping embeds the table mapping counters back
 * to source ranges. Without the second, llvm-cov has counts it cannot attribute to any line.
 *
 * Coexists with FLAGS_SANITIZE, so a coverage run is still a sanitized run and cannot report a line
 * as covered that only "worked" by reading uninitialised memory.
 * */
/**
 * What a benchmark is compiled with: optimised, headless, and *without* sanitizers.
 *
 * The absence is the point. FLAGS_TEST is -O0 under four sanitizers, and a benchmark built that way
 * measures the instrumentation rather than the code — unevenly, too, because the overhead lands hardest
 * on memory-heavy work. The engine's reverb profiled at 5.52% of frame time under ASAN and measured at
 * 0.22% of a core without it.
 *
 * Not FLAGS_RELEASE either: -flto makes every benchmark relink the world, and the hardening flags are
 * about shipping rather than about measuring.
 * */
#define FLAGS_BENCH     "-DNYA_EXECUTION_MODE=2", "-O2", "-DNYA_HEADLESS", "-fno-omit-frame-pointer"

#define FLAGS_COVERAGE  "-fprofile-instr-generate", "-fcoverage-mapping"

/** Where a coverage run puts the raw profiles, the merged profile and the instrumented binaries. */
#define COVERAGE_DIRECTORY    "./.coverage"
#define COVERAGE_PROFILE_DATA COVERAGE_DIRECTORY "/merged.profdata"

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

/*
 * Steam is release plus the Steam runtime. Same deploy shape, different execution mode, so the mode
 * can gate overlay and achievements without a second set of build rules.
 *
 * Unlike the other plugins this one has no NYA_VendorRule to carry its link flags: the Steamworks
 * SDK is a prebuilt redistributable checked into vendor/steam rather than something the build
 * system compiles, so there is nothing for a vendor rule to build and the flags are spelled out.
 *
 * -Wl,-rpath,$ORIGIN, not a path into vendor/: the .so ships *beside* the executable, so pointing
 * the rpath at the checkout would produce a binary that runs on this machine and on no other.
 * Copying libsteam_api.so next to the binary is part of using this, and there is no build rule that
 * does it yet — see plugins/steam/steam.h.
 *
 * Named by no rule today, which is why NYA_EXECUTION_MODE=3 is still unreachable.
 */
#define FLAGS_STEAM_LINUX_X86_64                                                                                                                     \
    "-DNYA_EXECUTION_MODE=3", "-DNYA_PLUGIN_STEAM", "-L./vendor/steam/redistributable_bin/linux64/", "-Wl,-rpath,$ORIGIN", "-lsteam_api"

#define FLAGS_STEAM_WINDOWS_X86_64                                                                                                                   \
    "-DNYA_EXECUTION_MODE=3", "-DNYA_PLUGIN_STEAM", "-L./vendor/steam/redistributable_bin/win64/", "-lsteam_api64"

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

/*
 * Authenticode signing of the shipped .exe. See hook_sign_windows_executable.
 *
 * The checked in certificate is self signed, which no operating system trusts: it does not satisfy
 * SmartScreen and it is not a credential. It exists so the signing path is exercised by every
 * release build rather than discovered to be broken on release day, and because an unsigned
 * download draws harsher treatment from browsers than a signed one with an unknown publisher.
 * Its password is published here for the same reason it can be: the key protects nothing.
 *
 * A real certificate is never checked in. Point the three environment variables below at it
 * instead, from a secret store, and leave this file alone. See the README.
 */
// Deliberately not under assets/: that tree is walked by the asset indexer and embedded into
// assets.c, which would put the private key inside the shipped binary.
#define SIGNING_PFX_PATH      "./.signing/sample.pfx"
#define SIGNING_PFX_PASSWORD  "nyangine-sample-certificate"
#define SIGNING_TIMESTAMP_URL "http://timestamp.digicert.com"

/** Overrides for the three above, so CI can sign with a real certificate without editing this file. */
#define SIGNING_PFX_PATH_ENV      "NYA_SIGNING_PFX"
#define SIGNING_PFX_PASSWORD_ENV  "NYA_SIGNING_PASSWORD"
#define SIGNING_TIMESTAMP_URL_ENV "NYA_SIGNING_TIMESTAMP_URL"

// clang-format on
