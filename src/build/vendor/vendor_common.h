/**
 * @file vendor_common.h
 *
 * Settings every vendor rule shares: the compiler, the parallel job count, and the cmake flags that
 * make a dependency build statically.
 *
 * Split out of vendor.h because the dependency runs the wrong way for a single file: vendor.h
 * *includes* the individual vendor headers, so anything it defined after that point was invisible
 * to them. They compiled only because build.c happened to include vendor.h first, which meant
 * opening any vendor_*.h on its own reported undefined identifiers.
 * */
#pragma once

#include "nyangine/nyangine.h"
// For the per host toolchain names the vendor rules expand, e.g. NYA_WINDOWS_CC and NYA_CMAKE_WINDOWS_TOOLCHAIN.
#include "build/flags.h"

// For GetSystemInfo below, and after nyangine.h because that is what defines OS_WINDOWS.
//
// Included here rather than relied upon: nothing in nyangine.h pulls windows.h in, only the
// platform .c files do, so this header used to compile on a Windows host purely because build.c
// included nyangine.c ahead of build.h. It no longer does, and a header should not need its
// includer to have gone first anyway.
#if OS_WINDOWS
#include <windows.h>
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHARED BUILD SETTINGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define CC "clang"

/*
 * The cache in front of the compiler, for the vendors and for nothing else.
 *
 * Only the vendors, deliberately. A cache decides it has seen a compile before by hashing what the
 * compiler would see, and sccache does that by preprocessing the translation unit: this engine is a
 * unity build, so one translation unit is the whole of it, and preprocessing that costs nearly as much
 * as compiling it — measured, a hit on the engine saved 185 ms of 2750. It gets worse as the engine
 * grows, and a miss is the preprocess on top of the compile rather than instead of it, so the cache
 * would eventually cost more than it saves on exactly the translation units that are growing. The vendors are the opposite
 * shape, hundreds of small files at -O2, and they are what a fresh checkout or a worktree rebuilds from
 * nothing. So the engine, the tests and the examples call the compiler directly and the vendors go
 * through the cache.
 *
 * Runtime strings because whether there is a cache is a property of the machine, and taking their
 * address is still a compile time constant, so they sit inside the static vendor rules below.
 * VENDOR_CC is the program to run and VENDOR_CC_LAUNCHED goes in front of the arguments: with a cache
 * that is "sccache" and "clang", without one "clang" and "", which the command runner drops.
 */
extern char NYA_VENDOR_CC[16];
extern char NYA_VENDOR_CC_LAUNCHED[16];

/** `CC=...` for a vendor whose Makefile or configure script takes one. Both words when there is a cache. */
extern char NYA_VENDOR_CC_MAKE[32];

/** `-DCMAKE_C_COMPILER_LAUNCHER=...`, or "" for no cache, which the command runner drops. */
extern char NYA_VENDOR_CMAKE_LAUNCHER[48];

#define VENDOR_CC             NYA_VENDOR_CC
#define VENDOR_CC_LAUNCHED    NYA_VENDOR_CC_LAUNCHED
#define VENDOR_CC_MAKE        NYA_VENDOR_CC_MAKE
#define VENDOR_CMAKE_LAUNCHER NYA_VENDOR_CMAKE_LAUNCHER

char NYA_VENDOR_CC[16]             = CC;
char NYA_VENDOR_CC_LAUNCHED[16]    = "";
char NYA_VENDOR_CC_MAKE[32]        = "CC=" CC;
char NYA_VENDOR_CMAKE_LAUNCHER[48] = "";

/**
 * Parallel job count, as a string, for `make -j` and `cmake --build -- -j`.
 *
 * A buffer rather than a literal because the value is the machine's core count, which is only
 * known at runtime. Taking its address is still a compile time constant, so it can sit inside the
 * static vendor rule initializers below. nya_vendor_detect_nprocs fills it in; until then it reads
 * as a conservative "1".
 * */
extern char NYA_NPROCS[8];
#define NPROCS NYA_NPROCS

char NYA_NPROCS[8] = "1";

/**
 * Points the vendor builds at sccache when it is on the PATH, and changes nothing when it is not.
 *
 * Asking sccache for its version rather than looking for the file: a binary that is there but cannot
 * run — the wrong architecture, a broken install, a stale wrapper — would otherwise fail every compile
 * in the tool with a message about the cache rather than about the code. Call once, before building.
 * */
NYA_INTERNAL void nya_vendor_detect_compiler_cache(void) {
    /*
     * sccache first because it is the one that can be central: its storage is a directory, an S3
     * bucket or a Redis, so every worktree on this machine and a CI runner share one cache, and a
     * vendor set built once is built for all of them. ccache second, which caches per machine.
     */
    static const NYA_ConstCString CACHES[] = { "sccache", "ccache" };

    for (u32 index = 0; index < nya_carray_length(CACHES); index++) {
        NYA_Command probe = {
            .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
            .program   = CACHES[index],
            .arguments = { "--version", nullptr },
        };

        NYA_Error started = nya_command_run(&probe);
        defer nya_command_destroy(&probe);

        // asking it for its version rather than looking for the file: a binary that is there but
        // cannot run would otherwise fail every compile with a message about the cache, not the code.
        if (!started.ok || probe.exit_code != 0) continue;

        (void)snprintf(NYA_VENDOR_CC, sizeof(NYA_VENDOR_CC), "%s", CACHES[index]);
        (void)snprintf(NYA_VENDOR_CC_LAUNCHED, sizeof(NYA_VENDOR_CC_LAUNCHED), "%s", CC);
        (void)snprintf(NYA_VENDOR_CC_MAKE, sizeof(NYA_VENDOR_CC_MAKE), "CC=%s " CC, CACHES[index]);
        (void)snprintf(NYA_VENDOR_CMAKE_LAUNCHER, sizeof(NYA_VENDOR_CMAKE_LAUNCHER), "-DCMAKE_C_COMPILER_LAUNCHER=%s", CACHES[index]);

        nya_log_info("Building the vendors through %s.", CACHES[index]);

        return;
    }

    nya_log_debug("No compiler cache on the PATH; building the vendors without one.");
}

/** Fills NYA_NPROCS with the number of online cores. Call once, before building anything. */
NYA_INTERNAL void nya_vendor_detect_nprocs(void) {
    u32 cores = 1;

#if OS_WINDOWS
    SYSTEM_INFO system_info = { 0 };
    GetSystemInfo(&system_info);
    if (system_info.dwNumberOfProcessors > 0) cores = (u32)system_info.dwNumberOfProcessors;
#else
    s64 online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) cores = (u32)online;
#endif

    (void)snprintf(NYA_NPROCS, sizeof(NYA_NPROCS), FMTu32, cores);
}

/*
 * ─────────────────────────────────────────────────────────
 * SHARED CMAKE SETTINGS
 * ─────────────────────────────────────────────────────────
 */

// clang-format off

/**
 * How every vendor is optimised. Each function and object gets its own section, so the release link's --gc-sections
 * drops whatever the engine never reaches instead of keeping a whole object for one call into it.
 *
 * -O2 rather than cmake's -O3: -O3 made the vendors 0.85 MB bigger on Linux and 1.5 MB on Windows, and neither the
 * benches nor the frame times of either scene moved by more than their noise. The engine stays at -O3, where it pays.
 * */
#define NYA_VENDOR_OPTIMIZE "-O2 -DNDEBUG -ffunction-sections -fdata-sections"

/** The section flags on their own, for a vendor compiled directly that sets its own optimisation. */
#define NYA_VENDOR_SECTIONS "-ffunction-sections", "-fdata-sections"

/** NYA_VENDOR_OPTIMIZE for a cmake project, whose release build type would otherwise pick its own. */
#define NYA_CMAKE_OPTIMIZE                                  \
    "-DCMAKE_C_FLAGS_RELEASE=" NYA_VENDOR_OPTIMIZE,         \
    "-DCMAKE_CXX_FLAGS_RELEASE=" NYA_VENDOR_OPTIMIZE

/**
 * Everything vendored is linked statically, so the shipped binary carries its dependencies with it
 * rather than relying on what happens to be installed. Only libraries that are guaranteed present
 * on a normal system are linked dynamically: libc, libm, pthread, dl, OpenGL, and the win32 system
 * DLLs.
 * */
#define NYA_CMAKE_STATIC                        \
    "-GNinja",                                  \
    "-DCMAKE_BUILD_TYPE=Release",               \
    NYA_CMAKE_OPTIMIZE,                         \
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",     \
    "-DBUILD_SHARED_LIBS=OFF",                  \
    VENDOR_CMAKE_LAUNCHER

// clang-format on

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VENDORS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * STEAMWORKS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Valve ships the Steamworks SDK prebuilt, so there is nothing to build and no file of its own to put
 * it in: a Steam target links the library and carries it beside the executable. The plugin declares
 * the flat C functions it calls, so no header is included either.
 */

// clang-format off

#define STEAM_REDISTRIBUTABLE_LINUX_X86_64   "./vendor/steam/redistributable_bin/linux64"
#define STEAM_REDISTRIBUTABLE_WINDOWS_X86_64 "./vendor/steam/redistributable_bin/win64"

#define STEAM_LIBRARY_LINUX_X86_64   STEAM_REDISTRIBUTABLE_LINUX_X86_64 "/libsteam_api.so"
#define STEAM_LIBRARY_WINDOWS_X86_64 STEAM_REDISTRIBUTABLE_WINDOWS_X86_64 "/steam_api64.dll"

// clang-format on

NYA_VendorRule vendor_steam_linux_x86_64 = {
    .name = "steam (linux-x86_64)",

    .linker_flags = { "-L" STEAM_REDISTRIBUTABLE_LINUX_X86_64, "-lsteam_api", },
};

NYA_VendorRule vendor_steam_windows_x86_64 = {
    .name = "steam (windows-x86_64)",

    // steam_api64.lib is an MSVC import library, which lld reads in mingw mode too.
    .linker_flags = { "-L" STEAM_REDISTRIBUTABLE_WINDOWS_X86_64, "-lsteam_api64", },
};
