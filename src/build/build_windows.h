/**
 * @file build_windows.h
 *
 * Producing Windows binaries, from either host.
 *
 * Not under on_linux/ or on_windows/ because nothing in it is per host. There were two copies of
 * this file, one in each host directory, and they were byte identical below their docblocks — the
 * on_windows one even said so, claiming "only the tooling below differs" when there was no tooling
 * below it at all. Every difference between a native Windows build and a mingw-w64 cross compile is
 * already absorbed by the toolchain macros: CC, WINDRES, NYA_WINDOWS_CC and
 * FLAGS_TARGET_WINDOWS_X86_64, which expands to the target triple on Linux and to nothing on
 * Windows. So the two copies had nothing left to disagree about except by accident, which is the
 * hazard render2d_headless.c's docblock describes from the other side.
 *
 * on_linux/build_linux.h stays where it is: only a Linux host produces Linux binaries, so that one
 * really is per host. See host.h for why the reverse is not supported.
 * */
#pragma once

// For the asset rules these depend on: build_shaders, index_assets and bundle_assets.
#include "build/asset_rules.h"
#include "build/flags.h"
#include "build/hooks.h"
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
            FLAGS_PLUGINS,
            LINKER_FLAGS,
            FLAGS_DEBUG,
            FLAGS_DEBUG_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
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
            FLAGS_PLUGINS,
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
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
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
            FLAGS_PLUGINS,
            LINKER_FLAGS,
            FLAGS_RELEASE,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            "./assets/icon/icon.res",
        },
    },

    .pre_build_hooks  = { &hook_add_version_flag_and_git_hash, },
    .vendors          = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .dependencies     = { &bundle_assets, }, // index_assets comes with it, in the right order
    // Signing last: the CRC patch rewrites bytes the signature would otherwise cover.
    .post_build_hooks = { &hook_insert_integrity_hash, &hook_sign_windows_executable, },
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
            FLAGS_PLUGINS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEV_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
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
            FLAGS_PLUGINS,
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
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .dependencies    = { &build_project_dev_executable_windows, &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_windows = {
    .name         = "build_project_dev_windows",
    .is_metarule  = true,
    .dependencies = { &build_project_dev_executable_windows, &build_project_dev_dll_windows, },
};
