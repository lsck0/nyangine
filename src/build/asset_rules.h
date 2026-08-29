/**
 * @file asset_rules.h
 *
 * The asset pipeline expressed as build rules: what `./build build assets` actually runs, and in
 * what order.
 *
 * Separate from build/pp/asset.h, which holds the pipeline itself. This file is orchestration — it
 * knows when things run and what they depend on; it does none of the work.
 *
 * A header rather than a .c, the way every other rule in the build system is defined in a header:
 * on_linux/build_linux.h, misc.h, rebuild.h and each vendor_*.h all do the same. A rule is a static
 * initialiser and nothing else, so there is no implementation to separate from.
 *
 * In its own header rather than in build.h so that on_linux/build_linux.h and its siblings can
 * include what they use. Otherwise those files compile only in build.h's include order, and opening
 * one on its own reports build_shaders and index_assets as undeclared.
 * */
#pragma once

#include "nyangine/nyangine.h"

// For WINDRES, which compiles the icon.
#include "build/toolchain.h"
// For the hooks the rules below hang the pipeline off.
#include "build/hooks.h"
// For SHADER_SOURCE_DIRECTORY, which the shader rules walk.
#include "build/pp/asset.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BUILD RULES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The asset pipeline expressed as build rules.
 *
 * Defined here rather than in asset.c, the way every other rule in the build system is defined in a
 * header: on_linux/build_linux.h, misc.h, rebuild.h and each vendor_*.h all do the same. A rule is
 * a static initialiser and nothing else, so there is no implementation to separate from, and a
 * reader looking for what `./build build assets` runs finds it beside the pipeline it drives.
 *
 * In this header specifically, rather than in build.h, so that on_linux/build_linux.h and its
 * siblings can include what they use. Otherwise those files compile only in build.h's include
 * order, and opening one on its own reports build_shaders and index_assets as undeclared.
 */

// Only the two rules below depend on this one.
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
    .vendors         = { &vendor_sdl_shadercross_host, },
};

/**
 * Generates src/generated/strings.h from the locale files.
 *
 * Before index_assets, and that ordering is load bearing for the same reason the shader rule's is:
 * the generated header lands in assets/, and the index has to see the tree as it will finally be.
 * */
NYA_INTERNAL NYA_BuildRule generate_strings = {
    .name             = "generate_strings",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_strings, },
};

/**
 * Regenerates the reflection tables from the @reflect annotations in the tree.
 *
 * Its own rule rather than a hook on a compile rule, because its output is *source* that those rules
 * then compile — so it has to have finished before any of them starts, which is what a dependency
 * expresses and an ordering convention does not.
 * */
NYA_INTERNAL NYA_BuildRule generate_reflection = {
    .name             = "generate_reflection",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_reflection, },
};

NYA_INTERNAL NYA_BuildRule index_assets = {
    .name             = "index_assets",
    .is_metarule      = true,
    // generate_reflection is here for the same reason generate_strings is: it writes *source* that
    // the compile rules then consume, and everything that compiles depends on this rule. It has
    // nothing to do with indexing assets beyond that shared ordering requirement.
    .dependencies     = { &build_windows_icon, &build_shaders, &generate_strings, &generate_reflection, },
    .post_build_hooks = { &hook_index_assets, },
};

/*
 * Depends on index_assets, and the order is the point.
 *
 * Indexing writes src/generated/assets.h, one handle per file; bundling writes src/generated/assets.c, the bytes
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

