/**
 * @file math_vector.h
 *
 * Short vector types built on clang's ext_vector_type, so the usual operators work elementwise and
 * `.x` / `.y` / `.z` / `.w` and swizzles are available:
 *
 * ```c
 * f32x3 a = { 1, 2, 3 };
 * f32x3 b = a * 2.0F + f32x3_unit_y;
 * f32   y = b.y;
 * ```
 *
 * There are deliberately no nya_vector_add style wrappers: the operators already do it, elementwise,
 * and a wrapper would only hide that.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef f16  f16x2 __attr_vector(2);
typedef f16  f16x3 __attr_vector(3);
typedef f16  f16x4 __attr_vector(4);
typedef f32  f32x2 __attr_vector(2);
typedef f32  f32x3 __attr_vector(3);
typedef f32  f32x4 __attr_vector(4);
typedef f64  f64x2 __attr_vector(2);
typedef f64  f64x3 __attr_vector(3);
typedef f64  f64x4 __attr_vector(4);
typedef f128 f128x2 __attr_vector(2);
typedef f128 f128x3 __attr_vector(3);
typedef f128 f128x4 __attr_vector(4);

/*
 * Integer lanes, for code that works on a whole register at once rather than on a point in space.
 *
 * The same mechanism and the same operators, but not the same idea: these are lanes, so they get no
 * `.x` / `.y` / `.z` / `.w` and no swizzles. Index them, or move lanes with __builtin_shufflevector.
 *
 * Only the widths something uses. math_random works on 256 bit registers as four 64 bit lanes, and
 * shuffles them as eight 32 bit ones; add others here when they are actually needed rather than
 * filling in the grid on spec.
 * */
typedef u32 u32x8 __attr_vector(8);
typedef u64 u64x4 __attr_vector(4);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FORMATTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define FMTf16x2  "(" FMTf16 ", " FMTf16 ")"
#define FMTf16x3  "(" FMTf16 ", " FMTf16 ", " FMTf16 ")"
#define FMTf16x4  "(" FMTf16 ", " FMTf16 ", " FMTf16 ", " FMTf16 ")"
#define FMTf32x2  "(" FMTf32 ", " FMTf32 ")"
#define FMTf32x3  "(" FMTf32 ", " FMTf32 ", " FMTf32 ")"
#define FMTf32x4  "(" FMTf32 ", " FMTf32 ", " FMTf32 ", " FMTf32 ")"
#define FMTf64x2  "(" FMTf64 ", " FMTf64 ")"
#define FMTf64x3  "(" FMTf64 ", " FMTf64 ", " FMTf64 ")"
#define FMTf64x4  "(" FMTf64 ", " FMTf64 ", " FMTf64 ", " FMTf64 ")"
#define FMTf128x2 "(" FMTf128 ", " FMTf128 ")"
#define FMTf128x3 "(" FMTf128 ", " FMTf128 ", " FMTf128 ")"
#define FMTf128x4 "(" FMTf128 ", " FMTf128 ", " FMTf128 ", " FMTf128 ")"

/*
 * f16 and f32 are promoted to double by the default argument promotions before printf ever sees
 * them, so FMTf16 and FMTf32 are double conversions. The casts here are what makes that explicit
 * instead of relying on it, and they keep -Wdouble-promotion quiet.
 */
#define FMTf16x2_ARG(val)  (f64)(val).x, (f64)(val).y
#define FMTf16x3_ARG(val)  (f64)(val).x, (f64)(val).y, (f64)(val).z
#define FMTf16x4_ARG(val)  (f64)(val).x, (f64)(val).y, (f64)(val).z, (f64)(val).w
#define FMTf32x2_ARG(val)  (f64)(val).x, (f64)(val).y
#define FMTf32x3_ARG(val)  (f64)(val).x, (f64)(val).y, (f64)(val).z
#define FMTf32x4_ARG(val)  (f64)(val).x, (f64)(val).y, (f64)(val).z, (f64)(val).w
#define FMTf64x2_ARG(val)  (val).x, (val).y
#define FMTf64x3_ARG(val)  (val).x, (val).y, (val).z
#define FMTf64x4_ARG(val)  (val).x, (val).y, (val).z, (val).w
#define FMTf128x2_ARG(val) (val).x, (val).y
#define FMTf128x3_ARG(val) (val).x, (val).y, (val).z
#define FMTf128x4_ARG(val) (val).x, (val).y, (val).z, (val).w

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define f16x2_zero  ((f16x2){ 0, 0 })
#define f16x3_zero  ((f16x3){ 0, 0, 0 })
#define f16x4_zero  ((f16x4){ 0, 0, 0, 0 })
#define f32x2_zero  ((f32x2){ 0, 0 })
#define f32x3_zero  ((f32x3){ 0, 0, 0 })
#define f32x4_zero  ((f32x4){ 0, 0, 0, 0 })
#define f64x2_zero  ((f64x2){ 0, 0 })
#define f64x3_zero  ((f64x3){ 0, 0, 0 })
#define f64x4_zero  ((f64x4){ 0, 0, 0, 0 })
#define f128x2_zero ((f128x2){ 0, 0 })
#define f128x3_zero ((f128x3){ 0, 0, 0 })
#define f128x4_zero ((f128x4){ 0, 0, 0, 0 })

#define f16x2_unit_x  ((f16x2){ 1, 0 })
#define f16x2_unit_y  ((f16x2){ 0, 1 })
#define f32x2_unit_x  ((f32x2){ 1, 0 })
#define f32x2_unit_y  ((f32x2){ 0, 1 })
#define f64x2_unit_x  ((f64x2){ 1, 0 })
#define f64x2_unit_y  ((f64x2){ 0, 1 })
#define f128x2_unit_x ((f128x2){ 1, 0 })
#define f128x2_unit_y ((f128x2){ 0, 1 })

#define f16x3_unit_x  ((f16x3){ 1, 0, 0 })
#define f16x3_unit_y  ((f16x3){ 0, 1, 0 })
#define f16x3_unit_z  ((f16x3){ 0, 0, 1 })
#define f32x3_unit_x  ((f32x3){ 1, 0, 0 })
#define f32x3_unit_y  ((f32x3){ 0, 1, 0 })
#define f32x3_unit_z  ((f32x3){ 0, 0, 1 })
#define f64x3_unit_x  ((f64x3){ 1, 0, 0 })
#define f64x3_unit_y  ((f64x3){ 0, 1, 0 })
#define f64x3_unit_z  ((f64x3){ 0, 0, 1 })
#define f128x3_unit_x ((f128x3){ 1, 0, 0 })
#define f128x3_unit_y ((f128x3){ 0, 1, 0 })
#define f128x3_unit_z ((f128x3){ 0, 0, 1 })

#define f16x4_unit_x  ((f16x4){ 1, 0, 0, 0 })
#define f16x4_unit_y  ((f16x4){ 0, 1, 0, 0 })
#define f16x4_unit_z  ((f16x4){ 0, 0, 1, 0 })
#define f16x4_unit_w  ((f16x4){ 0, 0, 0, 1 })
#define f32x4_unit_x  ((f32x4){ 1, 0, 0, 0 })
#define f32x4_unit_y  ((f32x4){ 0, 1, 0, 0 })
#define f32x4_unit_z  ((f32x4){ 0, 0, 1, 0 })
#define f32x4_unit_w  ((f32x4){ 0, 0, 0, 1 })
#define f64x4_unit_x  ((f64x4){ 1, 0, 0, 0 })
#define f64x4_unit_y  ((f64x4){ 0, 1, 0, 0 })
#define f64x4_unit_z  ((f64x4){ 0, 0, 1, 0 })
#define f64x4_unit_w  ((f64x4){ 0, 0, 0, 1 })
#define f128x4_unit_x ((f128x4){ 1, 0, 0, 0 })
#define f128x4_unit_y ((f128x4){ 0, 1, 0, 0 })
#define f128x4_unit_z ((f128x4){ 0, 0, 1, 0 })
#define f128x4_unit_w ((f128x4){ 0, 0, 0, 1 })
