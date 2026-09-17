/**
 * @file vendor.h
 *
 * Every vendored dependency, one NYA_VendorRule per dependency per target. A vendor rule says how to
 * build it and what consumers compile and link with; a build rule names it in `.vendors` and the
 * flags are spliced in, so the lists cannot drift.
 *
 * NYA_VENDORS is built by nya_vendor_build_all before any rule runs. Parts are NYA_BUILD_ONCE, so
 * later runs only check that files exist.
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
#include "build/vendor/vendor_steam.h"
// a Linux host target: the sysroot is unpacked with symlinks, and the rules run make and configure directly.
#if !OS_WINDOWS
#include "build/vendor/vendor_steamrt.h"
#endif
#include "build/vendor/vendor_ufbx.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VENDOR LISTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off

/**
 * Everything the project links against, per target. Debug, developer and release link the same
 * libraries.
 *
 * Link order, not alphabetical: archives are searched left to right, so sqlean and sqlvec come before
 * libsqlite3, which they call into.
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
 * Build order, which differs from link order: sqlean and sqlvec compile against sqlite3.h, which
 * sqlite's configure generates, so sqlite is listed again first. Listing it twice is free, since its
 * parts are ONCE or IF_OUTDATED and nya_build memoizes rules within an invocation.
 */

/** Everything needed to produce a Linux target. */
NYA_VendorRule* NYA_VENDORS_LINUX_X86_64[] = {
    &vendor_sqlite_linux_x86_64,
    NYA_PROJECT_VENDORS_LINUX_X86_64,
    SHADERCROSS_HOST_VENDOR
    nullptr,
};

/** Everything needed to produce a Windows target. Requires the mingw-w64 toolchain on a Linux host. */
NYA_VendorRule* NYA_VENDORS_WINDOWS_X86_64[] = {
    &vendor_sqlite_windows_x86_64,
    NYA_PROJECT_VENDORS_WINDOWS_X86_64,
    SHADERCROSS_HOST_VENDOR
    nullptr,
};

/**
 * The vendors for every target this host builds, built before any rule runs (even `./build stats`) so
 * project rules can assume they exist.
 *
 * A Linux host builds both targets and needs mingw-w64; use NYA_VENDORS_LINUX_X86_64 to avoid that. A
 * Windows host builds Windows only (see build.h). The Linux rules carry no cross toolchain and would
 * configure natively, filling build-linux-x86_64/ with Windows artifacts.
 * */
NYA_VendorRule* NYA_VENDORS[] = {
#if !OS_WINDOWS
    &vendor_sqlite_linux_x86_64,
    NYA_PROJECT_VENDORS_LINUX_X86_64,
#endif
    &vendor_sqlite_windows_x86_64,
    NYA_PROJECT_VENDORS_WINDOWS_X86_64,
    // Unconditional: a host tool, needed to compile the shaders for whatever is being targeted.
    SHADERCROSS_HOST_VENDOR
    nullptr,
};

/**
 * Every vendor that has a rule, including ones nothing links against yet. Kept separate from
 * NYA_VENDORS so building the unused ones isn't a cost on every invocation; `./build vendor` uses
 * this list. Linux rules are still omitted on Windows, for the same reason as in NYA_VENDORS.
 * */
NYA_VendorRule* NYA_VENDORS_ALL[] = {
    // a host tool rather than a target artifact, so it is built on every host that can.
    SHADERCROSS_HOST_VENDOR
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
