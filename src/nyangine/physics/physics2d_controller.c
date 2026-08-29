#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_CharacterTuning2D nya_character2d_tuning_defaults(NYA_CharacterTuning2D tuning) {
    if (tuning.max_speed <= 0.0F) tuning.max_speed = 220.0F;
    if (tuning.acceleration <= 0.0F) tuning.acceleration = 2400.0F;
    if (tuning.deceleration <= 0.0F) tuning.deceleration = 2600.0F;
    if (tuning.air_control <= 0.0F) tuning.air_control = 0.65F;
    if (tuning.jump_speed <= 0.0F) tuning.jump_speed = 520.0F;
    if (tuning.coyote_time_s <= 0.0F) tuning.coyote_time_s = 0.10F;
    if (tuning.jump_buffer_s <= 0.0F) tuning.jump_buffer_s = 0.12F;
    if (tuning.gravity <= 0.0F) tuning.gravity = 1400.0F;
    if (tuning.fall_gravity_multiplier <= 0.0F) tuning.fall_gravity_multiplier = 1.9F;
    if (tuning.jump_cut_multiplier <= 0.0F) tuning.jump_cut_multiplier = 0.4F;
    if (tuning.max_fall_speed <= 0.0F) tuning.max_fall_speed = 1200.0F;

    return tuning;
}

void nya_character2d_update(NYA_CharacterController2D* controller, NYA_EntityHandle handle, NYA_CharacterInput2D input, f32 delta_time_s) {
    nya_assert(controller != nullptr);
    if (delta_time_s <= 0.0F) return;

    NYA_Entity* entity = nya_entity_get(handle);
    if (entity == nullptr || !nya_physics2d_body_attached(entity)) return;

    NYA_CharacterTuning2D tuning = nya_character2d_tuning_defaults(controller->tuning);
    controller->tuning           = tuning;

    b8 was_grounded = controller->grounded;

    // Contact-normal based and cached per tick, so a slope is ground and a wall is not.
    b8 grounded = nya_physics2d_grounded(entity);

    controller->landed   = grounded && !was_grounded;
    controller->grounded = grounded;
    controller->jumped   = false;

    /*
     * Coyote time is refilled while grounded and runs down once airborne.
     *
     * Refilled rather than started on leaving, so it is correct however the character left the ground —
     * walking off a ledge, being pushed off, or a platform vanishing all behave the same.
     */
    if (grounded) controller->coyote_left_s = tuning.coyote_time_s;
    else controller->coyote_left_s = nya_max(0.0F, controller->coyote_left_s - delta_time_s);

    // The buffer is armed by the press and runs down regardless, so a press far too early expires.
    if (input.jump_pressed) controller->buffer_left_s = tuning.jump_buffer_s;
    else controller->buffer_left_s = nya_max(0.0F, controller->buffer_left_s - delta_time_s);

    f32x2 velocity = nya_physics2d_velocity(entity);

    /*
     * ── Horizontal ──
     *
     * Accelerated toward the target rather than assigned, so a character has weight; decelerating
     * separately is what lets a stop be crisper than a start, which is most of what "tight" means.
     */
    f32 control = grounded ? 1.0F : tuning.air_control;
    f32 target  = nya_clamp(input.move, -1.0F, 1.0F) * tuning.max_speed;

    f32 rate = (fabsf(target) > 0.01F ? tuning.acceleration : tuning.deceleration) * control;

    if (velocity.x < target) velocity.x = nya_min(velocity.x + (rate * delta_time_s), target);
    else if (velocity.x > target) velocity.x = nya_max(velocity.x - (rate * delta_time_s), target);

    if (fabsf(input.move) > 0.01F) controller->facing = input.move > 0.0F ? 1.0F : -1.0F;
    else if (controller->facing == 0.0F) controller->facing = 1.0F;

    /*
     * ── Jump ──
     *
     * Both windows have to be open, and both are consumed on success — leaving either armed lets one
     * press produce a second jump the instant the character lands.
     */
    if (controller->buffer_left_s > 0.0F && controller->coyote_left_s > 0.0F) {
        velocity.y = -tuning.jump_speed;

        controller->jumping       = true;
        controller->jumped        = true;
        controller->buffer_left_s = 0.0F;
        controller->coyote_left_s = 0.0F;
        controller->grounded      = false;
    }

    // Positive y is down the screen, which is why rising is negative and every comparison here reads
    // backwards from the maths. See NYA_PHYSICS2D_PIXELS_PER_METER.
    b8 rising = velocity.y < 0.0F;

    // Releasing early cuts the rise rather than stopping it: a tap is a hop, a hold is a leap.
    if (controller->jumping && !input.jump_held && rising) {
        velocity.y      *= tuning.jump_cut_multiplier;
        controller->jumping = false;
    }

    if (!rising) controller->jumping = false;

    /*
     * ── Gravity ──
     *
     * Heavier on the way down than on the way up. Honest projectile motion feels floaty, and almost
     * every platformer worth playing lies about it in exactly this way.
     */
    f32 gravity = tuning.gravity * (rising ? 1.0F : tuning.fall_gravity_multiplier);

    velocity.y = nya_min(velocity.y + (gravity * delta_time_s), tuning.max_fall_speed);

    nya_physics2d_velocity_set(entity, velocity);
}

void nya_character2d_reset(NYA_CharacterController2D* controller) {
    if (controller == nullptr) return;

    controller->jumping       = false;
    controller->jumped        = false;
    controller->landed        = false;
    controller->coyote_left_s = 0.0F;
    controller->buffer_left_s = 0.0F;
}
