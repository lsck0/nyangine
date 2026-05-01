#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL f32 _nya_ease_linear(f32 t);
NYA_INTERNAL f32 _nya_ease_quad_in(f32 t);
NYA_INTERNAL f32 _nya_ease_quad_out(f32 t);
NYA_INTERNAL f32 _nya_ease_quad_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_cubic_in(f32 t);
NYA_INTERNAL f32 _nya_ease_cubic_out(f32 t);
NYA_INTERNAL f32 _nya_ease_cubic_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_quart_in(f32 t);
NYA_INTERNAL f32 _nya_ease_quart_out(f32 t);
NYA_INTERNAL f32 _nya_ease_quart_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_quint_in(f32 t);
NYA_INTERNAL f32 _nya_ease_quint_out(f32 t);
NYA_INTERNAL f32 _nya_ease_quint_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_sine_in(f32 t);
NYA_INTERNAL f32 _nya_ease_sine_out(f32 t);
NYA_INTERNAL f32 _nya_ease_sine_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_expo_in(f32 t);
NYA_INTERNAL f32 _nya_ease_expo_out(f32 t);
NYA_INTERNAL f32 _nya_ease_expo_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_circ_in(f32 t);
NYA_INTERNAL f32 _nya_ease_circ_out(f32 t);
NYA_INTERNAL f32 _nya_ease_circ_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_back_in(f32 t);
NYA_INTERNAL f32 _nya_ease_back_out(f32 t);
NYA_INTERNAL f32 _nya_ease_back_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_elastic_in(f32 t);
NYA_INTERNAL f32 _nya_ease_elastic_out(f32 t);
NYA_INTERNAL f32 _nya_ease_elastic_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_bounce_in(f32 t);
NYA_INTERNAL f32 _nya_ease_bounce_out(f32 t);
NYA_INTERNAL f32 _nya_ease_bounce_in_out(f32 t);
NYA_INTERNAL f32 _nya_ease_smoothstep(f32 t);
NYA_INTERNAL f32 _nya_ease_smootherstep(f32 t);

NYA_INTERNAL f32 _nya_bezier_solve_x(f32 x1, f32 x2, f32 t);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef f32 (*NYA_EaseFunc)(f32);

NYA_INTERNAL const NYA_EaseFunc NYA_EASE_TABLE[NYA_EASE_COUNT] = {
  [NYA_EASE_LINEAR]         = _nya_ease_linear,
  [NYA_EASE_QUAD_IN]        = _nya_ease_quad_in,
  [NYA_EASE_QUAD_OUT]       = _nya_ease_quad_out,
  [NYA_EASE_QUAD_IN_OUT]    = _nya_ease_quad_in_out,
  [NYA_EASE_CUBIC_IN]       = _nya_ease_cubic_in,
  [NYA_EASE_CUBIC_OUT]      = _nya_ease_cubic_out,
  [NYA_EASE_CUBIC_IN_OUT]   = _nya_ease_cubic_in_out,
  [NYA_EASE_QUART_IN]       = _nya_ease_quart_in,
  [NYA_EASE_QUART_OUT]      = _nya_ease_quart_out,
  [NYA_EASE_QUART_IN_OUT]   = _nya_ease_quart_in_out,
  [NYA_EASE_QUINT_IN]       = _nya_ease_quint_in,
  [NYA_EASE_QUINT_OUT]      = _nya_ease_quint_out,
  [NYA_EASE_QUINT_IN_OUT]   = _nya_ease_quint_in_out,
  [NYA_EASE_SINE_IN]        = _nya_ease_sine_in,
  [NYA_EASE_SINE_OUT]       = _nya_ease_sine_out,
  [NYA_EASE_SINE_IN_OUT]    = _nya_ease_sine_in_out,
  [NYA_EASE_EXPO_IN]        = _nya_ease_expo_in,
  [NYA_EASE_EXPO_OUT]       = _nya_ease_expo_out,
  [NYA_EASE_EXPO_IN_OUT]    = _nya_ease_expo_in_out,
  [NYA_EASE_CIRC_IN]        = _nya_ease_circ_in,
  [NYA_EASE_CIRC_OUT]       = _nya_ease_circ_out,
  [NYA_EASE_CIRC_IN_OUT]    = _nya_ease_circ_in_out,
  [NYA_EASE_BACK_IN]        = _nya_ease_back_in,
  [NYA_EASE_BACK_OUT]       = _nya_ease_back_out,
  [NYA_EASE_BACK_IN_OUT]    = _nya_ease_back_in_out,
  [NYA_EASE_ELASTIC_IN]     = _nya_ease_elastic_in,
  [NYA_EASE_ELASTIC_OUT]    = _nya_ease_elastic_out,
  [NYA_EASE_ELASTIC_IN_OUT] = _nya_ease_elastic_in_out,
  [NYA_EASE_BOUNCE_IN]      = _nya_ease_bounce_in,
  [NYA_EASE_BOUNCE_OUT]     = _nya_ease_bounce_out,
  [NYA_EASE_BOUNCE_IN_OUT]  = _nya_ease_bounce_in_out,
  [NYA_EASE_SMOOTHSTEP]     = _nya_ease_smoothstep,
  [NYA_EASE_SMOOTHERSTEP]   = _nya_ease_smootherstep,
};
static_assert(NYA_EASE_COUNT == 33, "Unhandled NYA_EaseType enum value.");

f32 nya_ease(NYA_EaseType type, f32 t) {
  nya_assert(type >= 0 && type < NYA_EASE_COUNT, "Invalid ease type: %d", type);
  return NYA_EASE_TABLE[type](t);
}

f32 nya_ease_bezier(f32 x1, f32 y1, f32 x2, f32 y2, f32 t) {
  if (t <= 0.0F) return 0.0F;
  if (t >= 1.0F) return 1.0F;

  f32 s   = _nya_bezier_solve_x(x1, x2, t);
  f32 inv = 1.0F - s;
  return 3.0F * inv * inv * s * y1 + 3.0F * inv * s * s * y2 + s * s * s;
}

f32 nya_ease_spring(f32 t, f32 frequency, f32 damping) {
  return 1.0F - expf(-damping * t) * cosf(frequency * t * 2.0F * (f32)M_PI);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL f32 _nya_ease_linear(f32 t) {
  return t;
}

NYA_INTERNAL f32 _nya_ease_quad_in(f32 t) {
  return t * t;
}
NYA_INTERNAL f32 _nya_ease_quad_out(f32 t) {
  return t * (2.0F - t);
}
NYA_INTERNAL f32 _nya_ease_quad_in_out(f32 t) {
  return t < 0.5F ? 2.0F * t * t : -1.0F + (4.0F - 2.0F * t) * t;
}

NYA_INTERNAL f32 _nya_ease_cubic_in(f32 t) {
  return t * t * t;
}
NYA_INTERNAL f32 _nya_ease_cubic_out(f32 t) {
  f32 u = t - 1.0F;
  return u * u * u + 1.0F;
}
NYA_INTERNAL f32 _nya_ease_cubic_in_out(f32 t) {
  return t < 0.5F ? 4.0F * t * t * t : 1.0F - powf(-2.0F * t + 2.0F, 3.0F) * 0.5F;
}

NYA_INTERNAL f32 _nya_ease_quart_in(f32 t) {
  return t * t * t * t;
}
NYA_INTERNAL f32 _nya_ease_quart_out(f32 t) {
  f32 u = t - 1.0F;
  return 1.0F - u * u * u * u;
}
NYA_INTERNAL f32 _nya_ease_quart_in_out(f32 t) {
  return t < 0.5F ? 8.0F * t * t * t * t : 1.0F - powf(-2.0F * t + 2.0F, 4.0F) * 0.5F;
}

NYA_INTERNAL f32 _nya_ease_quint_in(f32 t) {
  return t * t * t * t * t;
}
NYA_INTERNAL f32 _nya_ease_quint_out(f32 t) {
  f32 u = t - 1.0F;
  return 1.0F + u * u * u * u * u;
}
NYA_INTERNAL f32 _nya_ease_quint_in_out(f32 t) {
  return t < 0.5F ? 16.0F * t * t * t * t * t : 1.0F - powf(-2.0F * t + 2.0F, 5.0F) * 0.5F;
}

NYA_INTERNAL f32 _nya_ease_sine_in(f32 t) {
  return 1.0F - cosf(t * (f32)M_PI * 0.5F);
}
NYA_INTERNAL f32 _nya_ease_sine_out(f32 t) {
  return sinf(t * (f32)M_PI * 0.5F);
}
NYA_INTERNAL f32 _nya_ease_sine_in_out(f32 t) {
  return -(cosf((f32)M_PI * t) - 1.0F) * 0.5F;
}

NYA_INTERNAL f32 _nya_ease_expo_in(f32 t) {
  return t <= 0.0F ? 0.0F : powf(2.0F, 10.0F * t - 10.0F);
}
NYA_INTERNAL f32 _nya_ease_expo_out(f32 t) {
  return t >= 1.0F ? 1.0F : 1.0F - powf(2.0F, -10.0F * t);
}
NYA_INTERNAL f32 _nya_ease_expo_in_out(f32 t) {
  if (t <= 0.0F) return 0.0F;
  if (t >= 1.0F) return 1.0F;
  return t < 0.5F ? powf(2.0F, 20.0F * t - 10.0F) * 0.5F : (2.0F - powf(2.0F, -20.0F * t + 10.0F)) * 0.5F;
}

NYA_INTERNAL f32 _nya_ease_circ_in(f32 t) {
  return 1.0F - sqrtf(1.0F - t * t);
}
NYA_INTERNAL f32 _nya_ease_circ_out(f32 t) {
  f32 u = t - 1.0F;
  return sqrtf(1.0F - u * u);
}
NYA_INTERNAL f32 _nya_ease_circ_in_out(f32 t) {
  return t < 0.5F ? (1.0F - sqrtf(1.0F - 4.0F * t * t)) * 0.5F : (sqrtf(1.0F - powf(-2.0F * t + 2.0F, 2.0F)) + 1.0F) * 0.5F;
}

#define NYA_BACK_C1 1.70158F
#define NYA_BACK_C2 (NYA_BACK_C1 * 1.525F)
#define NYA_BACK_C3 (NYA_BACK_C1 + 1.0F)

NYA_INTERNAL f32 _nya_ease_back_in(f32 t) {
  return NYA_BACK_C3 * t * t * t - NYA_BACK_C1 * t * t;
}
NYA_INTERNAL f32 _nya_ease_back_out(f32 t) {
  f32 u = t - 1.0F;
  return 1.0F + NYA_BACK_C3 * u * u * u + NYA_BACK_C1 * u * u;
}
NYA_INTERNAL f32 _nya_ease_back_in_out(f32 t) {
  return t < 0.5F ? (powf(2.0F * t, 2.0F) * ((NYA_BACK_C2 + 1.0F) * 2.0F * t - NYA_BACK_C2)) * 0.5F
                  : (powf(2.0F * t - 2.0F, 2.0F) * ((NYA_BACK_C2 + 1.0F) * (2.0F * t - 2.0F) + NYA_BACK_C2) + 2.0F) * 0.5F;
}

#define NYA_ELASTIC_C4 (2.0F * (f32)M_PI / 3.0F)
#define NYA_ELASTIC_C5 (2.0F * (f32)M_PI / 4.5F)

NYA_INTERNAL f32 _nya_ease_elastic_in(f32 t) {
  if (t <= 0.0F) return 0.0F;
  if (t >= 1.0F) return 1.0F;
  return -powf(2.0F, 10.0F * t - 10.0F) * sinf((t * 10.0F - 10.75F) * NYA_ELASTIC_C4);
}
NYA_INTERNAL f32 _nya_ease_elastic_out(f32 t) {
  if (t <= 0.0F) return 0.0F;
  if (t >= 1.0F) return 1.0F;
  return powf(2.0F, -10.0F * t) * sinf((t * 10.0F - 0.75F) * NYA_ELASTIC_C4) + 1.0F;
}
NYA_INTERNAL f32 _nya_ease_elastic_in_out(f32 t) {
  if (t <= 0.0F) return 0.0F;
  if (t >= 1.0F) return 1.0F;
  return t < 0.5F ? -(powf(2.0F, 20.0F * t - 10.0F) * sinf((20.0F * t - 11.125F) * NYA_ELASTIC_C5)) * 0.5F
                  : (powf(2.0F, -20.0F * t + 10.0F) * sinf((20.0F * t - 11.125F) * NYA_ELASTIC_C5)) * 0.5F + 1.0F;
}

NYA_INTERNAL f32 _nya_ease_bounce_out(f32 t) {
  if (t < 1.0F / 2.75F) return 7.5625F * t * t;

  if (t < 2.0F / 2.75F) {
    t -= 1.5F / 2.75F;
    return 7.5625F * t * t + 0.75F;
  }

  if (t < 2.5F / 2.75F) {
    t -= 2.25F / 2.75F;
    return 7.5625F * t * t + 0.9375F;
  }

  t -= 2.625F / 2.75F;
  return 7.5625F * t * t + 0.984375F;
}
NYA_INTERNAL f32 _nya_ease_bounce_in(f32 t) {
  return 1.0F - _nya_ease_bounce_out(1.0F - t);
}
NYA_INTERNAL f32 _nya_ease_bounce_in_out(f32 t) {
  return t < 0.5F ? (1.0F - _nya_ease_bounce_out(1.0F - 2.0F * t)) * 0.5F : (1.0F + _nya_ease_bounce_out(2.0F * t - 1.0F)) * 0.5F;
}

NYA_INTERNAL f32 _nya_ease_smoothstep(f32 t) {
  return t * t * (3.0F - 2.0F * t);
}
NYA_INTERNAL f32 _nya_ease_smootherstep(f32 t) {
  return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

// Solve for parameter s on the x-axis of cubic bézier B(s) = (1-s)³·0 + 3(1-s)²s·x1 + 3(1-s)s²·x2 + s³·1
// given target x = t. Uses Newton-Raphson with bisection fallback.
NYA_INTERNAL f32 _nya_bezier_solve_x(f32 x1, f32 x2, f32 t) {
  f32 s = t; // initial guess

  for (u32 i = 0; i < 8; i++) {
    f32 s2   = s * s;
    f32 s3   = s2 * s;
    f32 inv  = 1.0F - s;
    f32 inv2 = inv * inv;

    f32 x  = 3.0F * inv2 * s * x1 + 3.0F * inv * s2 * x2 + s3;
    f32 dx = 3.0F * inv2 * x1 + 6.0F * inv * s * (x2 - x1) + 3.0F * s2 * (1.0F - x2);

    if (fabsf(dx) < 1e-7F) break;
    s -= (x - t) / dx;
    s  = fmaxf(0.0F, fminf(1.0F, s));
  }

  // bisection fallback for robustness
  f32 lo = 0.0F, hi = 1.0F;
  for (u32 i = 0; i < 16; i++) {
    f32 mid = (lo + hi) * 0.5F;
    f32 inv = 1.0F - mid;
    f32 x   = 3.0F * inv * inv * mid * x1 + 3.0F * inv * mid * mid * x2 + mid * mid * mid;
    if (x < t)
      lo = mid;
    else
      hi = mid;
  }
  f32 bisect = (lo + hi) * 0.5F;

  // pick whichever is closer to target
  f32 inv_s = 1.0F - s;
  f32 x_nr  = 3.0F * inv_s * inv_s * s * x1 + 3.0F * inv_s * s * s * x2 + s * s * s;
  f32 inv_b = 1.0F - bisect;
  f32 x_bi  = 3.0F * inv_b * inv_b * bisect * x1 + 3.0F * inv_b * bisect * bisect * x2 + bisect * bisect * bisect;

  return fabsf(x_nr - t) < fabsf(x_bi - t) ? s : bisect;
}
