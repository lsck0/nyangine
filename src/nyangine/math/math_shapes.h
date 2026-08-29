/**
 * @file math_shapes.h
 *
 * Plain 2D shapes and the tests you actually run on them: is this point inside, do these two touch,
 * what is the overlap.
 *
 * Distinct from NYA_Rect (core_window.h): that one is s32 *window geometry* — whole OS pixels, no
 * such thing as a window 1.5 pixels wide. NYA_Rectf here is f32 *content* — hit targets, viewports,
 * sprite boxes — anything laid out or animated, where snapping to integers would put a floor under
 * every animation. They stay separate rather than one converting to the other, so the rounding has
 * nowhere to silently live.
 *
 * Every containment test is half open: `x <= point < x + width`. Two rectangles laid edge to edge
 * therefore share no point, so a click on the seam hits exactly one menu item, not both.
 *
 * A rectangle with negative width or height contains nothing and overlaps nothing — it falls out of
 * the half-open comparisons rather than being special cased, which is why an empty intersection stays
 * empty through a chain instead of coming back to life.
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

typedef struct NYA_Rectf   NYA_Rectf;
typedef struct NYA_Circlef NYA_Circlef;

/**
 * An axis aligned rectangle, as a minimum corner and a size — the form every call site already has (a
 * draw takes an origin and an extent, a sprite has a position and a size); min/max would mean an
 * addition and a subtraction at every one of those.
 *
 * `x`/`y` are the *minimum* corner, which with the engine's y-down screen space means top left.
 * */
struct NYA_Rectf {
    f32 x, y, width, height;
};

/** A circle, as a centre and a radius. The other shape a 2D hit test is ever written against. */
struct NYA_Circlef {
    f32x2 center;
    f32   radius;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * RECTANGLE
 * ─────────────────────────────────────────────────────────
 */

/** From two opposite corners, in either order. Normalized, so the result never has a negative extent. */
NYA_API NYA_Rectf nya_rect_from_corners(f32x2 a, f32x2 b) __attr_no_discard;

/** From a centre and a full size — not a half size, which is the mistake this exists to prevent. */
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
 * Half open: `x <= point.x < x + width`, and the same on y. This is the menu and button hit test; half
 * open because menu items are laid out edge to edge — a closed test puts the seam in both of them.
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

/*
 * ─────────────────────────────────────────────────────────
 * CIRCLE
 * ─────────────────────────────────────────────────────────
 */

/** Half open at the rim, matching the rectangle: a point exactly `radius` away is outside. */
NYA_API b8 nya_circle_contains(NYA_Circlef circle, f32x2 point) __attr_no_discard;

NYA_API b8 nya_circle_overlaps(NYA_Circlef a, NYA_Circlef b) __attr_no_discard;

/** Compares against the closest point on the rectangle, so a corner is handled like any other. */
NYA_API b8 nya_circle_overlaps_rect(NYA_Circlef circle, NYA_Rectf rect) __attr_no_discard;

/** The rectangle that just contains the circle. */
NYA_API NYA_Rectf nya_circle_bounds(NYA_Circlef circle) __attr_no_discard;
