#include "nyangine/debug/debug_physics.h"

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/core/core_world.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_scalar.h"
#include "nyangine/math/math_shapes.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/physics/physics2d.h"
#include "nyangine/physics/physics3d.h"
#include "nyangine/physics/physics_types.h"
#include "nyangine/renderer/render2d.h"
#include "nyangine/renderer/render3d.h"

/**
 * How thick a debug line is drawn.
 *
 * The 3D solver works in metres and the 2D one in pixels, so one constant cannot serve both: the same
 * number is a hairline in one and a slab in the other.
 */
#define DEBUG_LINE_THICKNESS_3D 0.02F
#define DEBUG_LINE_THICKNESS_2D 1.50F

/** One turn. <math.h> has no tau and every ring here walks a whole one. */
#define NYA_DEBUG_PHYSICS_TAU ((f32)(2.0 * M_PI))

/** Which colour a body is drawn in, dimmed when the solver has put it to sleep. */
NYA_INTERNAL NYA_Color _nya_debug_physics_color(NYA_PhysicsBodyType type, b8 awake) {
    NYA_Color color = NYA_DEBUG_PHYSICS_COLOR_DYNAMIC;

    switch (type) {
        case NYA_PHYSICS_BODY_STATIC:    color = NYA_DEBUG_PHYSICS_COLOR_STATIC; break;
        case NYA_PHYSICS_BODY_KINEMATIC: color = NYA_DEBUG_PHYSICS_COLOR_KINEMATIC; break;
        case NYA_PHYSICS_BODY_DYNAMIC:   color = NYA_DEBUG_PHYSICS_COLOR_DYNAMIC; break;
        default:                         break;
    }

    if (!awake) color.a *= NYA_DEBUG_PHYSICS_ASLEEP_ALPHA;

    return color;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * 3D
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One ring of `radius` about `center`, in the plane spanned by `right` and `up`.
 *
 * Both axes come in already rotated, so a ring follows the body it belongs to rather than staying axis
 * aligned while the body turns.
 */
NYA_INTERNAL void _nya_debug_physics3d_ring(NYA_Window* window, f32x3 center, f32x3 right, f32x3 up, f32 radius, NYA_Color color) {
    f32x3 previous = center + (right * radius);

    for (u32 i = 1; i <= NYA_DEBUG_PHYSICS_CIRCLE_SEGMENTS; i++) {
        const f32 angle = NYA_DEBUG_PHYSICS_TAU * (f32)i / (f32)NYA_DEBUG_PHYSICS_CIRCLE_SEGMENTS;
        const f32x3 next = center + (right * (cosf(angle) * radius)) + (up * (sinf(angle) * radius));

        nya_render3d_line(window, previous, next, DEBUG_LINE_THICKNESS_3D, color);
        previous = next;
    }
}

u32 nya_debug_physics3d_draw(NYA_Window* window) {
    nya_assert(window != nullptr);

    if (!nya_world_exists()) return 0;

    u32 drawn = 0;

    /*
     * Over the scene rather than in it: a box's outline lies on the faces of the crate it belongs to and
     * would lose the depth test to them half the time. Handed back as found, since the mode outlives the
     * frame and a caller already drawing overlays wants to stay there.
     */
    NYA_Render3DDepth depth_before = nya_render3d_depth(window);
    nya_render3d_depth_set(window, NYA_RENDER3D_DEPTH_OVERLAY);

    nya_entity_foreach (entity) {
        if (!entity->physics3d.attached) continue;

        const NYA_Physics3DBody* body  = &entity->physics3d;
        const NYA_Color          color = _nya_debug_physics_color(body->type, nya_physics3d_awake(entity));

        // The body's own axes, so a shape that is not a box still turns with it.
        const f32x3 right   = nya_quaternion_rotate(entity->rotation, (f32x3){ 1.0F, 0.0F, 0.0F });
        const f32x3 up      = nya_quaternion_rotate(entity->rotation, (f32x3){ 0.0F, 1.0F, 0.0F });
        const f32x3 forward = nya_quaternion_rotate(entity->rotation, (f32x3){ 0.0F, 0.0F, 1.0F });

        switch (body->shape) {
            case NYA_PHYSICS3D_SHAPE_BOX: {
                nya_render3d_cube_outline(window, entity->position, body->size, entity->rotation, DEBUG_LINE_THICKNESS_3D, color);
                drawn++;
            } break;

            case NYA_PHYSICS3D_SHAPE_SPHERE: {
                // Three rings rather than a mesh: enough to read the radius and the orientation from,
                // and it stays three draws however many bodies there are.
                _nya_debug_physics3d_ring(window, entity->position, right, up, body->radius, color);
                _nya_debug_physics3d_ring(window, entity->position, right, forward, body->radius, color);
                _nya_debug_physics3d_ring(window, entity->position, up, forward, body->radius, color);
                drawn++;
            } break;

            case NYA_PHYSICS3D_SHAPE_CAPSULE: {
                // Upright in the body's own frame, so the caps sit half the length along its own up.
                const f32x3 top    = entity->position + (up * (body->length * 0.5F));
                const f32x3 bottom = entity->position - (up * (body->length * 0.5F));

                _nya_debug_physics3d_ring(window, top, right, forward, body->radius, color);
                _nya_debug_physics3d_ring(window, bottom, right, forward, body->radius, color);

                // The four seams, so the tube between the caps reads as a tube.
                const f32x3 offsets[4] = { right * body->radius, right * -body->radius, forward * body->radius, forward * -body->radius };
                for (u32 i = 0; i < 4; i++) nya_render3d_line(window, top + offsets[i], bottom + offsets[i], DEBUG_LINE_THICKNESS_3D, color);

                // And the caps themselves, as the half ring each one actually is.
                _nya_debug_physics3d_ring(window, top, right, up, body->radius, color);
                _nya_debug_physics3d_ring(window, bottom, right, up, body->radius, color);
                drawn++;
            } break;

            // Drawn by nothing: see the header.
            case NYA_PHYSICS3D_SHAPE_MESH:
            case NYA_PHYSICS3D_SHAPE_HEIGHTFIELD:
            default: break;
        }
    }

    nya_render3d_depth_set(window, depth_before);

    return drawn;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * 2D
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One circle of `radius` about `center`, as segments, since render2d draws circles filled. */
NYA_INTERNAL void _nya_debug_physics2d_circle(NYA_Window* window, f32x2 center, f32 radius, f32 rotation, NYA_Color color) {
    f32x2 previous = { center.x + (cosf(rotation) * radius), center.y + (sinf(rotation) * radius) };

    for (u32 i = 1; i <= NYA_DEBUG_PHYSICS_CIRCLE_SEGMENTS; i++) {
        const f32 angle = rotation + (NYA_DEBUG_PHYSICS_TAU * (f32)i / (f32)NYA_DEBUG_PHYSICS_CIRCLE_SEGMENTS);
        const f32x2 next = { center.x + (cosf(angle) * radius), center.y + (sinf(angle) * radius) };

        nya_render2d_line(window, previous, next, DEBUG_LINE_THICKNESS_2D, color);
        previous = next;
    }
}

u32 nya_debug_physics2d_draw(NYA_Window* window) {
    nya_assert(window != nullptr);

    if (!nya_world_exists()) return 0;

    u32 drawn = 0;

    nya_entity_foreach (entity) {
        if (!entity->physics2d.attached) continue;

        const NYA_Physics2DBody* body     = &entity->physics2d;
        const NYA_Color          color    = _nya_debug_physics_color(body->type, nya_physics2d_awake(entity));
        const f32                rotation = nya_physics2d_rotation(entity);
        const f32x2              center   = { entity->position.x, entity->position.y };

        switch (body->shape) {
            case NYA_PHYSICS2D_SHAPE_BOX: {
                nya_render2d_rect_rotated_outline(window, center, body->size, rotation, DEBUG_LINE_THICKNESS_2D, color);
                drawn++;
            } break;

            case NYA_PHYSICS2D_SHAPE_CIRCLE: {
                // The circle, and one spoke to the rim: a circle alone cannot show that it is spinning.
                _nya_debug_physics2d_circle(window, center, body->radius, rotation, color);

                const f32x2 rim = { center.x + (cosf(rotation) * body->radius), center.y + (sinf(rotation) * body->radius) };
                nya_render2d_line(window, center, rim, DEBUG_LINE_THICKNESS_2D, color);
                drawn++;
            } break;

            case NYA_PHYSICS2D_SHAPE_CAPSULE: {
                // Upright in the body's own frame, as in 3D, so the caps are along its own up.
                const f32   half = body->length * 0.5F;
                const f32x2 axis = { -sinf(rotation) * half, cosf(rotation) * half };

                const f32x2 top    = { center.x + axis.x, center.y + axis.y };
                const f32x2 bottom = { center.x - axis.x, center.y - axis.y };

                _nya_debug_physics2d_circle(window, top, body->radius, rotation, color);
                _nya_debug_physics2d_circle(window, bottom, body->radius, rotation, color);

                const f32x2 side = { cosf(rotation) * body->radius, sinf(rotation) * body->radius };
                nya_render2d_line(window, (f32x2){ top.x + side.x, top.y + side.y }, (f32x2){ bottom.x + side.x, bottom.y + side.y },
                                  DEBUG_LINE_THICKNESS_2D, color);
                nya_render2d_line(window, (f32x2){ top.x - side.x, top.y - side.y }, (f32x2){ bottom.x - side.x, bottom.y - side.y },
                                  DEBUG_LINE_THICKNESS_2D, color);
                drawn++;
            } break;

            // Drawn by nothing: see the header.
            case NYA_PHYSICS2D_SHAPE_CHAIN:
            default: break;
        }
    }

    return drawn;
}
