/**
 * @file vendor.h
 *
 * Every vendored third party dependency, one NYA_VendorRule per dependency per target.
 *
 * A vendor rule fully describes one dependency: how to build it, and what a consumer needs in
 * order to compile and link against it. A build rule never spells any of that out, it just names
 * the vendor in its `.vendors` list and the build system splices the flags in. That is what stops
 * a growing dependency list from turning into a growing pile of per-target flag macros.
 *
 * Everything in NYA_VENDORS is built up front by nya_vendor_build_all before any rule runs, so no
 * rule ever has to worry about whether what it links against exists yet. Vendor parts are
 * NYA_BUILD_ONCE, so after the first run this costs a few file existence checks.
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
 * Everything the project links against, per target.
 *
 * One list per target rather than one per rule. Every project rule names the same set — the debug
 * pair, the developer pair and the release executable all link the same libraries — so spelling it
 * out fifteen times only created fifteen places for it to drift apart.
 *
 * These are also what NYA_VENDORS is built from, which is what keeps "built before anything runs"
 * and "spliced into the link line" from ever disagreeing.
 *
 * Order is not alphabetical and is not free: archives are searched left to right, so a dependency
 * that calls into another has to come first. sqlean and sqlvec both call into libsqlite3, which is
 * why they sit ahead of it rather than beside it.
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
 * Build order is not link order, and the arrays below are build order.
 *
 * sqlean and sqlvec compile against sqlite3.h, which sqlite's configure step generates, so sqlite has
 * to be *built* before either of them. They also call into libsqlite3.a, so on the link line they
 * have to come *before* it. Both constraints hold at once, and the project lists above are link
 * order because linking is what they are spliced into.
 *
 * So sqlite is named once more here, up front, purely to get it built early. Listing a vendor twice
 * costs a handful of file existence checks and nothing else: every part it has is NYA_BUILD_ONCE or
 * NYA_BUILD_IF_OUTDATED, and nya_build memoizes a rule it has already run this invocation.
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
 * The vendors the engine compiles and links against, for every target this host builds.
 *
 * This is what gets built before any rule runs, so a clean checkout pays for all of it once —
 * including on `./build stats`, which needs none of it. That is the price of the project rules
 * being able to assume everything they link against already exists.
 *
 * On a Linux host that means both targets, so a machine without mingw-w64 will fail here; swap in
 * NYA_VENDORS_LINUX_X86_64 if that is not wanted. A Windows host builds Windows only (see build.h),
 * and the Linux rules are left out rather than merely unused: none of them carries a cross
 * compiling toolchain, so on that host they would configure natively and quietly fill
 * build-linux-x86_64/ with Windows artifacts.
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
 * Every vendor that has a rule, including ones nothing links against yet.
 *
 * Kept separate from NYA_VENDORS on purpose: these are described but not yet consumed, so building
 * them on every invocation would be a long wait for nothing. `./build vendor` uses this list, so
 * there is still one command that brings the whole tree up.
 *
 * "Whole tree" means whole tree *for this host*: the Linux rules are omitted on Windows for the
 * same reason they are omitted from NYA_VENDORS.
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
