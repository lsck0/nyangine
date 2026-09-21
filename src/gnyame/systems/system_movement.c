/**
 * @file system_movement.c
 *
 * Two registered systems, player input then camera follow, and their registration order.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PLAYER INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_system_player_input_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    // Held keys, so polling is right here in a way it is not for a click: what matters is whether the
    // key is down during this tick, not that it changed.
    if (gny_modal_active()) return;

    // Actions rather than keycodes, so the two keys each direction answers to are the player's to
    // change and are written into the settings file by name. See actions.h.
    f32x2 direction = f32x2_zero;
    if (nya_input_action_pressed(GNY_ACTION_MOVE_LEFT)) direction.x -= 1.0F;
    if (nya_input_action_pressed(GNY_ACTION_MOVE_RIGHT)) direction.x += 1.0F;
    if (nya_input_action_pressed(GNY_ACTION_MOVE_UP)) direction.y -= 1.0F;
    if (nya_input_action_pressed(GNY_ACTION_MOVE_DOWN)) direction.y += 1.0F;

    b8 idle = direction.x == 0.0F && direction.y == 0.0F;

    nya_entity_foreach_flags (GNY_ENTITY_FLAG_PLAYER_CONTROLLED, entity) {
        b8 is_camera = gny_entity_is(entity, GNY_ENTITY_CAMERA);

        // A camera that is chasing something is not also being steered. Asked per camera rather than
        // once, because with more than one camera each has its own answer.
        if (is_camera && nya_entity_is_valid(gny_entity_camera_target(entity->handle))) continue;

        if (entity->physics2d.attached) {
            /*
             * Velocity, not position.
             */
            if (idle) continue;

            f32x2 velocity = nya_physics2d_velocity(entity);
            nya_physics2d_velocity_set(entity, (f32x2){ direction.x * GNY_PLAYER_MOVE_SPEED, velocity.y });

            continue;
        }

        if (idle) continue;

        /*
         * A camera's speed is in screen terms rather than world ones, so it scales with the view:
         * without the divide, panning crawls when zoomed in and flies when zoomed out, because the
         * same world distance covers a different fraction of the screen.
         */
        f32 speed = GNY_PLAYER_MOVE_SPEED;

        if (is_camera) {
            f32 zoom = entity->scale.x > 0.0F ? entity->scale.x : 1.0F;
            speed    = GNY_CAMERA_PAN_SPEED / zoom;
        }

        f32x2 step = direction * (speed * delta_time_s);

        entity->position.x += step.x;
        entity->position.y += step.y;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CAMERA FOLLOW
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_system_camera_follow_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    nya_unused(delta_time_s);

    nya_entity_foreach_kind (GNY_ENTITY_CAMERA, camera) {
        // per camera, so an inset can chase a crate while the main view follows the keys. A despawned
        // target clears the link itself.
        NYA_Entity* target = nya_entity_get(gny_entity_camera_target(camera->handle));
        if (target == nullptr) continue;

        /*
         * Exponential easing toward the target rather than a constant chase speed.
         */
        camera->position.x += (target->position.x - camera->position.x) * GNY_CAMERA_FOLLOW_EASING;
        camera->position.y += (target->position.y - camera->position.y) * GNY_CAMERA_FOLLOW_EASING;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * REGISTRATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every gameplay system, in one place, so enabling and disabling them with the game layer is one loop
 * rather than four calls that can drift apart.
 * */
NYA_INTERNAL NYA_ConstCString _GNY_GAMEPLAY_SYSTEMS[] = { "player_input", "camera_follow", "music", "robots" };

void gny_systems_register_all(void) {
    /*
     * All four sit in the engine's tick between the layer stack and the tweens, which is exactly where
     * they used to run when the game layer drove them by hand. Each says `before` for itself rather
     * than the first one saying it for the group: the order only holds a system back if that system
     * declares it, so an anchor entry would let the tween through as soon as the anchor was placed.
     */
    nya_system_register((NYA_SystemEntry){ .name = "player_input", .after = "layers", .before = "tween_tick", .tick = gny_system_player_input_update, .owner = GNY_SYSTEM_OWNER });

    // After player_input: a camera chasing a player-controlled entity should close on where that
    // entity is now, not on where it was at the start of the tick.
    nya_system_register((NYA_SystemEntry){ .name   = "camera_follow",
                                           .after  = "player_input",
                                           .before = "tween_tick",
                                           .tick   = gny_system_camera_follow_update,
                                           .owner  = GNY_SYSTEM_OWNER });

    nya_system_register((NYA_SystemEntry){ .name = "music", .after = "layers", .before = "tween_tick", .tick = gny_system_music_update, .owner = GNY_SYSTEM_OWNER });

    // after player_input, so the drones chase where the player is this tick.
    nya_system_register((NYA_SystemEntry){ .name = "robots", .after = "player_input", .before = "tween_tick", .tick = gny_robots_update, .owner = GNY_SYSTEM_OWNER });

    // Off until the game layer is up. They used to be driven from that layer's on_update, so they never
    // ran on the menu or in the 3D demo, and the music would start on the title screen if they did now.
    gny_systems_gameplay_disable();

    NYA_EXPECT(nya_system_registry_finalize());
}

void gny_systems_gameplay_enable(void) {
    for (u32 i = 0; i < nya_carray_length(_GNY_GAMEPLAY_SYSTEMS); i++) nya_system_enable(_GNY_GAMEPLAY_SYSTEMS[i]);
}

void gny_systems_gameplay_disable(void) {
    for (u32 i = 0; i < nya_carray_length(_GNY_GAMEPLAY_SYSTEMS); i++) nya_system_disable(_GNY_GAMEPLAY_SYSTEMS[i]);
}
