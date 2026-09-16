/**
 * @file misc.h
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
