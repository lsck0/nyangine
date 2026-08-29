/**
 * @file entity_box.c
 *
 * The crate: a dynamic rigid body that falls, lands, tumbles and eventually goes to sleep.
 *
 * Everything about one lives here — what it is spawned as, what it does per tick, what colour it
 * draws in, and how it is counted. Its update is registered by name, so the whole kind survives a
 * hot reload without the layer that spawned it knowing anything about it.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_EntityHandle gny_entity_box_create(f32x2 position, GNY_EntityFlags flags) {
    GNY_World* world = gny_world();
    nya_assert(world != nullptr, "gny_entity_box_create before the world exists.");

    /*
     * Size and initial spin come out of the spawn counter rather than an RNG.
     *
     * Deterministic, so the same sequence of clicks produces the same pile — which is what makes a
     * physics bug reproducible instead of a story about something that happened once.
     *
     * nya_ihash2 rather than a multiplicative hash written here. The hand-rolled version wrapped on
     * the *second* crate, which aborts a sanitized build: -fsanitize=unsigned-integer-overflow treats
     * wraparound as the accident it usually is, and the engine's hash already carries the
     * __attr_no_sanitize that says the wrapping inside it is deliberate.
     */
    f32 t    = (nya_ihash2((s32)world->boxes_spawned, 0, GNY_BOX_SEED) * 0.5F) + 0.5F;
    f32 size = nya_lerp(GNY_BOX_MIN_SIZE, GNY_BOX_MAX_SIZE, t);
    f32 spin = ((t * 2.0F) - 1.0F) * 4.0F;

    NYA_EntityHandle box = nya_entity_spawn(
        .name             = "box",
        .type             = GNY_ENTITY_BOX,
        .flags            = flags,
        .position         = { position.x, position.y, 0.0F },
        .angular_velocity = { 0.0F, 0.0F, spin },
        .on_update        = nya_callback(gny_entity_box_on_update),
        .on_render        = nya_callback(gny_entity_box_on_render),
        .on_collision     = nya_callback(gny_entity_box_on_collision),
        .on_click         = nya_callback(gny_entity_box_on_click),

        /*
         * Sorted by where it sits rather than by a fixed layer.
         *
         * A pile of crates has no meaningful draw order otherwise — the entity index answers in grid
         * bucket order, so two overlapping crates can swap which is in front whenever one of them
         * moves between cells, which reads as flickering. Taking the order from `position.y` makes the
         * lower crate the nearer one and keeps it stable while nothing moves.
         *
         * No anchor: a crate is drawn from its centre and its body is square, so its centre is as good
         * a stand-in for where it rests as its lowest corner, and the corner moves as it tumbles.
         * */
        .visual = { .y_sorted = true },

        // Every crate glows a little. Not because a wooden box would, but because it is the clearest
        // demonstration the demo can give: a dozen of them falling past each other is a light map
        // that visibly moves, which a single static lamp is not.
        .light = {
            .radius    = GNY_BOX_LIGHT_RADIUS,
            .intensity = GNY_BOX_LIGHT_INTENSITY,
            .color     = GNY_BOX_LIGHT_COLOR,
        },
    );

    if (!nya_entity_is_valid(box)) {
        // The table is full, which nya_entity_spawn has already logged. Clicking is unmetered, so
        // this is a reachable state rather than an impossible one.
        return NYA_ENTITY_HANDLE_NONE;
    }

    b8 attached = nya_physics2d_body_attach(
        box,
        .type        = NYA_PHYSICS_BODY_DYNAMIC,
        .shape       = NYA_PHYSICS2D_SHAPE_BOX,
        .size        = { size, size },
        .density     = 1.0F,
        .friction    = 0.5F,
        .restitution = 0.15F,
    );

    if (!attached) {
        nya_entity_despawn(box);
        return NYA_ENTITY_HANDLE_NONE;
    }

    world->boxes_spawned++;

    return box;
}

void gny_entity_box_destroy(NYA_EntityHandle box) {
    // Guarded on the kind rather than despawning whatever it was handed. This is reachable from a
    // click, and a click resolves to whatever the physics query found — which may be the terrain.
    if (!gny_entity_is(nya_entity_get(box), GNY_ENTITY_BOX)) return;

    nya_entity_despawn_deferred(box);
}

void gny_entity_box_destroy_all(void) {
    // Deferred, because this is reachable from a key handled during an update, and despawning during
    // iteration is exactly what nya_entity_foreach is not safe under.
    nya_entity_foreach (entity) {
        if (!gny_entity_is(entity, GNY_ENTITY_BOX)) continue;

        nya_entity_despawn_deferred(entity->handle);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_entity_box_on_update(NYA_Entity* entity, f32 delta_time_s) {
    nya_unused(delta_time_s);

    // Opt in, per entity. A crate spawned without the flag is left to fall forever, which is
    // occasionally what a test wants and never what the demo does.
    if (!gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CULL_WHEN_LOST)) return;

    // The terrain is finite, so anything that misses its ends keeps falling. Without this the world
    // fills with bodies nobody can see and the solver keeps paying for them — they never sleep,
    // because they never stop accelerating.
    if (entity->position.y < GNY_WORLD_KILL_Y) return;

    nya_sim_record(GNY_SIM_BOX_LOST, &(GNY_SimBoxLost){ .box = entity->handle }, sizeof(GNY_SimBoxLost));

    nya_entity_despawn_deferred(entity->handle);
}

void gny_entity_box_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit) {
    if (!gny_entity_flag_check(entity, GNY_ENTITY_FLAG_AUDIBLE)) return;

    /*
     * Once per collision, not once per side.
     *
     * Both entities in a hit get this callback, so two crates striking each other would record the
     * same impact twice. Acting only for the lower handle index picks exactly one of the pair, and
     * still fires when `other` is the terrain, which has no on_collision of its own.
     */
    if (other != nullptr && other->handle.index < entity->handle.index) return;

    // A fact, not a decision. What it costs — a voice, a counter — is settled at the barrier by
    // gny_sim_observe, which can see every impact in the frame instead of just this one.
    nya_sim_record(
        GNY_SIM_IMPACT,
        &(GNY_SimImpact){
            .a              = entity->handle,
            .b              = other != nullptr ? other->handle : NYA_ENTITY_HANDLE_NONE,
            // The 2D world is the z = 0 plane, so the hit's z is always zero and the game's record
            // keeps the two components it can actually put a sound at. See physics_types.h.
            .point          = hit->point.xy,
            .approach_speed = hit->approach_speed,
        },
        sizeof(GNY_SimImpact)
    );
}

void gny_entity_box_on_click(NYA_Entity* entity, f32x3 world_point, u8 button) {
    nya_unused(world_point);

    // Right click removes it. Left click is the spawn, and the game layer handles that before this
    // is ever reached — a click on empty space has no entity to dispatch to.
    if (button == NYA_MOUSE_BUTTON_RIGHT) {
        gny_entity_box_destroy(entity->handle);
        return;
    }

    /*
     * Middle click hands it the camera.
     *
     * Clicking the one already being followed stops the follow, which is what makes the key both
     * take and release control rather than needing a second way to let go. The camera then finds its
     * GNY_ENTITY_FLAG_PLAYER_CONTROLLED again and the arrow keys work.
     */
    if (button == NYA_MOUSE_BUTTON_MIDDLE) {
        b8 already = gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CAMERA_TARGET);

        // The inset watches it, not the main view — which is the point of having more than one
        // camera. Clicking the crate already being watched closes the inset again.
        gny_entity_camera_follow(gny_world_inset_camera(), already ? NYA_ENTITY_HANDLE_NONE : entity->handle);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 gny_entity_box_count(OUT u32* out_awake) {
    nya_assert(out_awake != nullptr);

    u32 count = 0;
    u32 awake = 0;

    // Walked rather than counted on spawn and despawn, because a crate can also leave through the
    // deferred despawn its own update queues — and a counter with two write paths is a counter that
    // eventually disagrees with the world.
    nya_entity_foreach (entity) {
        if (!gny_entity_is(entity, GNY_ENTITY_BOX)) continue;

        count++;
        if (nya_physics2d_awake(entity)) awake++;
    }

    *out_awake = awake;
    return count;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Color gny_entity_box_color(const NYA_Entity* entity) {
    /*
     * Keyed on the slot, so a crate keeps its colour for its whole life and neighbouring spawns are
     * far apart on the wheel. Recomputed per frame rather than stored: it is a multiply and a
     * modulo, against a pointer the entity would otherwise have to own and free.
     *
     * Widened to u64 before the multiply. 47 and 360 are coprime, which is the point of the
     * arithmetic; it cannot overflow at the current NYA_ENTITY_MAX but would past ~91 million, and
     * the sanitized build is where that would surface rather than anywhere useful.
     */
    f32 hue = (f32)(((u64)entity->handle.index * 47U) % 360U);

    return nya_color_from_hsv((NYA_ColorHSV){ .h = hue, .s = 0.55F, .v = 0.95F, .a = 1.0F });
}

void gny_entity_box_on_render(NYA_Entity* entity, NYA_Window* window) {
    /*
     * One crate, not the whole pile. nya_system_entity_render does the walking and the visibility
     * check, so a kind only has to say what one of it looks like.
     *
     * That is the difference between this and the loop it replaced: adding a second drawable kind
     * used to mean another loop and another call in the layer's on_render, and now means a second
     * on_render and nothing else.
     */
    f32x2 center   = { entity->position.x, entity->position.y };
    f32   rotation = nya_physics2d_rotation(entity);

    NYA_Color color = gny_entity_box_color(entity);

    // Dimmed once the solver has parked it. Not decoration: sleeping is the thing that keeps a large
    // pile cheap, and being able to see which crates are still costing anything is the difference
    // between "the demo is slow" and "these four are still jittering".
    if (!nya_physics2d_awake(entity)) color = nya_color_darken(color, 0.35F);

    nya_render2d_rect_rotated(window, center, entity->physics2d.size, rotation, color);
    nya_render2d_rect_rotated_outline(window, center, entity->physics2d.size, rotation, 1.5F, nya_color_darken(color, 0.55F));
}
