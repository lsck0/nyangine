/**
 * @file pp/asset.h
 * */
#pragma once

#include "nyangine-core/nyangine.h"

// For SHADERCROSS_BINARY: the shader rules invoke the tool the vendor build produces, so the path belongs to whoever builds it. Spelling it out here is how it silently went stale when the vendor build directories were renamed.
#include "nyangine-build/vendor/vendor_sdl_shadercross.h"

/* FUNCTIONS AND MACROS */

/**
 * Where the hand written shaders live, and the include directory the compiler is given.
 * */
#define SHADER_SOURCE_DIRECTORY "./assets/shader/source"

/**
 * Where shadercross writes, one file per shader and format, named after the source. Everything in it is baked
 * into the release blob, so a file whose source is gone is removed rather than shipped.
 * */
#define SHADER_COMPILED_DIRECTORY "./assets/shader/compiled"

/**
 * A directory under ./assets/ that holds no assets: no code loads from it, so it is neither indexed nor baked into
 * the release blob. The icon set is 1383 SVGs kept for picking from. Remove the line once something loads one.
 * */
#define NYA_ASSET_UNUSED_DIRECTORY "./assets/icons/"

/**
 * Where the generated asset sources land.
 */
#define NYA_ASSET_INDEX_OUTPUT  "./src/genyarated/assets.h"
#define NYA_ASSET_BUNDLE_OUTPUT "./src/genyarated/assets.c"

/**
 * Optional at-rest obfuscation of the bundled blob. OFF by default: without NYA_ASSET_ENCRYPT_BLOB the
 * generated assets.c is byte for byte what it was, so an existing build is unchanged. Define it when
 * compiling the build tool to have nya_asset_bundle encrypt every entry with XChaCha20-Poly1305 under a
 * key derived from NYA_ASSET_ENCRYPT_KEY; core_asset.c then decrypts each entry transparently on load,
 * one entry at a time, following a flag the generator bakes into assets.c (so the engine needs no matching
 * flag of its own).
 *
 * Honest threat model: this is obfuscation, not DRM. The key is derived deterministically and ends up in
 * the shipped binary's .rodata, so it defeats a hex editor and casual extraction or swapping of assets, not
 * a determined attacker who reads the key out of the executable or single-steps the loader. The engine's
 * "encrypted or obfuscated" bar, no more.
 * */
#ifdef NYA_ASSET_ENCRYPT_BLOB
#ifndef NYA_ASSET_ENCRYPT_KEY
#define NYA_ASSET_ENCRYPT_KEY "nyangine-default-asset-obfuscation-key"
#endif
#endif

/**
 * Compiles every HLSL shader under assets/shader/source into the per backend formats the asset
 * system picks between at runtime.
 * */
void nya_asset_compile_shaders(void);

/**
 * Regenerates src/genyarated/assets.h: one `NYA_ASSET_<PATH>` define per asset file, so asset handles are
 * compile time constants that a typo turns into a build error rather than a missing file at
 * runtime.
 * */
void nya_asset_index(void);

/**
 * Regenerates src/genyarated/assets.c, embedding every asset as a byte blob for the release build, where
 * NYA_ASSET_PREFER_BLOB reads assets out of the executable, falling back to disk for anything missing.
 * */
void nya_asset_bundle(void);
