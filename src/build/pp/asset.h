/**
 * @file pp/asset.h
 *
 * Asset pipeline: shader compilation, the generated asset index, and the embedded asset blob.
 *
 * These are plain functions rather than build hooks on purpose. The build system calls them
 * through thin wrappers in build/hooks.h, which keeps what the pipeline does separate from
 * when the build system happens to run it, and means they can be driven directly from a command or
 * a test without inventing an NYA_BuildRule to hang them off.
 *
 * The NYA_BuildRule graph that schedules these lives in build/asset_rules.h. The two were one file
 * until the preprocessor moved under build/pp/: what the pipeline *does* is preprocessing, and when
 * the build system runs it is build orchestration, and only the latter needs to know about rules.
 * */
#pragma once

#include "nyangine/nyangine.h"

// For SHADERCROSS_BINARY: the shader rules invoke the tool the vendor build produces, so the path
// belongs to whoever builds it. Spelling it out here is how it silently went stale when the vendor
// build directories were renamed.
#include "build/vendor/vendor_sdl_shadercross.h"

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
 * Where the generated asset sources land.
 *
 * In src/generated/ with every other generated file rather than beside the assets they describe:
 * these are C, they are compiled, and a reader looking for generated source should find all of it in
 * one directory. The index is committed so a fresh clone builds; the blob is not, because it is the
 * whole asset tree as a byte array.
 */
#define NYA_ASSET_INDEX_OUTPUT  "./src/generated/assets.h"
#define NYA_ASSET_BUNDLE_OUTPUT "./src/generated/assets.c"

/**
 * Compiles every HLSL shader under assets/shader/source into the per backend formats the asset
 * system picks between at runtime.
 * */
void nya_asset_compile_shaders(void);

/**
 * Regenerates src/generated/assets.h: one `NYA_ASSET_<PATH>` define per asset file, so asset handles are
 * compile time constants that a typo turns into a build error rather than a missing file at
 * runtime.
 * */
void nya_asset_index(void);

/**
 * Regenerates src/generated/assets.c, embedding every asset as a byte blob for the release build, where
 * NYA_ASSET_PREFER_BLOB reads assets out of the executable, falling back to disk for anything missing.
 * */
void nya_asset_bundle(void);
