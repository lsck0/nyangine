/**
 * Rectangles and circles, and the two rules the whole module hangs on.
 *
 * The first is that containment is **half open**: `x <= point < x + width`. Menu items are laid out
 * edge to edge, so a closed test puts the seam in both of them and which one wins depends on
 * iteration order. Most of what is checked below is that boundary, on every edge, in both shapes.
 *
 * The second is that an **empty rectangle is inert** — it contains nothing, overlaps nothing, and
 * survives a union without dragging the origin into the result. That is what lets a bound be
 * accumulated over a loop starting from a zeroed rectangle, which is how anyone actually writes it.
 *
 * No app, no world, no clock: this is arithmetic.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Rectangles compare exactly here on purpose: every value below is representable and every
 * operation is one add or one compare, so a tolerance would only hide a real mistake. */
static b8 rect_equals(NYA_Rectf a, NYA_Rectf b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

s32 main(void) {
  nya_backtrace_init();
  defer nya_backtrace_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: constructors
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Either order of corners, because a drag selection produces both.
    NYA_Rectf forward  = nya_rect_from_corners((f32x2){ 10.0F, 20.0F }, (f32x2){ 40.0F, 60.0F });
    NYA_Rectf backward = nya_rect_from_corners((f32x2){ 40.0F, 60.0F }, (f32x2){ 10.0F, 20.0F });

    nya_assert(rect_equals(forward, (NYA_Rectf){ 10.0F, 20.0F, 30.0F, 40.0F }));
    nya_assert(rect_equals(forward, backward), "corner order does not matter; the result is normalized");
    nya_assert(!nya_rect_is_empty(forward), "a drag from either direction produces a real rectangle");

    // Full size, not half size. Getting this backwards is the mistake the constructor exists to stop.
    NYA_Rectf centered = nya_rect_from_center((f32x2){ 0.0F, 0.0F }, (f32x2){ 10.0F, 4.0F });
    nya_assert(rect_equals(centered, (NYA_Rectf){ -5.0F, -2.0F, 10.0F, 4.0F }));

    f32x2 center = nya_rect_center(centered);
    nya_assert(center.x == 0.0F && center.y == 0.0F, "the centre survives the round trip");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: accessors
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Rectf rect = { 10.0F, 20.0F, 30.0F, 40.0F };

    f32x2 min = nya_rect_min(rect);
    f32x2 max = nya_rect_max(rect);

    nya_assert(min.x == 10.0F && min.y == 20.0F);
    nya_assert(max.x == 40.0F && max.y == 60.0F);

    f32x2 size = nya_rect_size(rect);
    nya_assert(size.x == 30.0F && size.y == 40.0F);

    nya_assert(nya_rect_area(rect) == 1200.0F);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: containment is half open on every edge
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Rectf rect = { 0.0F, 0.0F, 10.0F, 10.0F };

    nya_assert(nya_rect_contains(rect, (f32x2){ 5.0F, 5.0F }), "the middle is inside");

    // The minimum corner is in, the maximum corner is out. That asymmetry is the whole point.
    nya_assert(nya_rect_contains(rect, (f32x2){ 0.0F, 0.0F }), "the minimum corner is inside");
    nya_assert(!nya_rect_contains(rect, (f32x2){ 10.0F, 10.0F }), "the maximum corner is outside");
    nya_assert(nya_rect_contains(rect, (f32x2){ 0.0F, 9.999F }));
    nya_assert(!nya_rect_contains(rect, (f32x2){ 10.0F, 5.0F }), "the right edge is outside");
    nya_assert(!nya_rect_contains(rect, (f32x2){ 5.0F, 10.0F }), "the bottom edge is outside");
    nya_assert(!nya_rect_contains(rect, (f32x2){ -0.001F, 5.0F }));

    /*
     * The reason it is half open, stated as the case that motivated it: two menu items stacked edge
     * to edge share no point, so a click on the seam hits exactly one of them.
     */
    NYA_Rectf first  = { 0.0F, 0.0F, 100.0F, 24.0F };
    NYA_Rectf second = { 0.0F, 24.0F, 100.0F, 24.0F };

    f32x2 seam = { 50.0F, 24.0F };

    nya_assert(!nya_rect_contains(first, seam), "the seam belongs to the item below it");
    nya_assert(nya_rect_contains(second, seam));
    nya_assert(!nya_rect_overlaps(first, second), "items laid edge to edge do not overlap");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty rectangle is inert
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Rectf zero     = { 0 };
    NYA_Rectf flat     = { 5.0F, 5.0F, 0.0F, 10.0F };
    NYA_Rectf negative = { 5.0F, 5.0F, -10.0F, -10.0F };
    NYA_Rectf real     = { 0.0F, 0.0F, 10.0F, 10.0F };

    nya_assert(nya_rect_is_empty(zero));
    nya_assert(nya_rect_is_empty(flat), "zero width is empty, not a line segment");
    nya_assert(nya_rect_is_empty(negative), "a negative extent is empty rather than mirrored");

    nya_assert(!nya_rect_contains(zero, (f32x2){ 0.0F, 0.0F }), "nothing is inside an empty rectangle");
    nya_assert(!nya_rect_contains(negative, (f32x2){ 0.0F, 0.0F }));
    nya_assert(!nya_rect_overlaps(zero, real));
    nya_assert(!nya_rect_overlaps(negative, real));

    nya_assert(nya_rect_area(negative) == 0.0F, "area is zero rather than the product of two negatives");
    nya_assert(nya_rect_area(flat) == 0.0F);

    // Contained-ness of an empty rectangle is false, not vacuously true: a collapsed box must not
    // report as being inside something it is nowhere near.
    nya_assert(!nya_rect_contains_rect(real, zero));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: overlap, intersection and union
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Rectf a = { 0.0F, 0.0F, 10.0F, 10.0F };
    NYA_Rectf b = { 5.0F, 5.0F, 10.0F, 10.0F };
    NYA_Rectf c = { 20.0F, 20.0F, 5.0F, 5.0F };

    nya_assert(nya_rect_overlaps(a, b));
    nya_assert(nya_rect_overlaps(b, a), "overlap is symmetric");
    nya_assert(!nya_rect_overlaps(a, c));

    nya_assert(rect_equals(nya_rect_intersection(a, b), (NYA_Rectf){ 5.0F, 5.0F, 5.0F, 5.0F }));

    /*
     * Disjoint boxes intersect to something empty with *zero* extents, not negative ones.
     *
     * A negative extent would carry the distance between them, and feeding that into a union or an
     * expand conjures a rectangle out of two that never touched.
     */
    NYA_Rectf none = nya_rect_intersection(a, c);
    nya_assert(nya_rect_is_empty(none));
    nya_assert(none.width == 0.0F && none.height == 0.0F, "an empty intersection is clamped, not negative");
    nya_assert(nya_rect_is_empty(nya_rect_intersection(nya_rect_intersection(a, c), a)), "empty stays empty when intersected again");

    nya_assert(rect_equals(nya_rect_union(a, b), (NYA_Rectf){ 0.0F, 0.0F, 15.0F, 15.0F }));
    nya_assert(rect_equals(nya_rect_union(a, c), (NYA_Rectf){ 0.0F, 0.0F, 25.0F, 25.0F }));

    // The accumulator case: starting from a zeroed rectangle must not pull the origin in.
    NYA_Rectf accumulated = { 0 };
    accumulated           = nya_rect_union(accumulated, c);
    nya_assert(rect_equals(accumulated, c), "an empty operand is ignored rather than folded in");

    accumulated = nya_rect_union(accumulated, (NYA_Rectf){ 30.0F, 20.0F, 5.0F, 5.0F });
    nya_assert(rect_equals(accumulated, (NYA_Rectf){ 20.0F, 20.0F, 15.0F, 5.0F }));

    nya_assert(rect_equals(nya_rect_union((NYA_Rectf){ 0 }, (NYA_Rectf){ 0 }), (NYA_Rectf){ 0 }), "two empties union to an empty at the origin");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: containment of one rectangle in another
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Rectf outer = { 0.0F, 0.0F, 100.0F, 100.0F };

    nya_assert(nya_rect_contains_rect(outer, (NYA_Rectf){ 10.0F, 10.0F, 10.0F, 10.0F }));
    nya_assert(nya_rect_contains_rect(outer, outer), "a rectangle contains itself");
    nya_assert(!nya_rect_contains_rect(outer, (NYA_Rectf){ 50.0F, 50.0F, 100.0F, 10.0F }), "sticking out on one axis is enough");
    nya_assert(!nya_rect_contains_rect(outer, (NYA_Rectf){ -1.0F, 0.0F, 10.0F, 10.0F }));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: expand, translate and the closest point
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Rectf rect = { 10.0F, 10.0F, 20.0F, 20.0F };

    // Both sides, so the width gains twice the amount. A hit target padded by 4 gets 8 wider.
    nya_assert(rect_equals(nya_rect_expand(rect, 5.0F), (NYA_Rectf){ 5.0F, 5.0F, 30.0F, 30.0F }));
    nya_assert(rect_equals(nya_rect_expand(rect, -5.0F), (NYA_Rectf){ 15.0F, 15.0F, 10.0F, 10.0F }));
    nya_assert(nya_rect_is_empty(nya_rect_expand(rect, -10.0F)), "shrinking past nothing empties it");

    nya_assert(rect_equals(nya_rect_translate(rect, (f32x2){ 5.0F, -5.0F }), (NYA_Rectf){ 15.0F, 5.0F, 20.0F, 20.0F }));

    // Inside, the closest point is the point itself; outside, it is clamped onto the border, corner
    // included. That last case is what makes the circle/rectangle test below correct.
    f32x2 inside = nya_rect_closest_point(rect, (f32x2){ 20.0F, 20.0F });
    nya_assert(inside.x == 20.0F && inside.y == 20.0F);

    f32x2 beside = nya_rect_closest_point(rect, (f32x2){ 100.0F, 20.0F });
    nya_assert(beside.x == 30.0F && beside.y == 20.0F);

    f32x2 corner = nya_rect_closest_point(rect, (f32x2){ -100.0F, -100.0F });
    nya_assert(corner.x == 10.0F && corner.y == 10.0F);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: circles
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Circlef circle = { .center = { 0.0F, 0.0F }, .radius = 10.0F };

    nya_assert(nya_circle_contains(circle, (f32x2){ 0.0F, 0.0F }));
    nya_assert(nya_circle_contains(circle, (f32x2){ 9.99F, 0.0F }));

    // Half open at the rim, matching the rectangle, so touching circles do not both claim a point.
    nya_assert(!nya_circle_contains(circle, (f32x2){ 10.0F, 0.0F }), "a point exactly on the rim is outside");
    nya_assert(!nya_circle_contains(circle, (f32x2){ 8.0F, 8.0F }), "the diagonal is further than the axis");

    NYA_Circlef degenerate = { .center = { 0.0F, 0.0F }, .radius = 0.0F };
    nya_assert(!nya_circle_contains(degenerate, (f32x2){ 0.0F, 0.0F }), "a zero radius circle contains nothing");

    nya_assert(nya_circle_overlaps(circle, (NYA_Circlef){ .center = { 15.0F, 0.0F }, .radius = 10.0F }));
    nya_assert(!nya_circle_overlaps(circle, (NYA_Circlef){ .center = { 20.0F, 0.0F }, .radius = 10.0F }), "circles that just touch do not overlap");
    nya_assert(!nya_circle_overlaps(circle, degenerate));

    nya_assert(rect_equals(nya_circle_bounds(circle), (NYA_Rectf){ -10.0F, -10.0F, 20.0F, 20.0F }));

    /*
     * A circle against a rectangle, including the case the naive test gets wrong.
     *
     * "Is the centre inside, or is any corner within the radius" reports a hit for a circle sitting
     * diagonally off a corner at a distance where nothing actually touches. Comparing against the
     * closest point on the box handles the corner and the edge with the same comparison.
     */
    NYA_Rectf box = { 20.0F, 20.0F, 20.0F, 20.0F };

    nya_assert(!nya_circle_overlaps_rect(circle, box), "far away on the diagonal is a miss");
    nya_assert(nya_circle_overlaps_rect((NYA_Circlef){ .center = { 25.0F, 15.0F }, .radius = 10.0F }, box), "beside an edge is a hit");
    nya_assert(nya_circle_overlaps_rect((NYA_Circlef){ .center = { 25.0F, 25.0F }, .radius = 1.0F }, box), "a circle inside the box is a hit");

    // Just off the corner: 5√2 ≈ 7.07 away, so a radius of 7 misses and 8 hits. This is exactly the
    // pair a corner-distance test gets wrong.
    NYA_Circlef near_corner_miss = { .center = { 15.0F, 15.0F }, .radius = 7.0F };
    NYA_Circlef near_corner_hit  = { .center = { 15.0F, 15.0F }, .radius = 8.0F };

    nya_assert(!nya_circle_overlaps_rect(near_corner_miss, box));
    nya_assert(nya_circle_overlaps_rect(near_corner_hit, box));

    nya_assert(!nya_circle_overlaps_rect(circle, (NYA_Rectf){ 0 }), "nothing overlaps an empty rectangle");
  }

  nya_info("PASSED: test_shapes (0 failures)");

  return EXIT_SUCCESS;
}
