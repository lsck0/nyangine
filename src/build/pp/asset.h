/**
 * @file pp/asset.h
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
 * */
#define SHADER_SOURCE_DIRECTORY "./assets/shader/source"

/**
 * Where the generated asset sources land.
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
