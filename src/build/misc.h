/**
 * @file misc.h
 *
 * The build rules that belong to no pipeline: the release metarule, and everything the convenience
 * commands drive — running a build, opening a profile, generating docs, counting lines, updating
 * submodules.
 *
 * They live here rather than in build.c because build.c is the entry point and nothing else, and
 * rather than in a directory of their own because there is no shared machinery behind them. Each is
 * a program to invoke and the dependencies that have to exist first. The asset pipeline has
 * build/asset.h, the tool's own rebuild has build/rebuild.h; what is left is this.
 * */
#pragma once

#include "nyangine/nyangine.h"
// For host_build_debug, HOST_DEBUG_BINARY, SANITIZER_ENVIRONMENT and the per host project rules.
#include "build/host.h"
// For hook_convert_perf_data_to_plain.
#include "build/hooks.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PROJECT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Every release artifact this host can produce. A Windows host produces only the Windows one, see build.h. */
NYA_INTERNAL NYA_BuildRule build_project_release = {
    .name         = "build_project_release",
    .is_metarule  = true,
    .dependencies = {
#if !OS_WINDOWS
        &build_project_linux_x86_64,
#endif
        &build_project_windows_x86_64,
    },
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
 * RUNNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Debug runs directly. Sanitized, hot reloading, slow.
 *
 * It used to run under perf, and the profile was worthless: this is the -O0 build with four
 * sanitizers, so every measurement is dominated by instrumentation that is not in a shipped binary.
 * Worse, it was the *only* thing that ever wrote perf.data, so `./build perf` showed a picture of the
 * sanitizers rather than of the game, and an optimisation chosen from it could as easily be a
 * pessimisation. Profiling moved to `./build run profile`, against the release build, which is the
 * only build whose performance is a fact about the game. See run_profile.
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
 * The release build under perf. The profile worth acting on.
 *
 * Release rather than developer: dev is optimised too, but it carries hot reload and its own entry
 * point, and the question a profile answers is what the shipped binary spends its time on.
 *
 * ⚠ **A release build is not built to be read.** Inlining and tail calls mean some frames belong to
 * whichever function survived rather than the one in the source, and a symbol that vanished entirely
 * shows up inside its caller. That is the trade for measuring the real thing; dwarf call graphs
 * recover most of the structure, which is why the unwinding is worth its cost here.
 *
 * A thousand samples a second rather than the hundred this used to take: a frame is sixteen
 * milliseconds, so a hundred hertz lands one or two samples in it and anything that is not a
 * sustained cost disappears into the noise.
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
             *
             * run_profile's post-build hooks do not run when the program exits non-zero, and a run
             * ended with ctrl-c — which is how a profiling run normally ends — is exactly that.
             * Without this, `./build perf` after one opens speedscope on a perf.data.txt that is
             * missing or is left over from some earlier session.
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

NYA_INTERNAL NYA_BuildRule open_docs = {
    .name   = "open_docs",
    .policy = NYA_BUILD_ALWAYS,

    .command = {
        .program   = "xdg-open",
        .arguments = { "./docs/doxygen/html/index.html", },
    },

    .dependencies = { &build_docs, },
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
         * Vendor and tests both excluded, so the number means "how much engine is there".
         *
         * Vendor is somebody else's code. Tests are ours but they are not the thing being measured:
         * a line count that moves when a test is added answers a different question than the one
         * anyone asks it, and test code is the part most likely to be repetitive besides. Count them
         * deliberately with `tokei ./tests` when that is the question.
         */
        .arguments = { ".", "--exclude", "vendor", "--exclude", "assets", "--exclude", "tests", },
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
