#include "build/build.h"

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

/** Writes the completion script for whatever the parser currently describes. See main, which short circuits to this. */
NYA_INTERNAL void completions_runner(NYA_ArgCommand* command) {
    NYA_ArgParameter* shell = command->parameters[0];
    nya_assert(shell != nullptr);

    NYA_Error result = nya_args_print_completions(&parser, BUILD_TOOL_BINARY, shell->value.as_string);

    // A misspelled shell is user input, not a broken build, so it gets the same message and exit
    // code main gives any other bad argument rather than a panic and a stack trace.
    if (!result.ok) {
        (void)fprintf(stderr, "Error: %s\n\n", result.message);
        nya_args_print_usage(&parser, command);
        exit(EXIT_FAILURE);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PARAMETERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArgParameter bench_files = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .variadic    = true,
    .value.type  = NYA_TYPE_STRING,
    .name        = "benchmarks",
    .description = "Which benchmarks to run. If none specified, all are run.",
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "bench", .glob = "*.c", },
};

NYA_INTERNAL NYA_ArgParameter test_files = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .variadic    = true,
    .value.type  = NYA_TYPE_STRING,
    .name        = "tests",
    .description = "Which tests to run. If none specified, all tests are run.",
    // Matched as a substring against every source under ./tests, so completing paths relative to
    // that directory hands the runner something it will always match.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "tests", .glob = "*.c", },
};

NYA_INTERNAL NYA_ArgParameter example_name = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    .name        = "example",
    .description = "Which example to build and run. The directory name under examples/.",
    // Straight from the directory listing, so an example added by creating a folder is offered here
    // without this file knowing its name. Not KIND_FILE: the argument is a directory component, not
    // a path, and completing paths would offer examples/hello_world/main.c — which is not accepted.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &example_completion_name, },
};

NYA_INTERNAL NYA_ArgParameter check_sources = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .variadic    = true,
    .value.type  = NYA_TYPE_STRING,
    .name        = "sources",
    .description = "Which translation units to check. If none specified, all of them are checked.",
    // Completing against ./src finds main.c and gnyame.c, which are two of the three roots. The
    // third is ./build.c and is a single well known name nobody needs completion for.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "src", .glob = "*.c", },
};

NYA_INTERNAL NYA_ArgParameter check_strict_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "strict",
    .description = "Fail on any finding, rather than reporting and succeeding. What CI wants.",
};

NYA_INTERNAL NYA_ArgParameter skip_self_rebuild_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "no-rebuild",
    .description = "Don't rebuild the build system before executing the command.",
};

NYA_INTERNAL NYA_ArgParameter regenerate_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "regenerate",
    .description = "Run every preprocessor pass even if nothing it reads has changed.",
};

NYA_INTERNAL NYA_ArgParameter help_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "help",
    .description = "Show this message.",
};

NYA_INTERNAL NYA_ArgParameter completions_shell = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    .name        = "shell",
    .description = "Which shell to generate for.",
    // Straight from the registry in base_args.c, so a shell added there is offered here without
    // this file knowing any shell's name.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &nya_args_completion_shell_name, },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * COMMANDS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArgCommand run = {
    .name = "run",
    .description = "Run things. Always the host's own build; cross compiled artifacts cannot run here.",
    .subcommands = {
        &(NYA_ArgCommand){
            .name        = "debug",
            .description = "Run the debug build. Sanitized, hot reloading, slow.",
            .build_rule  = &run_debug,
        },
        &(NYA_ArgCommand){
            .name        = "profile",
            .description = "Run the release build under perf, then './build perf' to read it.",
            .build_rule  = &run_profile,
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
            .name        = "example",
            .description = "Build and run one example from examples/, e.g. ./build run example hello_world",
            .handler     = &example_runner,
            .parameters  = { &example_name, },
        },
        &(NYA_ArgCommand){
            .name        = "test",
            .description = "Build and run the tests.",
            .handler     = &test_runner,
            .parameters  = { &test_files, },
        },
        &(NYA_ArgCommand){
            .name        = "bench",
            .description = "Build and run the benchmarks under bench/, optimised and without sanitizers.",
            .handler     = &bench_runner,
            .parameters  = { &bench_files, },
        },
        &(NYA_ArgCommand){
            .name        = "coverage",
            .description = "Build and run the tests instrumented, then report line coverage of src/nyangine.",
            .handler     = &coverage_runner,
            .parameters  = { &test_files, },
        },
    },
};

NYA_INTERNAL NYA_ArgCommand build = {
    .name        = "build",
    .description = "Build things.",
    .subcommands = {
// The linux targets are absent on a Windows host rather than present and failing: a command
// that cannot work on this machine should not be in the help output or the completions.
#if !OS_WINDOWS
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
#endif
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
#if !OS_WINDOWS
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
#endif
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
#if !OS_WINDOWS
        &(NYA_ArgCommand){
            .name        = "release-linux",
            .description = "Build the linux release executable.",
            .build_rule  = &build_project_linux_x86_64,
        },
#endif
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
            .description = "Regenerate src/generated/assets.h and src/generated/assets.c from what is on disk.",
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

NYA_INTERNAL NYA_ArgCommand check = {
    .name        = "check",
    .description = "Run clang-tidy over the translation units.",
    .handler     = &check_runner,
    .parameters  = { &check_sources, &check_strict_flag, },
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

NYA_INTERNAL NYA_ArgCommand completions = {
    .name        = "completions",
    .description = "Generate a shell completion script on stdout, e.g. ./build completions zsh > ~/.zsh/completions/_build",
    .handler     = &completions_runner,
    .parameters  = { &completions_shell, },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PARSER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArgParser parser = {
    .name    = "nyangine build system",
    .version = VERSION,

    .root_command = &(NYA_ArgCommand){
        .is_root    = true,
        .parameters = {
            &skip_self_rebuild_flag,
            &regenerate_flag,
            &help_flag,
        },
        .subcommands = {
            &run,
            &build,
            &check,
            &perf,
            &docs,
            &stats,
            &update,
            &completions,
        },
    },
};
