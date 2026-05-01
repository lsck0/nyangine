/**
 * @file base_tween.h
 *
 * Easing functions that map t ∈ [0, 1] → [0, 1] such that f(0) = 0 and f(1) = 1.
 * Used for animations etc..
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
  NYA_EASE_LINEAR,

  NYA_EASE_QUAD_IN,
  NYA_EASE_QUAD_OUT,
  NYA_EASE_QUAD_IN_OUT,

  NYA_EASE_CUBIC_IN,
  NYA_EASE_CUBIC_OUT,
  NYA_EASE_CUBIC_IN_OUT,

  NYA_EASE_QUART_IN,
  NYA_EASE_QUART_OUT,
  NYA_EASE_QUART_IN_OUT,

  NYA_EASE_QUINT_IN,
  NYA_EASE_QUINT_OUT,
  NYA_EASE_QUINT_IN_OUT,

  NYA_EASE_SINE_IN,
  NYA_EASE_SINE_OUT,
  NYA_EASE_SINE_IN_OUT,

  NYA_EASE_EXPO_IN,
  NYA_EASE_EXPO_OUT,
  NYA_EASE_EXPO_IN_OUT,

  NYA_EASE_CIRC_IN,
  NYA_EASE_CIRC_OUT,
  NYA_EASE_CIRC_IN_OUT,

  NYA_EASE_BACK_IN,
  NYA_EASE_BACK_OUT,
  NYA_EASE_BACK_IN_OUT,

  NYA_EASE_ELASTIC_IN,
  NYA_EASE_ELASTIC_OUT,
  NYA_EASE_ELASTIC_IN_OUT,

  NYA_EASE_BOUNCE_IN,
  NYA_EASE_BOUNCE_OUT,
  NYA_EASE_BOUNCE_IN_OUT,

  NYA_EASE_SMOOTHSTEP,
  NYA_EASE_SMOOTHERSTEP,

  NYA_EASE_COUNT,
} NYA_EaseType;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN f32 nya_ease(NYA_EaseType type, f32 t);
NYA_API NYA_EXTERN f32 nya_ease_bezier(f32 x1, f32 y1, f32 x2, f32 y2, f32 t);
NYA_API NYA_EXTERN f32 nya_ease_spring(f32 t, f32 frequency, f32 damping);
