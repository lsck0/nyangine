/**
 * @file math_random.h
 *
 * Base PRNG used: https://espadrine.github.io/blog/posts/shishua-the-fastest-prng-in-the-world.html
 *
 * Example:
 *
 * ```c
 * NYA_RNG rng = nya_rng_create();
 *
 * s32 random_value = nya_rng_sample_s32(
 *     &rng,
 *     (NYA_RNGDistribution){
 *         .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
 *         .uniform = { .min = 0.0F, .max = 100.0F },
 *     }
 * );
 * ```
 *
 * ```c
 * NYA_RNG rng = nya_rng_create(.seed = "DEADBEEF6767");
 *
 * f32 random_value = nya_rng_sample_f32(
 *     &rng,
 *     (NYA_RNGDistribution){
 *         .type    = NYA_RNG_DISTRIBUTION_NORMAL,
 *         .normal = { .mean = 0.0F, .stddev = 100.0F },
 *     }
 * );
 *
 * if (nya_rng_gen_bool(&rng, 0.1F)) {
 *    // 10% chance
 *    // ...
 * }
 * ```
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_RNGDistributionType NYA_RNGDistributionType;
typedef struct NYA_RNG               NYA_RNG;
typedef struct NYA_RNGOptions        NYA_RNGOptions;
typedef struct NYA_RNGDistribution   NYA_RNGDistribution;

#define _NYA_RNG_BUFFER_SIZE     1024
#define _NYA_RNG_INIT_ROUNDS     16
#define _NYA_RNG_MAX_SEED_LENGTH 64
#define _NYA_RNG_DEFAULT_OPTIONS .seed = nullptr

struct NYA_RNGOptions {
    /** upto 64 char hex string, NULL => random seed */
    const char* seed;
};

struct NYA_RNG {
    char seed[_NYA_RNG_MAX_SEED_LENGTH + 1];

    __m256i state[4];
    __m256i output[4];
    __m256i counter;

    u8  buffer[_NYA_RNG_BUFFER_SIZE];
    u64 cursor;
};

enum NYA_RNGDistributionType {
    NYA_RNG_DISTRIBUTION_UNIFORM,
    NYA_RNG_DISTRIBUTION_NORMAL,
    NYA_RNG_DISTRIBUTION_EXPONENTIAL,
    NYA_RNG_DISTRIBUTION_POISSON,
    NYA_RNG_DISTRIBUTION_BINOMIAL,
    NYA_RNG_DISTRIBUTION_GEOMETRIC,
    NYA_RNG_DISTRIBUTION_COUNT,
};

struct NYA_RNGDistribution {
    NYA_RNGDistributionType type;

    union {
        struct {
            f64 min;
            f64 max;
        } uniform;

        struct {
            f64 mean;
            f64 stddev;
        } normal;

        struct {
            f64 lambda;
        } exponential;

        struct {
            f64 lambda;
        } poisson;

        struct {
            u64 n;
            f64 p;
        } binomial;

        struct {
            f64 p;
        } geometric;
    };
};

static_assert(_NYA_RNG_BUFFER_SIZE % 128 == 0, "RNG buffer size must be a multiple of 128 bytes.");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_rng_create(...) nya_rng_create_with_options((NYA_RNGOptions){ _NYA_RNG_DEFAULT_OPTIONS, __VA_ARGS__ })
NYA_API NYA_RNG nya_rng_create_with_options(NYA_RNGOptions options);

NYA_API void nya_rng_gen_bytes(NYA_RNG* rng, u8 buffer[], u64 size);
NYA_API b8   nya_rng_gen_bool(NYA_RNG* rng, f32 true_chance);
NYA_API u8   nya_rng_sample_u8(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API u16  nya_rng_sample_u16(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API u32  nya_rng_sample_u32(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API u64  nya_rng_sample_u64(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API s8   nya_rng_sample_s8(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API s16  nya_rng_sample_s16(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API s32  nya_rng_sample_s32(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API s64  nya_rng_sample_s64(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API f16  nya_rng_sample_f16(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API f32  nya_rng_sample_f32(NYA_RNG* rng, NYA_RNGDistribution distribution);
NYA_API f64  nya_rng_sample_f64(NYA_RNG* rng, NYA_RNGDistribution distribution);
