/**
 * @file math_shapes.h
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_vector.h"

// TYPES

typedef struct NYA_Rectf   NYA_Rectf;
typedef struct NYA_Circlef NYA_Circlef;
typedef struct NYA_Box2f   NYA_Box2f;
typedef struct NYA_Box3f   NYA_Box3f;
typedef struct NYA_Spheref NYA_Spheref;
typedef struct NYA_Ray2f   NYA_Ray2f;
typedef struct NYA_Ray3f   NYA_Ray3f;

/** An axis aligned rectangle as minimum corner and size, the form draws and sprites already use. */
struct NYA_Rectf {
    f32 x, y, width, height;
};

/** A circle, as a centre and a radius. The other shape a 2D hit test is ever written against. */
struct NYA_Circlef {
    f32x2 center;
    f32   radius;
};

/**
 * An axis aligned box as two corners. Closed, unlike NYA_Rectf: a point is a box of no size, and the spatial
 * indices store points, so a box touching another overlaps it. Inverted on any axis is empty.
 * */
struct NYA_Box2f {
    f32x2 min, max;
};

/** The 3D NYA_Box2f, under the same closed rule. */
struct NYA_Box3f {
    f32x3 min, max;
};

struct NYA_Spheref {
    f32x3 center;
    f32   radius;
};

/** A half line. `direction` need not be unit length; a hit distance is in multiples of it. */
struct NYA_Ray2f {
    f32x2 origin, direction;
};

struct NYA_Ray3f {
    f32x3 origin, direction;
};

/** Inverted at the largest float, so the first union with anything is that thing: where an accumulated bound starts. */
#define NYA_BOX3F_EMPTY ((NYA_Box3f){ .min = { F32_MAX, F32_MAX, F32_MAX }, .max = { -F32_MAX, -F32_MAX, -F32_MAX } })

// FUNCTIONS

// RECTANGLE

/** From two opposite corners, in either order. Normalized, so the result never has a negative extent. */
NYA_API NYA_Rectf nya_rect_from_corners(f32x2 a, f32x2 b) __attr_no_discard;

/** From a centre and a full size, not a half size. */
NYA_API NYA_Rectf nya_rect_from_center(f32x2 center, f32x2 size) __attr_no_discard;

/** The minimum corner: top left, in the engine's y-down screen space. */
NYA_API f32x2 nya_rect_min(NYA_Rectf rect) __attr_no_discard;

/** The maximum corner. One past the last contained point, because containment is half open. */
NYA_API f32x2 nya_rect_max(NYA_Rectf rect) __attr_no_discard;

NYA_API f32x2 nya_rect_center(NYA_Rectf rect) __attr_no_discard;
NYA_API f32x2 nya_rect_size(NYA_Rectf rect) __attr_no_discard;

/** Zero area, or negative extent on either axis. An empty rectangle contains and overlaps nothing. */
NYA_API b8 nya_rect_is_empty(NYA_Rectf rect) __attr_no_discard;

/** width × height, and zero rather than negative for an empty rectangle. */
NYA_API f32 nya_rect_area(NYA_Rectf rect) __attr_no_discard;

/**
 * Half open: `x <= point.x < x + width`, same on y. Menu items are laid out edge to edge, and a closed
 * test would put the seam in both.
 * */
NYA_API b8 nya_rect_contains(NYA_Rectf rect, f32x2 point) __attr_no_discard;

/** Whether `inner` is entirely within `outer`. An empty `inner` is not contained by anything. */
NYA_API b8 nya_rect_contains_rect(NYA_Rectf outer, NYA_Rectf inner) __attr_no_discard;

/** Whether the two share any point. Touching edges do not, for the same reason containment is half open. */
NYA_API b8 nya_rect_overlaps(NYA_Rectf a, NYA_Rectf b) __attr_no_discard;

/** The shared region, or an empty rectangle when there is none. See nya_rect_is_empty. */
NYA_API NYA_Rectf nya_rect_intersection(NYA_Rectf a, NYA_Rectf b) __attr_no_discard;

/**
 * The smallest rectangle containing both. An empty operand is ignored rather than folded in, so
 * accumulating a bound over a loop can start from a zeroed rectangle without dragging the origin in.
 * */
NYA_API NYA_Rectf nya_rect_union(NYA_Rectf a, NYA_Rectf b) __attr_no_discard;

/** Grows by `amount` on every side, so the width gains twice it. Negative shrinks, and may empty it. */
NYA_API NYA_Rectf nya_rect_expand(NYA_Rectf rect, f32 amount) __attr_no_discard;

NYA_API NYA_Rectf nya_rect_translate(NYA_Rectf rect, f32x2 offset) __attr_no_discard;

/** The point in `rect` closest to `point`. Inside it, that is the point itself. */
NYA_API f32x2 nya_rect_closest_point(NYA_Rectf rect, f32x2 point) __attr_no_discard;

// CIRCLE

/** Half open at the rim, matching the rectangle: a point exactly `radius` away is outside. */
NYA_API b8 nya_circle_contains(NYA_Circlef circle, f32x2 point) __attr_no_discard;

NYA_API b8 nya_circle_overlaps(NYA_Circlef a, NYA_Circlef b) __attr_no_discard;

/** Compares against the closest point on the rectangle, so a corner is handled like any other. */
NYA_API b8 nya_circle_overlaps_rect(NYA_Circlef circle, NYA_Rectf rect) __attr_no_discard;

/** The rectangle that just contains the circle. */
NYA_API NYA_Rectf nya_circle_bounds(NYA_Circlef circle) __attr_no_discard;

// BOX

/** Half open on neither side, so a zero size rectangle becomes a point box rather than nothing. */
NYA_API NYA_Box2f nya_box2_from_rect(NYA_Rectf rect) __attr_no_discard;

NYA_API NYA_Box2f nya_box2_from_center(f32x2 center, f32x2 half_size) __attr_no_discard;
NYA_API b8        nya_box2_overlaps(NYA_Box2f a, NYA_Box2f b) __attr_no_discard;
NYA_API b8        nya_box2_contains(NYA_Box2f box, f32x2 point) __attr_no_discard;
NYA_API NYA_Box2f nya_box2_union(NYA_Box2f a, NYA_Box2f b) __attr_no_discard;

/** Squared distance from `point` to the nearest point of the box, zero inside it. */
NYA_API f32 nya_box2_distance_squared(NYA_Box2f box, f32x2 point) __attr_no_discard;

NYA_API NYA_Box3f nya_box3_from_center(f32x3 center, f32x3 half_size) __attr_no_discard;
NYA_API b8        nya_box3_is_empty(NYA_Box3f box) __attr_no_discard;
NYA_API b8        nya_box3_overlaps(NYA_Box3f a, NYA_Box3f b) __attr_no_discard;
NYA_API b8        nya_box3_contains(NYA_Box3f box, f32x3 point) __attr_no_discard;
NYA_API NYA_Box3f nya_box3_union(NYA_Box3f a, NYA_Box3f b) __attr_no_discard;
NYA_API f32x3     nya_box3_center(NYA_Box3f box) __attr_no_discard;

/** Zero for an empty box. What the surface area heuristic weighs a BVH node by. */
NYA_API f32 nya_box3_surface_area(NYA_Box3f box) __attr_no_discard;

/** Squared distance from `point` to the nearest point of the box, zero inside it. */
NYA_API f32 nya_box3_distance_squared(NYA_Box3f box, f32x3 point) __attr_no_discard;

// SPHERE

/** Closed at the surface, so a sphere of radius zero contains its centre. */
NYA_API b8 nya_sphere_contains(NYA_Spheref sphere, f32x3 point) __attr_no_discard;

NYA_API b8 nya_sphere_overlaps_box3(NYA_Spheref sphere, NYA_Box3f box) __attr_no_discard;

// RAY

/**
 * The slab test. Writes the entry distance, zero when the origin is inside. False past `max_distance`.
 * An axis the ray does not move along is a range check, not a division by zero.
 * */
NYA_API b8 nya_ray2_intersect_box(NYA_Ray2f ray, NYA_Box2f box, f32 max_distance, OUT f32* out_distance) __attr_no_discard;
NYA_API b8 nya_ray3_intersect_box(NYA_Ray3f ray, NYA_Box3f box, f32 max_distance, OUT f32* out_distance) __attr_no_discard;

/** The first surface crossing ahead of the origin, zero when it starts inside. False past `max_distance`. */
NYA_API b8 nya_ray3_intersect_sphere(NYA_Ray3f ray, NYA_Spheref sphere, f32 max_distance, OUT f32* out_distance) __attr_no_discard;
