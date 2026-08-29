/**
 * @file core_terrain2d.h
 *
 * A 2D height field: 1D fBm noise sampled at a fixed spacing, drawn as filled ground and collided
 * against as a static chain.
 *
 * ```c
 * NYA_Terrain2D* terrain = nullptr;
 * NYA_EXPECT(nya_terrain2d_create(arena, (NYA_Terrain2DOptions){ .entity_type = MY_ENTITY_TERRAIN }, &terrain));
 *
 * nya_terrain2d_generate(terrain, seed);
 * nya_terrain2d_draw(terrain, window);   // each frame, with the world camera set
 * ```
 *
 * **A height field, not an arbitrary polyline.** Samples are taken left to right at a fixed spacing so
 * x is strictly increasing, which is what makes it impossible for a segment to double back and trap
 * something inside the ground.
 *
 * The collider is a `NYA_PHYSICS2D_SHAPE_CHAIN` static body at the origin, and the points are already
 * in world units, so they are its body frame unchanged.
 *
 * Everything comes from the arena passed to nya_terrain2d_create, so there is no destroy — freeing the
 * arena frees the terrain. nya_terrain2d_release exists only for the physics body.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_types.h"
#include "nyangine/renderer/render_color.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Terrain2DOptions NYA_Terrain2DOptions;
typedef struct NYA_Terrain2D        NYA_Terrain2D;

/** Every field has a usable default at zero except `entity_type`, which only the caller knows. */
struct NYA_Terrain2DOptions {
    /** Half the width of the field, in world units, centred on x = 0. Default 2400. */
    f32 half_width;

    /** World units between samples. Smaller is a finer profile and a longer chain. Default 28. */
    f32 point_step;

    /** The height the profile varies around, and by how much. Defaults 260 and 110. */
    f32 base_y;
    f32 amplitude;

    /** fBm parameters. Defaults 0.0018, 4 octaves, lacunarity 2, gain 0.5. */
    f32 frequency;
    u32 octaves;
    f32 lacunarity;
    f32 gain;

    /** Static body tuning. Friction high enough that something landing on a slope settles. Defaults 0.8, 0. */
    f32 friction;
    f32 restitution;

    /** The ground body and the line along its surface. */
    NYA_Color fill;
    NYA_Color surface;

    /** How thick the surface line is drawn. Default 3. */
    f32 surface_thickness;

    /** What the static body is spawned as, in the caller's own entity-type enum. */
    u32 entity_type;
};

struct NYA_Terrain2D {
    /** Everything here came from this. Freeing it frees the terrain. */
    NYA_Arena* allocator;

    NYA_Terrain2DOptions options;

    /**
     * The profile, left to right, in world units. `point_count` of them.
     *
     * Allocated once and rewritten in place on regeneration: an arena does not hand memory back, so
     * allocating per generation would grow it on every reseed.
     * */
    f32x2* points;
    u32    point_count;

    /** The static chain body. Releasing it despawns the entity carrying it. */
    NYA_EntityHandle entity;

    /** What produced the current profile, so a landscape can be asked for again. */
    u64 seed;

    /** The extremes of the profile, for placing things above the ground. */
    f32 min_height;
    f32 max_height;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Allocates the terrain and its profile from `arena`, filling in defaults for anything left zero. */
NYA_API NYA_Error nya_terrain2d_create(NYA_Arena* arena, NYA_Terrain2DOptions options, OUT NYA_Terrain2D** out_terrain)
    __attr_no_discard;

/**
 * Samples the profile and spawns the static chain body carrying it.
 *
 * Despawns the previous body first, so calling this again with another seed replaces the ground rather
 * than stacking a second one on top of it.
 * */
NYA_API void nya_terrain2d_generate(NYA_Terrain2D* terrain, u64 seed);

/** Despawns the body. The profile stays; it belongs to the arena. */
NYA_API void nya_terrain2d_release(NYA_Terrain2D* terrain);

/**
 * The ground height at a world x, linear between the two samples around it.
 *
 * Clamped to the ends rather than extrapolated. Returns the base height before the first generation.
 * */
NYA_API f32 nya_terrain2d_height_at(const NYA_Terrain2D* terrain, f32 x) __attr_no_discard;

/** Draws the filled ground and the surface line. Call with the world camera set. */
NYA_API void nya_terrain2d_draw(const NYA_Terrain2D* terrain, NYA_Window* window);
