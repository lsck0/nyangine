/**
 * @file pp/pp.h
 *
 * The preprocessor: everything that turns files on disk into generated C before anything compiles,
 * and the build rules that run it.
 *
 * The passes themselves live one per file below. The rules at the bottom are metarules with no
 * command of their own; they exist so the rest of the build system can depend on a pass by name and
 * get the ordering that comes with it. Shaders compile, locales become strings.h, the @reflect
 * annotations become reflection.{h,c}, the asset tree becomes assets.h, and only then does the blob
 * get bundled, and the nya_lambda bodies become the companion headers their own sources include. They
 * live here rather than beside the project rules because the hooks they hang off are the ones declared
 * in this directory.
 * */
#pragma once

#include "nyangine/nyangine.h"

// For the hooks the rules below hang the pipeline off.
#include "build/hooks.h"

// First: every pass below opens by calling it.
#include "build/pp/stale.h"
/**/
#include "build/pp/asset.h"
#include "build/pp/cheatsheet.h"
#include "build/pp/i18n.h"
#include "build/pp/lambda.h"
#include "build/pp/luabind.h"
#include "build/pp/reflection.h"
#include "build/pp/watch.h"

/* BUILD RULES */

NYA_INTERNAL NYA_BuildRule build_shaders = {
    .name            = "build_shaders",
    .is_metarule     = true,
    .pre_build_hooks = { &hook_compile_shaders, },
    .vendors         = { SHADERCROSS_HOST_VENDOR },
};

/**
 * Generates src/genyarated/strings.h from the locale files.
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

/**
 * Regenerates docs/CHEATSHEET.md from the public headers.
 * */
NYA_INTERNAL NYA_BuildRule generate_cheatsheet = {
    .name             = "generate_cheatsheet",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_cheatsheet, },
};

/**
 * Regenerates src/genyarated/lua_bindings.c and docs/lua/nya.lua from the @lua annotations.
 * */
NYA_INTERNAL NYA_BuildRule generate_lua_bindings = {
    .name             = "generate_lua_bindings",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_lua_bindings, },
};

/**
 * Regenerates the lambda companions from the nya_lambda call sites in the tree.
 * */
NYA_INTERNAL NYA_BuildRule generate_lambdas = {
    .name             = "generate_lambdas",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_lambdas, },
};

/**
 * Regenerates the watch registrations from the @watch annotations in the tree.
 * */
NYA_INTERNAL NYA_BuildRule generate_watches = {
    .name             = "generate_watches",
    .policy           = NYA_BUILD_ALWAYS,
    .is_metarule      = true,
    .post_build_hooks = { &hook_generate_watches, },
};

NYA_INTERNAL NYA_BuildRule index_assets = {
    .name             = "index_assets",
    .is_metarule      = true,
    // generate_reflection is here for the same reason generate_strings is: it writes *source* that the compile rules then consume, and everything that compiles depends on this rule. It has nothing to do with indexing assets beyond that shared ordering requirement. generate_lua_bindings is here for that same reason and no other: it writes the C that lua_engine.c includes, so an annotation added to a header and the binding it produces have to land in one build. generate_cheatsheet writes no source at all, and hangs here so that a header edit and the reference to it land in the same build. A document that regenerates only when asked is a document that is wrong by the time anyone asks. generate_lambdas is here for the reason generate_lua_bindings is, and more sharply: it writes the headers the sources it read include, so a body and the function it becomes cannot land in different builds. generate_watches writes the same kind of header for the same reason: a local added to a watched function and the line that registers it are one edit.
    .dependencies     = { &build_shaders,        &generate_strings,   &generate_reflection, &generate_lua_bindings,
                          &generate_cheatsheet,  &generate_lambdas,   &generate_watches, },
    .post_build_hooks = { &hook_index_assets, },
};

/* Depends on index_assets, and the order is the point. */
NYA_INTERNAL NYA_BuildRule bundle_assets = {
    .name             = "bundle_assets",
    .is_metarule      = true,
    .dependencies     = { &build_shaders, &index_assets, },
    .post_build_hooks = { &hook_bundle_assets, },
};
