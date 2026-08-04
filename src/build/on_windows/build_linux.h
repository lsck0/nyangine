/**
 * @file build_linux.h
 *
 * Producing Linux binaries on a Windows host.
 *
 * This is the one combination in the host/target matrix that is not routinely exercised: unlike
 * mingw-w64, which ships everything needed to target Windows from Linux, targeting Linux from
 * Windows needs a full glibc sysroot that no toolchain provides out of the box. The rules mirror
 * on_linux/build_linux.h, and FLAGS_TARGET_LINUX_X86_64 is where the triple and --sysroot go once
 * such a sysroot exists.
 * */
#pragma once

#include "build/flags.h"
#include "build/hooks/hooks.h"
// The asset rules these depend on (build_shaders, index_assets, bundle_assets) live in
// build.c, which defines them above the point it includes this file.
#include "build/vendor/vendor.h"

NYA_INTERNAL NYA_BuildRule build_project_debug_executable_linux = {
    .name   = "build_project_debug_executable_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-o", LINUX_X86_64_DEBUG_BINARY,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_SANITIZE,
            FLAGS_DEBUG,
            FLAGS_DEBUG_LINUX_X86_64_ON_WINDOWS,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_linux_x86_64, &vendor_sdl_image_linux_x86_64, &vendor_sdl_ttf_linux_x86_64,
                       &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_dll_linux = {
    .name   = "build_project_debug_dll_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            DLL_SOURCE_PATH,
            "-o", LINUX_X86_64_DEBUG_DLL,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_SANITIZE,
            FLAGS_DEBUG,
            FLAGS_DEBUG_LINUX_X86_64_ON_WINDOWS,
            FLAGS_DLL,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_linux_x86_64, &vendor_sdl_image_linux_x86_64, &vendor_sdl_ttf_linux_x86_64,
                       &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_linux = {
    .name   = "build_project_debug_linux",
    .is_metarule  = true,
    .dependencies    = { &build_project_debug_executable_linux, &build_project_debug_dll_linux, },
};

NYA_INTERNAL NYA_BuildRule build_project_linux_x86_64 = {
    .name        = "build_project_linux_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_BINARY,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-o", LINUX_X86_64_BINARY,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_RELEASE,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks  = { &hook_add_version_flag_and_git_hash, },
    .vendors          = { &vendor_sdl_linux_x86_64, &vendor_sdl_image_linux_x86_64, &vendor_sdl_ttf_linux_x86_64,
                        &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
    .dependencies     = { &bundle_assets, }, // index_assets comes with it, in the right order
    .post_build_hooks = { &hook_insert_integrity_hash, },
};

/*
 * ─────────────────────────────────────────────────────────
 * DEVELOPER
 * ─────────────────────────────────────────────────────────
 */

/*
 * Same shape as the debug rules, different flag set: optimized, no sanitizers, still hot reloading.
 * The artifacts are named .dev so both can sit in the tree at once without the loader picking up
 * the wrong DLL.
 */

NYA_INTERNAL NYA_BuildRule build_project_dev_executable_linux = {
    .name   = "build_project_dev_executable_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-o", LINUX_X86_64_DEV_BINARY,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEBUG_LINUX_X86_64,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_linux_x86_64, &vendor_sdl_image_linux_x86_64, &vendor_sdl_ttf_linux_x86_64,
                       &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_dll_linux = {
    .name   = "build_project_dev_dll_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            DLL_SOURCE_PATH,
            "-o", LINUX_X86_64_DEV_DLL,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEBUG_LINUX_X86_64,
            FLAGS_DLL,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_linux_x86_64, &vendor_sdl_image_linux_x86_64, &vendor_sdl_ttf_linux_x86_64,
                       &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_linux = {
    .name         = "build_project_dev_linux",
    .is_metarule  = true,
    .dependencies = { &build_project_dev_executable_linux, &build_project_dev_dll_linux, },
};
