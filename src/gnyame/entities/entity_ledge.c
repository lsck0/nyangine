/**
 * @file entity_ledge.c
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_EntityHandle gny_entity_ledge_create(f32x2 position, f32x2 size, f32 patrol_distance) {
    b8 moving = patrol_distance > 0.0F;

    NYA_EntityHandle ledge = nya_entity_spawn(
        .name      = moving ? "ledge_moving" : "ledge",
        .type      = GNY_ENTITY_LEDGE,
        .position  = { position.x, position.y, 0.0F },
        .scale     = { 1.0F, 1.0F, 1.0F },
        .state     = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .on_render = nya_callback(gny_entity_ledge_on_render)
    );

    if (!nya_entity_is_valid(ledge)) return ledge;

    /*
     * Kinematic when it patrols, static when it does not.
     */
    nya_physics2d_body_attach(
        ledge,
        .type     = moving ? NYA_PHYSICS_BODY_KINEMATIC : NYA_PHYSICS_BODY_STATIC,
        .shape    = NYA_PHYSICS2D_SHAPE_BOX,
        .size     = size,
        .friction = GNY_LEDGE_FRICTION,

        // Passable from below: a crate thrown up through it goes through and lands back on top.
        .one_way = NYA_PHYSICS2D_ONE_WAY_UP
    );

    if (!moving) return ledge;

    /*
     * The patrol, as one repeating tween rather than as state advanced in an on_update.
     */
    nya_entity_move_to_with_options(
        nya_entity_get(ledge), (f32x3){ position.x + patrol_distance, position.y, 0.0F }, GNY_LEDGE_PATROL_SECONDS,
        (NYA_TweenOptions){
            .ease   = NYA_EASE_SINE_IN_OUT,
            .repeat = NYA_TWEEN_REPEAT_FOREVER,
            .yoyo   = true,
        }
    );

    /*
     * A marker riding on it, parented rather than moved.
     */
    NYA_EntityHandle marker = nya_entity_spawn(
        .name      = "ledge_marker",
        .type      = GNY_ENTITY_LEDGE,
        .position  = { position.x, position.y - (size.y * 0.5F) - GNY_LEDGE_MARKER_LIFT, 0.0F },
        .scale     = { 1.0F, 1.0F, 1.0F },
        .state     = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .on_render = nya_callback(gny_entity_ledge_marker_on_render)
    );

    (void)nya_entity_parent_set(marker, ledge);

    return ledge;
}

void gny_entity_ledge_destroy_all(void) {
    // Collected before any is despawned: despawning a parented ledge takes its marker with it, and
    // removing entities while iterating the index is what the deferred queue exists to avoid.
    nya_entity_foreach_kind (GNY_ENTITY_LEDGE, entity) nya_entity_despawn_deferred(entity->handle);
}

u32 gny_entity_ledge_drop_everything_through(f32 seconds) {
    u32 dropped = 0;

    /*
     * Every crate, not the ones known to be standing on a ledge.
     */
    nya_entity_foreach_kind (GNY_ENTITY_BOX, entity) {
        if (!entity->physics2d.attached) continue;

        nya_physics2d_drop_through(entity, seconds);
        dropped++;
    }

    return dropped;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CALLBACKS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_entity_ledge_on_render(NYA_Entity* entity, NYA_Window* window) {
    f32x2 center = { entity->position.x, entity->position.y };

    // Brighter while it is moving, so which ledge is the kinematic one is visible without reading the
    // code — the two are otherwise identical rectangles.
    NYA_Color color = nya_entity_moving(entity) ? GNY_LEDGE_COLOR_MOVING : GNY_LEDGE_COLOR;

    nya_render2d_rect_rotated(window, center, entity->physics2d.size, 0.0F, color);

    /*
     * A line along the top edge only.
     */
    f32 half_width  = entity->physics2d.size.x * 0.5F;
    f32 half_height = entity->physics2d.size.y * 0.5F;

    nya_render2d_line(window, (f32x2){ center.x - half_width, center.y - half_height },
                      (f32x2){ center.x + half_width, center.y - half_height }, GNY_LEDGE_EDGE_THICKNESS, GNY_LEDGE_EDGE_COLOR);
}

void gny_entity_ledge_marker_on_render(NYA_Entity* entity, NYA_Window* window) {
    // Nothing here reads the parent. The transform this draws at was written by the hierarchy pass
    // from the ledge's, which is the property worth being able to see moving.
    nya_render2d_circle(window, (f32x2){ entity->position.x, entity->position.y }, GNY_LEDGE_MARKER_RADIUS, GNY_LEDGE_MARKER_COLOR);
}
