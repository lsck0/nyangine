/**
 * @file asset_rules.h
 * */
#pragma once

#include "nyangine/nyangine.h"

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
 */

NYA_INTERNAL NYA_BuildRule build_shaders = {
    .name            = "build_shaders",
    .is_metarule     = true,
    .pre_build_hooks = { &hook_compile_shaders, },
    .vendors         = { SHADERCROSS_HOST_VENDOR },
};

/**
 * Generates src/generated/strings.h from the locale files.
 * */
NYA_INTERNAL NYA_BuildRule generate_strings = {
    .name             = "generate_strings",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_strings, },
};

/**
 * Regenerates the reflection tables from the @reflect annotations in the tree.
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
    .dependencies     = { &build_shaders, &generate_strings, &generate_reflection, },
    .post_build_hooks = { &hook_index_assets, },
};

/*
 * Depends on index_assets, and the order is the point.
 */
NYA_INTERNAL NYA_BuildRule bundle_assets = {
    .name             = "bundle_assets",
    .is_metarule      = true,
    .dependencies     = { &build_shaders, &index_assets, },
    .post_build_hooks = { &hook_bundle_assets, },
};

