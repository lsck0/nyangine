/**
 * @file asset.h
 *
 * Asset pipeline: shader compilation, the generated asset index, and the embedded asset blob.
 *
 * These are plain functions rather than build hooks on purpose. The build system calls them
 * through thin wrappers in build/hooks.h, which keeps what the pipeline does separate from
 * when the build system happens to run it, and means they can be driven directly from a command or
 * a test without inventing an NYA_BuildRule to hang them off.
 * */
#pragma once

#include "nyangine/nyangine.h"
// For WINDRES, which compiles the icon.
#include "build/toolchain.h"
// For the three hooks the rules below hang the pipeline off.
#include "build/hooks.h"

// For SHADERCROSS_BINARY: the shader rules invoke the tool the vendor build produces, so the path
// belongs to whoever builds it. Spelling it out here is how it silently went stale when the vendor
// build directories were renamed.
#include "build/vendor/vendor_sdl_shadercross.h"

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
 * Generates assets/strings.h from the locale files.
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
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Where the hand written shaders live, and the include directory the compiler is given.
 *
 * Named once because it is used three ways: walked to find the shaders, stripped off their paths, and
 * handed to shadercross as `-I` so a shader can include a shared `.hlsli` from beside itself.
 * */
#define SHADER_SOURCE_DIRECTORY "./assets/shader/source"

/**
 * Compiles every HLSL shader under assets/shader/source into the per backend formats the asset
 * system picks between at runtime.
 * */
void nya_asset_compile_shaders(void);

/**
 * Regenerates assets/assets.h: one `NYA_ASSET_<PATH>` define per asset file, so asset handles are
 * compile time constants that a typo turns into a build error rather than a missing file at
 * runtime.
 * */
void nya_asset_index(void);

/**
 * Regenerates assets/assets.c, embedding every asset as a byte blob for the release build, where
 * NYA_ASSET_PREFER_BLOB reads assets out of the executable, falling back to disk for anything missing.
 * */
void nya_asset_bundle(void);
