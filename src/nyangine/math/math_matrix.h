/**
 * @file math_matrix.h
 *
 * Small dense matrix types built on clang's matrix_type, which requires -fenable-matrix (see
 * CFLAGS in src/build/flags.h). As with the vector types the arithmetic operators work directly,
 * including matrix times matrix, so only the things the extension does not provide live here.
 *
 * Indexing is `m[row][column]` and the constructors take rows, so nya_matrix_create(a, b) puts `a`
 * in row 0. That matches how a matrix is written down; it is not the column major convention
 * OpenGL uses at its API boundary.
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
 * Maps the rectangle (`left`, `top`) to (`right`, `bottom`) onto clip space, so a caller works in
 * whatever units it passes in and never writes a normalized coordinate by hand. For screen space
 * that means `nya_matrix_orthographic(0, width, 0, height)`: x grows right from the left edge, y
 * grows **down** from the top edge, and one unit is one pixel.
 *
 * y-down is a choice, not a property of the hardware. It is the convention screens, texture rows,
 * SDL's mouse coordinates and every UI layout system already use, and the whole point of this
 * matrix is that the conversion to the GPU's own convention happens here, once, instead of at every
 * call site. Passing `top` greater than `bottom` gives the y-up version for a world camera.
 *
 * Depth is fixed at the range clip space wants and is not a parameter: nothing drawn through this is
 * depth tested, and z is carried only so a 2D vertex can reuse the 3D vertex layout.
 * */
NYA_API f32_4x4 nya_matrix_orthographic(f32 left, f32 right, f32 top, f32 bottom);

/*
 * ── 3D projections ──
 *
 * All three target the clip space nya_matrix_orthographic describes: the Direct3D style one SDL_GPU
 * normalizes every backend to, with x and y in -1..+1, y pointing **up**, and z in **0..1** rather
 * than -1..1.
 *
 * That last detail is the one that matters and the one every piece of OpenGL-era reference material
 * gets differently. A -1..1 depth row here does not look like a sign error — it clips everything in
 * the near half of the frustum and renders as geometry with holes in it.
 *
 * The view convention is right-handed looking down **-z**, which is what nya_matrix_look_at
 * produces and what nya_vector_cross's handedness assumes.
 */

/**
 * A perspective projection: parallel lines converge, and distance shrinks things.
 *
 * `aspect` is width over height of the target. `fov_y` is the **vertical** field of view in radians,
 * vertical because the horizontal one then follows from the aspect — the other way round, a window
 * resize would change how much of the world is visible above and below.
 *
 * `near_plane` is the number worth tuning. Depth precision is concentrated close to the camera, so pushing
 * it toward zero spends all of it there and leaves distant geometry fighting over the few values
 * left; that is what z-fighting on a far wall actually is. Raise it as far as the scene allows.
 * */
NYA_API f32_4x4 nya_matrix_perspective(f32 fov_y, f32 aspect, f32 near_plane, f32 far_plane);

/**
 * An orthographic projection in three dimensions: no vanishing point, size ignores distance.
 *
 * The 3D counterpart of nya_matrix_orthographic, which is 2D and takes a rectangle in pixels. This
 * one takes a half-height and an aspect, matching nya_matrix_perspective so that swapping a camera
 * between the two is a change of function rather than a change of units.
 * */
NYA_API f32_4x4 nya_matrix_orthographic_3d(f32 height, f32 aspect, f32 near_plane, f32 far_plane);

/**
 * The view matrix for a camera at `eye` aimed at `target`.
 *
 * `up` only has to be roughly up: it is made perpendicular to the view direction on the way through,
 * so a camera looking slightly downward does not need its up vector tilted to match. What it must
 * not be is *parallel* to the view direction — there is no unique roll then, and the cross product
 * collapses to zero. That case returns the identity rather than a matrix of NaNs.
 * */
NYA_API f32_4x4 nya_matrix_look_at(f32x3 eye, f32x3 target, f32x3 up);

/**
 * A model matrix: scale, then rotate, then translate, in that order.
 *
 * The order is not a preference. Scaling after rotating shears anything whose scale is not uniform —
 * a flattened box turned forty-five degrees comes out as a parallelogram — and translating before
 * rotating swings the object around the world origin instead of turning it in place. This composes them
 * the only way that means what a caller intends by "here, this big, facing that way".
 *
 * `rotation` is a 3x3 so this needs nothing from math_quaternion. Callers holding a quaternion pass
 * nya_quaternion_to_matrix3.
 *
 * Note for anything reading the result as raw floats — a vertex attribute, a uniform: the storage is
 * column-major, so the first four floats are the first *column*, not the first row.
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

/*
 * Declared after nya_matrix_create because they expand to calls to it. An overload set has to be
 * complete at the point of use, so these cannot sit next to the typedefs.
 */

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
