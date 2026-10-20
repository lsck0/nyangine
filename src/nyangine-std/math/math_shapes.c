#include "nyangine-core/nyangine.h"

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

// BOX

/**
 * One axis of the slab test, narrowing [*near, *far]. False once the interval is empty. A ray that does not move on
 * this axis is inside the slab or never in it, which keeps a zero direction out of the division.
 * */
NYA_INTERNAL b8 _nya_ray_slab(f32 origin, f32 direction, f32 min, f32 max, f32* near, f32* far) {
    if (direction == 0.0F) return origin >= min && origin <= max;

    f32 inverse = 1.0F / direction;
    f32 enter   = (min - origin) * inverse;
    f32 leave   = (max - origin) * inverse;

    if (enter > leave) {
        f32 swap = enter;
        enter    = leave;
        leave    = swap;
    }

    *near = nya_max(*near, enter);
    *far  = nya_min(*far, leave);

    return *near <= *far;
}

NYA_Box2f nya_box2_from_rect(NYA_Rectf rect) {
    return (NYA_Box2f){ .min = { rect.x, rect.y }, .max = { rect.x + rect.width, rect.y + rect.height } };
}

NYA_Box2f nya_box2_from_center(f32x2 center, f32x2 half_size) {
    return (NYA_Box2f){ .min = center - half_size, .max = center + half_size };
}

b8 nya_box2_overlaps(NYA_Box2f a, NYA_Box2f b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
}

b8 nya_box2_contains(NYA_Box2f box, f32x2 point) {
    return point.x >= box.min.x && point.x <= box.max.x && point.y >= box.min.y && point.y <= box.max.y;
}

NYA_Box2f nya_box2_union(NYA_Box2f a, NYA_Box2f b) {
    return (NYA_Box2f){ .min = nya_min(a.min, b.min), .max = nya_max(a.max, b.max) };
}

f32 nya_box2_distance_squared(NYA_Box2f box, f32x2 point) {
    f32x2 outside = nya_max(nya_max(box.min - point, point - box.max), f32x2_zero);

    return (outside.x * outside.x) + (outside.y * outside.y);
}

NYA_Box3f nya_box3_from_center(f32x3 center, f32x3 half_size) {
    return (NYA_Box3f){ .min = center - half_size, .max = center + half_size };
}

b8 nya_box3_is_empty(NYA_Box3f box) {
    return box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z;
}

b8 nya_box3_overlaps(NYA_Box3f a, NYA_Box3f b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y && a.min.z <= b.max.z && b.min.z <= a.max.z;
}

b8 nya_box3_contains(NYA_Box3f box, f32x3 point) {
    return point.x >= box.min.x && point.x <= box.max.x && point.y >= box.min.y && point.y <= box.max.y && point.z >= box.min.z
        && point.z <= box.max.z;
}

NYA_Box3f nya_box3_union(NYA_Box3f a, NYA_Box3f b) {
    return (NYA_Box3f){ .min = nya_min(a.min, b.min), .max = nya_max(a.max, b.max) };
}

f32x3 nya_box3_center(NYA_Box3f box) {
    return (box.min + box.max) * 0.5F;
}

f32 nya_box3_surface_area(NYA_Box3f box) {
    if (nya_box3_is_empty(box)) return 0.0F;

    f32x3 size = box.max - box.min;

    return 2.0F * ((size.x * size.y) + (size.y * size.z) + (size.z * size.x));
}

f32 nya_box3_distance_squared(NYA_Box3f box, f32x3 point) {
    f32x3 outside = nya_max(nya_max(box.min - point, point - box.max), f32x3_zero);

    return (outside.x * outside.x) + (outside.y * outside.y) + (outside.z * outside.z);
}

// SPHERE

b8 nya_sphere_contains(NYA_Spheref sphere, f32x3 point) {
    f32x3 offset = point - sphere.center;

    return nya_vector_dot(offset, offset) <= sphere.radius * sphere.radius;
}

b8 nya_sphere_overlaps_box3(NYA_Spheref sphere, NYA_Box3f box) {
    return nya_box3_distance_squared(box, sphere.center) <= sphere.radius * sphere.radius;
}

// RAY

b8 nya_ray2_intersect_box(NYA_Ray2f ray, NYA_Box2f box, f32 max_distance, OUT f32* out_distance) {
    nya_assert(out_distance != nullptr);

    f32 near = 0.0F;
    f32 far  = max_distance;

    if (!_nya_ray_slab(ray.origin.x, ray.direction.x, box.min.x, box.max.x, &near, &far)) return false;
    if (!_nya_ray_slab(ray.origin.y, ray.direction.y, box.min.y, box.max.y, &near, &far)) return false;

    *out_distance = near;

    return true;
}

b8 nya_ray3_intersect_box(NYA_Ray3f ray, NYA_Box3f box, f32 max_distance, OUT f32* out_distance) {
    nya_assert(out_distance != nullptr);

    f32 near = 0.0F;
    f32 far  = max_distance;

    if (!_nya_ray_slab(ray.origin.x, ray.direction.x, box.min.x, box.max.x, &near, &far)) return false;
    if (!_nya_ray_slab(ray.origin.y, ray.direction.y, box.min.y, box.max.y, &near, &far)) return false;
    if (!_nya_ray_slab(ray.origin.z, ray.direction.z, box.min.z, box.max.z, &near, &far)) return false;

    *out_distance = near;

    return true;
}

b8 nya_ray3_intersect_sphere(NYA_Ray3f ray, NYA_Spheref sphere, f32 max_distance, OUT f32* out_distance) {
    nya_assert(out_distance != nullptr);

    f32x3 offset = ray.origin - sphere.center;
    f32   a      = nya_vector_dot(ray.direction, ray.direction);
    f32   b      = nya_vector_dot(offset, ray.direction);
    f32   c      = nya_vector_dot(offset, offset) - (sphere.radius * sphere.radius);

    if (c <= 0.0F) {
        *out_distance = 0.0F;
        return true;
    }

    // outside and moving away, or not moving at all: no crossing ahead.
    if (b >= 0.0F || a == 0.0F) return false;

    f32 discriminant = (b * b) - (a * c);
    if (discriminant < 0.0F) return false;

    f32 distance = (-b - sqrtf(discriminant)) / a;
    if (distance > max_distance) return false;

    *out_distance = distance;

    return true;
}
