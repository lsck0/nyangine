/**
 * @file asset.h
 *
 * Asset pipeline: shader compilation, the generated asset index, and the embedded asset blob.
 *
 * These are plain functions rather than build hooks on purpose. The build system calls them
 * through thin wrappers in build/hooks/hooks.h, which keeps what the pipeline does separate from
 * when the build system happens to run it, and means they can be driven directly from a command or
 * a test without inventing an NYA_BuildRule to hang them off.
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
