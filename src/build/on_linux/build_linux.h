/**
 * @file build_linux.h
 *
 * Producing Linux binaries on a Linux host. Native, so no target triple and no cross tooling.
 *
 * Every artifact is a compile rule to an object and a link rule that depends on it, so a compiler cache
 * can serve the compile. See the note on compile and link flags in flags.h.
 * */
#pragma once

// For the asset rules these depend on: build_shaders, index_assets and bundle_assets.
#include "build/pp/pp.h"
#include "build/flags.h"
#include "build/hooks.h"
#include "build/vendor/vendor.h"

#define LINUX_X86_64_DEBUG_OBJECT     OBJECT_DIRECTORY "/" LINUX_X86_64_DEBUG_BINARY OBJECT_SUFFIX
#define LINUX_X86_64_DEBUG_DLL_OBJECT OBJECT_DIRECTORY "/" LINUX_X86_64_DEBUG_DLL OBJECT_SUFFIX
#define LINUX_X86_64_DEV_OBJECT       OBJECT_DIRECTORY "/" LINUX_X86_64_DEV_BINARY OBJECT_SUFFIX
#define LINUX_X86_64_DEV_DLL_OBJECT   OBJECT_DIRECTORY "/" LINUX_X86_64_DEV_DLL OBJECT_SUFFIX
#define LINUX_X86_64_OBJECT           OBJECT_DIRECTORY "/" LINUX_X86_64_BINARY OBJECT_SUFFIX

NYA_INTERNAL NYA_BuildRule compile_project_debug_executable_linux = {
    .name        = "compile_project_debug_executable_linux",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_DEBUG_OBJECT,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            BINARY_SOURCE_PATH,
            "-c", "-o", LINUX_X86_64_DEBUG_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_SANITIZE,
            FLAGS_DEBUG,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    // The codegen every translation unit here reads. The launcher compiles src/main.c, which pulls in
    // nyangine.c and therefore src/genyarated/reflection.c and strings.h, so it depends on them as the dll
    // does. nya_build builds a shared dependency once per invocation, so naming it on both costs nothing.
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_executable_linux = {
    .name   = "build_project_debug_executable_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            LINUX_X86_64_DEBUG_OBJECT,
            "-o", LINUX_X86_64_DEBUG_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_SANITIZE,
            FLAGS_DEBUG,
            FLAGS_DEBUG_LINUX_X86_64,
            FLAGS_LINUX_X86_64,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &compile_project_debug_executable_linux, },
};

NYA_INTERNAL NYA_BuildRule compile_project_debug_dll_linux = {
    .name        = "compile_project_debug_dll_linux",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_DEBUG_DLL_OBJECT,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            DLL_SOURCE_PATH,
            "-c", "-o", LINUX_X86_64_DEBUG_DLL_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_SANITIZE,
            FLAGS_DEBUG,
            FLAGS_DLL_COMPILE,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_dll_linux = {
    .name   = "build_project_debug_dll_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            LINUX_X86_64_DEBUG_DLL_OBJECT,
            "-o", LINUX_X86_64_DEBUG_DLL,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_SANITIZE,
            FLAGS_DEBUG,
            FLAGS_DEBUG_LINUX_X86_64,
            FLAGS_DLL_COMPILE,
            FLAGS_DLL_LINK,
            FLAGS_LINUX_X86_64,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &compile_project_debug_dll_linux, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_linux = {
    .name   = "build_project_debug_linux",
    .is_metarule  = true,
    .dependencies    = { &build_project_debug_executable_linux, &build_project_debug_dll_linux, },
};

NYA_INTERNAL NYA_BuildRule compile_project_linux_x86_64 = {
    .name        = "compile_project_linux_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_OBJECT,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            BINARY_SOURCE_PATH,
            "-c", "-o", LINUX_X86_64_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_RELEASE,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &bundle_assets, }, // index_assets comes with it, in the right order
};

NYA_INTERNAL NYA_BuildRule build_project_linux_x86_64 = {
    .name        = "build_project_linux_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_BINARY,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            LINUX_X86_64_OBJECT,
            "-o", LINUX_X86_64_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_RELEASE,
            FLAGS_RELEASE_LINK,
            FLAGS_RELEASE_LINK_LINUX_X86_64,
            FLAGS_LINUX_X86_64,
        },
    },

    .vendors          = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags     = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies     = { &compile_project_linux_x86_64, },
    .post_build_hooks = { &hook_insert_integrity_hash, },
};

/*
 * ─────────────────────────────────────────────────────────
 * STEAM
 * ─────────────────────────────────────────────────────────
 */

/*
 * The release build with the Steamworks plugin, for the Steam Linux Runtime: compiled and linked against the sniper
 * sysroot, see vendor_steamrt.h. Its vendors are built by the rule that needs them rather than up front, since nothing
 * else wants the sysroot.
 */

#define STEAM_LINUX_X86_64_OBJECT OBJECT_DIRECTORY "/" STEAM_LINUX_X86_64_DIRECTORY OBJECT_SUFFIX
#define FLAGS_STEAMRT             "--sysroot=" STEAMRT_SYSROOT

// glibc 2.31 keeps dlopen in libdl, and gcc 10's libgcc has no long double to half float conversion, which
// compiler-rt's builtins do. Unwinding stays with libgcc_s, which the runtime carries.
#define FLAGS_STEAMRT_LINK "-ldl", "-rtlib=compiler-rt", "-unwindlib=libgcc"

NYA_INTERNAL NYA_BuildRule build_steamrt_vendors = {
    .name            = "build_steamrt_vendors",
    .is_metarule     = true,
    .pre_build_hooks = { &hook_build_steamrt_vendors, },
};

NYA_INTERNAL NYA_BuildRule compile_project_steam_linux_x86_64 = {
    .name        = "compile_project_steam_linux_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = STEAM_LINUX_X86_64_OBJECT,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            BINARY_SOURCE_PATH,
            "-c", "-o", STEAM_LINUX_X86_64_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_STEAM,
            FLAGS_STEAMRT,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_STEAMRT_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &build_steamrt_vendors, &bundle_assets, },
};

NYA_INTERNAL NYA_BuildRule link_project_steam_linux_x86_64 = {
    .name        = "link_project_steam_linux_x86_64",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = STEAM_LINUX_X86_64_BINARY,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            STEAM_LINUX_X86_64_OBJECT,
            "-o", STEAM_LINUX_X86_64_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_STEAM,
            FLAGS_STEAMRT,
            FLAGS_STEAMRT_LINK,
            FLAGS_RELEASE_LINK,
            FLAGS_RELEASE_LINK_LINUX_X86_64,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks  = { &hook_create_output_directory, },
    .vendors          = { NYA_PROJECT_VENDORS_STEAMRT_X86_64, &vendor_steam_linux_x86_64, },
    .vendor_flags     = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies     = { &compile_project_steam_linux_x86_64, },
    .post_build_hooks = { &hook_insert_integrity_hash, },
};

/** libsteam_api.so beside the executable, where its $ORIGIN rpath finds it. */
NYA_INTERNAL NYA_BuildRule copy_steam_library_linux_x86_64 = {
    .name        = "copy_steam_library_linux_x86_64",
    .policy      = NYA_BUILD_IF_OUTDATED,
    .is_metarule = true,
    .input_file  = STEAM_LIBRARY_LINUX_X86_64,
    .output_file = STEAM_LINUX_X86_64_LIBRARY,

    .pre_build_hooks  = { &hook_create_output_directory, },
    .post_build_hooks = { &hook_copy_file, },
};

NYA_INTERNAL NYA_BuildRule build_project_steam_linux_x86_64 = {
    .name         = "build_project_steam_linux_x86_64",
    .is_metarule  = true,
    .dependencies = { &link_project_steam_linux_x86_64, &copy_steam_library_linux_x86_64, },
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

NYA_INTERNAL NYA_BuildRule compile_project_dev_executable_linux = {
    .name        = "compile_project_dev_executable_linux",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_DEV_OBJECT,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            BINARY_SOURCE_PATH,
            "-c", "-o", LINUX_X86_64_DEV_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEVELOPER,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    // The same codegen as the debug executable, for the same reason.
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_executable_linux = {
    .name   = "build_project_dev_executable_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            LINUX_X86_64_DEV_OBJECT,
            "-o", LINUX_X86_64_DEV_BINARY,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEBUG_LINUX_X86_64,
            FLAGS_LINUX_X86_64,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &compile_project_dev_executable_linux, },
};

NYA_INTERNAL NYA_BuildRule compile_project_dev_dll_linux = {
    .name        = "compile_project_dev_dll_linux",
    .policy      = NYA_BUILD_ALWAYS,
    .output_file = LINUX_X86_64_DEV_DLL_OBJECT,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            DLL_SOURCE_PATH,
            "-c", "-o", LINUX_X86_64_DEV_DLL_OBJECT,
            CFLAGS,
            WARNINGS,
            INCLUDE_PATHS,
            FLAGS_PLUGINS,
            FLAGS_DEVELOPER,
            FLAGS_DLL_COMPILE,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag, &hook_add_build_info_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
    .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    .dependencies    = { &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_dll_linux = {
    .name   = "build_project_dev_dll_linux",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = CC,
        .arguments = { CC_LAUNCHED,
            LINUX_X86_64_DEV_DLL_OBJECT,
            "-o", LINUX_X86_64_DEV_DLL,
            CFLAGS,
            LINKER_FLAGS,
            FLAGS_DEVELOPER,
            FLAGS_DEBUG_LINUX_X86_64,
            FLAGS_DLL_COMPILE,
            FLAGS_DLL_LINK,
            FLAGS_LINUX_X86_64,
        },
    },

    .vendors      = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
    .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    .dependencies = { &compile_project_dev_dll_linux, },
};

NYA_INTERNAL NYA_BuildRule build_project_dev_linux = {
    .name         = "build_project_dev_linux",
    .is_metarule  = true,
    .dependencies = { &build_project_dev_executable_linux, &build_project_dev_dll_linux, },
};
