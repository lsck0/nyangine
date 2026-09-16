/**
 * @file entity_box.c
 *
 * The crate: spawned with a 2D body and a light, reports impacts to the sim barrier, removed when it falls
 * out of the world, and clickable.
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
         * */
        .visual = { .y_sorted = true },

        // every crate glows a little, so a dozen falling past each other show a moving light map.
        .light = {
            .radius    = GNY_BOX_LIGHT_RADIUS,
            .intensity = GNY_BOX_LIGHT_INTENSITY,
            .color     = GNY_BOX_LIGHT_COLOR,
        },
    );

    if (!nya_entity_is_valid(box)) {
        // the table is full and nya_entity_spawn logged it. Clicking is unmetered, so this is reachable.
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
    // checked, because a click resolves to whatever the physics query hit, which may be the terrain.
    if (!gny_entity_is(nya_entity_get(box), GNY_ENTITY_BOX)) return;

    nya_entity_despawn_deferred(box);
}

void gny_entity_box_destroy_all(void) {
    // deferred: this runs from a key handled during update, and nya_entity_foreach does not allow
    // despawning while iterating.
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

    // opt in per entity. Tests sometimes want a crate that falls forever.
    if (!gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CULL_WHEN_LOST)) return;

    // the terrain is finite. Bodies that miss it never sleep, because they never stop accelerating.
    if (entity->position.y < GNY_WORLD_KILL_Y) return;

    nya_sim_record(GNY_SIM_BOX_LOST, &(GNY_SimBoxLost){ .box = entity->handle }, sizeof(GNY_SimBoxLost));

    nya_entity_despawn_deferred(entity->handle);
}

void gny_entity_box_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit) {
    if (!gny_entity_flag_check(entity, GNY_ENTITY_FLAG_AUDIBLE)) return;

    /*
     * Once per collision, not once per side.
     */
    if (other != nullptr && other->handle.index < entity->handle.index) return;

    // a fact, not a decision. gny_sim_observe sees every impact in the frame and decides what it costs.
    nya_sim_record(
        GNY_SIM_IMPACT,
        &(GNY_SimImpact){
            .a              = entity->handle,
            .b              = other != nullptr ? other->handle : NYA_ENTITY_HANDLE_NONE,
            // the 2D world is the z = 0 plane, so the record keeps x and y. See physics_types.h.
            .point          = hit->point.xy,
            .approach_speed = hit->approach_speed,
        },
        sizeof(GNY_SimImpact)
    );
}

void gny_entity_box_on_click(NYA_Entity* entity, f32x3 world_point, u8 button) {
    nya_unused(world_point);

    // right click removes it. The game layer handles left click spawns before this.
    if (button == NYA_MOUSE_BUTTON_RIGHT) {
        gny_entity_box_destroy(entity->handle);
        return;
    }

    /*
     * Middle click hands it the camera.
     */
    if (button == NYA_MOUSE_BUTTON_MIDDLE) {
        b8 already = gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CAMERA_TARGET);

        // the inset watches it, not the main view. Clicking the watched crate closes the inset.
        gny_entity_camera_follow(gny_world_inset_camera(), already ? NYA_ENTITY_HANDLE_NONE : entity->handle);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 gny_entity_box_count(OUT u32* out_awake) {
    u32 count = 0;
    u32 awake = 0;

    // walked rather than counted: a crate also leaves through its own deferred despawn, and a counter
    // with two write paths drifts.
    nya_entity_foreach (entity) {
        if (!gny_entity_is(entity, GNY_ENTITY_BOX)) continue;

        count++;
        if (nya_physics2d_awake(entity)) awake++;
    }

    // optional: the awake count costs a physics query per crate, and the Lua tick only wants the total.
    if (out_awake != nullptr) *out_awake = awake;

    return count;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Color gny_entity_box_color(const NYA_Entity* entity) {
    /*
     * Keyed on the slot, so a crate keeps its colour for life and neighbours differ. Recomputed per
     * frame since it is a multiply and a modulo.
     */
    f32 hue = (f32)(((u64)entity->handle.index * 47U) % 360U);

    return nya_color_from_hsv((NYA_ColorHSV){ .h = hue, .s = 0.55F, .v = 0.95F, .a = 1.0F });
}

void gny_entity_box_on_render(NYA_Entity* entity, NYA_Window* window) {
    /* One crate. nya_system_entity_render walks and culls. */
    f32x2 center   = { entity->position.x, entity->position.y };
    f32   rotation = nya_physics2d_rotation(entity);

    NYA_Color color = gny_entity_box_color(entity);

    // dimmed once asleep, so it is visible which crates still cost solver time.
    if (!nya_physics2d_awake(entity)) color = nya_color_darken(color, 0.35F);

    nya_render2d_rect_rotated(window, center, entity->physics2d.size, rotation, color);
    nya_render2d_rect_rotated_outline(window, center, entity->physics2d.size, rotation, 1.5F, nya_color_darken(color, 0.55F));
}
