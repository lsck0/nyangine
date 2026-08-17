/**
 * @file system_movement.c
 *
 * Two systems: what the player drives, and what the camera chases.
 *
 * Both are queries over a flag rather than anything that knows what a camera or a crate is. Between
 * them they are the whole of "things move because someone wanted them to" — gravity and collisions
 * are the solver's, and an entity moving under its own logic is that entity's on_update.
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
             *
             * Writing a rigid body's transform every tick fights the solver: it resolves the contact,
             * this puts the body back, and the result shivers against whatever it is resting on. The
             * vertical component is deliberately left alone so gravity still owns it — driving y
             * directly would make a crate fly.
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
        // Per camera, so an inset can be chasing a crate while the main view is still on the keys.
        // Nothing being followed is what leaves that camera to the keys — not an error, and not a
        // state anyone has to clear, since an entity that despawns takes the link with it.
        NYA_Entity* target = nya_entity_get(gny_entity_camera_target(camera->handle));
        if (target == nullptr) continue;

        /*
         * Exponential easing toward the target rather than a constant chase speed.
         *
         * It starts fast when the gap is large and settles without overshoot, and it never needs to
         * know how fast the target is moving — which matters here because the target is a rigid body
         * whose speed is the solver's business.
         *
         * Framerate dependent in the strict sense, and deliberately so: this runs on the fixed
         * timestep, where the tick length is a constant, so the per tick fraction is stable.
         */
        camera->position.x += (target->position.x - camera->position.x) * GNY_CAMERA_FOLLOW_EASING;
        camera->position.y += (target->position.y - camera->position.y) * GNY_CAMERA_FOLLOW_EASING;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ORDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_system_movement_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    // Input first, follow second: a camera chasing a player-controlled entity should close on where
    // that entity is now, not on where it was at the start of the tick.
    gny_system_player_input_update(delta_time_s);
    gny_system_camera_follow_update(delta_time_s);
}
