#include "nyangine/base/base_basic.h"
#include "nyangine/nyangine.h"
/**/
#include "nyangine/base/base_arena.c"
#include "nyangine/base/base_args.c"
#include "nyangine/base/base_build.c"
#include "nyangine/base/base_crc.c"
#include "nyangine/base/base_error.c"
#include "nyangine/base/base_file.c"
#include "nyangine/base/base_integrity.c"
#include "nyangine/base/base_lexer.c"
#include "nyangine/base/base_logging.c"
#include "nyangine/base/base_perf.c"
#include "nyangine/base/base_string.c"

#if OS_WINDOWS
#include "nyangine/platform/clock/clock_windows.c"
#include "nyangine/platform/command/command_windows.c"
#include "nyangine/platform/filesystem/filesystem_windows.c"
#elif OS_LINUX
#include "nyangine/platform/clock/clock_linux.c"
#include "nyangine/platform/command/command_linux.c"
#include "nyangine/platform/filesystem/filesystem_linux.c"
#else
#error "Unsupported OS"
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * COMMON FLAGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off
#define PROJECT_NAME "gnyame"
#define VERSION      "0.0.0"

#define BINARY_SOURCE_PATH "./src/main.c"
#define DLL_SOURCE_PATH    "./src/gnyame/gnyame.c"

#define LINUX_X86_64_DEBUG_BINARY   PROJECT_NAME ".debug"
#define LINUX_X86_64_DEBUG_DLL      PROJECT_NAME ".debug.so"
#define LINUX_X86_64_BINARY         PROJECT_NAME "." VERSION ".linux-x86_64"
#define WINDOWS_X86_64_DEBUG_BINARY PROJECT_NAME ".debug.exe"
#define WINDOWS_X86_64_DEBUG_DLL    PROJECT_NAME ".debug.dll"
#define WINDOWS_X86_64_BINARY       PROJECT_NAME "." VERSION ".windows-x86_64.exe"

#define CC            "clang"
#define CFLAGS        "-std=c23", "-ggdb", "-fenable-matrix", "-mavx", "-mavx2"
#define WARNINGS      "-Wall", "-Wextra", "-Wstrict-prototypes", "-Wswitch", "-Wswitch-default", "-Wimplicit-fallthrough", "-Wno-gnu", "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro"
#define INCLUDE_PATHS "-I./", "-I./src/", "-I./vendor/sdl/include/"
#define LINKER_FLAGS  "-lm", "-pthread"
#define NPROCS        "16"

#define FLAGS_DEBUG          "-DDEBUG=true", "-O0", "-fuse-ld=mold", "-rdynamic", "-DNYA_ASSET_BACKEND_FS"
#define FLAGS_DLL            "-fPIC", "-shared"
#define FLAGS_SANITIZE       "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls", "-fno-sanitize-recover=all", "-fsanitize=address,leak,undefined,signed-integer-overflow,unsigned-integer-overflow,shift,float-cast-overflow,float-divide-by-zero,pointer-overflow"
#define FLAGS_RELEASE        "-O3", "-flto", "-fPIE", "-fuse-ld=lld", "-DNYA_ASSET_BACKEND_BLOB", "-D_FORTIFY_SOURCE=2", "-fcf-protection=full", "-fstack-protector-strong", "-fno-omit-frame-pointer"
#define FLAGS_WINDOWS_X86_64 "--target=x86_64-w64-mingw32", "-Wl,-subsystem,windows", "-static", "-L./vendor/sdl/build-window-x86_64/", "-lSDL3", "-lcomdlg32", "-ldxguid", "-lgdi32", "-limm32", "-lkernel32", "-lole32", "-loleaut32", "-lsetupapi", "-luser32", "-luuid", "-lversion", "-lwinmm"
#define FLAGS_LINUX_X86_64   "-Wl,-rpath,$ORIGIN", "-L./vendor/sdl/build-linux-x86_64/", "-lSDL3"
// clang-format on

NYA_INTERNAL void hook_add_version_flag_and_git_hash(NYA_BuildRule* rule);
NYA_INTERNAL void hook_remove_output_file(NYA_BuildRule* rule);
NYA_INTERNAL void hook_bundle_project(NYA_BuildRule* rule);
NYA_INTERNAL void hook_convert_perf_data_to_plain(NYA_BuildRule* rule);
NYA_INTERNAL void hook_compile_shaders(NYA_BuildRule* rule);
NYA_INTERNAL void hook_index_assets(NYA_BuildRule* rule);
NYA_INTERNAL void hook_bundle_assets(NYA_BuildRule* rule);
NYA_INTERNAL void hook_insert_integrity_hash(NYA_BuildRule* rule);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BUILD RULES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_Command build_rebuild_command = {
    .program   = CC,
    .arguments = {
        "build.c",
        "-o", "build",
        CFLAGS,
        WARNINGS,
        INCLUDE_PATHS,
        LINKER_FLAGS,
        FLAGS_DEBUG,
        FLAGS_SANITIZE,
        FLAGS_LINUX_X86_64,
    },
};

/*
 * ─────────────────────────────────────────────────────────
 * VENDOR RULES
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_BuildRule build_prepare_linux_x86_64_sdl = {
    .name        = "build_prepare_linux_x86_64_sdl",
    .policy      = NYA_BUILD_ONCE,
    .output_file = "./vendor/sdl/build-linux-x86_64/libSDL3.a",

    .command = {
        .program   = "cmake",
        .arguments = {
            "-S", "./vendor/sdl",
            "-B", "./vendor/sdl/build-linux-x86_64/",
            "-GNinja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DSDL_SHARED=OFF",
            "-DSDL_STATIC=ON",
        },
    },
};

NYA_INTERNAL NYA_BuildRule build_linux_x64_64_sdl = {
    .name        = "build_linux_x64_64_sdl",
    .policy      = NYA_BUILD_ONCE,
    .output_file = "./vendor/sdl/build-linux-x86_64/libSDL3.a",

    .command = {
        .program   = "cmake",
        .arguments = {
            "--build", "./vendor/sdl/build-linux-x86_64/",
            "--config", "Release",
            "--", "-j", NPROCS,
        },
    },

    .dependencies = { &build_prepare_linux_x86_64_sdl, },
};

NYA_INTERNAL NYA_BuildRule build_prepare_windows_x86_64_sdl = {
    .name        = "build_prepare_windows_x86_64_sdl",
    .policy      = NYA_BUILD_ONCE,
    .output_file = "./vendor/sdl/build-window-x86_64/libSDL3.a",

    .command = {
        .program   = "cmake",
        .arguments = {
            "-S", "./vendor/sdl",
            "-B", "./vendor/sdl/build-window-x86_64/",
            "-GNinja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DSDL_SHARED=OFF",
            "-DSDL_STATIC=ON",
            "-DCMAKE_SYSTEM_NAME=Windows",
            "-DCMAKE_C_COMPILER=/usr/bin/x86_64-w64-mingw32-gcc",
            "-DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++",
            "-DCMAKE_LINKER=/usr/bin/x86_64-w64-mingw32-ld",
            "-DCMAKE_RC_COMPILER=/usr/bin/x86_64-w64-mingw32-windres",
            "-DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32",
            "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH",
            "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
            "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=BOTH",
        },
    },
};

NYA_INTERNAL NYA_BuildRule build_windows_x86_64_sdl = {
    .name        = "build_windows_x86_64_sdl",
    .policy      = NYA_BUILD_ONCE,
    .output_file = "./vendor/sdl/build-window-x86_64/libSDL3.a",

    .command = {
        .program   = "cmake",
        .arguments = {
            "--build", "./vendor/sdl/build-window-x86_64/",
            "--config", "Release",
            "--", "-j", NPROCS,
        },
    },

    .dependencies = { &build_prepare_windows_x86_64_sdl, },
};

NYA_INTERNAL NYA_BuildRule build_prepare_shadercross_linux_x86_64 = {
    .name        = "build_prepare_shadercross_linux_x86_64",
    .policy      = NYA_BUILD_ONCE,
    .output_file = "./vendor/sdl-shadercross/build/shadercross",

    .command = {
        .program   = "cmake",
        .arguments = {
            "-S", "./vendor/sdl-shadercross",
            "-B", "./vendor/sdl-shadercross/build",
            "-GNinja",
            "-DSDLSHADERCROSS_VENDORED=ON",
        },
    },
};

NYA_INTERNAL NYA_BuildRule build_shadercross_linux_x86_64 = {
    .name        = "build_shadercross_linux_x86_64",
    .policy      = NYA_BUILD_ONCE,
    .output_file = "./vendor/sdl-shadercross/build/shadercross",

    .command = {
        .program   = "cmake",
        .arguments = {
            "--build", "./vendor/sdl-shadercross/build",
            "--config", "Release",
            "--", "-j", NPROCS,
        },
    },

    .dependencies = { &build_prepare_shadercross_linux_x86_64, },
};

/*
 * ─────────────────────────────────────────────────────────
 * ASSET RULES
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_BuildRule build_windows_icon = {
    .name        = "build_windows_icon",
    .policy      = NYA_BUILD_IF_OUTDATED,
    .input_file  = "./assets/icon/icon.ico",
    .output_file = "./assets/icon/icon.res",

    .command = {
        .program   = "x86_64-w64-mingw32-windres",
        .arguments = {
            "./assets/icon/icon.rc",
            "-O", "coff",
            "-o", "./assets/icon/icon.res",
        },
    },
};

NYA_INTERNAL NYA_BuildRule build_shaders = {
    .name            = "build_shaders",
    .is_metarule     = true,
    .pre_build_hooks = { &hook_compile_shaders, },
    .dependencies    = { &build_shadercross_linux_x86_64, },
};

NYA_INTERNAL NYA_BuildRule index_assets = {
    .name             = "index_assets",
    .is_metarule      = true,
    .dependencies     = { &build_windows_icon, &build_shaders, },
    .post_build_hooks = { &hook_index_assets, },
};

NYA_INTERNAL NYA_BuildRule bundle_assets = {
    .name             = "bundle_assets",
    .is_metarule      = true,
    .dependencies     = { &build_windows_icon, &build_shaders, },
    .post_build_hooks = { &hook_bundle_assets, },
};

/*
 * ─────────────────────────────────────────────────────────
 * PROJECT BUILD RULES
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_BuildRule build_project_debug_executable = {
    .name   = "build_project_debug_executable",
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
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .dependencies    = { &build_linux_x64_64_sdl, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug_dll = {
    .name   = "build_project_debug_dll",
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
            FLAGS_DLL,
            FLAGS_LINUX_X86_64,
        },
    },

    .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
    .dependencies    = { &build_linux_x64_64_sdl, &build_shaders, &index_assets, },
};

NYA_INTERNAL NYA_BuildRule build_project_debug = {
    .name   = "build_project_debug",
    .is_metarule  = true,
    .dependencies    = { &build_project_debug_executable, &build_project_debug_dll, },
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
    .dependencies     = { &build_linux_x64_64_sdl, &bundle_assets, &index_assets, },
    .post_build_hooks = { &hook_insert_integrity_hash, },
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
            FLAGS_WINDOWS_X86_64,
            "./assets/icon/icon.res",
        },
    },

    .pre_build_hooks  = { &hook_add_version_flag_and_git_hash, },
    .dependencies     = { &build_windows_x86_64_sdl, &bundle_assets, &index_assets, },
    .post_build_hooks = { &hook_insert_integrity_hash, },
};

NYA_INTERNAL NYA_BuildRule build_project = {
    .name         = "build_project",
    .is_metarule  = true,
    .dependencies = { &build_project_linux_x86_64, &build_project_windows_x86_64, },
};

NYA_INTERNAL NYA_BuildRule bundle_project = {
    .name             = "bundle_project",
    .is_metarule      = true,
    .dependencies     = { &build_project, },
    .post_build_hooks = { &hook_bundle_project, },
};

NYA_INTERNAL NYA_BuildRule build_docs = {
    .name    = "build_docs",
    .policy  = NYA_BUILD_ALWAYS,

    .command = {
        .program   = "doxygen",
        .arguments = { "./docs/doxygen.config", },
    },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * OTHER TARGETS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_BuildRule run_debug = {
    .name   = "run_debug",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program = "perf",
        .arguments = {
            "record",
            "-T",
            "-F", "100",
            "-g", "--call-graph", "dwarf",
            "-e", "cycles,instructions,cache-misses",
            "./" LINUX_X86_64_DEBUG_BINARY,
        },
        .environment = {
            "ASAN_OPTIONS=suppressions=./.sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1",
            "LSAN_OPTIONS=suppressions=./.sanitizers/lsan.supp",
            "TSAN_OPTIONS=suppressions=./.sanitizers/tsan.supp",
            "UBSAN_OPTIONS=suppressions=./.sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1",
        },
    },

    .dependencies     = { &build_project_debug, },
    .post_build_hooks = { &hook_convert_perf_data_to_plain, },
};

NYA_INTERNAL NYA_BuildRule run_release = {
    .name   = "run_release",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program = "./" LINUX_X86_64_BINARY,
    },

    .dependencies = { &build_project_linux_x86_64, },
};

NYA_INTERNAL NYA_BuildRule open_perf_report = {
    .name        = "open_perf_report",
    .is_metarule = true,
    .dependencies = {
        &(NYA_BuildRule){
            .name   = "open_speedscope",
            .policy = NYA_BUILD_ALWAYS,

            .command = {
                .program   = "speedscope",
                .arguments = { "./perf.data.txt", },
            },
        },
        &(NYA_BuildRule){
            .name   = "open_hotspot",
            .policy = NYA_BUILD_ALWAYS,

            .command = {
                .program   = "hotspot",
                .arguments = { "./perf.data", },
            },
        },
    },
};

NYA_INTERNAL NYA_BuildRule open_docs = {
    .name   = "open_docs",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = "xdg-open",
        .arguments = { "./docs/doxygen/html/index.html", },
    },

    .dependencies = { &build_docs, },
};

NYA_INTERNAL NYA_BuildRule show_stats = {
    .name   = "show_stats",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = "tokei",
        .arguments = { ".", "--exclude", "vendor", },
    },
};

NYA_INTERNAL NYA_BuildRule update_submodules = {
    .name   = "update_submodules",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = "git",
        .arguments = { "submodule", "foreach", "git", "pull", "origin", "main", },
    },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HOOKS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void hook_add_version_flag_and_git_hash(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    static b8          initialized = false;
    static NYA_CString GIT_HASH_FLAG;
    static NYA_CString VERSION_FLAG;

    if (!initialized) {
        NYA_Command git_hash_command = {
            .arena     = nya_arena_global,
            .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
            .program   = "git",
            .arguments = { "rev-parse", "HEAD" },
        };
        NYA_EXPECT(nya_command_run(&git_hash_command));
        nya_assert(git_hash_command.exit_code == 0, "Failed to get git commit hash.");

        nya_string_trim_whitespace(git_hash_command.stdout_content);
        NYA_CString git_hash      = nya_string_to_cstring(nya_arena_global, git_hash_command.stdout_content);
        NYA_String* git_hash_flag = nya_string_sprintf(nya_arena_global, "-DGIT_COMMIT=\"%s\"", git_hash);
        NYA_String* version_flag  = nya_string_sprintf(nya_arena_global, "-DVERSION=\"%s\"", VERSION);
        GIT_HASH_FLAG             = nya_string_to_cstring(nya_arena_global, git_hash_flag);
        VERSION_FLAG              = nya_string_to_cstring(nya_arena_global, version_flag);
        initialized               = true;
    }

    u64 length = 0;
    while (length < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[length] != nullptr) length++;
    nya_assert(length < NYA_COMMAND_MAX_ARGUMENTS - 2, "Not enough space to add version flags.");
    rule->command.arguments[length + 0] = GIT_HASH_FLAG;
    rule->command.arguments[length + 1] = VERSION_FLAG;
}

NYA_INTERNAL void hook_remove_output_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file);

    NYA_EXPECT(nya_filesystem_delete(rule->output_file));
}

// we just need to move everything into the right place
NYA_INTERNAL void hook_bundle_project(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_ConstCString dist_path    = "./dist/";
    NYA_ConstCString linux_path   = "./dist/" PROJECT_NAME "." VERSION ".linux-x86_64/";
    NYA_ConstCString windows_path = "./dist/" PROJECT_NAME "." VERSION ".windows-x86_64/";

    NYA_Command clean_dist_command = {
        .program   = "rm",
        .arguments = { "-rf", dist_path },
    };
    NYA_EXPECT(nya_command_run(&clean_dist_command));
    nya_assert(clean_dist_command.exit_code == 0, "Failed to clean dist directory.");

    NYA_Command create_dirs_command = {
    .program   = "mkdir",
    .arguments = { "-p", linux_path, windows_path, },
  };
    NYA_EXPECT(nya_command_run(&create_dirs_command));
    nya_assert(create_dirs_command.exit_code == 0, "Failed to create dist directories.");

    // LINUX

    NYA_Command copy_linux_binary_command = {
    .program   = "cp",
    .arguments = { LINUX_X86_64_BINARY, linux_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_linux_binary_command));
    nya_assert(copy_linux_binary_command.exit_code == 0, "Failed to copy linux binary.");

    NYA_Command copy_steam_sdk_linux_command = {
    .program   = "cp",
    .arguments = { "./vendor/steam/redistributable_bin/linux64/libsteam_api.so", linux_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_steam_sdk_linux_command));
    nya_assert(copy_steam_sdk_linux_command.exit_code == 0, "Failed to copy steam sdk for linux.");

    NYA_Command zip_linux_command = {
        .program           = "zip",
        .arguments         = { "-r", "../" PROJECT_NAME "." VERSION ".linux-x86_64.zip", "." },
        .working_directory = linux_path,
    };
    NYA_EXPECT(nya_command_run(&zip_linux_command));
    nya_assert(zip_linux_command.exit_code == 0, "Failed to create linux zip.");

    // WINDOWS

    NYA_Command copy_windows_binary_command = {
    .program   = "cp",
    .arguments = { WINDOWS_X86_64_BINARY, windows_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_windows_binary_command));
    nya_assert(copy_windows_binary_command.exit_code == 0, "Failed to copy windows binary.");

    NYA_Command copy_steam_sdk_windows_command = {
    .program   = "cp",
    .arguments = { "./vendor/steam/redistributable_bin/win64/steam_api64.dll", windows_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_steam_sdk_windows_command));
    nya_assert(copy_steam_sdk_windows_command.exit_code == 0, "Failed to copy steam sdk for windows.");

    NYA_Command zip_windows_command = {
        .program           = "zip",
        .arguments         = { "-r", "../" PROJECT_NAME "." VERSION ".windows-x86_64.zip", "." },
        .working_directory = windows_path,
    };
    NYA_EXPECT(nya_command_run(&zip_windows_command));
    nya_assert(zip_windows_command.exit_code == 0, "Failed to create windows zip.");
}

NYA_INTERNAL void hook_convert_perf_data_to_plain(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_Command convert_command = {
        .arena     = nya_arena_global,
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .program   = "perf",
        .arguments = { "script", "-i", "./perf.data" },
    };
    NYA_EXPECT(nya_command_run(&convert_command));
    nya_assert(convert_command.exit_code == 0, "Failed to convert perf data to plain text.");

    NYA_EXPECT(nya_file_write("./perf.data.txt", convert_command.stdout_content));
}

NYA_INTERNAL void hook_compile_shaders(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_Command find_source_shaders_command = {
    .arena     = nya_arena_global,
    .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
    .program   = "find",
    .arguments = { "./assets/shader/source/", "-name", "*.hlsl", },
  };
    NYA_EXPECT(nya_command_run(&find_source_shaders_command));
    nya_assert(find_source_shaders_command.exit_code == 0, "Failed to find source shaders.");
    NYA_StringArray* shaders = nya_string_split_lines(nya_arena_global, find_source_shaders_command.stdout_content);

    nya_array_foreach (shaders, shader) {
        if (nya_string_is_empty(shader)) continue;

        NYA_CString source = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_prefix(shader, "./assets/shader/source/");
        nya_string_strip_suffix(shader, ".hlsl");
        nya_string_extend_front(shader, "./assets/shader/compiled/");

        nya_string_extend(shader, ".dxil");
        NYA_CString target_dxil = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_suffix(shader, ".dxil");

        nya_string_extend(shader, ".msl");
        NYA_CString target_metal = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_suffix(shader, ".msl");

        nya_string_extend(shader, ".spv");
        NYA_CString target_spirv = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_suffix(shader, ".spv");

        NYA_Command create_dirs_command = {
        .program   = "mkdir",
        .arguments = {
            "-p",
            "./assets/shader/compiled/",
        },
    };
        NYA_EXPECT(nya_command_run(&create_dirs_command));
        nya_assert(create_dirs_command.exit_code == 0, "Failed to create shader output directories.");

        // compile to DXIL
        NYA_String* compile_to_dxil_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_dxil);
        NYA_BuildRule compile_to_dxil_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_dxil_name),
        .policy      = NYA_BUILD_IF_OUTDATED,
        .input_file  = source,
        .output_file = target_dxil,
        .command = {
            .program = "./vendor/sdl-shadercross/build/shadercross",
            .arguments = {
                source,
                "-o", target_dxil,
                "-s", "hlsl",
                "-d", "dxil",
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_dxil_rule));

        // compile to Metal
        NYA_String* compile_to_metal_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_metal);
        NYA_BuildRule compile_to_metal_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_metal_name),
        .policy      = NYA_BUILD_IF_OUTDATED,
        .input_file  = source,
        .output_file = target_metal,
        .command = {
            .program = "./vendor/sdl-shadercross/build/shadercross",
            .arguments = {
                source,
                "-o", target_metal,
                "-s", "hlsl",
                "-d", "msl",
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_metal_rule));

        // compile to SPIR-V
        NYA_String* compile_to_spirv_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_spirv);
        NYA_BuildRule compile_to_spirv_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_spirv_name),
        .policy      = NYA_BUILD_IF_OUTDATED,
        .input_file  = source,
        .output_file = target_spirv,
        .command = {
            .program = "./vendor/sdl-shadercross/build/shadercross",
            .arguments = {
                source,
                "-o", target_spirv,
                "-s", "hlsl",
                "-d", "spirv",
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_spirv_rule));
    }
}

NYA_INTERNAL void hook_index_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_ConstCString asset_directory = "./assets/";
    NYA_ConstCString output_file     = "./assets/assets.h";

    NYA_Arena*  arena  = nya_arena_global;
    NYA_String* result = nya_string_create(arena);

    NYA_Command find_assets_command = {
      .arena     = arena,
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .program   = "find",
      .arguments = {
          asset_directory,
          "-type", "f",
          "-not", "-name", "*.c",
          "-not", "-name", "*.h",
          "-not", "-name", ".keep",
      },
  };
    NYA_EXPECT(nya_command_run(&find_assets_command));
    NYA_StringArray* files = nya_string_split_lines(arena, find_assets_command.stdout_content);
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#pragma once\n\n");

    nya_array_foreach (files, file) {
        if (nya_string_is_empty(file)) continue;

        // ignore compiled shaders, since they are then picked by the asset system depending on the platform
        if (nya_string_contains(file, "/shader/compiled/")) continue;

        NYA_String* var_name = nya_string_clone(arena, file);

        // strip unnecessary infomations
        nya_string_replace(var_name, "shader/source", "shader");
        nya_string_strip_suffix(var_name, ".hlsl");

        // cleanup and convert to a valid C identifier
        nya_string_strip_prefix(var_name, "./");
        nya_string_replace(var_name, "/", "_");
        nya_string_replace(var_name, ".", "_");
        nya_string_replace(var_name, "-", "_");
        nya_string_replace(var_name, " ", "_");
        nya_string_to_upper(var_name);

        nya_string_extend_sprintf(
            result,
            "#define NYA_" NYA_FMT_STRING " \"" NYA_FMT_STRING "\"\n",
            NYA_FMT_STRING_ARG(var_name),
            NYA_FMT_STRING_ARG(file)
        );
    }

    NYA_EXPECT(nya_file_write(output_file, result));

    NYA_Command format_command = {
    .program   = "clang-format",
    .arguments = { "-i", output_file, },
  };
    NYA_EXPECT(nya_command_run(&format_command));
}

NYA_INTERNAL void hook_bundle_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_ConstCString asset_directory = "./assets/";
    NYA_ConstCString output_file     = "./assets/assets.c";

    NYA_Arena*  arena               = nya_arena_global;
    NYA_String* result              = nya_string_create(arena);
    NYA_String* header_count_string = nya_string_create(arena);
    NYA_String* header_string       = nya_string_create(arena);
    NYA_String* blob_string         = nya_string_create(arena);

    NYA_Command find_assets_command = {
      .arena     = arena,
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .program   = "find",
      .arguments = {
          asset_directory,
          "-type", "f",
          "-not", "-name", "*.c",
          "-not", "-name", "*.h",
          "-not", "-name", ".keep",
      },
  };
    NYA_EXPECT(nya_command_run(&find_assets_command));
    NYA_StringArray* files = nya_string_split_lines(arena, find_assets_command.stdout_content);
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#include \"nyangine/nyangine.h\"\n\n");
    header_count_string = nya_string_sprintf(arena, "static const u64 NYA_ASSET_BLOB_HEADER_COUNT = " FMTu64 ";\n", files->length);
    nya_string_extend(header_string, "static const NYA_AssetBlobHeader NYA_ASSET_BLOB_HEADER[] = {\n");
    nya_string_extend(blob_string, "static const u8 NYA_ASSET_BLOB[] = {\n");

    u64 cursor = 0;
    nya_array_foreach (files, file) {
        if (nya_string_is_empty(file)) continue;

        NYA_String* content = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(file, content));

        nya_string_extend_sprintf(header_string, "  { \"%.*s\", " FMTu64 ", " FMTu64 " },\n", NYA_FMT_STRING_ARG(file), cursor, content->length);

        nya_array_foreach (content, c) {
            NYA_String* new = nya_string_sprintf(arena, "0x%02X,\n", *c);
            nya_string_extend(blob_string, new);
        }

        cursor += content->length;
    }
    nya_string_extend(blob_string, "};\n\n");
    nya_string_extend(header_string, "};\n\n");

    nya_string_extend(result, header_count_string);
    nya_string_extend(result, header_string);
    nya_string_extend(result, blob_string);

    NYA_EXPECT(nya_file_write(output_file, result));

    NYA_Command format_command = {
    .program   = "clang-format",
    .arguments = { "-i", output_file, },
  };
    NYA_EXPECT(nya_command_run(&format_command));
}

NYA_INTERNAL void hook_insert_integrity_hash(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file != nullptr, "Output file must be specified to insert integrity hash.");

    u64 integrity_hash;
    b8  ok = nya_integrity_patch(rule->output_file, &integrity_hash);
    nya_assert(ok, "Failed to insert integrity hash into binary.");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TEST RUNNER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void test_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* test_files = command->parameters[0];
    nya_assert(test_files != nullptr);
    nya_assert(nya_string_equals(test_files->name, "tests"));

    NYA_Command find_tests_command = {
    .arena     = nya_arena_global,
    .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
    .program   = "find",
    .arguments = { "./tests/", "-name", "*.c", },
  };
    NYA_EXPECT(nya_command_run(&find_tests_command));
    NYA_StringArray* tests = nya_string_split_lines(nya_arena_global, find_tests_command.stdout_content);

    nya_array_foreach (tests, original_test) {
        NYA_String* test = nya_string_clone(nya_arena_global, original_test);
        if (nya_string_is_empty(test)) continue;
        NYA_CString test_cstr = nya_string_to_cstring(nya_arena_global, test);

        // check if we should run this test, by checking if its name contains any of the requested test names
        b8 should_run = test_files->values_count == 0 ? true : false;
        for (u32 param_index = 0; param_index < test_files->values_count; param_index++) {
            NYA_CString requested_test = test_files->values[param_index].as_string;
            if (nya_string_contains(test, requested_test)) {
                should_run = true;
                break;
            }
        }
        if (!should_run) continue;

        nya_string_strip_suffix(test, ".c");
        NYA_CString test_binary = nya_string_to_cstring(nya_arena_global, test);

        NYA_String* build_test_name = nya_string_sprintf(nya_arena_global, "build_test:%s", test_binary);
        NYA_BuildRule build_test_rule = {
        .name        = nya_string_to_cstring(nya_arena_global, build_test_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = test_binary,

        .command = {
            .program   = CC,
            .arguments = {
                test_cstr,
                "-o", test_binary,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                LINKER_FLAGS,
                FLAGS_SANITIZE,
                FLAGS_DEBUG,
                FLAGS_LINUX_X86_64,
                "-Wl,-rpath,$ORIGIN/../../../vendor/steam/redistributable_bin/linux64",
            },
        },

        .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
        .dependencies    = { &build_linux_x64_64_sdl, },
    };

        NYA_String* run_test_name = nya_string_sprintf(nya_arena_global, "run_test:%s", test_binary);
        NYA_BuildRule run_test_rule = {
        .name        = nya_string_to_cstring(nya_arena_global, run_test_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = test_binary,

        .command = {
            .program     = test_binary,
            .environment = {
              "ASAN_OPTIONS=suppressions=./.sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1",
              "LSAN_OPTIONS=suppressions=./.sanitizers/lsan.supp",
              "TSAN_OPTIONS=suppressions=./.sanitizers/tsan.supp",
              "UBSAN_OPTIONS=suppressions=./.sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1",
            },
        },

        .dependencies      = { &build_test_rule, },
        .post_build_hooks  = { &hook_remove_output_file, },
    };

        NYA_EXPECT(nya_build(&run_test_rule));
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CLI PARSER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArgParameter test_files = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .variadic    = true,
    .value.type  = NYA_TYPE_STRING,
    .name        = "tests",
    .description = "Which tests to run. If none specified, all tests are run.",
};

NYA_INTERNAL NYA_ArgParameter skip_self_rebuild_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_BOOL,
    .name        = "no-rebuild",
    .description = "Don't rebuild the build system before executing the command.",
};

NYA_INTERNAL NYA_ArgParameter help_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_BOOL,
    .name        = "help",
    .description = "Show this message.",
};

NYA_INTERNAL NYA_ArgCommand run = {
    .name = "run",
    .description = "Run things.",
    .subcommands = {
        &(NYA_ArgCommand){
            .name        = "debug",
            .description = "Run the debug executable.",
            .build_rule  = &run_debug,
        },
        &(NYA_ArgCommand){
            .name        = "release",
            .description = "Run the release executable.",
            .build_rule  = &run_release,
        },
        &(NYA_ArgCommand){
            .name        = "test",
            .description = "Run all tests.",
            .handler     = &test_runner,
            .parameters = { &test_files, },
        },
}};

NYA_INTERNAL NYA_ArgCommand build = {
    .name        = "build",
    .description = "Build things.",
    .subcommands = {
        &(NYA_ArgCommand){
            .name        = "debug",
            .description = "Build the debug executable and dll.",
            .build_rule  = &build_project_debug,
        },
        &(NYA_ArgCommand){
            .name        = "debug-exe",
            .description = "Build the debug executable.",
            .build_rule  = &build_project_debug_executable,
        },
        &(NYA_ArgCommand){
            .name        = "debug-dll",
            .description = "Build the debug dll.",
            .build_rule  = &build_project_debug_dll,
        },
        &(NYA_ArgCommand){
            .name        = "release",
            .description = "Build all release executables.",
            .build_rule  = &build_project,
        },
        &(NYA_ArgCommand){
            .name        = "linux",
            .description = "Build the linux release executable.",
            .build_rule  = &build_project_linux_x86_64,
        },
        &(NYA_ArgCommand){
            .name        = "windows",
            .description = "Build the windows release executable.",
            .build_rule  = &build_project_windows_x86_64,
        },
        &(NYA_ArgCommand){
            .name        = "shaders",
            .description = "Build the shaders.",
            .build_rule  = &build_shaders,
        },
    },
};

NYA_INTERNAL NYA_ArgCommand bundle = {
    .name        = "bundle",
    .description = "Bundle for shipping.",
    .build_rule  = &bundle_project,
};

NYA_INTERNAL NYA_ArgCommand perf = {
    .name        = "perf",
    .description = "Open Profiler with last profiling data.",
    .build_rule  = &open_perf_report,
};

NYA_INTERNAL NYA_ArgCommand docs = {
    .name        = "docs",
    .description = "Open doxygen generated documentation.",
    .build_rule  = &open_docs,
};

NYA_INTERNAL NYA_ArgCommand stats = {
    .name        = "stats",
    .description = "Show code statistics.",
    .build_rule  = &show_stats,
};

NYA_INTERNAL NYA_ArgCommand update = {
    .name        = "update",
    .description = "Update git submodules.",
    .build_rule  = &update_submodules,
};

NYA_INTERNAL NYA_ArgParser parser = {
    .name    = "nyangine build system",
    .version = VERSION,

    .root_command = &(NYA_ArgCommand){
        .is_root    = true,
        .parameters = {
            &skip_self_rebuild_flag,
            &help_flag,
        },
        .subcommands = {
            &run,
            &build,
            &bundle,
            &perf,
            &docs,
            &stats,
            &update,
        },
    },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    parser.executable_name = argv[0];

    NYA_ArgCommand* command;
    NYA_Result      parse_result = nya_args_parse(&parser, argc, argv, &command);
    if (parse_result.error != NYA_ERROR_NONE) {
        (void)fprintf(stderr, "Error: %s\n\n", parse_result.message);
        nya_args_print_usage(&parser, nullptr);
        return EXIT_FAILURE;
    }

    if (!skip_self_rebuild_flag.value.as_bool) nya_rebuild_yourself(&argc, argv, build_rebuild_command);

    if (help_flag.value.as_bool) {
        nya_args_print_usage(&parser, command);
        return EXIT_SUCCESS;
    }

    NYA_Result run_result = nya_args_run_command(command);
    if (run_result.error != NYA_ERROR_NONE) {
        (void)fprintf(stderr, "Error: %s\n\n", run_result.message);
        nya_args_print_usage(&parser, command);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
