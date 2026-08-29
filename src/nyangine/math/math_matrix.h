/**
 * @file math_matrix.h
 *
 * Small dense matrix types built on clang's matrix_type (-fenable-matrix, see CFLAGS in
 * src/build/flags.h). Arithmetic operators work directly, including matrix times matrix, so only
 * what the extension doesn't provide lives here.
 *
 * Indexing is `m[row][column]` and constructors take rows, so nya_matrix_create(a, b) puts `a` in
 * row 0 — matching how a matrix is written down, not OpenGL's column-major API convention.
 * */
#pragma once

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API f16_2x2  nya_matrix_create(f16x2 row1, f16x2 row2) __attr_overloaded;
NYA_API f16_3x3  nya_matrix_create(f16x3 row1, f16x3 row2, f16x3 row3) __attr_overloaded;
NYA_API f16_4x4  nya_matrix_create(f16x4 row1, f16x4 row2, f16x4 row3, f16x4 row4) __attr_overloaded;
NYA_API f32_2x2  nya_matrix_create(f32x2 row1, f32x2 row2) __attr_overloaded;
NYA_API f32_3x3  nya_matrix_create(f32x3 row1, f32x3 row2, f32x3 row3) __attr_overloaded;
NYA_API f32_4x4  nya_matrix_create(f32x4 row1, f32x4 row2, f32x4 row3, f32x4 row4) __attr_overloaded;
NYA_API f64_2x2  nya_matrix_create(f64x2 row1, f64x2 row2) __attr_overloaded;
NYA_API f64_3x3  nya_matrix_create(f64x3 row1, f64x3 row2, f64x3 row3) __attr_overloaded;
NYA_API f64_4x4  nya_matrix_create(f64x4 row1, f64x4 row2, f64x4 row3, f64x4 row4) __attr_overloaded;
NYA_API f128_2x2 nya_matrix_create(f128x2 row1, f128x2 row2) __attr_overloaded;
NYA_API f128_3x3 nya_matrix_create(f128x3 row1, f128x3 row2, f128x3 row3) __attr_overloaded;
NYA_API f128_4x4 nya_matrix_create(f128x4 row1, f128x4 row2, f128x4 row3, f128x4 row4) __attr_overloaded;

NYA_API f16_2x2  nya_matrix_create(f16 entries[2][2]) __attr_overloaded;
NYA_API f16_3x3  nya_matrix_create(f16 entries[3][3]) __attr_overloaded;
NYA_API f16_4x4  nya_matrix_create(f16 entries[4][4]) __attr_overloaded;
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
 *
 * Maps the rectangle (`left`, `top`)-(`right`, `bottom`) onto clip space, so a caller works in
 * whatever units it passes and never writes a normalized coordinate by hand. For screen space:
 * `nya_matrix_orthographic(0, width, 0, height)` — x grows right, y grows **down** from the top edge
 * (screens, texture rows, SDL mouse coords and UI layout all use this convention; the conversion to
 * the GPU's own happens here, once). Pass `top` greater than `bottom` for the y-up world-camera form.
 *
 * Depth is fixed at the range clip space wants and is not a parameter: nothing drawn through this is
 * depth tested, and z is carried only so a 2D vertex can reuse the 3D vertex layout.
 * */
NYA_API f32_4x4 nya_matrix_orthographic(f32 left, f32 right, f32 top, f32 bottom);

/*
 * ── 3D projections ──
 *
 * All three target the clip space nya_matrix_orthographic describes: the Direct3D style one SDL_GPU
 * normalizes every backend to, x and y in -1..+1, y pointing **up**, z in **0..1** rather than -1..1.
 * That last detail is the one every piece of OpenGL-era reference material gets differently — a
 * -1..1 depth row here does not look like a sign error, it clips the near half of the frustum and
 * renders as geometry with holes in it.
 *
 * The view convention is right-handed looking down **-z**, matching nya_matrix_look_at and the
 * handedness nya_vector_cross assumes.
 */

/**
 * A perspective projection: parallel lines converge, and distance shrinks things.
 *
 * `aspect` is width over height of the target. `fov_y` is the **vertical** field of view in radians —
 * vertical because horizontal then follows from aspect; the other way round, a window resize would
 * change how much of the world is visible above and below.
 *
 * `near_plane` is the number worth tuning: depth precision concentrates close to the camera, so
 * pushing it toward zero leaves distant geometry fighting over the few values left — that is what
 * z-fighting on a far wall actually is. Raise it as far as the scene allows.
 * */
NYA_API f32_4x4 nya_matrix_perspective(f32 fov_y, f32 aspect, f32 near_plane, f32 far_plane);

/**
 * An orthographic projection in three dimensions: no vanishing point, size ignores distance. The 3D
 * counterpart of nya_matrix_orthographic (2D, takes a rectangle in pixels); this one takes a
 * half-height and an aspect, matching nya_matrix_perspective so swapping cameras is a change of
 * function, not units.
 * */
NYA_API f32_4x4 nya_matrix_orthographic_3d(f32 height, f32 aspect, f32 near_plane, f32 far_plane);

/**
 * The view matrix for a camera at `eye` aimed at `target`.
 *
 * `up` only has to be roughly up: it's made perpendicular to the view direction on the way through, so
 * a camera looking slightly downward needs no adjustment. It must not be *parallel* to the view
 * direction — no unique roll then, and the cross product collapses to zero — that case returns the
 * identity rather than a matrix of NaNs.
 * */
NYA_API f32_4x4 nya_matrix_look_at(f32x3 eye, f32x3 target, f32x3 up);

/**
 * A model matrix: scale, then rotate, then translate, in that order — not a preference. Scaling after
 * rotating shears anything whose scale isn't uniform (a flattened box turned 45° comes out a
 * parallelogram), and translating before rotating swings the object around the world origin instead
 * of turning it in place.
 *
 * `rotation` is a 3x3 so this needs nothing from math_quaternion; callers holding a quaternion pass
 * nya_quaternion_to_matrix3.
 *
 * Reading the result as raw floats (vertex attribute, uniform): storage is column-major, so the first
 * four floats are the first *column*, not the first row.
 * */
NYA_API f32_4x4 nya_matrix_transform(f32x3 translation, f32_3x3 rotation, f32x3 scale) __attr_no_discard;

NYA_API f16x2  nya_matrix_times_vector(f16_2x2 mat, f16x2 vec) __attr_overloaded;
NYA_API f16x3  nya_matrix_times_vector(f16_3x3 mat, f16x3 vec) __attr_overloaded;
NYA_API f16x4  nya_matrix_times_vector(f16_4x4 mat, f16x4 vec) __attr_overloaded;
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
