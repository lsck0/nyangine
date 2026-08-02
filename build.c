#include "nyangine/nyangine.h"
/**/
#include "nyangine/nyangine.c"
/**/
// Which host is doing the building decides the tool names every rule below uses.
#if OS_WINDOWS
#include "build/on_windows/toolchain.h"
#else
#include "build/on_linux/toolchain.h"
#endif
/**/
#include "build/flags.h"
#include "build/vendor/vendor.h"
/**/
#include "build/asset/asset.c"
#include "build/hooks/hooks.c"

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
        FLAGS_DEBUG_LINUX_X86_64,
        FLAGS_SANITIZE,
        FLAGS_LINUX_X86_64,
        FLAGS_BUILD_TOOL,
    },
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
        .program   = WINDRES,
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
    .vendors         = { &vendor_sdl_shadercross_linux_x86_64, },
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

// Which host is doing the building decides how each target is produced, so the rules live in a
// directory per host rather than behind conditionals inside one file.
#if OS_WINDOWS
#include "build/on_windows/build_linux.h"
#include "build/on_windows/build_windows.h"
#else
#include "build/on_linux/build_linux.h"
#include "build/on_linux/build_windows.h"
#endif

NYA_INTERNAL NYA_BuildRule build_project_release = {
    .name         = "build_project_release",
    .is_metarule  = true,
    .dependencies = { &build_project_linux_x86_64, &build_project_windows_x86_64, },
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

/*
 * Running always targets the host. Cross compiling to Windows from Linux is a build time
 * convenience; there is nothing sensible to do with the resulting .exe here, so `run` picks the
 * native artifact and the rules below are selected by host rather than exposed as a choice.
 */

#if OS_WINDOWS
#define HOST_DEBUG_BINARY   WINDOWS_X86_64_DEBUG_BINARY
#define HOST_DEV_BINARY     WINDOWS_X86_64_DEV_BINARY
#define HOST_RELEASE_BINARY WINDOWS_X86_64_BINARY
#define host_build_debug    build_project_debug_windows
#define host_build_dev      build_project_dev_windows
#define host_build_release  build_project_windows_x86_64
#else
#define HOST_DEBUG_BINARY   LINUX_X86_64_DEBUG_BINARY
#define HOST_DEV_BINARY     LINUX_X86_64_DEV_BINARY
#define HOST_RELEASE_BINARY LINUX_X86_64_BINARY
#define host_build_debug    build_project_debug_linux
#define host_build_dev      build_project_dev_linux
#define host_build_release  build_project_linux_x86_64
#endif

/** Sanitizer configuration shared by everything that runs an instrumented binary. */
#define SANITIZER_ENVIRONMENT                                                                                                                        \
    "ASAN_OPTIONS=suppressions=./.sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1",                                       \
        "LSAN_OPTIONS=suppressions=./.sanitizers/lsan.supp", "TSAN_OPTIONS=suppressions=./.sanitizers/tsan.supp",                                    \
        "UBSAN_OPTIONS=suppressions=./.sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1"

/*
 * Debug runs under perf. It is the slow, instrumented build, so the profile is not representative
 * of shipped performance, but it is the one where a bad frame can actually be traced to a line.
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
            "./" HOST_DEBUG_BINARY,
        },
        .environment = { SANITIZER_ENVIRONMENT, },
    },

    .dependencies     = { &host_build_debug, },
    .post_build_hooks = { &hook_convert_perf_data_to_plain, },
};

/** Developer: optimized and hot reloading, run directly. What you want while actually playing it. */
NYA_INTERNAL NYA_BuildRule run_dev = {
    .name   = "run_dev",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program = "./" HOST_DEV_BINARY,
    },

    .dependencies = { &host_build_dev, },
};

NYA_INTERNAL NYA_BuildRule run_release = {
    .name   = "run_release",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program = "./" HOST_RELEASE_BINARY,
    },

    .dependencies = { &host_build_release, },
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
        // --init --recursive, not just a pull: several vendors are themselves submodule trees.
        // SDL_mixer and SDL_ttf build their codecs from external/, and with those directories
        // empty cmake fails at configure time complaining about a missing CMakeLists.txt.
        .arguments = { "submodule", "update", "--init", "--recursive", },
    },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TEST RUNNER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds every vendored dependency, not just the ones the engine currently links against.
 *
 * NYA_VENDORS is already built before any command runs, so this exists to bring the rest of the
 * tree up in one go, which is what you want on a fresh checkout or after adding a dependency.
 * */
NYA_INTERNAL void vendor_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_EXPECT(nya_vendor_build_all(NYA_VENDORS_ALL), "while building all vendor dependencies");
}

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
    NYA_ArrayᐸNYA_Stringᐳ* tests = nya_string_split_lines(nya_arena_global, find_tests_command.stdout_content);

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
                FLAGS_DEBUG_LINUX_X86_64,
                FLAGS_LINUX_X86_64,
                // Only the test rule gets this. It compiles in nya_expect_crash, which lets a test
                // survive a deliberate assertion, panic or thrown error.
                "-DNYA_TESTING",
                "-Wl,-rpath,$ORIGIN/../../../vendor/steam/redistributable_bin/linux64",
            },
        },

        .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
        .vendors         = { &vendor_sdl_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
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
    .value.type  = NYA_TYPE_B8,
    .name        = "no-rebuild",
    .description = "Don't rebuild the build system before executing the command.",
};

NYA_INTERNAL NYA_ArgParameter help_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "help",
    .description = "Show this message.",
};

NYA_INTERNAL NYA_ArgCommand run = {
    .name = "run",
    .description = "Run things. Always the host's own build; cross compiled artifacts cannot run here.",
    .subcommands = {
        &(NYA_ArgCommand){
            .name        = "debug",
            .description = "Run the debug build under perf. Sanitized, hot reloading, slow.",
            .build_rule  = &run_debug,
        },
        &(NYA_ArgCommand){
            .name        = "dev",
            .description = "Run the developer build. Optimized, hot reloading, no sanitizers.",
            .build_rule  = &run_dev,
        },
        &(NYA_ArgCommand){
            .name        = "release",
            .description = "Run the release build.",
            .build_rule  = &run_release,
        },
        &(NYA_ArgCommand){
            .name        = "test",
            .description = "Build and run the tests.",
            .handler     = &test_runner,
            .parameters  = { &test_files, },
        },
    },
};

NYA_INTERNAL NYA_ArgCommand build = {
    .name        = "build",
    .description = "Build things.",
    .subcommands = {
        &(NYA_ArgCommand){
            .name        = "debug-linux",
            .description = "Build the linux debug executable and dll.",
            .build_rule  = &build_project_debug_linux,
        },
        &(NYA_ArgCommand){
            .name        = "debug-exe-linux",
            .description = "Build the linux debug executable.",
            .build_rule  = &build_project_debug_executable_linux,
        },
        &(NYA_ArgCommand){
            .name        = "debug-dll-linux",
            .description = "Build the linux debug dll.",
            .build_rule  = &build_project_debug_dll_linux,
        },
        &(NYA_ArgCommand){
            .name        = "debug-windows",
            .description = "Build the windows debug executable and dll.",
            .build_rule  = &build_project_debug_windows,
        },
        &(NYA_ArgCommand){
            .name        = "debug-exe-windows",
            .description = "Build the windows debug executable.",
            .build_rule  = &build_project_debug_executable_windows,
        },
        &(NYA_ArgCommand){
            .name        = "debug-dll-windows",
            .description = "Build the windows debug dll.",
            .build_rule  = &build_project_debug_dll_windows,
        },
        &(NYA_ArgCommand){
            .name        = "dev-linux",
            .description = "Build the linux developer executable and dll.",
            .build_rule  = &build_project_dev_linux,
        },
        &(NYA_ArgCommand){
            .name        = "dev-exe-linux",
            .description = "Build the linux developer executable.",
            .build_rule  = &build_project_dev_executable_linux,
        },
        &(NYA_ArgCommand){
            .name        = "dev-dll-linux",
            .description = "Build the linux developer dll.",
            .build_rule  = &build_project_dev_dll_linux,
        },
        &(NYA_ArgCommand){
            .name        = "dev-windows",
            .description = "Build the windows developer executable and dll.",
            .build_rule  = &build_project_dev_windows,
        },
        &(NYA_ArgCommand){
            .name        = "dev-exe-windows",
            .description = "Build the windows developer executable.",
            .build_rule  = &build_project_dev_executable_windows,
        },
        &(NYA_ArgCommand){
            .name        = "dev-dll-windows",
            .description = "Build the windows developer dll.",
            .build_rule  = &build_project_dev_dll_windows,
        },
        &(NYA_ArgCommand){
            .name        = "release-linux",
            .description = "Build the linux release executable.",
            .build_rule  = &build_project_linux_x86_64,
        },
        &(NYA_ArgCommand){
            .name        = "release-windows",
            .description = "Build the windows release executable.",
            .build_rule  = &build_project_windows_x86_64,
        },
        &(NYA_ArgCommand){
            .name        = "release",
            .description = "Build every release executable.",
            .build_rule  = &build_project_release,
        },
        &(NYA_ArgCommand){
            .name        = "shaders",
            .description = "Build the shaders.",
            .build_rule  = &build_shaders,
        },
        &(NYA_ArgCommand){
            .name        = "vendor",
            .description = "Build every vendored dependency, including ones nothing links against yet.",
            .handler     = &vendor_runner,
        },
    },
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

s32 main(s32 argc, NYA_CString argv[]) {
    // The build tool itself is compiled without libbacktrace, since it is what builds it. This
    // still wires up the fault handlers and the crash sink, just with no symbolization.
    nya_backtrace_init();

    parser.executable_name = argv[0];

    NYA_ArgCommand* command;
    NYA_Error       parse_result = nya_args_parse(&parser, argc, argv, &command);
    if (parse_result.kind != NYA_ERROR_NONE) {
        (void)fprintf(stderr, "Error: %s\n\n", parse_result.message);
        nya_args_print_usage(&parser, nullptr);
        return EXIT_FAILURE;
    }

    // Give the rebuilt tool symbolized stack traces, once there is a libbacktrace to give it.
    //
    // Chicken and egg: this tool is what *builds* libbacktrace, so on a fresh checkout the archive
    // does not exist yet and the flags below would fail the link. base_backtrace.h keys off
    // __has_include("backtrace.h") and degrades to a null backend, so the first build has no traces
    // and every rebuild after the vendors exist has them. That is why crashes in the build system
    // print "no stack trace available" exactly once per clean checkout.
    if (nya_filesystem_exists(BACKTRACE_A_LINUX_X86_64)) {
        u32 count = 0;
        while (count < NYA_COMMAND_MAX_ARGUMENTS && build_rebuild_command.arguments[count] != nullptr) count++;

        NYA_ConstCString extra[]   = { BACKTRACE_INCLUDES_LINUX_X86_64, BACKTRACE_A_LINUX_X86_64 };
        u32              extra_len = (u32)(sizeof(extra) / sizeof(extra[0]));

        nya_assert(count + extra_len < NYA_COMMAND_MAX_ARGUMENTS, "No room to add libbacktrace to the rebuild command.");
        for (u32 i = 0; i < extra_len; i++) build_rebuild_command.arguments[count + i] = extra[i];
    }

    if (!skip_self_rebuild_flag.value.as_b8) nya_rebuild_yourself(&argc, argv, build_rebuild_command);

    // Every vendor, before anything else runs. From here on no rule has to care whether what it
    // links against exists yet. Vendor parts are NYA_BUILD_ONCE, so this is nearly free once the
    // artifacts are on disk.
    nya_vendor_detect_nprocs();
    NYA_EXPECT(nya_vendor_build_all(NYA_VENDORS), "while building vendor dependencies");

    if (help_flag.value.as_b8) {
        nya_args_print_usage(&parser, command);
        return EXIT_SUCCESS;
    }

    NYA_Error run_result = nya_args_run_command(command);
    if (run_result.kind != NYA_ERROR_NONE) {
        (void)fprintf(stderr, "Error: %s\n\n", run_result.message);
        nya_args_print_usage(&parser, command);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
