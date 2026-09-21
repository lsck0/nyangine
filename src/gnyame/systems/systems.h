/**
 * @file systems.h
 *
 * Game systems. Per-tick ones are registered with nya_system_register in gny_systems_register_all; the rest
 * are helpers the layers call.
 * */
#pragma once

#include "nyangine/nyangine.h"

/** What every system this game registers is tagged with, so the overlay separates its cost from the engine's. */
#define GNY_SYSTEM_OWNER ((NYA_SystemOwner){ .kind = NYA_SYSTEM_OWNER_GAME })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MOVEMENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Moves every entity carrying GNY_ENTITY_FLAG_PLAYER_CONTROLLED under the direction keys.
 * */
void gny_system_player_input_update(f32 delta_time_s);

/**
 * Eases every camera toward the entity carrying GNY_ENTITY_FLAG_CAMERA_TARGET.
 * */
void gny_system_camera_follow_update(f32 delta_time_s);

/**
 * Registers every per-tick system with the engine's system registry, in the order they have to run
 * in, and finalizes the schedule. See system_movement.c for why follow has to come after input.
 *
 * They are registered disabled; the game layer turns them on. Once per process.
 * */
void gny_systems_register_all(void);

/**
 * Turns the gameplay systems on and off as the game layer is pushed and popped, so the menu and the 3D
 * demo tick nothing of the 2D game's. Their state survives: disabling is not unregistering.
 * */
void gny_systems_gameplay_enable(void);
void gny_systems_gameplay_disable(void);

/** Queues the background track, then starts it once loaded. See system_music.c. */
void gny_system_music_update(f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RENDERING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Draws the world once per camera: every secondary into its own texture, the primary into the
 * window, then the secondaries composited on top.
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
 * heightfield.
 */

/**
 * The scene's terrain handle, or null before the first generation.
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
 * */
void gny_terrain3d_update(f32x3 viewer);

/** Draws the surface. Must be called between nya_render3d_begin and _end. */
void gny_terrain3d_draw(NYA_Window* window);

/** Where a decal lands on the terrain. A NYA_Render3DDecalProbe; see nya_render3d_decal_probe_set. */
b8 gny_terrain3d_decal_probe(f32x3 origin, f32x3 direction, void* user_data, OUT f32x3* out_point, OUT f32x3* out_normal);
