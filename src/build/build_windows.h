/**
 * @file build_windows.h
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
    // The codegen every translation unit here reads. The dll rule listed these and the executable did
    // not, which is the same defect the test rules had: the launcher compiles src/main.c, which pulls in
    // nyangine.c and therefore src/generated/reflection.c and strings.h, so a new asset or an edited
    // `@reflect` reached the dll and not the executable beside it. nya_build builds a shared dependency
    // once per invocation, so naming it on both costs nothing.
    .dependencies    = { &build_shaders, &index_assets, },
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
    // The codegen every translation unit here reads. The dll rule listed these and the executable did
    // not, which is the same defect the test rules had: the launcher compiles src/main.c, which pulls in
    // nyangine.c and therefore src/generated/reflection.c and strings.h, so a new asset or an edited
    // `@reflect` reached the dll and not the executable beside it. nya_build builds a shared dependency
    // once per invocation, so naming it on both costs nothing.
    .dependencies    = { &build_shaders, &index_assets, },
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
