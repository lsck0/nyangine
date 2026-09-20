/**
 * @file entity_ledge.c
 *
 * One-way platforms: static or kinematic 2D bodies with pre-solve filtering, and a marker entity parented
 * to each.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MARKER ANIMATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The id of the marker's one frame event, on its last cell. */
enum { GNY_LEDGE_MARKER_EVENT_PUFF = 1 };

NYA_INTERNAL const NYA_SpriteAnimationEvent _GNY_LEDGE_MARKER_EVENTS[] = {
    { .frame = GNY_LEDGE_MARKER_FRAMES - 1, .id = GNY_LEDGE_MARKER_EVENT_PUFF },
};

NYA_INTERNAL const NYA_SpriteAnimation _GNY_LEDGE_MARKER_ANIMATION = {
    .first_frame       = 0,
    .frame_count       = GNY_LEDGE_MARKER_FRAMES,
    .frames_per_second = GNY_LEDGE_MARKER_FPS,
    .looping           = true,
    .ping_pong         = true,
    .events            = _GNY_LEDGE_MARKER_EVENTS,
    .event_count       = nya_carray_length(_GNY_LEDGE_MARKER_EVENTS),
};

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
        .one_way = NYA_PHYSICS2D_ONE_WAY_UP,

        .layers = nya_physics_layer(GNY_LAYER_LEDGE)
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
     * A marker riding on it, parented rather than moved, cycling through the tileset's cells.
     */
    NYA_Error sheet = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = GNY_LEDGE_MARKER_SHEET });

    // not fatal: the marker draws nothing until a sheet loads.
    if (!sheet.ok) nya_log_warn("%s", (NYA_ConstCString)sheet.message);

    NYA_EntityHandle marker = nya_entity_spawn(
        .name         = "ledge_marker",
        .type         = GNY_ENTITY_LEDGE,
        .position     = { position.x, position.y - (size.y * 0.5F) - GNY_LEDGE_MARKER_LIFT, 0.0F },
        .scale        = { 1.0F, 1.0F, 1.0F },
        .state        = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .on_update    = nya_callback(gny_entity_ledge_marker_on_update),
        .on_animation = nya_callback(gny_entity_ledge_marker_on_animation),
        .visual       = {
            .kind   = NYA_ENTITY_VISUAL_ANIMATION,
            .atlas  = nya_sprite_atlas_grid(GNY_LEDGE_MARKER_SHEET, GNY_LEDGE_MARKER_CELL, GNY_LEDGE_MARKER_CELL),
            .sprite = { .origin = { 0.5F, 0.5F }, .scale = { GNY_LEDGE_MARKER_SCALE, GNY_LEDGE_MARKER_SCALE } },
        },
    );

    if (!nya_entity_is_valid(marker)) return ledge;

    nya_sprite_animator_play(&nya_entity_get(marker)->visual.animator, &_GNY_LEDGE_MARKER_ANIMATION);

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
    f32x2 center = nya_entity_render_position(entity).xy;

    // brighter while moving, so the kinematic ledge is distinguishable from the static one.
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

void gny_entity_ledge_marker_on_update(NYA_Entity* entity, f32 delta_time_s) {
    nya_unused(delta_time_s);

    // read every tick, so an edit to the config file retimes the animation live.
    entity->visual.animator.speed = NYA_CONFIG.game.animation_speed > 0.0F ? NYA_CONFIG.game.animation_speed : GNY_ANIMATION_SPEED;
}

void gny_entity_ledge_marker_on_animation(NYA_Entity* entity, NYA_SpriteAnimationSignal signal) {
    if (signal.kind != NYA_SPRITE_ANIMATION_EVENT || signal.id != GNY_LEDGE_MARKER_EVENT_PUFF) return;

    // the position the hierarchy pass wrote from the ledge's, so the puff follows the patrol.
    (void)nya_particles_emit(
        gny_world()->sparks,
        (NYA_ParticleBurst){
            .shape       = NYA_PARTICLE_SHAPE_CONE,
            .position    = entity->position,
            .count       = GNY_LEDGE_MARKER_SPARKS,
            .direction   = { 0.0F, -1.0F, 0.0F },
            .spread      = GNY_SPARK_SPREAD,
            .speed       = { 60.0F, 160.0F },
            .lifetime_s  = { 0.2F, 0.5F },
            .size        = GNY_SPARK_SIZE_START,
            .size_end    = GNY_SPARK_SIZE_END,
            .color_start = GNY_SPARK_COLOR,
            .color_end   = { 1.0F, 0.35F, 0.05F, 0.0F },
            .gravity     = { 0.0F, GNY_SPARK_GRAVITY, 0.0F },
            .damping     = 1.5F,
        }
    );
}
