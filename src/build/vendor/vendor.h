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

#include "nyangine-core/nyangine.h"
#include "build/flags.h"
#include "build/vendor/vendor_box2d.h"
#include "build/vendor/vendor_box3d.h"
#include "build/vendor/vendor_common.h"
#include "build/vendor/vendor_curl.h"
#include "build/vendor/vendor_libbacktrace.h"
#include "build/vendor/vendor_lua.h"
#include "build/vendor/vendor_lz4.h"
#include "build/vendor/vendor_monocypher.h"
// a rule with no parts: the system's OpenSSL, for the reason its own file gives.
#if !OS_WINDOWS
#include "build/vendor/vendor_openssl.h"
#endif
#include "build/vendor/vendor_sdl.h"
#include "build/vendor/vendor_sdl_image.h"
#include "build/vendor/vendor_sdl_mixer.h"
#include "build/vendor/vendor_sdl_shadercross.h"
#include "build/vendor/vendor_sdl_ttf.h"
#include "build/vendor/vendor_sqlean.h"
#include "build/vendor/vendor_sqlite.h"
#include "build/vendor/vendor_sqlvec.h"
// a Linux host target: the sysroot is unpacked with symlinks, and the rules run make and configure directly.
#if !OS_WINDOWS
#include "build/vendor/vendor_steamrt.h"
#endif
#include "build/vendor/vendor_ufbx.h"

/* VENDOR LISTS */

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
    &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, \
    &vendor_box2d_linux_x86_64,   &vendor_box3d_linux_x86_64,      &vendor_curl_linux_x86_64,       \
    &vendor_lua_linux_x86_64,     &vendor_lz4_linux_x86_64,        &vendor_sqlean_linux_x86_64,     \
    &vendor_sqlvec_linux_x86_64,  &vendor_sqlite_linux_x86_64,     &vendor_ufbx_linux_x86_64,       \
    &vendor_monocypher_linux_x86_64, &vendor_openssl_linux_x86_64

#define NYA_PROJECT_VENDORS_WINDOWS_X86_64                                                          \
    &vendor_sdl_windows_x86_64,   &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64, \
    &vendor_sdl_mixer_windows_x86_64, &vendor_libbacktrace_windows_x86_64, \
    &vendor_box2d_windows_x86_64, &vendor_box3d_windows_x86_64,     &vendor_curl_windows_x86_64,    \
    &vendor_lua_windows_x86_64,   &vendor_lz4_windows_x86_64,       &vendor_sqlean_windows_x86_64,  \
    &vendor_sqlvec_windows_x86_64, &vendor_sqlite_windows_x86_64,  &vendor_ufbx_windows_x86_64,     \
    &vendor_monocypher_windows_x86_64

/**
 * A headless server's subset of the Linux project vendors: everything a web app links and nothing a
 * window does. A server has no renderer, no physics and no model loader, so SDL and its three
 * companions, box2d, box3d and ufbx drop out, and — since they are only ever compiled to feed a
 * renderer — so does shadercross, which is why this list is spliced without SHADERCROSS_HOST_VENDOR.
 * What is left is storage (sqlite and its two extensions), the network client (curl over the system
 * OpenSSL), the hashing a session and a plugin signature need (monocypher), compression (lz4),
 * symbolized crash traces (libbacktrace) and the scripting runtime (lua).
 *
 * Same left-to-right link order as NYA_PROJECT_VENDORS_LINUX_X86_64, which the archives require:
 * sqlean and sqlvec are searched before libsqlite3, which they call into, and curl before the
 * `-lssl`/`-lcrypto` its TLS resolves against.
 *
 * A caveat the deploy story leans on: this is the vendor set a headless server *links*, and `./build
 * --server` builds exactly it and skips the rest. The web_server example is not yet a headless binary
 * — core, http and crypto sit behind NYA_NO_SDL in nyangine.h, so the example still compiles the full
 * engine graph and links the full set. This list is the target that seam is being cut toward, and the
 * thing the server bootstrap needs the day it is; see examples/web_server/deploy/README.md.
 * */
#define NYA_SERVER_VENDORS_LINUX_X86_64                                                            \
    &vendor_libbacktrace_linux_x86_64, &vendor_curl_linux_x86_64,   &vendor_lua_linux_x86_64,      \
    &vendor_lz4_linux_x86_64,          &vendor_sqlean_linux_x86_64, &vendor_sqlvec_linux_x86_64,   \
    &vendor_sqlite_linux_x86_64,       &vendor_monocypher_linux_x86_64, &vendor_openssl_linux_x86_64

// clang-format on

/* Build order, which differs from link order: sqlean and sqlvec compile against sqlite3.h, which sqlite's configure generates, so sqlite is listed again first. Listing it twice is free, since its parts are ONCE or IF_OUTDATED and nya_build memoizes rules within an invocation. */

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
 * The headless server target: the minimal subset, and nothing to cross-compile to Windows, since the
 * deploy target is a Linux container. `./build --server` builds this in place of NYA_VENDORS, so a
 * server build never compiles SDL, box2d, box3d, ufbx or shadercross — the point of the flag.
 *
 * sqlite is named first for the same build-order reason NYA_VENDORS gives: sqlean and sqlvec compile
 * against sqlite3.h, which sqlite's configure generates, so it has to have run once before them.
 * Listing it twice is free — its parts are ONCE and nya_build memoizes within an invocation. Linux
 * only, and so guarded like the openssl rule it names.
 * */
#if !OS_WINDOWS
NYA_VendorRule* NYA_VENDORS_SERVER_LINUX_X86_64[] = {
    &vendor_sqlite_linux_x86_64,
    NYA_SERVER_VENDORS_LINUX_X86_64,
    nullptr,
};
#endif

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
    &vendor_sdl_ttf_linux_x86_64,
    &vendor_box2d_linux_x86_64,
    &vendor_box3d_linux_x86_64,
    &vendor_curl_linux_x86_64,
    &vendor_lua_linux_x86_64,
    &vendor_lz4_linux_x86_64,
    &vendor_monocypher_linux_x86_64,
    &vendor_sqlite_linux_x86_64,
    &vendor_sqlean_linux_x86_64,
    &vendor_sqlvec_linux_x86_64,
#endif
    &vendor_sdl_windows_x86_64,
    &vendor_libbacktrace_windows_x86_64,
    &vendor_sdl_image_windows_x86_64,
    &vendor_sdl_mixer_windows_x86_64,
    &vendor_sdl_ttf_windows_x86_64,
    &vendor_box2d_windows_x86_64,
    &vendor_box3d_windows_x86_64,
    &vendor_curl_windows_x86_64,
    &vendor_lua_windows_x86_64,
    &vendor_lz4_windows_x86_64,
    &vendor_monocypher_windows_x86_64,
    &vendor_sqlite_windows_x86_64,
    &vendor_sqlean_windows_x86_64,
    &vendor_sqlvec_windows_x86_64,
    nullptr,
};
