/**
 * @file flags.h
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

/*
 * A Steam build is a directory, since the Steamworks library ships beside the executable. Its contents are the depot.
 */
#define STEAM_LINUX_X86_64_DIRECTORY   PROJECT_NAME "." VERSION ".steam-linux-x86_64"
#define STEAM_LINUX_X86_64_BINARY      STEAM_LINUX_X86_64_DIRECTORY "/" PROJECT_NAME
#define STEAM_LINUX_X86_64_LIBRARY     STEAM_LINUX_X86_64_DIRECTORY "/libsteam_api.so"
#define STEAM_WINDOWS_X86_64_DIRECTORY PROJECT_NAME "." VERSION ".steam-windows-x86_64"
#define STEAM_WINDOWS_X86_64_BINARY    STEAM_WINDOWS_X86_64_DIRECTORY "/" PROJECT_NAME ".exe"
#define STEAM_WINDOWS_X86_64_LIBRARY   STEAM_WINDOWS_X86_64_DIRECTORY "/steam_api64.dll"

/*
 * Where split rules compile to before linking, one object per artifact. Nothing reads an object from a
 * previous build: every compile runs, and a compiler cache is what makes an unchanged one cheap.
 */
#define OBJECT_DIRECTORY "./.objects"
#define OBJECT_SUFFIX    ".o"

// CC and NPROCS come from build/vendor/vendor.h.
// -mfma is not implied by -mavx2, and nn/nn_simd.h falls back to separate multiply and add without it.
// -mf16c turns the half float casts in NYA_Vertex3D into one instruction instead of a libgcc call per channel.
// Every AVX2 CPU has FMA3 and F16C (all came with Haswell), so neither adds a requirement.
#define CFLAGS        "-std=c2y", "-mavx", "-mavx2", "-mfma", "-mf16c", "-fdefer-ts", "-fenable-matrix", "-ggdb"
#define WARNINGS      "-Werror", "-Wall", "-Wextra", "-Wstrict-prototypes", "-Wswitch", "-Wswitch-default", "-Wimplicit-fallthrough", "-Wno-gnu", "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro"
/*
 * A rule that compiles with `-c` takes only compile flags: under -Werror clang rejects a linker flag it
 * cannot use. Link only are LINKER_FLAGS, every *_LINK macro, FLAGS_LINUX_X86_64, FLAGS_WINDOWS_X86_64
 * and the FLAGS_*_WINDOWS_X86_64 and FLAGS_DEBUG_LINUX_X86_64 linker setups. A link repeats the mode
 * flags, since optimisation, LTO, sanitizers and coverage all need them there too, and clang ignores
 * the preprocessor flags among them.
 */

// Only the project's own paths. Everything a third party dependency needs lives on its
// NYA_VendorRule instead, so this does not grow as dependencies are added.
#define INCLUDE_PATHS "-I./", "-I./src/"
#define LINKER_FLAGS  "-lm", "-pthread"

/*
 * Which optional plugins the *project* compiles. See src/nyangine/plugins/plugins.h.
 */
#define FLAGS_PLUGINS "-DNYA_PLUGIN_CURL", "-DNYA_PLUGIN_SQLITE", "-DNYA_PLUGIN_DISCORD", "-DNYA_PLUGIN_LUA"

/* Every mode sets NYA_EXECUTION_MODE explicitly. It defaults to 0, which means debug, not unset. */
#define FLAGS_DEBUG     "-DNYA_EXECUTION_MODE=0", "-DDEBUG=true", "-O0", "-DNYA_ASSET_HOT_RELOAD"

// Developer: hot reload like debug, but optimized and without sanitizers. For actually playing the
// game while iterating on it, where debug is too slow to feel right.
#define FLAGS_DEVELOPER "-DNYA_EXECUTION_MODE=1", "-O2", "-DNYA_ASSET_HOT_RELOAD"

// Test: assertions live, nya_expect_crash compiled in so a test can survive a deliberate panic, and
// headless so it needs no GPU. Statically linked game code, no hot reload DLL to find.
#define FLAGS_TEST      "-DNYA_EXECUTION_MODE=4", "-O0", "-DNYA_TESTING", "-DNYA_HEADLESS"

/**
 * Source based coverage instrumentation, for `./build run coverage`.
 * */
/**
 * What a benchmark is compiled with: optimised, headless, and *without* sanitizers.
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
#define FLAGS_DLL_COMPILE "-fPIC"
#define FLAGS_DLL_LINK    "-shared"
#define FLAGS_SANITIZE "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls", "-fno-sanitize-recover=all", "-fsanitize=address,leak,undefined,signed-integer-overflow,unsigned-integer-overflow,shift,float-cast-overflow,float-divide-by-zero,pointer-overflow"

// -g1 so libbacktrace can print lines in shipped crash reports. The debug sections are covered by the
// integrity CRC, so never strip after hook_insert_integrity_hash.
#define FLAGS_SHIPPING "-O3", "-flto", "-fPIE", "-g1", "-DNYA_ASSET_PREFER_BLOB", "-D_FORTIFY_SOURCE=2", "-fcf-protection=full", "-fstack-protector-strong", "-fno-omit-frame-pointer"

// -DNYA_EXECUTION_MODE=2 is required: NYA_DEBUG is (NYA_EXECUTION_MODE == 0) and the default is 0, so
// without it a release binary compiles the hot reload entry point and skips the integrity check.
#define FLAGS_RELEASE  "-DNYA_EXECUTION_MODE=2", FLAGS_SHIPPING

// --gc-sections drops the vendor functions nothing reaches, which NYA_VENDOR_OPTIMIZE put in sections of their own.
#define FLAGS_RELEASE_LINK "-fuse-ld=lld", "-Wl,--gc-sections"

// no local symbols, and no COFF symbol table: libbacktrace names frames from the -g1 debug info and never reads them.
#define FLAGS_RELEASE_LINK_LINUX_X86_64   "-Wl,--discard-all"
#define FLAGS_RELEASE_LINK_WINDOWS_X86_64 "-Xlinker", "-Xlink=-debug:dwarf,nosymtab"

/*
 * Steam is release plus the Steamworks plugin. Same shipping flags, a mode of its own, so the mode can gate overlay and
 * achievements without a second set of build rules.
 */
#define FLAGS_STEAM "-DNYA_EXECUTION_MODE=3", "-DNYA_PLUGIN_STEAM", FLAGS_SHIPPING

#define FLAGS_LINUX_X86_64   "-Wl,-rpath,$ORIGIN"
#define FLAGS_WINDOWS_X86_64 "-Wl,-subsystem,windows", "-static"

// mold is Linux only, and -rdynamic is what lets the hot reloaded game DLL resolve engine symbols
// out of the executable via dlopen(nullptr).
#define FLAGS_DEBUG_LINUX_X86_64   "-fuse-ld=mold", "-rdynamic"
// the PE equivalent: the executable exports its symbols and produces an import library for the game
// DLL, since Windows has no dlopen(nullptr).
// The import library name is per artifact, or a dev DLL would link against the debug executable's
// symbols and misbehave at runtime.
#define FLAGS_HOTRELOAD_WINDOWS_X86_64 "-fuse-ld=lld", "-Wl,--export-all-symbols"
#define FLAGS_DEBUG_WINDOWS_X86_64     FLAGS_HOTRELOAD_WINDOWS_X86_64, "-Wl,--out-implib," WINDOWS_X86_64_DEBUG_IMPLIB
#define FLAGS_DEV_WINDOWS_X86_64       FLAGS_HOTRELOAD_WINDOWS_X86_64, "-Wl,--out-implib," WINDOWS_X86_64_DEV_IMPLIB

/*
 * Authenticode signing of the shipped .exe. See hook_sign_windows_executable.
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

/*
 * The compiler cache split compile rules launch through. Unset, ccache is used when it runs; empty,
 * "0" or "off" never uses one; anything else names the launcher, which then has to exist.
 */
#define COMPILER_CACHE_PROGRAM "ccache"
#define COMPILER_CACHE_ENV     "NYA_CCACHE"

// clang-format on
