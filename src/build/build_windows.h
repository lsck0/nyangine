/**
 * @file build_windows.h
 * */
#pragma once

// For the asset rules these depend on: build_shaders, index_assets and bundle_assets.
#include "build/pp/pp.h"
#include "build/flags.h"
#include "build/hooks.h"
#include "build/vendor/vendor.h"

/*
 * The Windows debug pair. Unlike ELF, a PE DLL cannot reach back into the executable that loaded
 * it, so the executable exports its symbols into an import library and the game DLL links against
 * that. Everything else mirrors the Linux pair, compile and link split included.
 */

#define WINDOWS_X86_64_DEBUG_OBJECT     OBJECT_DIRECTORY "/" WINDOWS_X86_64_DEBUG_BINARY OBJECT_SUFFIX

/** The terminal backend cross compiled for windows; see compile_terminal_windows. */
#define WINDOWS_X86_64_TERMINAL_OBJECT  OBJECT_DIRECTORY "/nya_terminal_windows" OBJECT_SUFFIX
#define WINDOWS_X86_64_DEBUG_DLL_OBJECT OBJECT_DIRECTORY "/" WINDOWS_X86_64_DEBUG_DLL OBJECT_SUFFIX
#define WINDOWS_X86_64_DEV_OBJECT       OBJECT_DIRECTORY "/" WINDOWS_X86_64_DEV_BINARY OBJECT_SUFFIX
#define WINDOWS_X86_64_DEV_DLL_OBJECT   OBJECT_DIRECTORY "/" WINDOWS_X86_64_DEV_DLL OBJECT_SUFFIX
#define WINDOWS_X86_64_OBJECT           OBJECT_DIRECTORY "/" WINDOWS_X86_64_BINARY OBJECT_SUFFIX
#define WINDOWS_X86_64_RESOURCES        OBJECT_DIRECTORY "/" WINDOWS_X86_64_BINARY ".res"

NYA_INTERNAL NYA_BuildRule compile_project_debug_executable_windows = {
    .name        = "compile_project_debug_executable_windows",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_DEBUG_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-c", "-o", WINDOWS_X86_64_DEBUG_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEBUG,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    // The codegen every translation unit here reads. The launcher compiles src/main.c, which pulls in
    // nyangine.c and therefore src/generated/reflection.c and strings.h, so it depends on them as the dll
    // does. nya_build builds a shared dependency once per invocation, so naming it on both costs nothing.
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_executable_windows = {
    .name   = "build_project_debug_executable_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            WINDOWS_X86_64_DEBUG_OBJECT,
            "-o", WINDOWS_X86_64_DEBUG_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_DEBUG,
            FLAGS_DEBUG_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &compile_project_debug_executable_windows, },
};

/**
 * The terminal backend, cross compiled for windows.
 *
 * `platform/terminal/terminal_windows.c` had never been compiled by anything. Every build target picks
 * the GPU renderer, and the one translation unit that compiles the terminal backend — the dashboard
 * example, in `./build check` — is built for the host, so on a Linux machine it reaches
 * `terminal_linux.c` and the windows half sat there unread since the day it was written.
 *
 * Compiled and not linked: linking wants the windows vendor archives and an SDL that a TUI does not
 * use, and what is in question is whether this code still compiles against the Win32 console API, not
 * whether a windows binary comes out. It still has never *run*; that needs a windows machine.
 * */
NYA_INTERNAL NYA_BuildRule compile_terminal_windows = {
    .name        = "compile_terminal_windows",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_TERMINAL_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            TERMINAL_SOURCE_PATH,
            "-c", "-o", WINDOWS_X86_64_TERMINAL_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEBUG,
            FLAGS_TERMINAL,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule compile_project_debug_dll_windows = {
    .name        = "compile_project_debug_dll_windows",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_DEBUG_DLL_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            DLL_SOURCE_PATH,
            "-c", "-o", WINDOWS_X86_64_DEBUG_DLL_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEBUG,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_dll_windows = {
    .name   = "build_project_debug_dll_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            WINDOWS_X86_64_DEBUG_DLL_OBJECT,
            "-o", WINDOWS_X86_64_DEBUG_DLL,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_DEBUG,
            FLAGS_DLL_LINK,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            // Resolves the engine symbols out of the executable.
            WINDOWS_X86_64_DEBUG_IMPLIB,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &build_project_debug_executable_windows, &compile_project_debug_dll_windows, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_windows = {
    .name         = "build_project_debug_windows",
    .is_metarule  = true,
    .dependencies = { &build_project_debug_executable_windows, &build_project_debug_dll_windows, },
};

/*
 * The icon and version resource the release executable links. Always rebuilt, since it depends on
 * VERSION as well as the icon, and windres is cheap next to the link it feeds.
 */
NYA_INTERNAL NYA_BuildRule build_windows_resources = {
    .name        = "build_windows_resources",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_RESOURCES,

    .command = {
        .program   = WINDRES,
        .arguments = {
            "./assets/icon/icon.rc",
            "-O", "coff",
            "-o", WINDOWS_X86_64_RESOURCES,
            "-DNYA_RC_NAME=" PROJECT_NAME,
            "-DNYA_RC_VERSION=" VERSION,
        },
    },

    .pre_build_hooks = { &hook_create_output_directory, &hook_add_version_resource_flags, },
};

NYA_INTERNAL NYA_BuildRule compile_project_windows_x86_64 = {
    .name        = "compile_project_windows_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-c", "-o", WINDOWS_X86_64_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_RELEASE,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &bundle_assets, }, // index_assets comes with it, in the right order
};

NYA_INTERNAL NYA_BuildRule build_project_windows_x86_64 = {
    .name        = "build_project_windows_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_BINARY,

    .command = {
        .program   = CC,
        .arguments = {
            WINDOWS_X86_64_OBJECT,
            "-o", WINDOWS_X86_64_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_RELEASE,
            FLAGS_RELEASE_LINK,
            FLAGS_RELEASE_LINK_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            WINDOWS_X86_64_RESOURCES,
        },
    },

    .vendors          = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags     = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies     = { &compile_project_windows_x86_64, &build_windows_resources, },
    // Signing last: the CRC patch rewrites bytes the signature would otherwise cover.
    .post_build_hooks = { &hook_insert_integrity_hash, &hook_sign_windows_executable, },
};

/*
 * ─────────────────────────────────────────────────────────
 * STEAM
 * ─────────────────────────────────────────────────────────
 */

/* The release build with the Steamworks plugin, and steam_api64.dll beside it where the loader looks first. */

#define STEAM_WINDOWS_X86_64_OBJECT OBJECT_DIRECTORY "/" STEAM_WINDOWS_X86_64_DIRECTORY OBJECT_SUFFIX

NYA_INTERNAL NYA_BuildRule compile_project_steam_windows_x86_64 = {
    .name        = "compile_project_steam_windows_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = STEAM_WINDOWS_X86_64_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-c", "-o", STEAM_WINDOWS_X86_64_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_STEAM,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &bundle_assets, },
};

NYA_INTERNAL NYA_BuildRule link_project_steam_windows_x86_64 = {
    .name        = "link_project_steam_windows_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = STEAM_WINDOWS_X86_64_BINARY,

    .command = {
        .program   = CC,
        .arguments = {
            STEAM_WINDOWS_X86_64_OBJECT,
            "-o", STEAM_WINDOWS_X86_64_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_STEAM,
            FLAGS_RELEASE_LINK,
            FLAGS_RELEASE_LINK_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            WINDOWS_X86_64_RESOURCES,
        },
    },

    .pre_build_hooks  = { &hook_create_output_directory, },
    .vendors          = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, &vendor_steam_windows_x86_64, },
    .vendor_flags     = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies     = { &compile_project_steam_windows_x86_64, &build_windows_resources, },
    .post_build_hooks = { &hook_insert_integrity_hash, &hook_sign_windows_executable, },
};

NYA_INTERNAL NYA_BuildRule copy_steam_library_windows_x86_64 = {
    .name        = "copy_steam_library_windows_x86_64",
    .policy      = NYA_BUILD_IF_OUTDATED,
    .is_metarule = true,
    .input_file  = STEAM_LIBRARY_WINDOWS_X86_64,
    .output_file = STEAM_WINDOWS_X86_64_LIBRARY,

    .pre_build_hooks  = { &hook_create_output_directory, },
    .post_build_hooks = { &hook_copy_file, },
};

NYA_INTERNAL NYA_BuildRule build_project_steam_windows_x86_64 = {
    .name         = "build_project_steam_windows_x86_64",
    .is_metarule  = true,
    .dependencies = { &link_project_steam_windows_x86_64, &copy_steam_library_windows_x86_64, },
};

/*
 * ─────────────────────────────────────────────────────────
 * DEVELOPER
 * ─────────────────────────────────────────────────────────
 */

/* Debug's shape with the developer flag set: optimized, no sanitizers, still hot reloading. */

NYA_INTERNAL NYA_BuildRule compile_project_dev_executable_windows = {
    .name        = "compile_project_dev_executable_windows",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_DEV_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            BINARY_SOURCE_PATH,
            "-c", "-o", WINDOWS_X86_64_DEV_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEVELOPER,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    // The same codegen as the debug executable, for the same reason.
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_executable_windows = {
    .name   = "build_project_dev_executable_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            WINDOWS_X86_64_DEV_OBJECT,
            "-o", WINDOWS_X86_64_DEV_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEV_WINDOWS_X86_64,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &compile_project_dev_executable_windows, },
};

NYA_INTERNAL NYA_BuildRule compile_project_dev_dll_windows = {
    .name        = "compile_project_dev_dll_windows",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = WINDOWS_X86_64_DEV_DLL_OBJECT,

    .command = {
        .program   = CC,
        .arguments = {
            DLL_SOURCE_PATH,
            "-c", "-o", WINDOWS_X86_64_DEV_DLL_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEVELOPER,
            FLAGS_TARGET_WINDOWS_X86_64
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_dll_windows = {
    .name   = "build_project_dev_dll_windows",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = {
            WINDOWS_X86_64_DEV_DLL_OBJECT,
            "-o", WINDOWS_X86_64_DEV_DLL,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DLL_LINK,
            FLAGS_TARGET_WINDOWS_X86_64
            FLAGS_WINDOWS_X86_64,
            // Resolves the engine symbols out of the executable.
            WINDOWS_X86_64_DEV_IMPLIB,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &build_project_dev_executable_windows, &compile_project_dev_dll_windows, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_windows = {
    .name         = "build_project_dev_windows",
    .is_metarule  = true,
    .dependencies = { &build_project_dev_executable_windows, &build_project_dev_dll_windows, },
};
