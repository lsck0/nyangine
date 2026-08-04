#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"
/**/
#include "build/build.h"

#include "build/build.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BUILD RULES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// Builds the host's own build tool, so every flag that names a platform comes from the host block
// in build.h rather than from a target's set. See the note there.
NYA_INTERNAL NYA_Command build_rebuild_command = {
    .program   = CC,
    .arguments = {
        "build.c",
        "-o", BUILD_TOOL_BINARY,
        CFLAGS,
        WARNINGS,
        INCLUDE_PATHS,
        LINKER_FLAGS,
        FLAGS_DEBUG,
        FLAGS_HOST_NATIVE,
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

/*
 * Depends on index_assets, and the order is the point.
 *
 * Indexing writes assets/assets.h, one handle per file; bundling writes assets/assets.c, the bytes
 * behind those handles. Conceptually you enumerate first and embed second, and both read the same
 * memoised listing, so the handle set and the blob cannot disagree — but only if indexing is what
 * populates that memo, which this ordering is what guarantees.
 */
NYA_INTERNAL NYA_BuildRule bundle_assets = {
    .name             = "bundle_assets",
    .is_metarule      = true,
    .dependencies     = { &build_windows_icon, &build_shaders, &index_assets, },
    .post_build_hooks = { &hook_bundle_assets, },
};

/*
 * ─────────────────────────────────────────────────────────
 * PROJECT BUILD RULES
 * ─────────────────────────────────────────────────────────
 */

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
 * COMMAND HANDLERS
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
            .name        = "assets",
            .description = "Regenerate assets/assets.h and assets/assets.c from what is on disk.",
            // bundle_assets, not index_assets: it depends on the index, so this writes the handle
            // header and the byte blob from one walk of the asset tree rather than two that could
            // disagree. Compiling the shaders and the icon comes with it, because the index has to
            // list artifacts that exist.
            .build_rule  = &bundle_assets,
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
    if (nya_filesystem_exists(BACKTRACE_A_HOST)) {
        u32 count = 0;
        while (count < NYA_COMMAND_MAX_ARGUMENTS && build_rebuild_command.arguments[count] != nullptr) count++;

        NYA_ConstCString extra[]   = { BACKTRACE_INCLUDES_HOST, BACKTRACE_A_HOST };
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
