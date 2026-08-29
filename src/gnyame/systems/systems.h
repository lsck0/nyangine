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
 * gny_systems_register_all hands the per-frame ones to the engine's system registry once, from
 * gny_world_create; the layer that owns the world then runs them all with one
 * nya_system_registry_run_update from its on_update, instead of calling each by name. A system is
 * still just a function over a query, and the order is still a decision the game makes out loud —
 * it is just spelled as `after` in a registration call now, rather than as call order in a layer.
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
 * Registers every per-frame system with the engine's system registry, in the order they have to run
 * in, and finalizes the schedule. See system_movement.c for why follow has to come after input.
 *
 * Called once, from gny_world_create — before the world's first on_update, which is the only thing
 * the registry requires of whoever calls it.
 * */
void gny_systems_register_all(void);

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

/**
 * The scene's terrain handle, or null before the first generation.
 *
 * The terrain itself is NYA_Terrain3D in the engine; these are the game's wrappers over it, holding the
 * shape constants from constants.h and GNY_ENTITY_TERRAIN so call sites do not repeat them.
 * */
NYA_Terrain3D* gny_terrain3d(void);

/** Creates the terrain on first call, then samples it. `arena` is only used the first time. */
void gny_terrain3d_generate(NYA_Window* window, NYA_Arena* arena, u64 seed);

/** Despawns the terrain body and releases its geometry. The sample grid stays; it is the world's. */
void gny_terrain3d_destroy(NYA_Window* window);

/** The ground height at a world xz. Zero before the first generation. See nya_terrain3d_height_at. */
f32 gny_terrain3d_height_at(f32 x, f32 z);

/**
 * Re-picks each terrain chunk's detail level from where the viewer is.
 *
 * Once per frame, from the 3D layer's on_update. A no-op before the terrain is created, and cheap on
 * the frames where nothing crosses a level boundary — see nya_terrain3d_update.
 * */
void gny_terrain3d_update(f32x3 viewer);

/** Draws the surface. Must be called between nya_render3d_begin and _end. */
void gny_terrain3d_draw(NYA_Window* window);
