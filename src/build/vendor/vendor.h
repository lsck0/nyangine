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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VENDOR LISTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Everything needed to produce a Linux target. */
NYA_VendorRule* NYA_VENDORS_LINUX_X86_64[] = {
    &vendor_sdl_linux_x86_64,
    &vendor_libbacktrace_linux_x86_64,
    &vendor_sdl_shadercross_linux_x86_64,
    nullptr,
};

/** Everything needed to produce a Windows target. Requires the mingw-w64 toolchain. */
NYA_VendorRule* NYA_VENDORS_WINDOWS_X86_64[] = {
    &vendor_sdl_windows_x86_64,
    &vendor_libbacktrace_windows_x86_64,
    nullptr,
};

/**
 * The vendors the engine currently compiles and links against, for every target.
 *
 * This is what gets built before any rule runs, so it deliberately holds only what is actually
 * consumed. Adding a dependency here makes every command, even `./build stats`, wait for it to
 * build on a clean checkout.
 *
 * Note that it includes the Windows cross builds, so a machine without mingw-w64 will fail here.
 * Swap in NYA_VENDORS_LINUX_X86_64 if that is not wanted.
 * */
NYA_VendorRule* NYA_VENDORS[] = {
    &vendor_sdl_linux_x86_64,          &vendor_sdl_windows_x86_64,          &vendor_sdl_image_linux_x86_64,       &vendor_sdl_image_windows_x86_64,
    &vendor_sdl_ttf_linux_x86_64,      &vendor_sdl_ttf_windows_x86_64,      &vendor_sdl_mixer_linux_x86_64,       &vendor_sdl_mixer_windows_x86_64,
    &vendor_libbacktrace_linux_x86_64, &vendor_libbacktrace_windows_x86_64, &vendor_sdl_shadercross_linux_x86_64, nullptr,
};

/**
 * Every vendor that has a rule, including ones nothing links against yet.
 *
 * Kept separate from NYA_VENDORS on purpose: these are described but not yet consumed, so building
 * them on every invocation would be a long wait for nothing. `./build vendor` uses this list, so
 * there is still one command that brings the whole tree up.
 * */
NYA_VendorRule* NYA_VENDORS_ALL[] = {
    &vendor_sdl_linux_x86_64,
    &vendor_sdl_windows_x86_64,
    &vendor_libbacktrace_linux_x86_64,
    &vendor_libbacktrace_windows_x86_64,
    &vendor_sdl_shadercross_linux_x86_64,
    &vendor_sdl_image_linux_x86_64,
    &vendor_sdl_image_windows_x86_64,
    &vendor_sdl_mixer_linux_x86_64,
    &vendor_sdl_mixer_windows_x86_64,
    &vendor_sdl_net_linux_x86_64,
    &vendor_sdl_net_windows_x86_64,
    &vendor_sdl_ttf_linux_x86_64,
    &vendor_sdl_ttf_windows_x86_64,
    &vendor_box2d_linux_x86_64,
    &vendor_box2d_windows_x86_64,
    &vendor_box3d_linux_x86_64,
    &vendor_box3d_windows_x86_64,
    &vendor_curl_linux_x86_64,
    &vendor_curl_windows_x86_64,
    &vendor_lua_linux_x86_64,
    &vendor_lua_windows_x86_64,
    &vendor_lz4_linux_x86_64,
    &vendor_lz4_windows_x86_64,
    &vendor_sqlite_linux_x86_64,
    &vendor_sqlite_windows_x86_64,
    // sqlean has no build parts, see vendor_sqlean.h.
    &vendor_sqlean_linux_x86_64,
    &vendor_sqlean_windows_x86_64,
    nullptr,
};
