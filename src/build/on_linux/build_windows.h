/**
 * @file build_windows.h
 *
 * Producing Windows binaries on a Linux host, so every rule here is a mingw-w64 cross compile.
 * That is the only thing separating these rules from their on_windows counterparts: the target
 * triple, and a resource compiler that has to be the mingw one.
 * */
#pragma once

#include "build/flags.h"
#include "build/hooks/hooks.h"
// The asset rules these depend on (build_shaders, index_assets, bundle_assets) live in
// build.c, which defines them above the point it includes this file.
#include "build/vendor/vendor.h"

/*
 * The Windows debug pair. Unlike ELF, a PE DLL cannot reach back into the executable that loaded
 * it, so the executable exports its symbols into an import library and the game DLL links against
 * that. Everything else mirrors the Linux pair.
 *
 * No sanitizers here: asan on mingw is not usable in the same way it is on Linux.
 */

NYA_INTERNAL NYA_BuildRule build_project_debug_executable_windows = {
    .name   = "build_project_debug_executable_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-o", WINDOWS_X86_64_DEBUG_BINARY,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_DEBUG,
            FLAGS_DEBUG_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_windows_x86_64, &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64,
                       &vendor_sdl_mixer_windows_x86_64, &vendor_libbacktrace_windows_x86_64, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_dll_windows = {
    .name   = "build_project_debug_dll_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            DLL_SOURCE_PATH,
            "-o", WINDOWS_X86_64_DEBUG_DLL,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_DEBUG,
            "-shared",
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            // Resolves the engine symbols out of the executable.
            WINDOWS_X86_64_DEBUG_IMPLIB,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_windows_x86_64, &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64,
                       &vendor_sdl_mixer_windows_x86_64, &vendor_libbacktrace_windows_x86_64, },
    .dependencies    = { &build_project_debug_executable_windows, &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_windows = {
    .name         = "build_project_debug_windows",
    .is_metarule  = true,
    .dependencies = { &build_project_debug_executable_windows, &build_project_debug_dll_windows, },
};

NYA_INTERNAL NYA_BuildRule build_project_windows_x86_64 = {
    .name        = "build_project_windows_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_BINARY,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-o", WINDOWS_X86_64_BINARY,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_RELEASE,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            "./assets/icon/icon.res",
        },
    },

    .pre_build_hooks  = { &hook_add_version_flag_and_git_hash, },
    .vendors          = { &vendor_sdl_windows_x86_64, &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64,
                        &vendor_sdl_mixer_windows_x86_64, &vendor_libbacktrace_windows_x86_64, },
    .dependencies     = { &bundle_assets, }, // index_assets comes with it, in the right order
    .post_build_hooks = { &hook_insert_integrity_hash, },
};

/*
 * ─────────────────────────────────────────────────────────
 * DEVELOPER
 * ─────────────────────────────────────────────────────────
 */

/* Debug's shape with the developer flag set: optimized, no sanitizers, still hot reloading. */

NYA_INTERNAL NYA_BuildRule build_project_dev_executable_windows = {
    .name   = "build_project_dev_executable_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-o", WINDOWS_X86_64_DEV_BINARY,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEV_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_windows_x86_64, &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64,
                       &vendor_sdl_mixer_windows_x86_64, &vendor_libbacktrace_windows_x86_64, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_dll_windows = {
    .name   = "build_project_dev_dll_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            DLL_SOURCE_PATH,
            "-o", WINDOWS_X86_64_DEV_DLL,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            "-shared",
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            // Resolves the engine symbols out of the executable.
            WINDOWS_X86_64_DEV_IMPLIB,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { &vendor_sdl_windows_x86_64, &vendor_sdl_image_windows_x86_64, &vendor_sdl_ttf_windows_x86_64,
                       &vendor_sdl_mixer_windows_x86_64, &vendor_libbacktrace_windows_x86_64, },
    .dependencies    = { &build_project_dev_executable_windows, &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_windows = {
    .name         = "build_project_dev_windows",
    .is_metarule  = true,
    .dependencies = { &build_project_dev_executable_windows, &build_project_dev_dll_windows, },
};
