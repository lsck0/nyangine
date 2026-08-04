/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: RNG creation with various seeds
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG rng1 = nya_rng_create();
  NYA_RNG rng2 = nya_rng_create(.seed = rng1.seed);
  NYA_RNG rng3 = nya_rng_create(.seed = "BEEF");
  NYA_RNG rng4 = nya_rng_create(.seed = "DEADBEEF");
  NYA_RNG rng5 = nya_rng_create(.seed = "0123456789ABCDEF");
  NYA_RNG rng6 = nya_rng_create(.seed = "AABBCCDD");
  (void)rng3;
  (void)rng4;
  (void)rng5;
  (void)rng6;

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: RNG creation with invalid seeds should panic
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash({
    NYA_RNG bad_rng = nya_rng_create(.seed = "BEGgEFZ");
    (void)bad_rng;
  });
  nya_expect_crash({
    NYA_RNG bad_rng = nya_rng_create(.seed = "AAasd786hgfasPOAISDnAAAAAAAAAAAaafsdsdsAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    (void)bad_rng;
  });
  nya_expect_crash({
    NYA_RNG bad_rng = nya_rng_create(.seed = "lowercase");
    (void)bad_rng;
  });

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Distribution definitions
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNGDistribution dist_uniform = {
    .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
    .uniform = { .min = 10.0, .max = 20.0 },
  };
  NYA_RNGDistribution dist_normal = {
    .type   = NYA_RNG_DISTRIBUTION_NORMAL,
    .normal = { .mean = 50.0, .stddev = 10.0 },
  };
  NYA_RNGDistribution dist_exponential = {
    .type        = NYA_RNG_DISTRIBUTION_EXPONENTIAL,
    .exponential = { .lambda = 0.5 },
  };
  NYA_RNGDistribution dist_poisson = {
    .type    = NYA_RNG_DISTRIBUTION_POISSON,
    .poisson = { .lambda = 4.0 },
  };
  NYA_RNGDistribution dist_binomial = {
    .type     = NYA_RNG_DISTRIBUTION_BINOMIAL,
    .binomial = { .n = 10, .p = 0.5 },
  };
  NYA_RNGDistribution dist_geometric = {
    .type      = NYA_RNG_DISTRIBUTION_GEOMETRIC,
    .geometric = { .p = 0.3 },
  };

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Same seed produces same sequence
  // ─────────────────────────────────────────────────────────────────────────────
  for (u32 counter = 0; counter < 1000; ++counter) {
    nya_assert(nya_rng_sample_f64(&rng1, dist_uniform) == nya_rng_sample_f64(&rng2, dist_uniform));
    nya_assert(nya_rng_sample_f64(&rng1, dist_normal) == nya_rng_sample_f64(&rng2, dist_normal));
    nya_assert(nya_rng_sample_f64(&rng1, dist_exponential) == nya_rng_sample_f64(&rng2, dist_exponential));
    nya_assert(nya_rng_sample_u32(&rng1, dist_poisson) == nya_rng_sample_u32(&rng2, dist_poisson));
    nya_assert(nya_rng_sample_u32(&rng1, dist_binomial) == nya_rng_sample_u32(&rng2, dist_binomial));
    nya_assert(nya_rng_sample_u32(&rng1, dist_geometric) == nya_rng_sample_u32(&rng2, dist_geometric));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Uniform distribution produces values in range
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG             range_rng  = nya_rng_create(.seed = "ABCD1234");
  NYA_RNGDistribution range_dist = {
    .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
    .uniform = { .min = 0.0, .max = 100.0 },
  };
  for (u32 i = 0; i < 10000; ++i) {
    f64 val = nya_rng_sample_f64(&range_rng, range_dist);
    nya_assert(val >= 0.0 && val <= 100.0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Uniform integer distribution
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNGDistribution int_dist = {
    .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
    .uniform = { .min = 0, .max = 255 },
  };
  for (u32 i = 0; i < 1000; ++i) {
    u8 val = nya_rng_sample_u8(&range_rng, int_dist);
    nya_assert(val <= 255);
  }
  for (u32 i = 0; i < 1000; ++i) {
    u16 val = nya_rng_sample_u16(&range_rng, int_dist);
    nya_assert(val <= 255);
  }
  for (u32 i = 0; i < 1000; ++i) {
    u32 val = nya_rng_sample_u32(&range_rng, int_dist);
    nya_assert(val <= 255);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Signed integer sampling
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNGDistribution signed_dist = {
    .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
    .uniform = { .min = -50, .max = 50 },
  };
  for (u32 i = 0; i < 1000; ++i) {
    s32 val = nya_rng_sample_s32(&range_rng, signed_dist);
    nya_assert(val >= -50 && val <= 50);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Boolean generation
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG bool_rng   = nya_rng_create(.seed = "1234ABCD");
  u32     true_count = 0;
  u32     total      = 10000;
  for (u32 i = 0; i < total; ++i) {
    if (nya_rng_gen_bool(&bool_rng, 0.5F)) { true_count++; }
  }
  // With 50% chance, expect roughly half true (allow ±5% margin)
  nya_assert(true_count > total * 0.45 && true_count < total * 0.55);

  // Test extreme probabilities
  NYA_RNG extreme_rng        = nya_rng_create(.seed = "EEEE");
  u32     always_true_count  = 0;
  u32     always_false_count = 0;
  for (u32 i = 0; i < 100; ++i) {
    if (nya_rng_gen_bool(&extreme_rng, 1.0F)) always_true_count++;
    if (!nya_rng_gen_bool(&extreme_rng, 0.0F)) always_false_count++;
  }
  nya_assert(always_true_count == 100);
  nya_assert(always_false_count == 100);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Byte buffer generation
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG byte_rng    = nya_rng_create(.seed = "B0E0C123");
  u8      buffer1[64] = { 0 };
  u8      buffer2[64] = { 0 };
  nya_rng_gen_bytes(&byte_rng, buffer1, 64);
  // Verify buffer was filled (extremely unlikely to be all zeros)
  b8 has_nonzero = false;
  for (u32 i = 0; i < 64; ++i) {
    if (buffer1[i] != 0) {
      has_nonzero = true;
      break;
    }
  }
  nya_assert(has_nonzero);

  // Generate another buffer, should be different
  nya_rng_gen_bytes(&byte_rng, buffer2, 64);
  b8 buffers_differ = false;
  for (u32 i = 0; i < 64; ++i) {
    if (buffer1[i] != buffer2[i]) {
      buffers_differ = true;
      break;
    }
  }
  nya_assert(buffers_differ);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Different seeds produce different sequences
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG             diff_rng1   = nya_rng_create(.seed = "AAAA");
  NYA_RNG             diff_rng2   = nya_rng_create(.seed = "BBBB");
  NYA_RNGDistribution simple_dist = {
    .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
    .uniform = { .min = 0, .max = 1000000 },
  };
  b8 found_difference = false;
  for (u32 i = 0; i < 100; ++i) {
    if (nya_rng_sample_u32(&diff_rng1, simple_dist) != nya_rng_sample_u32(&diff_rng2, simple_dist)) {
      found_difference = true;
      break;
    }
  }
  nya_assert(found_difference);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Binomial distribution bounds
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG             binom_rng  = nya_rng_create(.seed = "B1A0CD00");
  NYA_RNGDistribution binom_dist = {
    .type     = NYA_RNG_DISTRIBUTION_BINOMIAL,
    .binomial = { .n = 20, .p = 0.5 },
  };
  for (u32 i = 0; i < 1000; ++i) {
    u32 val = nya_rng_sample_u32(&binom_rng, binom_dist);
    nya_assert(val <= 20);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Exponential distribution is non-negative
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG             exp_rng  = nya_rng_create(.seed = "E0F00000");
  NYA_RNGDistribution exp_dist = {
    .type        = NYA_RNG_DISTRIBUTION_EXPONENTIAL,
    .exponential = { .lambda = 1.0 },
  };
  for (u32 i = 0; i < 1000; ++i) {
    f64 val = nya_rng_sample_f64(&exp_rng, exp_dist);
    nya_assert(val >= 0.0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Geometric distribution produces positive integers
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG             geo_rng  = nya_rng_create(.seed = "AE0E00AA");
  NYA_RNGDistribution geo_dist = {
    .type      = NYA_RNG_DISTRIBUTION_GEOMETRIC,
    .geometric = { .p = 0.5 },
  };
  for (u32 i = 0; i < 1000; ++i) {
    u32 val = nya_rng_sample_u32(&geo_rng, geo_dist);
    nya_assert(val >= 1);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Float types sampling
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_RNG             float_rng  = nya_rng_create(.seed = "F10A0000");
  NYA_RNGDistribution float_dist = {
    .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
    .uniform = { .min = -1.0, .max = 1.0 },
  };
  for (u32 i = 0; i < 100; ++i) {
    f32 val32 = nya_rng_sample_f32(&float_rng, float_dist);
    nya_assert(val32 >= -1.0F && val32 <= 1.0F);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Normal distribution produces finite values (no NaN/inf from log(0))
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_RNG             rng  = nya_rng_create(.seed = "FFFFFFFFFFFFFFFF");
    NYA_RNGDistribution dist = {
      .type   = NYA_RNG_DISTRIBUTION_NORMAL,
      .normal = { .mean = 0.0, .stddev = 1.0 },
    };
    for (u32 i = 0; i < 100000; ++i) {
      f64 val = nya_rng_sample_f64(&rng, dist);
      nya_assert(!isnan(val), "Normal distribution produced NaN");
      nya_assert(!isinf(val), "Normal distribution produced infinity");
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Exponential distribution produces finite values (no NaN/inf)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_RNG             rng  = nya_rng_create(.seed = "FFFFFFFFFFFFFFFF");
    NYA_RNGDistribution dist = {
      .type        = NYA_RNG_DISTRIBUTION_EXPONENTIAL,
      .exponential = { .lambda = 1.0 },
    };
    for (u32 i = 0; i < 100000; ++i) {
      f64 val = nya_rng_sample_f64(&rng, dist);
      nya_assert(!isnan(val), "Exponential distribution produced NaN");
      nya_assert(!isinf(val), "Exponential distribution produced infinity");
      nya_assert(val >= 0.0);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Geometric distribution produces finite values (no NaN/inf)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_RNG             rng  = nya_rng_create(.seed = "FFFFFFFFFFFFFFFF");
    NYA_RNGDistribution dist = {
      .type      = NYA_RNG_DISTRIBUTION_GEOMETRIC,
      .geometric = { .p = 0.5 },
    };
    for (u32 i = 0; i < 100000; ++i) {
      f64 val = nya_rng_sample_f64(&rng, dist);
      nya_assert(!isnan(val), "Geometric distribution produced NaN");
      nya_assert(!isinf(val), "Geometric distribution produced infinity");
      nya_assert(val >= 1.0);
    }
  }

  return 0;
}
