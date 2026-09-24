/**
 * @file misc.h
 *
 * Every rule that is not the project itself: the release metarule, running what was built, opening a
 * report, touching the repository, and the tool's own rebuild.
 * */
#pragma once

#include "nyangine/nyangine.h"
// For host_build_debug, HOST_DEBUG_BINARY, SANITIZER_ENVIRONMENT and BUILD_TOOL_BINARY.
#include "build/flags.h"
// For hook_convert_perf_data_to_plain.
#include "build/hooks.h"
// For CC.
#include "build/vendor/vendor_common.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PROJECT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every release artifact this host can produce. A Windows host produces only the Windows ones, see build.h. Steam Linux
 * is left out: its first build downloads a sysroot and rebuilds every vendor against it.
 * */
NYA_INTERNAL NYA_BuildRule build_project_release = {
    .name         = "build_project_release",
    .is_metarule  = true,
    .dependencies = {
#if !OS_WINDOWS
        &build_project_linux_x86_64,
#endif
        &build_project_windows_x86_64,
        &build_project_steam_windows_x86_64,
    },
};

/**
 * Assembles the whole documentation site into one deployable tree under ./site: the hand-written
 * GitBook prose and its SUMMARY.md, the cheatsheet generated from the headers, and the doxygen HTML
 * generated from the source. Landing there together is the point — the three tiers cross-link with
 * relative paths, so wherever ./site is served they reach one another.
 *
 * The order is load-bearing. generate_cheatsheet (a dependency) rewrites docs/CHEATSHEET.md; then
 * hook_assemble_docs stages docs/ (prose plus that cheatsheet) into ./site; then this rule's own
 * command runs doxygen over docs/doxygen.config, whose OUTPUT_DIRECTORY is ./site/doxygen, so the
 * generated reference lands inside the already-staged tree.
 * */
NYA_INTERNAL NYA_BuildRule assemble_docs = {
    .name    = "assemble_docs",
    .policy  = NYA_BUILD_ALWAYS,

    .command = {
        .program   = "doxygen",
        .arguments = { "./docs/doxygen.config", },
    },

    .dependencies    = { &generate_cheatsheet, },
    .pre_build_hooks = { &hook_assemble_docs, },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RUNNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Debug runs directly. Sanitized, hot reloading, slow.
 * */
NYA_INTERNAL NYA_BuildRule run_debug = {
    .name   = "run_debug",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program     = "./" HOST_DEBUG_BINARY,
        .environment = { SANITIZER_ENVIRONMENT, },
    },

    .dependencies = { &host_build_debug, },
};

/**
 * The second app, gnyame-cli, run under the same host with hot reload. Debug: sanitized, hot reloading,
 * slow, exactly like run_debug — the difference is only which DLL the host loads, chosen by the binary's
 * name. See build_gnyame_cli_debug_linux. Linux only, where the second app's rules live.
 * */
#if !OS_WINDOWS
NYA_INTERNAL NYA_BuildRule run_gnyame_cli_debug = {
    .name   = "run_gnyame_cli_debug",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program     = "./" LINUX_X86_64_GNYAME_CLI_DEBUG_BINARY,
        .environment = { SANITIZER_ENVIRONMENT, },
    },

    .dependencies = { &build_gnyame_cli_debug_linux, },
};
#endif

/**
 * The release build under perf. The profile worth acting on.
 *
 * Inlining and tail calls attribute some frames to the surviving function, and vanished symbols show up
 * inside their callers. Dwarf call graphs recover most of the structure, which is why the unwinding is
 * worth its cost.
 * */
NYA_INTERNAL NYA_BuildRule run_profile = {
    .name   = "run_profile",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program = "perf",
        .arguments = {
            "record",
            "-T",
            "-F", "999",
            "-g", "--call-graph", "dwarf",
            "-e", "cycles,instructions,cache-misses",
            "./" HOST_RELEASE_BINARY,
        },
    },

    .dependencies     = { &host_build_release, },
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * OPENING THINGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

            /*
             * Converted here as well as after the run that recorded it.
             */
            .pre_build_hooks = { &hook_convert_perf_data_to_plain, },
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * REPOSITORY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_BuildRule show_stats = {
    .name   = "show_stats",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program = "tokei",

        /*
         * Vendor, tests, the examples and the generated tree are all excluded, so the number means "how
         * much engine is there": nobody wrote src/genyarated, and an example is a caller rather than the
         * thing being measured.
         */
        .arguments = { ".",           "--exclude", "vendor",         "--exclude", "assets", "--exclude", "tests",
                       "--exclude",   "examples",  "--exclude",      "src/genyarated", },
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
 * THE TOOL ITSELF
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds the host's own build tool. Read only by main, which hands it to nya_rebuild_yourself after
 * appending libbacktrace to it when there is one.
 * */
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
        "-fsanitize-ignorelist=src/build/sanitizer_ignorelist.txt",
    },
};
