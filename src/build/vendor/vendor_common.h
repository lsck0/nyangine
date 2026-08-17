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
#include "build/toolchain.h"

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
 * Everything vendored is linked statically, so the shipped binary carries its dependencies with it
 * rather than relying on what happens to be installed. Only libraries that are guaranteed present
 * on a normal system are linked dynamically: libc, libm, pthread, dl, OpenGL, and the win32 system
 * DLLs.
 * */
#define NYA_CMAKE_STATIC                        \
    "-GNinja",                                  \
    "-DCMAKE_BUILD_TYPE=Release",               \
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",     \
    "-DBUILD_SHARED_LIBS=OFF"

// clang-format on

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VENDORS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */
