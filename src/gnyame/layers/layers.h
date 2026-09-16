/**
 * @file layers.h
 * */
#pragma once

// nyangine is a different subsystem, so its umbrella. Not gnyame.h: that is this subsystem's own
// umbrella and it includes this file, so naming it here is a cycle that only #pragma once hides.
#include "nyangine/nyangine.h"

// Named here rather than left to gnyame.h having gone first, so this header still compiles on its
// own — which is what clangd does to it, and what makes GNY_TERRAIN_POINT_COUNT resolve in an editor.
#include "gnyame/constants.h"
#include "gnyame/entities/entities.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum GNY_MenuAction GNY_MenuAction;
typedef struct GNY_Cube3DScene GNY_Cube3DScene;
typedef struct GNY_Menu     GNY_Menu;
typedef struct GNY_MenuItem   GNY_MenuItem;
typedef enum GNY_MenuItemKind GNY_MenuItemKind;
typedef struct GNY_World    GNY_World;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MENUS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * What choosing an item does.
 * */
enum GNY_MenuAction {
    /** Nothing was chosen this event. The usual answer. */
    GNY_MENU_ACTION_NONE = 0,

    GNY_MENU_ACTION_START,
    GNY_MENU_ACTION_RESUME,
    GNY_MENU_ACTION_RESTART,
    GNY_MENU_ACTION_MAIN_MENU,
    GNY_MENU_ACTION_CUBE3D,
    GNY_MENU_ACTION_QUIT,
};

/**
 * Everything the 3D demo remembers between frames.
 * */
/** One of the cubes dropped onto the terrain. The pool is fixed; see GNY_TERRAIN3D_CUBE_COUNT. */
typedef struct GNY_FallingCube GNY_FallingCube;

struct GNY_FallingCube {
    NYA_EntityHandle entity;

    /** Full edge length in metres. Kept here because the draw needs it and the body does not expose it. */
    f32 size;

    NYA_Color color;

    /**
     * Whether this one is glass: translucent, and drawn in the sorted transparent pass.
     * */
    b8 glass;

    /**
     * How frosted this one is, in [0, 1]. Meaningless unless `glass`.
     * */
    f32 blur;
};

struct GNY_Cube3DScene {
    NYA_EntityHandle cube;

    /**
     * The two loaded models, as simulated bodies rather than decorations.
     * */
    NYA_EntityHandle model;
    NYA_EntityHandle pill;

    /** The noise-generated ground everything in this scene stands on. */
    NYA_Terrain3D* terrain;

    /**
     * The pile, in a fixed array rather than found by querying for GNY_ENTITY_CUBE3D.
     * */
    GNY_FallingCube cubes[GNY_TERRAIN3D_CUBE_COUNT];

    /** How many of `cubes` are live. Zero until the first drop. */
    u32 cube_count;

    /** How many have been recycled after falling off the world. Shown in the HUD. */
    u32 cubes_recycled;

    /** Radians. Pitch is clamped short of the poles; see the note in gny_layer_cube3d_on_event. */
    f32 orbit_yaw;
    f32 orbit_pitch;

    /** Metres from the target. */
    f32 orbit_range;

    /** Whether the left button is down *on the cube*. Not the same as the button being down. */
    b8 dragging;

    /** Whether it has ever been grabbed, so the hint can stop telling someone what they already did. */
    b8 grabbed_once;

    /**
     * Dust kicked up where the cube lands.
     * */
    NYA_ParticleSystem* dust;

    /**
     * The fire and the smoke above it, as two systems rather than one.
     * */
    NYA_ParticleSystem* fire;
    NYA_ParticleSystem* smoke;

    /** How long since the plume last emitted, so it feeds continuously rather than once. */
    f32 plume_timer_s;
};

/**
 * What a menu row *is*, which decides what left and right do to it.
 * */
enum GNY_MenuItemKind {
    /** Chosen with confirm, does one thing. The ordinary row. */
    GNY_MENU_ITEM_KIND_ACTION = 0,

    /**
     * A volume, edited in place with left and right. `action` is ignored.
     * */
    GNY_MENU_ITEM_KIND_VOLUME,
};

struct GNY_MenuItem {
    NYA_ConstCString label;

    /** ACTION unless stated, so an ordinary row is still one line with two fields. */
    GNY_MenuItemKind kind;

    /** What choosing this row does. Meaningless for a VOLUME row. */
    GNY_MenuAction action;

    /** Which mix a VOLUME row edits. Meaningless for an ACTION row. */
    NYA_VolumeChannel channel;
};

/**
 * One menu: a title, a fixed list of items, and which of them is highlighted.
 * */
struct GNY_Menu {
    NYA_ConstCString title;

    /** What the title says underneath, in the dim colour. Optional. */
    NYA_ConstCString subtitle;

    const GNY_MenuItem* items;
    u32                 item_count;

    /**
     * The highlighted item.
     * */
    u32 selected;
};

/*
 * ── Shared menu behaviour ──
 *
 * Both menus are the same widget with different items, so the navigation, the hit testing and the
 * drawing live here once. A layer supplies the list and reacts to what comes back.
 */

/**
 * Where an item sits on screen, in the window's pixels.
 * */
NYA_Rectf gny_menu_item_bounds(const NYA_Window* window, const GNY_Menu* menu, u32 index);

/**
 * Runs the menu's navigation against one event.
 * */
b8 gny_menu_handle_event(const NYA_Window* window, GNY_Menu* menu, const NYA_Event* event, OUT GNY_MenuAction* out_action);

/** The scrim, the panel, the title and the items. Screen space; the caller has no camera to reset. */
void gny_menu_draw(NYA_Window* window, const GNY_Menu* menu);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCREENS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Rearranges the layer stack, at the next simulation barrier.
 * */
void gny_screen_start_game(void);
void gny_screen_pause(void);
void gny_screen_resume(void);
void gny_screen_main_menu(void);

/** Swaps the main menu for the 3D demo. Escape inside it comes back. */
void gny_screen_cube3d(void);
void gny_screen_restart(void);
void gny_screen_quit(void);

/**
 * Whether a menu is on top of the world right now.
 * */
b8 gny_modal_active(void);

/**
 * Everything the demo owns that is not an entity.
 * */
struct GNY_World {
    /** Owns everything below it, including this struct. Outlives every reload. */
    NYA_Arena* allocator;

    /** The 2D ground. Null until gny_terrain_generate first runs. See core_terrain2d.h. */
    NYA_Terrain2D* terrain2d;

    /** The ground body, mirrored from the terrain so existing call sites keep working. */
    NYA_EntityHandle terrain;

    /** Seeds the terrain. Kept so regenerating produces a different shape rather than the same one. */
    u64 terrain_seed;

    /**
     * The Tiled map drawn over the terrain, or null before the game layer loads it.
     * */
    NYA_Tilemap* tilemap;

    /**
     * The camera entity. Read it as an NYA_Camera2DTopDown with gny_entity_camera_get.
     * */
    NYA_EntityHandle camera;

    /**
     * The picture-in-picture camera, created the first time something is watched.
     * */
    NYA_EntityHandle inset_camera;

    /** Counters the UI reports. Cumulative since startup, not since the last clear. */
    u32 boxes_spawned;
    u32 boxes_lost;

    /**
     * Impacts the physics world has reported, total.
     * */
    /** Impacts and losses, totalled by the simulation observer at the end of each frame. See sim.h. */
    u32 hits;

    /**
     * Whether the background track has been started yet.
     * */
    b8 music_started;

    /** The two menus' highlighted item, kept here so it survives a hot reload and a pop/push cycle. */
    GNY_Menu main_menu;
    GNY_Menu pause_menu;

    /** The 3D demo's state. Zeroed until its layer is pushed. See GNY_Cube3DScene. */
    GNY_Cube3DScene cube3d;

    /*
     * ── Scripting ──
     */

    /**
     * The Lua VM, or null before the startup script has loaded.
     *
     * ⚠ **Here rather than in a static inside a .c**, which is the first of lua.h's two hot-reload
     * rules: GNY_World lives in the engine world's arena, which lives in the *host* executable, so
     * this survives the game library being replaced. A pointer in the game's own data would not.
     * */
    NYA_LuaVM* lua;

    /** Seconds since the script's tick hook last ran. It is a once-a-second hook, not a per-frame one. */
    f32 lua_tick_timer_s;

    /**
     * Whether the startup script has been run.
     * */
    b8 lua_started;

    /**
     * Sparks thrown off by crate impacts.
     * */
    NYA_ParticleSystem* sparks;

    /*
     * ── Post processing ──
     */

    /**
     * The offscreen target the world is drawn into before the bloom pass reads it back.
     * */
    /**
     * The post-processing chain the world is composited through.
     * */
    NYA_PostChain post;

    /** Toggled with `b`. Off draws the scene straight to the window with no second pass. */
    b8 bloom_enabled;

    /**
     * Seconds added to the clock before the day phase is taken from it.
     * */
    f32 sky_offset_s;

    /** Toggled with `t`. Draws the previous frame's perf spans over the HUD. See layer_ui.c. */
    b8 trace_enabled;

    /** Latches the one-shot frame report, so it lands in the log once rather than every frame. */
    b8 trace_logged;
};

/** Null until gnyame_init has run. Every layer goes through this rather than holding its own copy. */
GNY_World* gny_world(void);

/** Allocates the world and parks it in NYA_App.state. Called once, before any window exists. */
void gny_world_create(void);

/**
 * Despawns the terrain and every crate, leaving the world struct itself intact.
 * */
void gny_world_clear(void);

/**
 * The inset camera, creating it on first use.
 * */
NYA_EntityHandle gny_world_inset_camera(void);

/**
 * Runs the startup script once it has loaded, then its optional per-second hook.
 * */
void gny_world_script_tick(f32 delta_time_s);

/** Releases the arena. Everything reachable from GNY_World is invalid afterwards. */
void gny_world_destroy(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layers_init(void);

/**
 * The title screen. The only thing over the background at startup.
 * */
void*     GNY_LAYER_MAIN_MENU_ID = "gny_layer_main_menu";
NYA_Layer GNY_LAYER_MAIN_MENU;
void      gny_layer_main_menu_on_create(NYA_Window* window);
void      gny_layer_main_menu_on_destroy(NYA_Window* window);
void      gny_layer_main_menu_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_main_menu_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_main_menu_on_render(NYA_Window* window);

/**
 * The pause menu, over a stopped world.
 * */
void*     GNY_LAYER_PAUSE_MENU_ID = "gny_layer_pause_menu";
NYA_Layer GNY_LAYER_PAUSE_MENU;
void      gny_layer_pause_menu_on_create(NYA_Window* window);
void      gny_layer_pause_menu_on_destroy(NYA_Window* window);
void      gny_layer_pause_menu_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_pause_menu_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_pause_menu_on_render(NYA_Window* window);

/** Sky, parallax hills and drifting motes. Draws behind the world and reads nothing but the camera. */
/** The 3D demo: one perspective camera, one Box3D body, one cube you can click and spin. */
void*     GNY_LAYER_CUBE3D_ID = "gny_layer_cube3d";
NYA_Layer GNY_LAYER_CUBE3D;
void      gny_layer_cube3d_on_create(NYA_Window* window);
void      gny_layer_cube3d_on_destroy(NYA_Window* window);
void      gny_layer_cube3d_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_cube3d_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/**
 * Grabs the cube. Registered as its on_click, so the ground declines simply by having none.
 * */
void      gny_layer_cube3d_on_cube_click(NYA_Entity* entity, f32x3 world_point, u8 button);

/** Drops a fresh pile onto the terrain, replacing whatever is already lying on it. */
void      gny_layer_cube3d_cubes_drop(void);

/** Despawns the pile. The terrain and the draggable cube stay. */
void      gny_layer_cube3d_cubes_clear(void);

/**
 * Attaches a body to either model whose mesh has finished loading. A no-op once both have one.
 * */
void      gny_layer_cube3d_models_attach(NYA_Window* window);
void      gny_layer_cube3d_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_cube3d_on_render(NYA_Window* window);

void*     GNY_LAYER_BACKGROUND_ID = "gny_layer_background";
NYA_Layer GNY_LAYER_BACKGROUND;
void      gny_layer_background_on_create(NYA_Window* window);
void      gny_layer_background_on_destroy(NYA_Window* window);
void      gny_layer_background_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_background_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_background_on_render(NYA_Window* window);

/** The terrain, the crates, the camera and the input that spawns things. */
void*     GNY_LAYER_GAME_ID = "gny_layer_game";
NYA_Layer GNY_LAYER_GAME;
void      gny_layer_game_on_create(NYA_Window* window);
void      gny_layer_game_on_destroy(NYA_Window* window);
void      gny_layer_game_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_game_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_game_on_render(NYA_Window* window);

/** Screen space only: the counters, the frame cost and the key bindings. */
void*     GNY_LAYER_UI_ID = "gny_layer_ui";
NYA_Layer GNY_LAYER_UI;
void      gny_layer_ui_on_create(NYA_Window* window);
void      gny_layer_ui_on_destroy(NYA_Window* window);
void      gny_layer_ui_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_ui_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_ui_on_render(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD BUILDING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Fills in the terrain polyline and spawns the static body carrying it.
 * */
void gny_terrain_generate(u64 seed);

/**
 * Draws the world — terrain and every entity that knows how to draw itself — through `camera`.
 * */
void gny_world_draw(NYA_Window* window, NYA_Camera2DTopDown camera);

/** Creates or resizes the primary camera's offscreen target, which the bloom pass reads back. */
/**
 * Makes sure the bloom pipeline and its fragment shader are queued. Safe to call more than once.
 * */
void gny_bloom_pipeline_ensure(NYA_Window* window);


/**
 * Where a point on screen is in the world, under the game layer's camera.
 * */
f32x2 gny_screen_to_world(const NYA_Window* window, f32x2 screen);
