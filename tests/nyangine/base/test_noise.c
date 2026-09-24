/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <math.h>

#define EPSILON 0.001F

s32 main(void) {
  NYA_RNG   rng   = nya_rng_create(.seed = "A1B2C3D4E5F60789");
  NYA_Noise noise = nya_noise_create(&rng);

  // TEST: perlin1 returns values in [-1, 1] and is deterministic
  {
    f32 v1 = nya_noise_perlin1(&noise, 0.5F);
    f32 v2 = nya_noise_perlin1(&noise, 0.5F);
    nya_assert(v1 == v2, "perlin1 not deterministic");

    for (f32 x = -10.0F; x <= 10.0F; x += 0.13F) {
      f32 v = nya_noise_perlin1(&noise, x);
      nya_assert(v >= -1.0F && v <= 1.0F, "perlin1(%.2f) = %.4f out of range", x, v);
    }
  }

  // TEST: perlin2 returns values in [-1, 1] and varies spatially
  {
    f32 min_v = 1.0F, max_v = -1.0F;
    for (f32 x = -5.0F; x <= 5.0F; x += 0.37F) {
      for (f32 y = -5.0F; y <= 5.0F; y += 0.37F) {
        f32 v = nya_noise_perlin2(&noise, x, y);
        nya_assert(v >= -1.01F && v <= 1.01F, "perlin2(%.2f,%.2f) = %.4f", x, y, v);
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
      }
    }
    // should have some range, not constant
    nya_assert(max_v - min_v > 0.5F, "perlin2 range too narrow: [%.4f, %.4f]", min_v, max_v);
  }

  // TEST: perlin3 returns values in [-1, 1]
  {
    for (f32 x = -3.0F; x <= 3.0F; x += 0.7F) {
      for (f32 y = -3.0F; y <= 3.0F; y += 0.7F) {
        for (f32 z = -3.0F; z <= 3.0F; z += 0.7F) {
          f32 v = nya_noise_perlin3(&noise, x, y, z);
          nya_assert(v >= -1.5F && v <= 1.5F, "perlin3(%.1f,%.1f,%.1f) = %.4f", x, y, z, v);
        }
      }
    }
  }

  // TEST: perlin noise at integer coordinates is 0 (gradients cross zero)
  {
    for (s32 i = -5; i <= 5; i++) {
      f32 v = nya_noise_perlin1(&noise, (f32)i);
      nya_assert(fabsf(v) < EPSILON, "perlin1(%d) = %.4f, expected ~0", i, v);
    }
  }

  // TEST: simplex2 returns values in [-1, 1] and varies
  {
    f32 min_v = 1.0F, max_v = -1.0F;
    for (f32 x = -5.0F; x <= 5.0F; x += 0.31F) {
      for (f32 y = -5.0F; y <= 5.0F; y += 0.31F) {
        f32 v = nya_noise_simplex2(&noise, x, y);
        nya_assert(v >= -1.5F && v <= 1.5F, "simplex2(%.2f,%.2f) = %.4f", x, y, v);
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
      }
    }
    nya_assert(max_v - min_v > 0.3F, "simplex2 range too narrow: [%.4f, %.4f]", min_v, max_v);
  }

  // TEST: simplex3 returns finite values and varies
  {
    f32 min_v = 1.0F, max_v = -1.0F;
    for (f32 x = -2.0F; x <= 2.0F; x += 0.5F) {
      for (f32 y = -2.0F; y <= 2.0F; y += 0.5F) {
        for (f32 z = -2.0F; z <= 2.0F; z += 0.5F) {
          f32 v = nya_noise_simplex3(&noise, x, y, z);
          nya_assert(isfinite(v), "simplex3(%.1f,%.1f,%.1f) not finite", x, y, z);
          if (v < min_v) min_v = v;
          if (v > max_v) max_v = v;
        }
      }
    }
    nya_assert(max_v - min_v > 0.1F, "simplex3 range too narrow");
  }

  // TEST: value2 is deterministic and varies
  {
    f32 v1 = nya_noise_value2(&noise, 1.23F, 4.56F);
    f32 v2 = nya_noise_value2(&noise, 1.23F, 4.56F);
    nya_assert(v1 == v2, "value2 not deterministic");

    f32 v3 = nya_noise_value2(&noise, 7.89F, 0.12F);
    nya_assert(v1 != v3, "value2 same at different positions");
  }

  // TEST: value3 is deterministic
  {
    f32 v1 = nya_noise_value3(&noise, 1.0F, 2.0F, 3.0F);
    f32 v2 = nya_noise_value3(&noise, 1.0F, 2.0F, 3.0F);
    nya_assert(v1 == v2, "value3 not deterministic");
  }

  // TEST: FBM1 returns bounded values
  {
    NYA_NoiseParams params = { .octaves = 4, .lacunarity = 2.0F, .gain = 0.5F };
    for (f32 x = -10.0F; x <= 10.0F; x += 0.3F) {
      f32 v = nya_noise_fbm1(&noise, x, params);
      nya_assert(v >= -1.5F && v <= 1.5F, "fbm1(%.2f) = %.4f", x, v);
    }
  }

  // TEST: FBM2 with different octave counts produces different results
  {
    NYA_NoiseParams p1 = { .octaves = 1, .lacunarity = 2.0F, .gain = 0.5F };
    NYA_NoiseParams p6 = { .octaves = 6, .lacunarity = 2.0F, .gain = 0.5F };

    b8 any_differ = false;
    for (f32 x = 0.0F; x <= 5.0F; x += 0.1F) {
      f32 v1 = nya_noise_fbm2(&noise, x, 1.23F, p1);
      f32 v6 = nya_noise_fbm2(&noise, x, 1.23F, p6);
      if (fabsf(v1 - v6) > EPSILON) { any_differ = true; break; }
    }
    nya_assert(any_differ, "fbm2 with different octaves should produce different output");
  }

  // TEST: FBM3
  {
    NYA_NoiseParams params = { .octaves = 4, .lacunarity = 2.0F, .gain = 0.5F };
    f32 v = nya_noise_fbm3(&noise, 1.0F, 2.0F, 3.0F, params);
    nya_assert(isfinite(v), "fbm3 not finite");
    f32 v2 = nya_noise_fbm3(&noise, 1.0F, 2.0F, 3.0F, params);
    nya_assert(v == v2, "fbm3 not deterministic");
  }

  // TEST: remap01
  {
    nya_assert(fabsf(nya_noise_remap01(-1.0F) - 0.0F) < EPSILON);
    nya_assert(fabsf(nya_noise_remap01(0.0F)  - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_noise_remap01(1.0F)  - 1.0F) < EPSILON);
  }

  // TEST: ridged2 returns values in [0, 1]
  {
    NYA_NoiseParams params = { .octaves = 4, .lacunarity = 2.0F, .gain = 0.5F };
    for (f32 x = -5.0F; x <= 5.0F; x += 0.37F) {
      for (f32 y = -5.0F; y <= 5.0F; y += 0.37F) {
        f32 v = nya_noise_ridged2(&noise, x, y, params);
        nya_assert(v >= -0.1F && v <= 1.1F, "ridged2(%.2f,%.2f) = %.4f", x, y, v);
      }
    }
  }

  // TEST: turbulence2 returns values in [0, 1]
  {
    NYA_NoiseParams params = { .octaves = 4, .lacunarity = 2.0F, .gain = 0.5F };
    for (f32 x = -5.0F; x <= 5.0F; x += 0.37F) {
      for (f32 y = -5.0F; y <= 5.0F; y += 0.37F) {
        f32 v = nya_noise_turbulence2(&noise, x, y, params);
        nya_assert(v >= -0.1F && v <= 1.1F, "turbulence2(%.2f,%.2f) = %.4f", x, y, v);
      }
    }
  }

  // TEST: noise is continuous (nearby inputs → nearby outputs)
  {
    f32 step = 0.001F;
    for (f32 x = 0.0F; x < 5.0F; x += 0.1F) {
      f32 v1 = nya_noise_perlin2(&noise, x, 0.0F);
      f32 v2 = nya_noise_perlin2(&noise, x + step, 0.0F);
      f32 diff = fabsf(v2 - v1);
      nya_assert(diff < 0.1F, "perlin2 discontinuity at x=%.3f: diff=%.6f", x, diff);
    }
  }

  // TEST: same seed produces identical noise patterns
  {
    NYA_RNG   rng_a   = nya_rng_create(.seed = "DEADBEEF01234567");
    NYA_Noise noise_a = nya_noise_create(&rng_a);
    NYA_RNG   rng_b   = nya_rng_create(.seed = "DEADBEEF01234567");
    NYA_Noise noise_b = nya_noise_create(&rng_b);

    for (f32 x = -3.0F; x <= 3.0F; x += 0.5F) {
      for (f32 y = -3.0F; y <= 3.0F; y += 0.5F) {
        f32 pa = nya_noise_perlin2(&noise_a, x, y);
        f32 pb = nya_noise_perlin2(&noise_b, x, y);
        nya_assert(pa == pb, "same seed: perlin2 mismatch at (%.1f, %.1f)", x, y);

        f32 sa = nya_noise_simplex2(&noise_a, x, y);
        f32 sb = nya_noise_simplex2(&noise_b, x, y);
        nya_assert(sa == sb, "same seed: simplex2 mismatch at (%.1f, %.1f)", x, y);

        f32 va = nya_noise_value2(&noise_a, x, y);
        f32 vb = nya_noise_value2(&noise_b, x, y);
        nya_assert(va == vb, "same seed: value2 mismatch at (%.1f, %.1f)", x, y);
      }
    }
  }

  // TEST: different seeds produce different noise patterns
  {
    NYA_RNG   rng_a   = nya_rng_create(.seed = "AAAA1111BBBB2222");
    NYA_Noise noise_a = nya_noise_create(&rng_a);
    NYA_RNG   rng_b   = nya_rng_create(.seed = "CCCC3333DDDD4444");
    NYA_Noise noise_b = nya_noise_create(&rng_b);

    b8 perlin_differ  = false;
    b8 simplex_differ = false;
    b8 value_differ   = false;
    for (f32 x = -3.0F; x <= 3.0F; x += 0.5F) {
      for (f32 y = -3.0F; y <= 3.0F; y += 0.5F) {
        if (!perlin_differ && fabsf(nya_noise_perlin2(&noise_a, x, y) - nya_noise_perlin2(&noise_b, x, y)) > EPSILON) perlin_differ = true;
        if (!simplex_differ && fabsf(nya_noise_simplex2(&noise_a, x, y) - nya_noise_simplex2(&noise_b, x, y)) > EPSILON) simplex_differ = true;
        if (!value_differ && fabsf(nya_noise_value2(&noise_a, x, y) - nya_noise_value2(&noise_b, x, y)) > EPSILON) value_differ = true;
      }
    }
    nya_assert(perlin_differ, "different seeds should produce different perlin2 patterns");
    nya_assert(simplex_differ, "different seeds should produce different simplex2 patterns");
    nya_assert(value_differ, "different seeds should produce different value2 patterns");
  }

  printf("PASSED: test_noise\n");
  return 0;
}
