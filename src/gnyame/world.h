/**
 * @file world.h
 *
 * Everything the game owns that is not an entity. GNY_World is allocated from the engine world's arena
 * and parked with nya_world_user_data_set, so it lives in the host executable and survives a code
 * reload of this library. DLL globals do not; anything that must outlast a reload goes here.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "gnyame/layers/layer_cube3d.h"

typedef struct GNY_World {
    /** The engine world's arena, which owns this struct and everything below. */
    NYA_Arena* allocator;

    /** What the command line asked for. Read as GNY_LAUNCH. */
    NYA_NetLaunchConfig launch;

    /** The main window, or none on a dedicated server. Read as GNY_WINDOW_MAIN. */
    NYA_WindowHandle window_main;

    /** The 2D ground, created by the first gny_terrain_generate. */
    NYA_Terrain2D*   terrain2d;
    NYA_EntityHandle terrain;
    u64              terrain_seed;

    /** The Tiled map drawn over the terrain, loaded by the game layer. */
    NYA_Tilemap* tilemap;

    /** The main camera entity and the picture-in-picture one, created on demand. */
    NYA_EntityHandle camera;
    NYA_EntityHandle inset_camera;

    /** HUD counters since startup. `hits` and `boxes_lost` are totalled by the sim observer, see sim.h. */
    u32 boxes_spawned;
    u32 boxes_lost;
    u32 hits;

    b8 music_started;

    GNY_Cube3DScene cube3d;

    /** The Lua VM, or null if it could not be created. */
    NYA_LuaVM* lua;
    b8         lua_started;
    f32        lua_tick_timer_s;

    /** Sparks thrown off by crate impacts. */
    NYA_ParticleSystem* sparks;

    /** The offscreen chain the world is composited through for grading and bloom. */
    NYA_PostChain post;
    b8            bloom_enabled;

    /** The grade key's state. The config still names the table and strength; see gny_post_passes. */
    b8 grade_enabled;

    /** The table currently loaded for grading, empty for none, so a config edit can swap or release it. */
    char                 grade_lut[NYA_CONFIG_ASSET_PATH_MAX];
    NYA_ShaderLutUniform grade_uniform;

    /** Seconds added to the clock before the day phase is taken from it. */
    f32 sky_offset_s;

    /** The engine's debug overlay, toggled with `t` in either scene. */
    b8 overlay_enabled;

    /** Which page it shows, `y`, and how the trace page orders its rows, `u`. */
    NYA_DebugOverlayPage overlay_page;
    NYA_TraceSort        trace_sort;

    /** The learning drones, while the 2D scene runs and the config has them on. See robots.h. */
    GNY_Robots* robots;
} GNY_World;

/** Null until gny_world_create has run. */
GNY_World* gny_world(void);

/** Allocates the world, the Lua VM and the named fonts, and registers the game systems. Once per process. */
void gny_world_create(NYA_NetLaunchConfig launch);

/** Registers "ui", "menu" and "menu_title", with their distance field modes. Called by gny_world_create. */
void gny_fonts_register(void);

/** Nothing to release: the engine world's arena owns everything. The partner of gny_world_create. */
void gny_world_destroy(void);

/** Despawns the 2D scene (terrain, crates, map colliders, cameras) and keeps the struct. */
void gny_world_clear(void);

/** Fills the 2D terrain polyline from `seed` and spawns its static body. */
void gny_terrain_generate(u64 seed);

/** Draws the 2D world through `camera`: terrain, map, entities, sparks, then lights. */
void gny_world_draw(NYA_Window* window, NYA_Camera2DTopDown camera);

/** Queues the grayscale shader and pipeline. Safe to call more than once. */
void gny_post_pipelines_ensure(NYA_Window* window);

/** Shows or hides the debug overlay. Showing it also logs every arena with its resident bytes. */
void gny_overlay_toggle(void);

/** The overlay and trace keys, shared by both scenes: toggle, page, sort, log and capture. True when `key` was one. */
b8 gny_overlay_key(const NYA_KeyEvent* key);

/** The debug overlay in the top right corner, on the page the keys chose, if it is on. */
void gny_overlay_draw(NYA_Window* window);

/**
 * Fills `out_passes` with this frame's post passes, at most GNY_POST_PASSES_MAX: the grade when the config names
 * a table and the key has it on. Loads the table on first use and releases it once grading is off, so a disabled
 * grade holds no texture. Returns how many were written.
 * */
u32 gny_post_passes(NYA_Window* window, OUT NYA_PostPass* out_passes);

/**
 * Hands the config's bloom to the renderer, off while the key has it off. A zero field takes `scene`'s number, so
 * each scene keeps its own look until the config sets one.
 * */
void gny_bloom_apply(NYA_Window* window, NYA_PostBloom scene);

/** The inset camera, created on first use. */
NYA_EntityHandle gny_world_inset_camera(void);

/** Runs the startup script once it has loaded, then its optional once-a-second hook. */
void gny_world_script_tick(f32 delta_time_s);

/** Where a window pixel is in the world, under the main camera. */
f32x2 gny_screen_to_world(const NYA_Window* window, f32x2 screen);
