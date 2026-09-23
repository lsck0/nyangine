/**
 * @file math_matrix.h
 * */
#pragma once

#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef f16  f16_2x2 __attr_matrix(2, 2);
typedef f16  f16_3x3 __attr_matrix(3, 3);
typedef f16  f16_4x4 __attr_matrix(4, 4);
typedef f32  f32_2x2 __attr_matrix(2, 2);
typedef f32  f32_3x3 __attr_matrix(3, 3);
typedef f32  f32_4x4 __attr_matrix(4, 4);
typedef f64  f64_2x2 __attr_matrix(2, 2);
typedef f64  f64_3x3 __attr_matrix(3, 3);
typedef f64  f64_4x4 __attr_matrix(4, 4);
typedef f128 f128_2x2 __attr_matrix(2, 2);
typedef f128 f128_3x3 __attr_matrix(3, 3);
typedef f128 f128_4x4 __attr_matrix(4, 4);

nya_derive_array(f16_2x2);
nya_derive_array(f16_3x3);
nya_derive_array(f16_4x4);
nya_derive_array(f32_2x2);
nya_derive_array(f32_3x3);
nya_derive_array(f32_4x4);
nya_derive_array(f64_2x2);
nya_derive_array(f64_3x3);
nya_derive_array(f64_4x4);
nya_derive_array(f128_2x2);
nya_derive_array(f128_3x3);
nya_derive_array(f128_4x4);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if !NYA_F16_IS_F32
NYA_API f16_2x2  nya_matrix_create(f16x2 row1, f16x2 row2) __attr_overloaded;
NYA_API f16_3x3  nya_matrix_create(f16x3 row1, f16x3 row2, f16x3 row3) __attr_overloaded;
NYA_API f16_4x4  nya_matrix_create(f16x4 row1, f16x4 row2, f16x4 row3, f16x4 row4) __attr_overloaded;
#endif
NYA_API f32_2x2  nya_matrix_create(f32x2 row1, f32x2 row2) __attr_overloaded;
NYA_API f32_3x3  nya_matrix_create(f32x3 row1, f32x3 row2, f32x3 row3) __attr_overloaded;
NYA_API f32_4x4  nya_matrix_create(f32x4 row1, f32x4 row2, f32x4 row3, f32x4 row4) __attr_overloaded;
NYA_API f64_2x2  nya_matrix_create(f64x2 row1, f64x2 row2) __attr_overloaded;
NYA_API f64_3x3  nya_matrix_create(f64x3 row1, f64x3 row2, f64x3 row3) __attr_overloaded;
NYA_API f64_4x4  nya_matrix_create(f64x4 row1, f64x4 row2, f64x4 row3, f64x4 row4) __attr_overloaded;
NYA_API f128_2x2 nya_matrix_create(f128x2 row1, f128x2 row2) __attr_overloaded;
NYA_API f128_3x3 nya_matrix_create(f128x3 row1, f128x3 row2, f128x3 row3) __attr_overloaded;
NYA_API f128_4x4 nya_matrix_create(f128x4 row1, f128x4 row2, f128x4 row3, f128x4 row4) __attr_overloaded;

#if !NYA_F16_IS_F32
NYA_API f16_2x2  nya_matrix_create(f16 entries[2][2]) __attr_overloaded;
NYA_API f16_3x3  nya_matrix_create(f16 entries[3][3]) __attr_overloaded;
NYA_API f16_4x4  nya_matrix_create(f16 entries[4][4]) __attr_overloaded;
#endif
NYA_API f32_2x2  nya_matrix_create(f32 entries[2][2]) __attr_overloaded;
NYA_API f32_3x3  nya_matrix_create(f32 entries[3][3]) __attr_overloaded;
NYA_API f32_4x4  nya_matrix_create(f32 entries[4][4]) __attr_overloaded;
NYA_API f64_2x2  nya_matrix_create(f64 entries[2][2]) __attr_overloaded;
NYA_API f64_3x3  nya_matrix_create(f64 entries[3][3]) __attr_overloaded;
NYA_API f64_4x4  nya_matrix_create(f64 entries[4][4]) __attr_overloaded;
NYA_API f128_2x2 nya_matrix_create(f128 entries[2][2]) __attr_overloaded;
NYA_API f128_3x3 nya_matrix_create(f128 entries[3][3]) __attr_overloaded;
NYA_API f128_4x4 nya_matrix_create(f128 entries[4][4]) __attr_overloaded;

/**
 * Orthographic projection onto the GPU's clip space, for 2D drawing in pixels.
 * */
NYA_API f32_4x4 nya_matrix_orthographic(f32 left, f32 right, f32 top, f32 bottom);

/*
 * 3D projections
 *
 * All three target the clip space described at nya_matrix_orthographic: Direct3D style, as SDL_GPU
 * normalizes every backend, x and y in -1..+1 with y up, z in 0..1. OpenGL references use -1..1 depth,
 * which here clips the near half of the frustum.
 *
 * The view is right-handed looking down -z, matching nya_matrix_look_at and nya_vector_cross.
 */

/**
 * A perspective projection: parallel lines converge, and distance shrinks things.
 * */
NYA_API f32_4x4 nya_matrix_perspective(f32 fov_y, f32 aspect, f32 near_plane, f32 far_plane);

/**
 * An orthographic projection in 3D: no vanishing point, size ignores distance. Takes a half-height
 * and an aspect like nya_matrix_perspective, so switching cameras changes the function, not the
 * units. nya_matrix_orthographic is the 2D pixel version.
 * */
NYA_API f32_4x4 nya_matrix_orthographic_3d(f32 height, f32 aspect, f32 near_plane, f32 far_plane);

/**
 * The view matrix for a camera at `eye` aimed at `target`.
 * */
NYA_API f32_4x4 nya_matrix_look_at(f32x3 eye, f32x3 target, f32x3 up);

/**
 * A model matrix: scale, then rotate, then translate. Scaling after rotating shears non-uniform scale,
 * and translating before rotating orbits the world origin.
 * */
NYA_API f32_4x4 nya_matrix_transform(f32x3 translation, f32_3x3 rotation, f32x3 scale) __attr_no_discard;

#if !NYA_F16_IS_F32
NYA_API f16x2  nya_matrix_times_vector(f16_2x2 mat, f16x2 vec) __attr_overloaded;
NYA_API f16x3  nya_matrix_times_vector(f16_3x3 mat, f16x3 vec) __attr_overloaded;
NYA_API f16x4  nya_matrix_times_vector(f16_4x4 mat, f16x4 vec) __attr_overloaded;
#endif
NYA_API f32x2  nya_matrix_times_vector(f32_2x2 mat, f32x2 vec) __attr_overloaded;
NYA_API f32x3  nya_matrix_times_vector(f32_3x3 mat, f32x3 vec) __attr_overloaded;
NYA_API f32x4  nya_matrix_times_vector(f32_4x4 mat, f32x4 vec) __attr_overloaded;
NYA_API f64x2  nya_matrix_times_vector(f64_2x2 mat, f64x2 vec) __attr_overloaded;
NYA_API f64x3  nya_matrix_times_vector(f64_3x3 mat, f64x3 vec) __attr_overloaded;
NYA_API f64x4  nya_matrix_times_vector(f64_4x4 mat, f64x4 vec) __attr_overloaded;
NYA_API f128x2 nya_matrix_times_vector(f128_2x2 mat, f128x2 vec) __attr_overloaded;
NYA_API f128x3 nya_matrix_times_vector(f128_3x3 mat, f128x3 vec) __attr_overloaded;
NYA_API f128x4 nya_matrix_times_vector(f128_4x4 mat, f128x4 vec) __attr_overloaded;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* Declared after nya_matrix_create because they expand to calls to it; an overload set must be complete at the point of use. */

#define f16_2x2_zero  (nya_matrix_create(f16x2_zero, f16x2_zero))
#define f16_3x3_zero  (nya_matrix_create(f16x3_zero, f16x3_zero, f16x3_zero))
#define f16_4x4_zero  (nya_matrix_create(f16x4_zero, f16x4_zero, f16x4_zero, f16x4_zero))
#define f32_2x2_zero  (nya_matrix_create(f32x2_zero, f32x2_zero))
#define f32_3x3_zero  (nya_matrix_create(f32x3_zero, f32x3_zero, f32x3_zero))
#define f32_4x4_zero  (nya_matrix_create(f32x4_zero, f32x4_zero, f32x4_zero, f32x4_zero))
#define f64_2x2_zero  (nya_matrix_create(f64x2_zero, f64x2_zero))
#define f64_3x3_zero  (nya_matrix_create(f64x3_zero, f64x3_zero, f64x3_zero))
#define f64_4x4_zero  (nya_matrix_create(f64x4_zero, f64x4_zero, f64x4_zero, f64x4_zero))
#define f128_2x2_zero (nya_matrix_create(f128x2_zero, f128x2_zero))
#define f128_3x3_zero (nya_matrix_create(f128x3_zero, f128x3_zero, f128x3_zero))
#define f128_4x4_zero (nya_matrix_create(f128x4_zero, f128x4_zero, f128x4_zero, f128x4_zero))

#define f16_2x2_id  (nya_matrix_create(f16x2_unit_x, f16x2_unit_y))
#define f16_3x3_id  (nya_matrix_create(f16x3_unit_x, f16x3_unit_y, f16x3_unit_z))
#define f16_4x4_id  (nya_matrix_create(f16x4_unit_x, f16x4_unit_y, f16x4_unit_z, f16x4_unit_w))
#define f32_2x2_id  (nya_matrix_create(f32x2_unit_x, f32x2_unit_y))
#define f32_3x3_id  (nya_matrix_create(f32x3_unit_x, f32x3_unit_y, f32x3_unit_z))
#define f32_4x4_id  (nya_matrix_create(f32x4_unit_x, f32x4_unit_y, f32x4_unit_z, f32x4_unit_w))
#define f64_2x2_id  (nya_matrix_create(f64x2_unit_x, f64x2_unit_y))
#define f64_3x3_id  (nya_matrix_create(f64x3_unit_x, f64x3_unit_y, f64x3_unit_z))
#define f64_4x4_id  (nya_matrix_create(f64x4_unit_x, f64x4_unit_y, f64x4_unit_z, f64x4_unit_w))
#define f128_2x2_id (nya_matrix_create(f128x2_unit_x, f128x2_unit_y))
#define f128_3x3_id (nya_matrix_create(f128x3_unit_x, f128x3_unit_y, f128x3_unit_z))
#define f128_4x4_id (nya_matrix_create(f128x4_unit_x, f128x4_unit_y, f128x4_unit_z, f128x4_unit_w))
