/**
 * @file vendor_steamrt.h
 *
 * The Steam Linux Runtime 3.0 (sniper) target. A Steam game runs inside that runtime's container, which has glibc 2.31
 * and no OpenSSL 3, so a binary linked against a current distribution does not start there. Everything is compiled
 * against Valve's sniper SDK sysroot instead, by the host clang: the engine needs clang 22, the SDK carries clang 11.
 *
 * The vendor rules are the Linux ones, derived rather than written out a third time. Each copy builds into
 * build-steamrt-x86_64 beside build-linux-x86_64, and every compiler it runs is handed the sysroot. Only the steam-linux
 * target builds them, since the sysroot is a 1.2 GB download.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks.h"
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
#include "build/vendor/vendor_sdl_ttf.h"
#include "build/vendor/vendor_sqlean.h"
#include "build/vendor/vendor_sqlite.h"
#include "build/vendor/vendor_sqlvec.h"
#include "build/vendor/vendor_ufbx.h"

/*
 * ─────────────────────────────────────────────────────────
 * THE SYSROOT
 * ─────────────────────────────────────────────────────────
 */

// clang-format off

/** A pinned snapshot, so a build does not change under a runtime update. The directory is per version. */
#define STEAMRT_VERSION         "3.0.20260805.254768"
#define STEAMRT_SYSROOT_PATH    "vendor/steamrt/sniper-" STEAMRT_VERSION
#define STEAMRT_SYSROOT         "./" STEAMRT_SYSROOT_PATH
#define STEAMRT_SYSROOT_ARCHIVE "sniper-" STEAMRT_VERSION ".tar.gz"
#define STEAMRT_SYSROOT_URL     "https://repo.steampowered.com/steamrt-images-sniper/snapshots/" STEAMRT_VERSION "/com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz"
#define STEAMRT_SYSROOT_SHA256  "32903ecfe7c06ce8cba26722adb876a665b6292606a0666f43d0bfd9beb2c10a"

/**
 * The dynamic loader is an absolute symlink in the archive and only resolves once the links point inside the sysroot,
 * so it existing means the unpack finished.
 * */
#define STEAMRT_SYSROOT_READY STEAMRT_SYSROOT "/lib64/ld-linux-x86-64.so.2"

// clang-format on

NYA_VendorRule vendor_steamrt_sysroot = {
    .name = "steamrt sniper sysroot",

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_steamrt_sysroot_download",
            .policy      = NYA_BUILD_ONCE,
            .output_file = STEAMRT_SYSROOT_READY,

            .command = {
                .working_directory = STEAMRT_SYSROOT,
                .program           = "curl",
                .arguments         = { "--fail", "--silent", "--show-error", "--location", "--output", "../" STEAMRT_SYSROOT_ARCHIVE, STEAMRT_SYSROOT_URL, },
            },

            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_steamrt_sysroot_verify",
            .policy      = NYA_BUILD_ONCE,
            .output_file = STEAMRT_SYSROOT_READY,

            .command = {
                .working_directory = STEAMRT_SYSROOT "/..",
                .program           = "sh",
                .arguments         = { "-c", "echo '" STEAMRT_SYSROOT_SHA256 "  " STEAMRT_SYSROOT_ARCHIVE "' | sha256sum --check --quiet", },
            },
        },
        /*
         * Only what a build reads: headers, the 64 bit libraries and pkg-config files, gcc 10's startup files, and the
         * wayland-scanner matching sniper's libwayland. 1.2 GB unpacked instead of 3.7. Not SDL, which the SDK carries
         * too: cmake searches the sysroot before a prefix path, so the SDL libraries would build against it.
         */
        &(NYA_BuildRule){
            .name        = "vendor_steamrt_sysroot_unpack",
            .policy      = NYA_BUILD_ONCE,
            .input_file  = STEAMRT_SYSROOT "/../" STEAMRT_SYSROOT_ARCHIVE,
            .output_file = STEAMRT_SYSROOT_READY,

            .command = {
                .working_directory = STEAMRT_SYSROOT,
                .program           = "tar",
                .arguments         = {
                    "--extract", "--gzip", "--file", "../" STEAMRT_SYSROOT_ARCHIVE, "--exclude=*SDL[23]*", "--exclude=*sdl[23]*",
                    "usr/include", "usr/lib/x86_64-linux-gnu", "usr/lib/gcc/x86_64-linux-gnu/10", "usr/share/pkgconfig",
                    "usr/bin/wayland-scanner", "lib/x86_64-linux-gnu", "lib64",
                },
            },

            .post_build_hooks = { &hook_relativize_symlinks, &hook_remove_input_file, },
        },
    },
};

/*
 * ─────────────────────────────────────────────────────────
 * THE DERIVED VENDORS
 * ─────────────────────────────────────────────────────────
 */

NYA_VendorRule vendor_sdl_steamrt_x86_64;
NYA_VendorRule vendor_sdl_image_steamrt_x86_64;
NYA_VendorRule vendor_sdl_ttf_steamrt_x86_64;
NYA_VendorRule vendor_sdl_mixer_steamrt_x86_64;
NYA_VendorRule vendor_sdl_net_steamrt_x86_64;
NYA_VendorRule vendor_libbacktrace_steamrt_x86_64;
NYA_VendorRule vendor_box2d_steamrt_x86_64;
NYA_VendorRule vendor_box3d_steamrt_x86_64;
NYA_VendorRule vendor_curl_steamrt_x86_64;
NYA_VendorRule vendor_lua_steamrt_x86_64;
NYA_VendorRule vendor_lz4_steamrt_x86_64;
NYA_VendorRule vendor_sqlean_steamrt_x86_64;
NYA_VendorRule vendor_sqlvec_steamrt_x86_64;
NYA_VendorRule vendor_sqlite_steamrt_x86_64;
NYA_VendorRule vendor_ufbx_steamrt_x86_64;

// clang-format off

/** NYA_PROJECT_VENDORS_LINUX_X86_64, in the same link order. */
#define NYA_PROJECT_VENDORS_STEAMRT_X86_64                                                              \
    &vendor_sdl_steamrt_x86_64,     &vendor_sdl_image_steamrt_x86_64,  &vendor_sdl_ttf_steamrt_x86_64,  \
    &vendor_sdl_mixer_steamrt_x86_64, &vendor_sdl_net_steamrt_x86_64,  &vendor_libbacktrace_steamrt_x86_64, \
    &vendor_box2d_steamrt_x86_64,   &vendor_box3d_steamrt_x86_64,      &vendor_curl_steamrt_x86_64,     \
    &vendor_lua_steamrt_x86_64,     &vendor_lz4_steamrt_x86_64,        &vendor_sqlean_steamrt_x86_64,   \
    &vendor_sqlvec_steamrt_x86_64,  &vendor_sqlite_steamrt_x86_64,     &vendor_ufbx_steamrt_x86_64

// clang-format on

/** The sysroot first, then sqlite ahead of the extensions compiled against its header, as in NYA_VENDORS_LINUX_X86_64. */
NYA_VendorRule* NYA_VENDORS_STEAMRT_X86_64[] = {
    &vendor_steamrt_sysroot,
    &vendor_sqlite_steamrt_x86_64,
    NYA_PROJECT_VENDORS_STEAMRT_X86_64,
    nullptr,
};

#define _NYA_STEAMRT_EXTRA_MAX 8

typedef struct {
    const NYA_VendorRule* linux;
    NYA_VendorRule*       steamrt;

    /** Appended to a cmake configure, `%SYSROOT%` expanded. */
    NYA_ConstCString configure[_NYA_STEAMRT_EXTRA_MAX];

    /** Replaces the derived linker flags when set. */
    NYA_ConstCString linker_flags[NYA_VENDOR_MAX_FLAGS];
} _NYA_SteamrtVendor;

NYA_INTERNAL _NYA_SteamrtVendor _NYA_STEAMRT_VENDORS[] = {
    // protocol code generated for sniper's libwayland 1.18; the host scanner emits calls it does not have.
    { .linux = &vendor_sdl_linux_x86_64, .steamrt = &vendor_sdl_steamrt_x86_64, .configure = { "-DWAYLAND_SCANNER=%SYSROOT%/usr/bin/wayland-scanner", }, },
    { .linux = &vendor_sdl_image_linux_x86_64, .steamrt = &vendor_sdl_image_steamrt_x86_64 },
    { .linux = &vendor_sdl_ttf_linux_x86_64, .steamrt = &vendor_sdl_ttf_steamrt_x86_64 },
    { .linux = &vendor_sdl_mixer_linux_x86_64, .steamrt = &vendor_sdl_mixer_steamrt_x86_64 },
    { .linux = &vendor_sdl_net_linux_x86_64, .steamrt = &vendor_sdl_net_steamrt_x86_64 },
    { .linux = &vendor_libbacktrace_linux_x86_64, .steamrt = &vendor_libbacktrace_steamrt_x86_64 },
    { .linux = &vendor_box2d_linux_x86_64, .steamrt = &vendor_box2d_steamrt_x86_64 },
    { .linux = &vendor_box3d_linux_x86_64, .steamrt = &vendor_box3d_steamrt_x86_64 },
    /*
     * GnuTLS from the runtime instead of OpenSSL: sniper has no OpenSSL 3, and a static 1.1 would freeze its security
     * fixes into the game, while the runtime's GnuTLS is Valve's to patch. The CA paths are Debian's, which sniper is.
     */
    {
        .linux        = &vendor_curl_linux_x86_64,
        .steamrt      = &vendor_curl_steamrt_x86_64,
        .configure    = { "-DCURL_USE_OPENSSL=OFF", "-DCURL_USE_GNUTLS=ON", "-DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt", "-DCURL_CA_PATH=/etc/ssl/certs", },
        .linker_flags = { "./vendor/curl/build-steamrt-x86_64/lib/libcurl.a", "-lgnutls", "-lnettle", },
    },
    { .linux = &vendor_lua_linux_x86_64, .steamrt = &vendor_lua_steamrt_x86_64 },
    { .linux = &vendor_lz4_linux_x86_64, .steamrt = &vendor_lz4_steamrt_x86_64 },
    { .linux = &vendor_sqlean_linux_x86_64, .steamrt = &vendor_sqlean_steamrt_x86_64 },
    { .linux = &vendor_sqlvec_linux_x86_64, .steamrt = &vendor_sqlvec_steamrt_x86_64 },
    { .linux = &vendor_sqlite_linux_x86_64, .steamrt = &vendor_sqlite_steamrt_x86_64 },
    { .linux = &vendor_ufbx_linux_x86_64, .steamrt = &vendor_ufbx_steamrt_x86_64 },
};

/** `text` with every "linux" made "steamrt", which is what moves names, build directories and archives aside. */
NYA_INTERNAL NYA_ConstCString _nya_steamrt_rename(NYA_ConstCString text) {
    if (text == nullptr || strstr(text, "linux") == nullptr) return text;

    NYA_String* renamed = nya_string_from(nya_arena_global, text);
    nya_string_replace(renamed, "linux", "steamrt");
    return nya_string_to_cstring(nya_arena_global, renamed);
}

NYA_INTERNAL void _nya_steamrt_append(NYA_BuildRule* part, NYA_ConstCString argument) {
    u32 count = 0;
    while (count < NYA_COMMAND_MAX_ARGUMENTS && part->command.arguments[count] != nullptr) count++;

    nya_assert(count + 1 < NYA_COMMAND_MAX_ARGUMENTS, "No room to hand '%s' the Steam Runtime sysroot.", part->name);
    part->command.arguments[count] = argument;
}

NYA_INTERNAL NYA_ConstCString _nya_steamrt_format(NYA_ConstCString format, NYA_ConstCString sysroot) {
    NYA_String* text = nya_string_from(nya_arena_global, format);
    nya_string_replace(text, "%SYSROOT%", sysroot);
    return nya_string_to_cstring(nya_arena_global, text);
}

/** A copy of a Linux vendor part, moved aside and compiling against `sysroot`. */
NYA_INTERNAL NYA_BuildRule* _nya_steamrt_derive_part(const NYA_BuildRule* linux, const _NYA_SteamrtVendor* vendor, NYA_ConstCString sysroot) {
    NYA_BuildRule* part = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
    nya_assert(part != nullptr);
    *part = *linux;

    part->name                      = _nya_steamrt_rename(part->name);
    part->input_file                = _nya_steamrt_rename(part->input_file);
    part->output_file               = _nya_steamrt_rename(part->output_file);
    part->command.working_directory = _nya_steamrt_rename(part->command.working_directory);

    b8 configures_cmake = false;

    for (u32 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS && part->command.arguments[i] != nullptr; i++) {
        NYA_ConstCString argument = _nya_steamrt_rename(part->command.arguments[i]);

        // make and configure take the compiler as a variable.
        if (nya_string_starts_with(argument, "CC=")) {
            argument = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%s --sysroot=%s", argument, sysroot));
        }
        if (nya_string_equals(argument, "-S")) configures_cmake = true;

        part->command.arguments[i] = argument;
    }

    if (part->command.program != nullptr && nya_string_equals(part->command.program, CC)) {
        _nya_steamrt_append(part, _nya_steamrt_format("--sysroot=%SYSROOT%", sysroot));
    }

    if (part->command.program != nullptr && nya_string_equals(part->command.program, "cmake")) {
        u32 environment = 0;
        while (environment < NYA_COMMAND_MAX_ENV_VARS && part->command.environment[environment] != nullptr) environment++;
        nya_assert(environment + 2 < NYA_COMMAND_MAX_ENV_VARS);

        // pkg-config ignores CMAKE_SYSROOT, and the host's .pc files describe newer libraries than sniper has. Set on
        // the build too, which reconfigures when a CMakeLists.txt changed.
        part->command.environment[environment++] = (NYA_CString)_nya_steamrt_format("PKG_CONFIG_SYSROOT_DIR=%SYSROOT%", sysroot);
        part->command.environment[environment++] =
            (NYA_CString)_nya_steamrt_format("PKG_CONFIG_LIBDIR=%SYSROOT%/usr/lib/x86_64-linux-gnu/pkgconfig:%SYSROOT%/usr/share/pkgconfig", sysroot);
    }

    if (configures_cmake) {
        // clang and ninja from the host, not the SDK's clang 11. lld, because GNU ld resolves a shared library's own
        // dependencies against the host and fails every configure check that links X11.
        NYA_ConstCString toolchain[] = {
            "-DCMAKE_C_COMPILER=" CC,
            "-DCMAKE_CXX_COMPILER=" CC "++",
            "-DCMAKE_SYSROOT=%SYSROOT%",
            "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
            "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld",
            "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",
        };
        for (u32 i = 0; i < nya_carray_length(toolchain); i++) _nya_steamrt_append(part, _nya_steamrt_format(toolchain[i], sysroot));

        for (u32 i = 0; i < _NYA_STEAMRT_EXTRA_MAX && vendor->configure[i] != nullptr; i++) {
            _nya_steamrt_append(part, _nya_steamrt_format(vendor->configure[i], sysroot));
        }
    }

    return part;
}

/**
 * Derives the Steam Runtime vendors on first use and builds them, the sysroot first.
 * */
NYA_INTERNAL NYA_Error nya_vendor_steamrt_build(void) {
    static b8 derived = false;

    if (!derived) {
        derived = true;

        char working_directory[4096];
        if (getcwd(working_directory, sizeof(working_directory)) == nullptr) return nya_error(NYA_ERROR_IO, "could not read the working directory");

        // absolute: cmake and the compilers run from inside the build directories.
        NYA_ConstCString sysroot = nya_string_to_cstring(nya_arena_global, nya_path_join(nya_arena_global, working_directory, STEAMRT_SYSROOT_PATH));

        for (u32 v = 0; v < nya_carray_length(_NYA_STEAMRT_VENDORS); v++) {
            const _NYA_SteamrtVendor* vendor = &_NYA_STEAMRT_VENDORS[v];

            *vendor->steamrt      = *vendor->linux;
            vendor->steamrt->name = _nya_steamrt_rename(vendor->linux->name);

            for (u32 i = 0; i < NYA_VENDOR_MAX_FLAGS; i++) {
                vendor->steamrt->cflags[i]       = _nya_steamrt_rename(vendor->linux->cflags[i]);
                vendor->steamrt->includes[i]     = _nya_steamrt_rename(vendor->linux->includes[i]);
                vendor->steamrt->linker_flags[i] = _nya_steamrt_rename(vendor->linux->linker_flags[i]);
            }

            if (vendor->linker_flags[0] != nullptr) nya_memcpy(vendor->steamrt->linker_flags, vendor->linker_flags, sizeof(vendor->linker_flags));

            for (u32 i = 0; i < NYA_VENDOR_MAX_PARTS && vendor->linux->parts[i] != nullptr; i++) {
                vendor->steamrt->parts[i] = _nya_steamrt_derive_part(vendor->linux->parts[i], vendor, sysroot);
            }
        }
    }

    return nya_vendor_build_all(NYA_VENDORS_STEAMRT_X86_64);
}
