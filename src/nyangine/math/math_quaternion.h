/**
 * @file math_quaternion.h
 *
 * Unit quaternions for 3D rotation: no gimbal lock, cheap to compose, shortest-arc interpolation.
 * Euler conversions exist for editor input. f32 only, unlike the rest of math/ — rotation is the one
 * thing quaternions are for, so the f16/f128 overloads the vector and matrix headers carry would be
 * dead weight here.
 *
 * Functions documented as taking a rotation expect a *unit* quaternion; composing many rotations
 * drifts off the unit sphere, so re-normalize periodically:
 *
 * ```c
 * NYA_Quaternion yaw   = nya_quaternion_from_axis_angle(f32x3_unit_y, angle);
 * orientation          = nya_quaternion_normalize(nya_quaternion_multiply(yaw, orientation));
 * f32x3 forward        = nya_quaternion_rotate(orientation, -f32x3_unit_z);
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_matrix.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Quaternion NYA_Quaternion;

/**
 * `x`, `y`, `z` are the vector part, `w` the scalar part. A struct rather than an f32x4 typedef so it
 * can't be passed where a vector is wanted and __attr_overloaded functions can tell them apart; it is
 * laid out exactly like f32x4, so uploading one to a GPU buffer is a plain copy.
 * */
struct NYA_Quaternion {
    f32 x;
    f32 y;
    f32 z;
    f32 w;
};

static_assert(sizeof(NYA_Quaternion) == sizeof(f32x4), "NYA_Quaternion must stay layout compatible with f32x4.");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The rotation that does nothing. */
#define nya_quaternion_identity ((NYA_Quaternion){ 0.0F, 0.0F, 0.0F, 1.0F })

#define FMTquaternion          "(" FMTf32 ", " FMTf32 ", " FMTf32 ", " FMTf32 ")"
#define FMTquaternion_ARG(val) (f64)(val).x, (f64)(val).y, (f64)(val).z, (f64)(val).w

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * CONSTRUCTION
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_Quaternion nya_quaternion_create(f32 x, f32 y, f32 z, f32 w) __attr_no_discard;

/** `axis` need not be normalized; a zero axis yields the identity rather than a NaN. */
NYA_API NYA_Quaternion nya_quaternion_from_axis_angle(f32x3 axis, f32 radians) __attr_no_discard;

/**
 * Euler angles in radians, applied roll (Z) then pitch (X) then yaw (Y) — the convention a first
 * person camera wants: yaw turns around the world up axis regardless of look direction, pitch tilts
 * in the local frame.
 * */
NYA_API NYA_Quaternion nya_quaternion_from_euler(f32 pitch, f32 yaw, f32 roll) __attr_no_discard;

/** Inverse of nya_quaternion_from_euler. Any OUT pointer may be null. */
NYA_API void nya_quaternion_to_euler(NYA_Quaternion quaternion, OUT f32* out_pitch, OUT f32* out_yaw, OUT f32* out_roll);

/** Decomposes into the axis and angle it rotates about. A near identity rotation reports the X axis and angle 0. */
NYA_API void nya_quaternion_to_axis_angle(NYA_Quaternion quaternion, OUT f32x3* out_axis, OUT f32* out_radians);

/** Shortest rotation taking direction `from` to direction `to`. Neither need be normalized. */
NYA_API NYA_Quaternion nya_quaternion_from_to(f32x3 from, f32x3 to) __attr_no_discard;

/** Rotation whose -Z axis points along `direction` and whose Y axis is as close to `up` as possible. */
NYA_API NYA_Quaternion nya_quaternion_look(f32x3 direction, f32x3 up) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ALGEBRA
 * ─────────────────────────────────────────────────────────
 */

/** Composition: applies `b` first, then `a`, matching the equivalent matrix product; does not commute. */
NYA_API NYA_Quaternion nya_quaternion_multiply(NYA_Quaternion a, NYA_Quaternion b) __attr_no_discard;

NYA_API NYA_Quaternion nya_quaternion_add(NYA_Quaternion a, NYA_Quaternion b) __attr_no_discard;
NYA_API NYA_Quaternion nya_quaternion_scale(NYA_Quaternion quaternion, f32 scalar) __attr_no_discard;

/** Negates the vector part. For a unit quaternion this is the inverse rotation, and it is cheaper. */
NYA_API NYA_Quaternion nya_quaternion_conjugate(NYA_Quaternion quaternion) __attr_no_discard;

/** True inverse, valid for non unit quaternions too. Returns the identity for a zero quaternion. */
NYA_API NYA_Quaternion nya_quaternion_inverse(NYA_Quaternion quaternion) __attr_no_discard;

NYA_API f32 nya_quaternion_dot(NYA_Quaternion a, NYA_Quaternion b) __attr_no_discard;
NYA_API f32 nya_quaternion_length(NYA_Quaternion quaternion) __attr_no_discard;
NYA_API f32 nya_quaternion_length_squared(NYA_Quaternion quaternion) __attr_no_discard;

/** Returns the identity for a zero quaternion rather than dividing by zero. */
NYA_API NYA_Quaternion nya_quaternion_normalize(NYA_Quaternion quaternion) __attr_no_discard;

/** Angle of the shortest rotation between two orientations, in radians. */
NYA_API f32 nya_quaternion_angle_between(NYA_Quaternion a, NYA_Quaternion b) __attr_no_discard;

/** `q` and `-q` are the same rotation, so this compares rotations, not representations. */
NYA_API b8 nya_quaternion_approx_equals(NYA_Quaternion a, NYA_Quaternion b, f32 epsilon) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * APPLICATION
 * ─────────────────────────────────────────────────────────
 */

/** Rotates `vector` by a unit `quaternion`. */
NYA_API f32x3 nya_quaternion_rotate(NYA_Quaternion quaternion, f32x3 vector) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * INTERPOLATION
 * ─────────────────────────────────────────────────────────
 */

/** Normalized linear interpolation: cheap, shortest arc, non-constant angular velocity. Good for blending poses every frame. */
NYA_API NYA_Quaternion nya_quaternion_nlerp(NYA_Quaternion a, NYA_Quaternion b, f32 t) __attr_no_discard;

/** Spherical linear interpolation: constant angular velocity, shortest arc, more expensive. Use when the path itself matters, like a camera sweeping to a target. */
/**
 * How parallel two rotations must be before slerp falls back to nlerp, as a cosine.
 *
 * The two curves are not the same, so this is a claim about *how far apart* they are. Measured, worst
 * case over t, as the separation grows:
 *
 *     cos      angle    worst angular error
 *     0.9999   0.81°    0.000003°
 *     0.9989   2.63°    0.000088°
 *     0.9939   6.34°    0.001248°
 *     0.9890   8.51°    0.003018°
 *     0.9643  15.36°    0.017755°
 *
 * At 0.99 the arc is at most 8.1° and the error at most ~0.0026° — a four-hundredth of a degree, which
 * over a one metre bone is five hundredths of a micron. Nothing downstream resolves that: the palette
 * is f32, and the bake it came from quantised the pose harder than this does.
 *
 * It matters because the case is the common one. Sampling a clip interpolates *adjacent baked frames*,
 * which are a few degrees apart at most, so this path is taken almost every time a pose is built.
 * A crossfade between two unrelated clips is far apart and takes the exact path, which is correct —
 * that is where the difference would be visible.
 * */
#define NYA_QUATERNION_NLERP_THRESHOLD 0.99F

NYA_API NYA_Quaternion nya_quaternion_slerp(NYA_Quaternion a, NYA_Quaternion b, f32 t) __attr_no_discard;

/**
 * The same, for rotations already known to be unit length.
 *
 * Skips the two normalizations, which are two square roots and eight multiplies of pure waste when the
 * caller already knows — as skeletal animation does, since baked clip frames and blended poses are unit
 * by construction. Measurably faster; see bench/bench_slerp.c.
 *
 * ⚠ Undefined for non-unit input, in the ordinary way: the result will not be a rotation. Use
 * nya_quaternion_slerp when the input came from arithmetic that could have drifted.
 * */
NYA_API NYA_Quaternion nya_quaternion_slerp_unit(NYA_Quaternion a, NYA_Quaternion b, f32 t) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * MATRIX CONVERSION
 * ─────────────────────────────────────────────────────────
 */

/* Two names, not one overload set: return type differs and C overloading can't resolve on that alone. */

NYA_API f32_3x3 nya_quaternion_to_matrix3(NYA_Quaternion quaternion) __attr_no_discard;

/** The 3x3 rotation in the upper left of an otherwise identity 4x4, ready to compose with a transform. */
NYA_API f32_4x4 nya_quaternion_to_matrix4(NYA_Quaternion quaternion) __attr_no_discard;

/** Expects a pure rotation matrix; scale or shear in `matrix` gives a meaningless result. */
NYA_API NYA_Quaternion nya_quaternion_from_matrix(f32_3x3 matrix) __attr_no_discard;
