/**
 * @file systems.h
 *
 * Behaviour that belongs to a *property* rather than to a kind.
 *
 * The second of the three ways code is written here. A hook on an entity answers "what does a crate
 * do"; a system answers "what happens to everything that is flammable", and it does that by querying
 * for a flag or a kind and acting on whatever comes back. Neither knows about the other.
 *
 * The movement system is the clearest case. Panning used to live in the camera's own update, which
 * meant "the thing the player drives" and "the thing the world is viewed from" were the same
 * statement — and moving the keys onto a crate would have meant moving code. As a system reading
 * GNY_ENTITY_FLAG_PLAYER_CONTROLLED, it means moving a flag.
 *
 * ## Who runs them
 *
 * The layer that owns the world, from its on_update. Systems are not registered anywhere and the
 * engine does not know they exist — a system is just a function over a query, and the order they run
 * in is a decision the game should be making out loud rather than one buried in a registration
 * order.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MOVEMENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Moves every entity carrying GNY_ENTITY_FLAG_PLAYER_CONTROLLED under the direction keys.
 *
 * Does nothing while a menu is up. Polling reads the keyboard directly and never sees an event, so a
 * modal layer consuming input does not stop it — without the check, holding a direction key drives
 * whatever has the flag behind an open pause menu.
 *
 * An entity with a rigid body has its **velocity** set rather than its position moved: writing a
 * position under the solver every tick fights it, and shows up as a body that shivers against
 * whatever it is resting on instead of sliding.
 * */
void gny_system_player_input_update(f32 delta_time_s);

/**
 * Eases every camera toward the entity carrying GNY_ENTITY_FLAG_CAMERA_TARGET.
 *
 * Nothing happens when no entity carries it, which is what leaves the keys in charge. Following and
 * being driven are two different intentions, so a camera that is chasing something ignores the keys
 * for as long as it is chasing.
 * */
void gny_system_camera_follow_update(f32 delta_time_s);

/**
 * Runs the movement systems, in the order they have to run in.
 *
 * Follow after input, so a camera chasing a player-controlled entity closes on where that entity is
 * *now* rather than trailing it by a tick.
 * */
void gny_system_movement_update(f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RENDERING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Draws the world once per camera: every secondary into its own texture, the primary into the
 * window, then the secondaries composited on top.
 *
 * Run from the game layer's on_render, inside no camera of its own — this sets and resets one per
 * pass. See system_camera.c for why the order is what it is.
 * */
void gny_system_camera_render(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SKY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct GNY_SkyState GNY_SkyState;

/**
 * Everything the time of day decides, derived once.
 *
 * One struct rather than a function per value, because the whole point is that they agree: the sky
 * reddening at dusk while the shadows keep pointing north is worse than no cycle at all, and that is what
 * two callers computing the phase independently produce.
 * */
struct GNY_SkyState {
    /** Where in the day it is, in [0, 1). Zero is midnight, 0.5 is noon. */
    f32 phase;

    /** How far the sun or moon is along its own half of the day, in [0, 1]. */
    f32 arc;

    /** The gradient, top to bottom. */
    NYA_Color top;
    NYA_Color bottom;

    /** The disc in the sky, and the colour of the light it casts. */
    NYA_Color disc;
    NYA_Color light;

    /** Ready for nya_render3d_light_set. `direction` is the way light travels, not where it comes from. */
    f32x3 direction;
    f32   ambient;
    f32   intensity;

    /** How visible the stars are, in [0, 1]. Zero at noon. */
    f32 stars;

    /** Whether the moon is up rather than the sun. */
    b8 is_night;
};

/** Where in the day it is, in [0, 1). Wall clock, so it keeps moving while the simulation is paused. */
f32 gny_sky_phase(void);

/**
 * The sky and the sun for right now.
 *
 * Cheap enough to call more than once a frame — a table lookup, one lerp and a couple of trig calls — so
 * the background layer and the 3D scene each ask rather than passing it between them.
 * */
GNY_SkyState gny_sky_state(void);

/** Draws the gradient, the stars, the disc and the clouds. Called by the background layer. */
void gny_sky_draw(NYA_Window* window);

/** A deterministic value in roughly 0 to 1 from an index, a channel and a seed. See the note at its definition. */
NYA_INTERNAL f32 _gny_sky_random(u32 index, s32 channel, u32 seed);

NYA_INTERNAL void _gny_sky_stars_draw(NYA_Window* window, GNY_SkyState sky);
NYA_INTERNAL void _gny_sky_disc_draw(NYA_Window* window, GNY_SkyState sky);
NYA_INTERNAL void _gny_sky_clouds_draw(NYA_Window* window, GNY_SkyState sky);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TERRAIN 3D
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The 3D scene's ground: a heightmap from fBm noise, drawn as flat triangles and collided against as a
 * static triangle mesh. See system_terrain3d.c for why the collider and the draw are built separately.
 */

typedef struct GNY_Terrain3D GNY_Terrain3D;

/**
 * The 3D scene's ground, as a heightmap and the static body built from it.
 *
 * Held by value on the scene rather than allocated beside it, because it has exactly one instance and
 * its only variable-sized part is the sample grid. See system_terrain3d.c for what is derived from that
 * grid and why the collider and the draw cannot share a representation.
 * */
struct GNY_Terrain3D {
    /**
     * GNY_TERRAIN3D_VERTS squared samples, row major in z then x, from the world's arena.
     *
     * Kept across a regeneration and across leaving the scene: the grid never changes size, and an arena
     * does not hand memory back, so allocating per generation would grow the world by a hundred
     * kilobytes on every press of R.
     * */
    f32* heights;

    /** The static body carrying the triangle mesh. Despawning it releases the mesh Box3D built. */
    NYA_EntityHandle entity;

    /** What produced the current surface. Shown in the HUD, so a landscape can be asked for again. */
    u64 seed;

    /** The extremes of `heights`, for the colour bands and for deciding how high to drop things from. */
    f32 min_height;
    f32 max_height;
};

/** The scene's terrain state. On the world, so it survives a hot reload. */
GNY_Terrain3D* gny_terrain3d(void);

/**
 * Samples the heightmap, builds the collider and spawns the static body carrying it.
 *
 * `arena` is only used the first time, for the sample grid; later calls reuse it. Despawns the previous
 * terrain body first, so calling this again with another seed replaces the surface rather than stacking
 * a second one on top of it.
 * */
void gny_terrain3d_generate(NYA_Window* window, NYA_Arena* arena, u64 seed);

/** Despawns the terrain body and releases its geometry. The sample grid stays; it is the world's. */
void gny_terrain3d_destroy(NYA_Window* window);

/**
 * The ground height at a world xz, bilinear between the four samples around it.
 *
 * Clamped to the terrain rather than extrapolated, and approximate by a few centimetres in the middle of
 * a steep cell — this places things above the ground, it does not resolve contacts. Zero before the
 * first generation.
 * */
f32 gny_terrain3d_height_at(f32 x, f32 z);

/**
 * Draws the surface. Must be called between nya_render3d_begin and _end.
 *
 * One instanced draw of geometry uploaded at generation, not a rebuild — see the note in
 * system_terrain3d.c for what that changed and what it costs.
 * */
void gny_terrain3d_draw(NYA_Window* window);

/** The handle the surface's geometry is registered under. See nya_render3d_mesh_register. */
#define GNY_TERRAIN3D_MESH "gny_terrain3d_mesh"

NYA_INTERNAL f32x3     _gny_terrain3d_corner(const GNY_Terrain3D* terrain, u32 i, u32 j);
NYA_INTERNAL NYA_Color _gny_terrain3d_shade(const GNY_Terrain3D* terrain, f32 height, u32 cell, u32 half);
