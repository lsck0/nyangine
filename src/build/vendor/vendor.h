/**
 * @file vendor.h
 *
 * Every vendored third party dependency, one NYA_VendorRule per dependency per target. A vendor
 * rule describes how to build it and what a consumer needs to compile/link against it; a build
 * rule just names it in `.vendors` and the flags get spliced in, so the dependency list can't drift.
 *
 * NYA_VENDORS is built up front by nya_vendor_build_all before any rule runs; parts are
 * NYA_BUILD_ONCE so after the first run this is just a few file existence checks.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_box2d.h"
#include "build/vendor/vendor_box3d.h"
#include "build/vendor/vendor_common.h"
#include "build/vendor/vendor_curl.h"
#include "build/vendor/vendor_libbacktrace.h"
#include "build/vendor/vendor_lua.h"
#include "build/vendor/vendor_lz4.h"
#include "build/vendor/vendor_sdl.h"
#include "build/vendor/vendor_sdl_image.h"
#include "build/vendor/vendor_sdl_mixer.h"
#include "build/vendor/vendor_sdl_net.h"
#include "build/vendor/vendor_sdl_shadercross.h"
#include "build/vendor/vendor_sdl_ttf.h"
#include "build/vendor/vendor_sqlean.h"
#include "build/vendor/vendor_sqlite.h"
#include "build/vendor/vendor_sqlvec.h"
#include "build/vendor/vendor_ufbx.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VENDOR LISTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off

/**
 * Everything the project links against, per target. One list per target rather than one per rule,
 * since the debug/developer/release rules all link the same libraries.
 *
 * Order is not alphabetical: archives are searched left to right, so a dependency that calls into
 * another must come first — sqlean and sqlvec both call into libsqlite3, so they sit ahead of it.
 * */
#define NYA_PROJECT_VENDORS_LINUX_X86_64                                                            \
    &vendor_sdl_linux_x86_64,     &vendor_sdl_image_linux_x86_64,  &vendor_sdl_ttf_linux_x86_64,    \
    &vendor_sdl_mixer_linux_x86_64, &vendor_sdl_net_linux_x86_64,  &vendor_libbacktrace_linux_x86_64, \
    &vendor_box2d_linux_x86_64,   &vendor_box3d_linux_x86_64,      &vendor_curl_linux_x86_64,       \
    &vendor_lua_linux_x86_64,     &vendor_lz4_linux_x86_64,        &vendor_sqlean_linux_x86_64,     \
    &vendor_sqlvec_linux_x86_64,  &vendor_sqlite_linux_x86_64,     &vendor_ufbx_linux_x86_64

#define NYA_PROJECT_VENDORS_WINDOWS_X86_64                                                          \
    &vendor_sdl_windows_x86_64,   &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64, \
    &vendor_sdl_mixer_windows_x86_64, &vendor_sdl_net_windows_x86_64, &vendor_libbacktrace_windows_x86_64, \
    &vendor_box2d_windows_x86_64, &vendor_box3d_windows_x86_64,     &vendor_curl_windows_x86_64,    \
    &vendor_lua_windows_x86_64,   &vendor_lz4_windows_x86_64,       &vendor_sqlean_windows_x86_64,  \
    &vendor_sqlvec_windows_x86_64, &vendor_sqlite_windows_x86_64,  &vendor_ufbx_windows_x86_64

// clang-format on

/*
 * Build order is not link order, and the arrays below are build order: sqlean and sqlvec compile
 * against sqlite3.h, which sqlite's configure step generates, so sqlite must be built before them —
 * hence it's named again here, up front. Listing it twice costs nothing since its parts are
 * NYA_BUILD_ONCE/NYA_BUILD_IF_OUTDATED and nya_build memoizes rules already run this invocation.
 */

/** Everything needed to produce a Linux target. */
NYA_VendorRule* NYA_VENDORS_LINUX_X86_64[] = {
    &vendor_sqlite_linux_x86_64,
    NYA_PROJECT_VENDORS_LINUX_X86_64,
    &vendor_sdl_shadercross_host,
    nullptr,
};

/** Everything needed to produce a Windows target. Requires the mingw-w64 toolchain on a Linux host. */
NYA_VendorRule* NYA_VENDORS_WINDOWS_X86_64[] = {
    &vendor_sqlite_windows_x86_64,
    NYA_PROJECT_VENDORS_WINDOWS_X86_64,
    &vendor_sdl_shadercross_host,
    nullptr,
};

/**
 * The vendors the engine compiles and links against, for every target this host builds. Built
 * before any rule runs (even `./build stats`), so project rules can assume everything they link
 * against already exists.
 *
 * On a Linux host that means both targets, so a machine without mingw-w64 will fail here; swap in
 * NYA_VENDORS_LINUX_X86_64 if that is not wanted. A Windows host builds Windows only (see build.h);
 * the Linux rules are left out rather than merely unused because none carries a cross compiling
 * toolchain, so they would configure natively and quietly fill build-linux-x86_64/ with Windows
 * artifacts.
 * */
NYA_VendorRule* NYA_VENDORS[] = {
#if !OS_WINDOWS
    &vendor_sqlite_linux_x86_64,
    NYA_PROJECT_VENDORS_LINUX_X86_64,
#endif
    &vendor_sqlite_windows_x86_64,
    NYA_PROJECT_VENDORS_WINDOWS_X86_64,
    // Unconditional: a host tool, needed to compile the shaders for whatever is being targeted.
    &vendor_sdl_shadercross_host,
    nullptr,
};

/**
 * Every vendor that has a rule, including ones nothing links against yet. Kept separate from
 * NYA_VENDORS so building the unused ones isn't a cost on every invocation; `./build vendor` uses
 * this list. Linux rules are still omitted on Windows, for the same reason as in NYA_VENDORS.
 * */
NYA_VendorRule* NYA_VENDORS_ALL[] = {
    // A host tool rather than a target artifact, so it is built on every host.
    &vendor_sdl_shadercross_host,
#if !OS_WINDOWS
    &vendor_sdl_linux_x86_64,
    &vendor_libbacktrace_linux_x86_64,
    &vendor_sdl_image_linux_x86_64,
    &vendor_sdl_mixer_linux_x86_64,
    &vendor_sdl_net_linux_x86_64,
    &vendor_sdl_ttf_linux_x86_64,
    &vendor_box2d_linux_x86_64,
    &vendor_box3d_linux_x86_64,
    &vendor_curl_linux_x86_64,
    &vendor_lua_linux_x86_64,
    &vendor_lz4_linux_x86_64,
    &vendor_sqlite_linux_x86_64,
    &vendor_sqlean_linux_x86_64,
    &vendor_sqlvec_linux_x86_64,
#endif
    &vendor_sdl_windows_x86_64,
    &vendor_libbacktrace_windows_x86_64,
    &vendor_sdl_image_windows_x86_64,
    &vendor_sdl_mixer_windows_x86_64,
    &vendor_sdl_net_windows_x86_64,
    &vendor_sdl_ttf_windows_x86_64,
    &vendor_box2d_windows_x86_64,
    &vendor_box3d_windows_x86_64,
    &vendor_curl_windows_x86_64,
    &vendor_lua_windows_x86_64,
    &vendor_lz4_windows_x86_64,
    &vendor_sqlite_windows_x86_64,
    &vendor_sqlean_windows_x86_64,
    &vendor_sqlvec_windows_x86_64,
    nullptr,
};
