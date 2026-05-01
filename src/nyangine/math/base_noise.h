/**
 * @file base_noise.h
 *
 * Coherent noise functions for procedural generation: Perlin, simplex, value noise,
 * and fractal Brownian motion (FBM) layering. All functions return values in [-1, 1].
 *
 * Noise is seeded via an NYA_RNG, so the same seed always produces the same pattern.
 *
 * Example:
 * ```c
 * NYA_RNG   rng   = nya_rng_create(.seed = "my_world_seed");
 * NYA_Noise noise = nya_noise_create(&rng);
 *
 * // basic 2D Perlin noise
 * f32 n = nya_noise_perlin2(&noise, x * 0.01F, y * 0.01F);
 *
 * // FBM with 6 octaves for terrain
 * f32 height = nya_noise_fbm2(&noise, x * 0.005F, y * 0.005F, (NYA_NoiseParams){
 *     .octaves    = 6,
 *     .lacunarity = 2.0F,
 *     .gain       = 0.5F,
 * });
 *
 * // remap to [0, 1]
 * f32 normalized = nya_noise_remap01(height);
 * ```
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_random.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NoiseParams NYA_NoiseParams;
typedef struct NYA_Noise       NYA_Noise;

struct NYA_Noise {
  u8 perm[512];
};

/**
 * NYA_NoiseParams
 *
 * Parameters for fractal noise (FBM).
 * */
struct NYA_NoiseParams {
  /** Number of layers. Default: 4 */
  u32 octaves;

  /** Frequency multiplier per octave. Default: 2.0 */
  f32 lacunarity;

  /** Amplitude multiplier per octave. Default: 0.5 */
  f32 gain;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN NYA_Noise nya_noise_create(NYA_RNG* rng);

NYA_API NYA_EXTERN f32 nya_noise_perlin1(NYA_Noise* noise, f32 x);
NYA_API NYA_EXTERN f32 nya_noise_perlin2(NYA_Noise* noise, f32 x, f32 y);
NYA_API NYA_EXTERN f32 nya_noise_perlin3(NYA_Noise* noise, f32 x, f32 y, f32 z);

NYA_API NYA_EXTERN f32 nya_noise_simplex2(NYA_Noise* noise, f32 x, f32 y);
NYA_API NYA_EXTERN f32 nya_noise_simplex3(NYA_Noise* noise, f32 x, f32 y, f32 z);

NYA_API NYA_EXTERN f32 nya_noise_value2(NYA_Noise* noise, f32 x, f32 y);
NYA_API NYA_EXTERN f32 nya_noise_value3(NYA_Noise* noise, f32 x, f32 y, f32 z);

NYA_API NYA_EXTERN f32 nya_noise_fbm1(NYA_Noise* noise, f32 x, NYA_NoiseParams params);
NYA_API NYA_EXTERN f32 nya_noise_fbm2(NYA_Noise* noise, f32 x, f32 y, NYA_NoiseParams params);
NYA_API NYA_EXTERN f32 nya_noise_fbm3(NYA_Noise* noise, f32 x, f32 y, f32 z, NYA_NoiseParams params);

NYA_API NYA_EXTERN f32 nya_noise_ridged2(NYA_Noise* noise, f32 x, f32 y, NYA_NoiseParams params);

NYA_API NYA_EXTERN f32 nya_noise_turbulence2(NYA_Noise* noise, f32 x, f32 y, NYA_NoiseParams params);

NYA_API NYA_EXTERN f32 nya_noise_remap01(f32 value);
