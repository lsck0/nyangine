#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_terrain2d_create(NYA_Arena* arena, NYA_Terrain2DOptions options, OUT NYA_Terrain2D** out_terrain) {
    if (arena == nullptr || out_terrain == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");

    // Zero means "unset" for every field, so a caller can name only what it cares about.
    if (options.half_width <= 0.0F) options.half_width = 2400.0F;
    if (options.point_step <= 0.0F) options.point_step = 28.0F;
    if (options.base_y == 0.0F) options.base_y = 260.0F;
    if (options.amplitude <= 0.0F) options.amplitude = 110.0F;
    if (options.frequency <= 0.0F) options.frequency = 0.0018F;
    if (options.octaves == 0) options.octaves = 4;
    if (options.lacunarity <= 0.0F) options.lacunarity = 2.0F;
    if (options.gain <= 0.0F) options.gain = 0.5F;
    if (options.friction <= 0.0F) options.friction = 0.8F;
    if (options.surface_thickness <= 0.0F) options.surface_thickness = 3.0F;

    if (options.fill.a == 0.0F) options.fill = (NYA_Color){ 0.13F, 0.15F, 0.18F, 1.0F };
    if (options.surface.a == 0.0F) options.surface = (NYA_Color){ 0.35F, 0.62F, 0.42F, 1.0F };

    const u32 count = (u32)((options.half_width * 2.0F) / options.point_step) + 1;
    if (count < 2) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a terrain needs at least two points, got " FMTu32, count);

    NYA_Terrain2D* terrain = nya_arena_alloc(arena, sizeof(NYA_Terrain2D));

    *terrain = (NYA_Terrain2D){
        .allocator   = arena,
        .options     = options,
        .point_count = count,
        .entity      = NYA_ENTITY_HANDLE_NONE,
        .min_height  = options.base_y,
        .max_height  = options.base_y,
    };

    // Allocated once and rewritten in place on regeneration: an arena does not hand memory back.
    terrain->points = nya_arena_alloc(arena, (u64)count * sizeof(f32x2));

    *out_terrain = terrain;

    return NYA_OK;
}

void nya_terrain2d_generate(NYA_Terrain2D* terrain, u64 seed) {
    nya_assert(terrain != nullptr && terrain->points != nullptr, "the terrain must come from nya_terrain2d_create");

    // Immediate rather than deferred: this runs from setup and from a key press, never from inside an
    // entity iteration, and the new body has to exist before this returns.
    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn(terrain->entity);
    terrain->entity = NYA_ENTITY_HANDLE_NONE;

    terrain->seed = seed;

    // The RNG takes its seed as an uppercase hex string of at most 64 digits, left padded — not an
    // arbitrary label. A descriptive one like "terrain-1" is rejected at the first letter.
    char seed_text[17];
    (void)snprintf(seed_text, sizeof(seed_text), "%016llX", (unsigned long long)seed);

    NYA_RNG   rng   = nya_rng_create(.seed = seed_text);
    NYA_Noise noise = nya_noise_create(&rng);

    NYA_NoiseParams params = {
        .octaves    = terrain->options.octaves,
        .lacunarity = terrain->options.lacunarity,
        .gain       = terrain->options.gain,
    };

    /*
     * Sampled left to right at a fixed spacing, which is what makes this a height field rather than an
     * arbitrary polyline: x strictly increases, so no segment can double back and trap something inside
     * the ground.
     *
     * fbm returns roughly [-1, 1]; the low frequency term gives the hills their shape and the higher
     * octaves the small bumps something visibly tumbles over.
     */
    terrain->min_height = FLT_MAX;
    terrain->max_height = -FLT_MAX;

    for (u32 i = 0; i < terrain->point_count; i++) {
        f32 x = -terrain->options.half_width + ((f32)i * terrain->options.point_step);

        f32 height = nya_noise_fbm1(&noise, x * terrain->options.frequency, params);
        f32 y      = terrain->options.base_y + (height * terrain->options.amplitude);

        terrain->points[i] = (f32x2){ x, y };

        terrain->min_height = nya_min(terrain->min_height, y);
        terrain->max_height = nya_max(terrain->max_height, y);
    }

    terrain->entity = nya_entity_spawn(
        .name  = "terrain2d",
        .type  = terrain->options.entity_type,
        .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE | NYA_ENTITY_STATE_STATIC,
    );
    nya_assert(nya_entity_is_valid(terrain->entity), "Failed to spawn the terrain entity.");

    // The points are already in world units relative to the origin, and the entity sits at the origin,
    // so they are its body frame unchanged.
    b8 attached = nya_physics2d_body_attach(
        terrain->entity,
        .type        = NYA_PHYSICS_BODY_STATIC,
        .shape       = NYA_PHYSICS2D_SHAPE_CHAIN,
        .points      = terrain->points,
        .point_count = terrain->point_count,
        .friction    = terrain->options.friction,
        .restitution = terrain->options.restitution,
    );
    nya_assert(attached, "Failed to attach the terrain's chain body.");

    nya_log_info("Terrain2D generated: " FMTu32 " points, seed %llu.", terrain->point_count, (unsigned long long)seed);
}

void nya_terrain2d_release(NYA_Terrain2D* terrain) {
    if (terrain == nullptr) return;

    if (nya_entity_is_valid(terrain->entity)) nya_entity_despawn(terrain->entity);
    terrain->entity = NYA_ENTITY_HANDLE_NONE;
}

f32 nya_terrain2d_height_at(const NYA_Terrain2D* terrain, f32 x) {
    if (terrain == nullptr || terrain->points == nullptr || terrain->point_count < 2) return 0.0F;

    // Clamped rather than extrapolated: past the end the ground is level with its last sample.
    f32 grid = (x + terrain->options.half_width) / terrain->options.point_step;
    grid     = nya_clamp(grid, 0.0F, (f32)(terrain->point_count - 1));

    u32 i = (u32)grid;
    if (i >= terrain->point_count - 1) i = terrain->point_count - 2;

    f32 t = grid - (f32)i;

    return nya_lerp(terrain->points[i].y, terrain->points[i + 1].y, t);
}

void nya_terrain2d_draw(const NYA_Terrain2D* terrain, NYA_Window* window) {
    if (terrain == nullptr || terrain->point_count < 2) return;

    /*
     * Filled as one quad per sample interval, from the surface down to a flat bottom well below the view.
     *
     * Not a triangle fan from a single centre: this is a wide, shallow height field, so a fan would
     * produce long thin slivers whose shared vertex is off screen, and any concavity in the profile
     * would fold the fan back over itself. A quad per interval has neither problem and is the same two
     * triangles the batch is built for.
     */
    f32 bottom = terrain->max_height + 2000.0F;

    for (u32 i = 0; i + 1 < terrain->point_count; i++) {
        f32x2 left  = terrain->points[i];
        f32x2 right = terrain->points[i + 1];

        nya_render2d_triangle(window, left, right, (f32x2){ right.x, bottom }, terrain->options.fill);
        nya_render2d_triangle(window, left, (f32x2){ right.x, bottom }, (f32x2){ left.x, bottom }, terrain->options.fill);
    }

    // The surface line last, so it sits on top of the fill rather than being half covered by it.
    nya_render2d_polyline(window, terrain->points, terrain->point_count, terrain->options.surface_thickness,
                          terrain->options.surface);
}
