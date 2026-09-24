/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <math.h>

#define EPSILON 0.001F

NYA_INTERNAL void assert_bounds(f32 val, f32 lo, f32 hi, const char* name, f32 t) {
  nya_assert(val >= lo && val <= hi, "%s(%.3f) = %.6f, expected [%.3f, %.3f]", name, t, val, lo, hi);
}

s32 main(void) {
  // TEST: all easings return 0 at t=0 and 1 at t=1
  {
    for (s32 i = 0; i < NYA_EASE_COUNT; i++) {
      f32 at0 = nya_ease((NYA_EaseType)i, 0.0F);
      f32 at1 = nya_ease((NYA_EaseType)i, 1.0F);
      nya_assert(fabsf(at0) < EPSILON, "ease(%d, 0) = %.6f", i, at0);
      nya_assert(fabsf(at1 - 1.0F) < EPSILON, "ease(%d, 1) = %.6f", i, at1);
    }
  }

  // TEST: linear is identity
  {
    for (f32 t = 0.0F; t <= 1.0F; t += 0.1F) {
      nya_assert(fabsf(nya_ease(NYA_EASE_LINEAR, t) - t) < EPSILON);
    }
  }

  // TEST: quad_in is convex (below linear for 0 < t < 1)
  {
    for (f32 t = 0.1F; t < 1.0F; t += 0.1F) {
      nya_assert(nya_ease(NYA_EASE_QUAD_IN, t) < t, "quad_in(%.1f) should be < t", t);
    }
  }

  // TEST: quad_out is concave (above linear for 0 < t < 1)
  {
    for (f32 t = 0.1F; t < 1.0F; t += 0.1F) {
      nya_assert(nya_ease(NYA_EASE_QUAD_OUT, t) > t, "quad_out(%.1f) should be > t", t);
    }
  }

  // TEST: in_out symmetry: f(0.5) ≈ 0.5 for polynomial easings
  {
    nya_assert(fabsf(nya_ease(NYA_EASE_QUAD_IN_OUT, 0.5F) - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_CUBIC_IN_OUT, 0.5F) - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_QUART_IN_OUT, 0.5F) - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_QUINT_IN_OUT, 0.5F) - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_SINE_IN_OUT, 0.5F) - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_EXPO_IN_OUT, 0.5F) - 0.5F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_CIRC_IN_OUT, 0.5F) - 0.5F) < EPSILON);
  }

  // TEST: sine easing known values
  {
    nya_assert(fabsf(nya_ease(NYA_EASE_SINE_IN, 1.0F) - 1.0F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_SINE_OUT, 0.0F) - 0.0F) < EPSILON);
    // sine_in at 0.5 = 1 - cos(π/4) ≈ 0.2929
    nya_assert(fabsf(nya_ease(NYA_EASE_SINE_IN, 0.5F) - 0.2929F) < 0.01F);
  }

  // TEST: expo edges
  {
    nya_assert(nya_ease(NYA_EASE_EXPO_IN, 0.0F) == 0.0F);
    nya_assert(nya_ease(NYA_EASE_EXPO_OUT, 1.0F) == 1.0F);
    nya_assert(nya_ease(NYA_EASE_EXPO_IN_OUT, 0.0F) == 0.0F);
    nya_assert(nya_ease(NYA_EASE_EXPO_IN_OUT, 1.0F) == 1.0F);
  }

  // TEST: back overshoots (goes below 0 or above 1)
  {
    // back_in goes negative near start
    nya_assert(nya_ease(NYA_EASE_BACK_IN, 0.1F) < 0.0F);
    // back_out goes above 1 near end
    nya_assert(nya_ease(NYA_EASE_BACK_OUT, 0.9F) > 1.0F);
  }

  // TEST: elastic overshoots
  {
    // elastic_out overshoots above 1 somewhere in 0..1
    b8 found_overshoot = false;
    for (f32 t = 0.01F; t < 1.0F; t += 0.01F) {
      if (nya_ease(NYA_EASE_ELASTIC_OUT, t) > 1.0F) { found_overshoot = true; break; }
    }
    nya_assert(found_overshoot, "elastic_out should overshoot above 1");
  }

  // TEST: bounce_out stays in [0, 1]
  {
    for (f32 t = 0.0F; t <= 1.0F; t += 0.01F) {
      f32 v = nya_ease(NYA_EASE_BOUNCE_OUT, t);
      assert_bounds(v, 0.0F, 1.0F, "bounce_out", t);
    }
  }

  // TEST: bounce_in stays in [0, 1]
  {
    for (f32 t = 0.0F; t <= 1.0F; t += 0.01F) {
      f32 v = nya_ease(NYA_EASE_BOUNCE_IN, t);
      assert_bounds(v, 0.0F, 1.0F, "bounce_in", t);
    }
  }

  // TEST: cubic bézier linear (0.0, 0.0, 1.0, 1.0) ≈ identity
  {
    for (f32 t = 0.0F; t <= 1.0F; t += 0.1F) {
      f32 v = nya_ease_bezier(0.0F, 0.0F, 1.0F, 1.0F, t);
      nya_assert(fabsf(v - t) < 0.02F, "bezier linear(%.1f) = %.4f", t, v);
    }
  }

  // TEST: cubic bézier CSS ease (0.25, 0.1, 0.25, 1.0)
  {
    f32 v0 = nya_ease_bezier(0.25F, 0.1F, 0.25F, 1.0F, 0.0F);
    f32 v1 = nya_ease_bezier(0.25F, 0.1F, 0.25F, 1.0F, 1.0F);
    nya_assert(fabsf(v0) < EPSILON);
    nya_assert(fabsf(v1 - 1.0F) < EPSILON);

    // CSS ease at t=0.5 should be > 0.5 (it's an ease-out-ish curve)
    f32 mid = nya_ease_bezier(0.25F, 0.1F, 0.25F, 1.0F, 0.5F);
    nya_assert(mid > 0.5F, "CSS ease at 0.5 = %.4f", mid);
  }

  // TEST: smoothstep known values
  {
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHSTEP, 0.0F)) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHSTEP, 1.0F) - 1.0F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHSTEP, 0.5F) - 0.5F) < EPSILON);
    // smoothstep(0.25) = 3*(0.0625) - 2*(0.015625) = 0.15625
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHSTEP, 0.25F) - 0.15625F) < EPSILON);
  }

  // TEST: smootherstep known values
  {
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHERSTEP, 0.0F)) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHERSTEP, 1.0F) - 1.0F) < EPSILON);
    nya_assert(fabsf(nya_ease(NYA_EASE_SMOOTHERSTEP, 0.5F) - 0.5F) < EPSILON);
  }

  // TEST: spring converges to 1 for large t
  {
    f32 v = nya_ease_spring(10.0F, 4.0F, 3.0F);
    nya_assert(fabsf(v - 1.0F) < 0.01F, "spring(10) = %.4f, expected ~1", v);
  }

  // TEST: spring oscillates early
  {
    b8 above = false;
    for (f32 t = 0.05F; t < 2.0F; t += 0.05F) {
      f32 v = nya_ease_spring(t, 6.0F, 2.0F);
      if (v > 1.05F) above = true;
    }
    nya_assert(above, "spring should overshoot above 1");
  }

  // TEST: dispatch table covers all enum values
  {
    for (s32 i = 0; i < NYA_EASE_COUNT; i++) {
      f32 v = nya_ease((NYA_EaseType)i, 0.5F);
      // just verify it doesn't crash and returns finite
      nya_assert(isfinite(v), "ease(%d, 0.5) returned non-finite", i);
    }
  }

  // TEST: monotonicity of non-overshooting easings at sampled points
  {
    NYA_EaseType monotonic[] = {
      NYA_EASE_LINEAR, NYA_EASE_QUAD_IN, NYA_EASE_QUAD_OUT,
      NYA_EASE_CUBIC_IN, NYA_EASE_CUBIC_OUT,
      NYA_EASE_SINE_IN, NYA_EASE_SINE_OUT,
      NYA_EASE_CIRC_IN, NYA_EASE_CIRC_OUT,
    };
    for (u32 i = 0; i < sizeof(monotonic) / sizeof(monotonic[0]); i++) {
      f32 prev = 0.0F;
      for (f32 t = 0.01F; t <= 1.0F; t += 0.01F) {
        f32 v = nya_ease(monotonic[i], t);
        nya_assert(v >= prev - EPSILON, "ease(%d) not monotonic at t=%.2f: %.4f < %.4f", monotonic[i], t, v, prev);
        prev = v;
      }
    }
  }

  printf("PASSED: test_tween\n");
  return 0;
}
