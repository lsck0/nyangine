#include "build/build.h"

/* COMMAND HANDLERS */

/**
 * Builds every vendored dependency, not just the ones the engine currently links against.
 * */
NYA_INTERNAL void vendor_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_EXPECT(nya_vendor_build_all(NYA_VENDORS_ALL), "while building all vendor dependencies");
}

/**
 * Compiles the headless demo to WebAssembly with emcc, then proves the artifacts are real: both files
 * exist and the loader names the exported symbol.
 *
 * A handler rather than an NYA_BuildRule because it does three things a rule cannot: it makes the
 * output directory emcc will not, and it opens the generated loader afterward to confirm the export
 * survived, which is the one thing that silently goes wrong (a typo'd -sEXPORTED_FUNCTIONS builds
 * clean and exports nothing). Off the critical path and its own command, like `./build vendor`.
 * */
NYA_INTERNAL void wasm_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "wasm_runner");
    defer nya_arena_destroy(arena);

    // emcc writes web/nyangine.js and .wasm but does not create web/ itself. Idempotent: an existing
    // directory is not an error.
    NYA_EXPECT(nya_filesystem_create_directory(WASM_OUTPUT_DIRECTORY), "while creating %s", WASM_OUTPUT_DIRECTORY);

    NYA_BuildRule build_wasm = {
        .name        = "build_wasm",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = WASM_JS_OUTPUT,

        .command = {
            .program   = EMCC,
            .arguments = {
                WASM_DEMO_SOURCE,
                "-o", WASM_JS_OUTPUT,
                FLAGS_WASM,
                // The engine's own include roots, so the NYA_WASM_WITH_ENGINE path in wasm_demo.c
                // resolves its headers once the wasm engine port makes that block compile.
                INCLUDE_PATHS,
            },
        },
    };

    NYA_EXPECT(nya_build(&build_wasm), "while compiling the wasm demo");

    // The artifacts, by hand: nya_build only knows emcc exited zero, not that it wrote what we named.
    if (!nya_filesystem_exists(WASM_JS_OUTPUT)) nya_log_panic("emcc reported success but %s is missing.", WASM_JS_OUTPUT);
    if (!nya_filesystem_exists(WASM_WASM_OUTPUT)) nya_log_panic("emcc reported success but %s is missing.", WASM_WASM_OUTPUT);

    // The export, by reading it back: the loader references the symbol by name, so its absence there
    // means the wasm exports nothing the page can call, whatever emcc's exit code said.
    NYA_String* loader = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(WASM_JS_OUTPUT, loader), "while reading %s back", WASM_JS_OUTPUT);
    if (!nya_string_contains(nya_string_to_cstring(arena, loader), WASM_EXPORTED_SYMBOL)) {
        nya_log_panic("%s does not name %s: the export was dropped.", WASM_JS_OUTPUT, WASM_EXPORTED_SYMBOL);
    }

    nya_log_info("Built %s and %s; %s is exported. Serve %s over HTTP to run it.", WASM_JS_OUTPUT, WASM_WASM_OUTPUT, WASM_EXPORTED_SYMBOL,
                 WASM_OUTPUT_DIRECTORY);
}

/**
 * Compiles the client-side UI to WebAssembly with emcc, then proves the artifacts are real: both files
 * exist and the loader names both exported symbols. The CSR bridge — the immediate-mode UI component
 * rendered to the DOM and driven from it, no server. A sibling of wasm_runner, off the critical path and
 * its own command for the same reason: emcc is not part of the default toolchain.
 * */
NYA_INTERNAL void wasm_ui_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "wasm_ui_runner");
    defer nya_arena_destroy(arena);

    // emcc writes into web/ but does not create it; idempotent, an existing directory is not an error.
    NYA_EXPECT(nya_filesystem_create_directory(WASM_OUTPUT_DIRECTORY), "while creating %s", WASM_OUTPUT_DIRECTORY);

    NYA_BuildRule build_wasm_ui = {
        .name        = "build_wasm_ui",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = WASM_UI_JS_OUTPUT,

        .command = {
            .program   = EMCC,
            .arguments = {
                WASM_UI_SOURCE,
                "-o", WASM_UI_JS_OUTPUT,
                FLAGS_WASM_UI,
                // The engine's own include roots, beside the vendored ones FLAGS_WASM_UI adds, so the
                // full header graph NYA_App needs resolves.
                INCLUDE_PATHS,
            },
        },
    };

    NYA_EXPECT(nya_build(&build_wasm_ui), "while compiling the wasm UI");

    // The artifacts, by hand: nya_build only knows emcc exited zero, not that it wrote what we named.
    if (!nya_filesystem_exists(WASM_UI_JS_OUTPUT)) nya_log_panic("emcc reported success but %s is missing.", WASM_UI_JS_OUTPUT);
    if (!nya_filesystem_exists(WASM_UI_WASM_OUTPUT)) nya_log_panic("emcc reported success but %s is missing.", WASM_UI_WASM_OUTPUT);

    // Both exports, by reading the loader back: the page calls both, so either one dropped means a page
    // that cannot render or cannot forward a click, whatever emcc's exit code said.
    NYA_String* loader = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(WASM_UI_JS_OUTPUT, loader), "while reading %s back", WASM_UI_JS_OUTPUT);

    NYA_ConstCString loader_text = nya_string_to_cstring(arena, loader);
    if (!nya_string_contains(loader_text, WASM_UI_RENDER_SYMBOL)) {
        nya_log_panic("%s does not name %s: the export was dropped.", WASM_UI_JS_OUTPUT, WASM_UI_RENDER_SYMBOL);
    }
    if (!nya_string_contains(loader_text, WASM_UI_EVENT_SYMBOL)) {
        nya_log_panic("%s does not name %s: the export was dropped.", WASM_UI_JS_OUTPUT, WASM_UI_EVENT_SYMBOL);
    }

    nya_log_info("Built %s and %s; %s and %s are exported. Serve %s over HTTP and open ui.html.", WASM_UI_JS_OUTPUT, WASM_UI_WASM_OUTPUT,
                 WASM_UI_RENDER_SYMBOL, WASM_UI_EVENT_SYMBOL, WASM_OUTPUT_DIRECTORY);
}

/**
 * Compiles the 2D game slice to WebAssembly with emcc, then proves the artifacts are real: both files
 * exist and the loader names the self-check export. The engine's 2D renderer — a cleared background and
 * a textured sprite — drawn to a WebGL2 canvas through the SDL_GPU → GLES3 shim. A sibling of
 * wasm_ui_runner, off the critical path and its own command for the same reason: emcc is not part of the
 * default toolchain.
 * */
NYA_INTERNAL void wasm_game_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "wasm_game_runner");
    defer nya_arena_destroy(arena);

    // emcc writes into web/ but does not create it; idempotent, an existing directory is not an error.
    NYA_EXPECT(nya_filesystem_create_directory(WASM_OUTPUT_DIRECTORY), "while creating %s", WASM_OUTPUT_DIRECTORY);

    NYA_BuildRule build_wasm_game = {
        .name        = "build_wasm_game",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = WASM_GAME_JS_OUTPUT,

        .command = {
            .program   = EMCC,
            .arguments = {
                WASM_GAME_SOURCE,
                "-o", WASM_GAME_JS_OUTPUT,
                FLAGS_WASM_GAME,
                // The engine's own include roots, beside the vendored ones FLAGS_WASM_GAME adds, so the
                // full header graph NYA_Vertex2D and the SDL_GPU types come from resolves.
                INCLUDE_PATHS,
            },
        },
    };

    NYA_EXPECT(nya_build(&build_wasm_game), "while compiling the wasm game slice");

    // The artifacts, by hand: nya_build only knows emcc exited zero, not that it wrote what we named.
    if (!nya_filesystem_exists(WASM_GAME_JS_OUTPUT)) nya_log_panic("emcc reported success but %s is missing.", WASM_GAME_JS_OUTPUT);
    if (!nya_filesystem_exists(WASM_GAME_WASM_OUTPUT)) nya_log_panic("emcc reported success but %s is missing.", WASM_GAME_WASM_OUTPUT);

    // The export, by reading the loader back: the self-check the page and a headless node run both call.
    NYA_String* loader = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(WASM_GAME_JS_OUTPUT, loader), "while reading %s back", WASM_GAME_JS_OUTPUT);
    NYA_ConstCString loader_text = nya_string_to_cstring(arena, loader);
    if (!nya_string_contains(loader_text, WASM_GAME_SYMBOL)) {
        nya_log_panic("%s does not name %s: the export was dropped.", WASM_GAME_JS_OUTPUT, WASM_GAME_SYMBOL);
    }
    if (!nya_string_contains(loader_text, WASM_GAME_SYMBOL_3D)) {
        nya_log_panic("%s does not name %s: the export was dropped.", WASM_GAME_JS_OUTPUT, WASM_GAME_SYMBOL_3D);
    }
    if (!nya_string_contains(loader_text, WASM_GAME_SYMBOL_SCENE)) {
        nya_log_panic("%s does not name %s: the export was dropped.", WASM_GAME_JS_OUTPUT, WASM_GAME_SYMBOL_SCENE);
    }

    nya_log_info("Built %s and %s; %s, %s and %s are exported. Serve %s over HTTP and open game.html.", WASM_GAME_JS_OUTPUT,
                 WASM_GAME_WASM_OUTPUT, WASM_GAME_SYMBOL, WASM_GAME_SYMBOL_3D, WASM_GAME_SYMBOL_SCENE, WASM_OUTPUT_DIRECTORY);
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

/* PARAMETERS */

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

NYA_INTERNAL NYA_ArgParameter coverage_fail_under = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    // S64, not F64: a percentage floor is expressed in whole points, and the parser takes B8, S64,
    // F64 and STRING. The measured coverage is compared as a real, so a run at 44.9% still fails a
    // floor of 45.
    .value.type    = NYA_TYPE_S64,
    .name          = "fail-under",
    .description   = "Exit non-zero if total line coverage of src/nyangine is below this percent.",
    .default_value = { .type = NYA_TYPE_S64, .as_s64 = COVERAGE_DEFAULT_FAIL_UNDER },
};

NYA_INTERNAL NYA_ArgParameter coverage_html_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "html",
    .description = "Also write an annotated HTML listing under " COVERAGE_HTML_DIRECTORY " (gitignored).",
};

NYA_INTERNAL NYA_ArgParameter example_name = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    .name        = "example",
    .description = "Which example to build and run. The directory name under examples/.",
    // straight from the directory listing, so a new example folder is offered without being named here.
    // Not KIND_FILE: the argument is a directory name, and path completion would offer
    // examples/hello_world/main.c, which is not accepted.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &example_completion_name, },
};

NYA_INTERNAL NYA_ArgParameter fuzz_target = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    // variadic so it is optional, not so it takes several: the parser makes a variadic positional the
    // only optional kind, and running the command bare lists what there is, which for a command that
    // otherwise runs until it is interrupted is the useful thing to do. More than one is refused.
    .variadic    = true,
    .name        = "target",
    .description = "Which fuzz target to run. If none specified, the targets are listed.",
    // straight from the files under tests/fuzz, so a new target is offered without being named here.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &fuzz_completion_target_name, },
};

NYA_INTERNAL NYA_ArgParameter simulation_seed = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    // S64, not U64: the parser takes B8, S64, F64 and STRING. A seed is a bit pattern rather than a
    // count, so the sign is meaningless and the cast back to u64 in the runner is exact.
    .value.type    = NYA_TYPE_S64,
    .name          = "seed",
    .description   = "Which seed to simulate. If none specified, a fresh one is drawn and printed.",
    // zero means "draw one": a seed of zero is as good as any other and nobody asks for it by name,
    // so this costs no reachable value. See simulation_runner.
    .default_value = { .type = NYA_TYPE_S64, .as_s64 = 0 },
};

NYA_INTERNAL NYA_ArgParameter simulation_steps = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_S64,
    .name        = "steps",
    .description = "How many actions to take. Longer runs reach deeper states.",
    // a hundred thousand is about a minute under sanitizers, which is long enough for a scheduled run
    // to find something and short enough to wait for.
    .default_value = { .type = NYA_TYPE_S64, .as_s64 = 100000 },
};

NYA_INTERNAL NYA_ArgParameter simulation_verbose_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "verbose",
    .description = "Print every action as it is taken, and leave the engine's own logging on.",
};

NYA_INTERNAL NYA_ArgParameter agent_kind = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type    = NYA_TYPE_STRING,
    .name          = "kind",
    .description   = "Which agent plays: random, dqn or neat.",
    .default_value = { .type = NYA_TYPE_STRING, .as_string = "dqn" },
    .completion    = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &agent_completion_kind, },
};

NYA_INTERNAL NYA_ArgParameter agent_seed = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_S64,
    .name        = "seed",
    .description = "What the run is derived from. If none specified, a fresh one is drawn and printed.",
    // zero means "draw one", as it does for the simulation and for the same reason.
    .default_value = { .type = NYA_TYPE_S64, .as_s64 = 0 },
};

NYA_INTERNAL NYA_ArgParameter agent_episodes = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_S64,
    .name        = "episodes",
    .description = "Sessions to play, or NEAT generations. Each one is a session per genome.",
    // eight of the default length is a few minutes and enough for a DQN's exploration to anneal
    // most of the way, which is where it starts playing rather than flailing.
    .default_value = { .type = NYA_TYPE_S64, .as_s64 = 8 },
};

NYA_INTERNAL NYA_ArgParameter agent_ticks = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_S64,
    .name        = "ticks",
    .description = "Fixed steps per episode. Four simulated minutes at the default tick rate.",
    .default_value = { .type = NYA_TYPE_S64, .as_s64 = 15000 },
};

NYA_INTERNAL NYA_ArgParameter agent_verbose_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "verbose",
    .description = "Print every action as it is taken, and leave the engine's own logging on.",
};

NYA_INTERNAL NYA_ArgParameter new_name = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    .name        = "name",
    .description = "The new program's name. Becomes a C identifier, so letters, digits and underscores.",
};

NYA_INTERNAL NYA_ArgParameter new_kind = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type    = NYA_TYPE_STRING,
    .name          = "kind",
    .description   = "What to scaffold: app, example or headless.",
    // The commonest ask is a place to try something out, which is the example, so it is the default.
    .default_value = { .type = NYA_TYPE_STRING, .as_string = "example" },
    .completion    = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &new_completion_kind, },
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

NYA_INTERNAL NYA_ArgParameter commit_check_target = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    // variadic so it is optional, not so it takes several: with none, the HEAD commit is checked; with
    // one, either a message file or a git range. A second argument is refused.
    .variadic    = true,
    .value.type  = NYA_TYPE_STRING,
    .name        = "target",
    .description = "A commit message file, or a git revision range like origin/master..HEAD. Defaults to HEAD.",
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE, },
};

NYA_INTERNAL NYA_ArgParameter format_check_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "check",
    .description = "Report what is not formatted and exit non-zero, rather than rewriting. What CI wants.",
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

/*
 * The headless-server switch, read in two places from the one flag: build.c brings up only
 * NYA_VENDORS_SERVER_LINUX_X86_64 instead of every vendor, so a server build never compiles SDL,
 * box2d, box3d, ufbx or shadercross; and `run example` builds the example as a shipping artifact —
 * release flags, dead code collected, the binary stripped — and does not run it, which is what a
 * container image is built from. Declared extern in example.c, which is compiled before this file.
 */
NYA_ArgParameter server_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "server",
    .description = "Build for a headless server: only the vendors a web app links, and examples built to ship rather than to run.",
};

NYA_INTERNAL NYA_ArgParameter dist_target = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    // Several in one run, because a run is the unit a checksum is consistent over: each invocation
    // rebuilds and rearchives what it stages, so two of them leave a manifest hashing an archive the
    // second one has already replaced.
    .variadic    = true,
    .value.type  = NYA_TYPE_STRING,
    .name        = "targets",
    .description = "Which distributions to stage. If none specified, every one this host can produce.",
    // Straight from the table in dist.c that also parses this argument, so a target added there is
    // offered here without this file learning its name.
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &dist_completion_target, },
};

NYA_INTERNAL NYA_ArgParameter changelog_release_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "release",
    .description = "Print only the newest version's section, on stdout. The release notes CD publishes.",
};

NYA_INTERNAL NYA_ArgParameter plugin_directory = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    .name        = "directory",
    .description = "The plugin directory to sign, e.g. plugins/hello.",
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "plugins", },
};

NYA_INTERNAL NYA_ArgParameter plugin_seed = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type    = NYA_TYPE_STRING,
    .name          = "seed",
    .description   = "The signing key's seed file. keygen writes it; sign reads it.",
    .default_value = { .type = NYA_TYPE_STRING, .as_string = "plugin_signing.seed" },
    .completion    = { .kind = NYA_ARG_COMPLETION_KIND_FILE, },
};

NYA_INTERNAL NYA_ArgParameter plugin_publisher = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type    = NYA_TYPE_STRING,
    .name          = "publisher",
    .description   = "Who the signature names. Defaults to the manifest's author.",
    // A default makes it optional: without one the parser treats a flag as required.
    .default_value = { .type = NYA_TYPE_STRING, .as_string = "" },
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

/* COMMANDS */

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
#if !OS_WINDOWS
        // The second app, to show a project runs more than one binary. Same host, same hot reload; the
        // host loads gnyame-cli.debug.so because the binary it runs is named gnyame-cli.debug. `run debug`
        // above is still the default app, gnyame. See build_gnyame_cli_debug_linux.
        &(NYA_ArgCommand){
            .name        = "gnyame-cli",
            .description = "Build and run the gnyame-cli app under the host with hot reload. Sanitized, like `run debug`.",
            .build_rule  = &run_gnyame_cli_debug,
        },
#endif
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
            .name        = "fuzz",
            .description = "Fuzz one parser or input boundary with AFL++, seeded from its committed corpus.",
            .handler     = &fuzz_runner,
            .parameters  = { &fuzz_target, },
        },
        &(NYA_ArgCommand){
            .name        = "simulation",
            .description = "Run one deterministic simulation over the engine. A failing seed replays exactly.",
            .handler     = &simulation_runner,
            .parameters  = { &simulation_seed, &simulation_steps, &simulation_verbose_flag, },
        },
        &(NYA_ArgCommand){
            .name        = "agent",
            .description = "Let a DQN or a NEAT population play gnyame as a user, headless and far faster than real time.",
            .handler     = &agent_runner,
            .parameters  = { &agent_kind, &agent_seed, &agent_episodes, &agent_ticks, &agent_verbose_flag, },
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
        // The second app: its debug host (gnyame's host relinked under gnyame-cli's name) and its dll.
        // The default app's rules above are unchanged. See build_gnyame_cli_debug_linux.
        &(NYA_ArgCommand){
            .name        = "gnyame-cli",
            .description = "Build the gnyame-cli app: its debug host and dll.",
            .build_rule  = &build_gnyame_cli_debug_linux,
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
        &(NYA_ArgCommand){
            .name        = "terminal-windows",
            .description = "Compile the terminal backend for windows. Compiles only: nothing here has ever run on windows.",
            .build_rule  = &compile_terminal_windows,
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
#if !OS_WINDOWS
        &(NYA_ArgCommand){
            .name        = "steam-linux",
            .description = "Build the Steam Linux Runtime executable and libsteam_api.so. Downloads the sniper SDK sysroot once.",
            .build_rule  = &build_project_steam_linux_x86_64,
        },
        &(NYA_ArgCommand){
            .name        = "steam-linux-vendor",
            .description = "Build only the sniper SDK sysroot and the vendors against it, so CI can cache them apart from the game.",
            .build_rule  = &build_steamrt_vendors,
        },
#endif
        &(NYA_ArgCommand){
            .name        = "steam-windows",
            .description = "Build the Steam windows executable and steam_api64.dll.",
            .build_rule  = &build_project_steam_windows_x86_64,
        },
        &(NYA_ArgCommand){
            .name        = "release",
            .description = "Build every release executable, Steam windows included. steam-linux is separate.",
            .build_rule  = &build_project_release,
        },
        &(NYA_ArgCommand){
            .name        = "assets",
            .description = "Regenerate src/genyarated/assets.h and src/genyarated/assets.c from what is on disk.",
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

NYA_INTERNAL NYA_ArgCommand new_command = {
    .name        = "new",
    .description = "Scaffold a new nyangine program from a template, e.g. ./build new my_app --kind example",
    .handler     = &new_runner,
    .parameters  = { &new_name, &new_kind, },
};

NYA_INTERNAL NYA_ArgParameter project_manifest = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    // variadic to make it optional: bare `./build project` reads ./project.nya. More than one is refused.
    .variadic    = true,
    .name        = "manifest",
    .description = "The project.nya to resolve. Defaults to ./project.nya.",
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE },
};

NYA_INTERNAL NYA_ArgCommand project_command = {
    .name        = "project",
    .description = "Read a project.nya manifest and print the build plan it resolves to. See project.c.",
    .handler     = &project_runner,
    .parameters  = { &project_manifest, },
};

NYA_INTERNAL NYA_ArgCommand check = {
    .name        = "check",
    .description = "Run clang-tidy over the translation units.",
    .handler     = &check_runner,
    .parameters  = { &check_sources, &check_strict_flag, },
};

NYA_INTERNAL NYA_ArgCommand typos = {
    .name        = "typos",
    .description = "Spell-check the prose and code under src/, tests/, examples/ and docs/. Needs the `typos` tool; skips with a notice if absent.",
    .handler     = &typos_runner,
};

NYA_INTERNAL NYA_ArgCommand verify = {
    .name        = "verify",
    .description = "Model-check the untrusted-input parsers under tests/cbmc with CBMC. Needs the `cbmc` tool; skips with a notice if absent.",
    .handler     = &verify_runner,
};

NYA_INTERNAL NYA_ArgCommand commit_check = {
    .name        = "commit-check",
    .description = "Lint a commit message, or every message in a git range, against the repo's rules. What CI runs on a PR.",
    .handler     = &commit_check_runner,
    .parameters  = { &commit_check_target, },
};

NYA_INTERNAL NYA_ArgCommand format = {
    .name        = "format",
    .description = "clang-format the C under src/, tests/, examples/ and bench/. --check reports and fails instead of rewriting. Advisory; needs clang-format.",
    .handler     = &format_runner,
    .parameters  = { &format_check_flag, },
};

NYA_INTERNAL NYA_ArgCommand coverage = {
    .name        = "coverage",
    .description = "Build and run the tests instrumented, report line coverage of src/nyangine, and gate on --fail-under.",
    .handler     = &coverage_runner,
    .parameters  = { &test_files, &coverage_fail_under, &coverage_html_flag, },
};

NYA_INTERNAL NYA_ArgCommand dist = {
    .name        = "dist",
    .description = "Stage the distributions under dist/: one directory per target, plus the archives a release publishes.",
    .handler     = &dist_runner,
    .parameters  = { &dist_target, },
};

NYA_INTERNAL NYA_ArgCommand changelog = {
    .name        = "changelog",
    .description = "Regenerate CHANGELOG.md from the conventional commits in the history.",
    .handler     = &changelog_runner,
    .parameters  = { &changelog_release_flag, },
};

NYA_INTERNAL NYA_ArgCommand sbom = {
    .name        = "sbom",
    .description = "Generate the SBOM from the vendored submodules, fail on a licence off the allowlist, and run the CVE hook.",
    .handler     = &sbom_runner,
};

NYA_INTERNAL NYA_ArgCommand version = {
    .name        = "version",
    .description = "Print the version on stdout. The only place anything outside this tool may read it from.",
    .handler     = &version_runner,
};

NYA_INTERNAL NYA_ArgCommand perf = {
    .name        = "perf",
    .description = "Open Profiler with last profiling data.",
    .build_rule  = &open_perf_report,
};

NYA_INTERNAL NYA_ArgCommand docs = {
    .name        = "docs",
    .description = "Assemble the deployable docs site under ./site: prose, cheatsheet, and doxygen HTML.",
    .build_rule  = &assemble_docs,
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

NYA_INTERNAL NYA_ArgCommand wasm = {
    .name        = "wasm",
    .description = "Compile the headless demo to web/nyangine.wasm + .js for the browser. Needs emcc; the seed of the CSR path.",
    .handler     = &wasm_runner,
};

NYA_INTERNAL NYA_ArgCommand wasm_ui = {
    .name        = "wasm-ui",
    .description = "Compile the client-side UI to web/nyangine_ui.wasm + .js. Needs emcc; the CSR bridge, driven by web/ui.html.",
    .handler     = &wasm_ui_runner,
};

NYA_INTERNAL NYA_ArgCommand wasm_game = {
    .name        = "wasm-game",
    .description = "Compile the 2D game slice to web/nyangine_game.wasm + .js: a textured sprite on a WebGL2 canvas via the SDL_GPU→GLES3 shim. Needs emcc; driven by web/game.html.",
    .handler     = &wasm_game_runner,
};

NYA_INTERNAL NYA_ArgCommand plugin = {
    .name        = "plugin",
    .description = "Sign plugins so a build that requires it will load them.",
    .subcommands = {
        &(NYA_ArgCommand){
            .name        = "keygen",
            .description = "Draw an Ed25519 signing key, write its seed to a file, and print the public key to pin.",
            .handler     = &plugin_keygen_runner,
            .parameters  = { &plugin_seed, },
        },
        &(NYA_ArgCommand){
            .name        = "sign",
            .description = "Sign a plugin directory with a seed off disk, writing its plugin.sig.",
            .handler     = &plugin_sign_runner,
            .parameters  = { &plugin_directory, &plugin_seed, &plugin_publisher, },
        },
    },
};

NYA_INTERNAL NYA_ArgCommand completions = {
    .name        = "completions",
    .description = "Generate a shell completion script on stdout, e.g. ./build completions zsh > ~/.zsh/completions/_build",
    .handler     = &completions_runner,
    .parameters  = { &completions_shell, },
};

/* THE PARSER */

NYA_INTERNAL NYA_ArgParser parser = {
    .name    = "nyangine build system",
    .version = VERSION,

    .root_command = &(NYA_ArgCommand){
        .is_root    = true,
        .parameters = {
            &skip_self_rebuild_flag,
            &regenerate_flag,
            &help_flag,
            &server_flag,
        },
        .subcommands = {
            &run,
            &build,
            &new_command,
            &project_command,
            &dist,
            &check,
            &typos,
            &verify,
            &commit_check,
            &format,
            &coverage,
            &changelog,
            &sbom,
            &version,
            &perf,
            &docs,
            &stats,
            &update,
            &wasm,
            &wasm_ui,
            &wasm_game,
            &plugin,
            &completions,
        },
    },
};
