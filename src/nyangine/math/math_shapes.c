#include "nyangine/nyangine.h"

// PUBLIC API IMPLEMENTATION

// RECTANGLE

NYA_Rectf nya_rect_from_corners(f32x2 a, f32x2 b) {
    f32 min_x = nya_min(a.x, b.x);
    f32 min_y = nya_min(a.y, b.y);

    return (NYA_Rectf){
        .x      = min_x,
        .y      = min_y,
        .width  = nya_max(a.x, b.x) - min_x,
        .height = nya_max(a.y, b.y) - min_y,
    };
}

NYA_Rectf nya_rect_from_center(f32x2 center, f32x2 size) {
    return (NYA_Rectf){
        .x      = center.x - (size.x * 0.5F),
        .y      = center.y - (size.y * 0.5F),
        .width  = size.x,
        .height = size.y,
    };
}

f32x2 nya_rect_min(NYA_Rectf rect) {
    return (f32x2){ rect.x, rect.y };
}

f32x2 nya_rect_max(NYA_Rectf rect) {
    return (f32x2){ rect.x + rect.width, rect.y + rect.height };
}

f32x2 nya_rect_center(NYA_Rectf rect) {
    return (f32x2){ rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };
}

f32x2 nya_rect_size(NYA_Rectf rect) {
    return (f32x2){ rect.width, rect.height };
}

b8 nya_rect_is_empty(NYA_Rectf rect) {
    // `<=` so a zero-width rectangle counts as empty, not a line segment; otherwise nya_rect_intersection could return a box containing no points.
    return rect.width <= 0.0F || rect.height <= 0.0F;
}

f32 nya_rect_area(NYA_Rectf rect) {
    if (nya_rect_is_empty(rect)) return 0.0F;

    return rect.width * rect.height;
}

b8 nya_rect_contains(NYA_Rectf rect, f32x2 point) {
    if (point.x < rect.x || point.x >= rect.x + rect.width) return false;
    if (point.y < rect.y || point.y >= rect.y + rect.height) return false;

    return true;
}

b8 nya_rect_contains_rect(NYA_Rectf outer, NYA_Rectf inner) {
    // An empty inner rectangle is vacuously contained anywhere; answer false so a collapsed box is not reported inside something far away.
    if (nya_rect_is_empty(inner)) return false;

    if (inner.x < outer.x || inner.y < outer.y) return false;
    if (inner.x + inner.width > outer.x + outer.width) return false;
    if (inner.y + inner.height > outer.y + outer.height) return false;

    return true;
}

b8 nya_rect_overlaps(NYA_Rectf a, NYA_Rectf b) {
    if (nya_rect_is_empty(a) || nya_rect_is_empty(b)) return false;

    if (a.x + a.width <= b.x || b.x + b.width <= a.x) return false;
    if (a.y + a.height <= b.y || b.y + b.height <= a.y) return false;

    return true;
}

NYA_Rectf nya_rect_intersection(NYA_Rectf a, NYA_Rectf b) {
    f32 min_x = nya_max(a.x, b.x);
    f32 min_y = nya_max(a.y, b.y);
    f32 max_x = nya_min(a.x + a.width, b.x + b.width);
    f32 max_y = nya_min(a.y + a.height, b.y + b.height);

    // Clamp the extent to zero, not negative: a negative extent carries the gap between the boxes and would conjure a rectangle in a union or expand.
    return (NYA_Rectf){
        .x      = min_x,
        .y      = min_y,
        .width  = nya_max(0.0F, max_x - min_x),
        .height = nya_max(0.0F, max_y - min_y),
    };
}

NYA_Rectf nya_rect_union(NYA_Rectf a, NYA_Rectf b) {
    // An empty operand is ignored, so a bound accumulated in a loop can start from a zeroed rectangle without dragging in the origin.
    if (nya_rect_is_empty(a)) return nya_rect_is_empty(b) ? (NYA_Rectf){ 0 } : b;
    if (nya_rect_is_empty(b)) return a;

    f32 min_x = nya_min(a.x, b.x);
    f32 min_y = nya_min(a.y, b.y);

    return (NYA_Rectf){
        .x      = min_x,
        .y      = min_y,
        .width  = nya_max(a.x + a.width, b.x + b.width) - min_x,
        .height = nya_max(a.y + a.height, b.y + b.height) - min_y,
    };
}

NYA_Rectf nya_rect_expand(NYA_Rectf rect, f32 amount) {
    return (NYA_Rectf){
        .x      = rect.x - amount,
        .y      = rect.y - amount,
        .width  = rect.width + (amount * 2.0F),
        .height = rect.height + (amount * 2.0F),
    };
}

NYA_Rectf nya_rect_translate(NYA_Rectf rect, f32x2 offset) {
    return (NYA_Rectf){
        .x      = rect.x + offset.x,
        .y      = rect.y + offset.y,
        .width  = rect.width,
        .height = rect.height,
    };
}

f32x2 nya_rect_closest_point(NYA_Rectf rect, f32x2 point) {
    return (f32x2){
        nya_clamp(point.x, rect.x, rect.x + rect.width),
        nya_clamp(point.y, rect.y, rect.y + rect.height),
    };
}

// CIRCLE

b8 nya_circle_contains(NYA_Circlef circle, f32x2 point) {
    if (circle.radius <= 0.0F) return false;

    f32x2 offset = point - circle.center;

    // Squared, so there is no square root on a test that runs per entity per click.
    return nya_vector_dot(offset, offset) < circle.radius * circle.radius;
}

b8 nya_circle_overlaps(NYA_Circlef a, NYA_Circlef b) {
    if (a.radius <= 0.0F || b.radius <= 0.0F) return false;

    f32x2 offset = b.center - a.center;
    f32   reach  = a.radius + b.radius;

    return nya_vector_dot(offset, offset) < reach * reach;
}

b8 nya_circle_overlaps_rect(NYA_Circlef circle, NYA_Rectf rect) {
    if (circle.radius <= 0.0F || nya_rect_is_empty(rect)) return false;

    // Test against the closest point on the box, which is correct for a circle beside an edge or tucked into a corner alike.
    f32x2 closest = nya_rect_closest_point(rect, circle.center);
    f32x2 offset  = circle.center - closest;

    return nya_vector_dot(offset, offset) < circle.radius * circle.radius;
}

NYA_Rectf nya_circle_bounds(NYA_Circlef circle) {
    return (NYA_Rectf){
        .x      = circle.center.x - circle.radius,
        .y      = circle.center.y - circle.radius,
        .width  = circle.radius * 2.0F,
        .height = circle.radius * 2.0F,
    };
}
