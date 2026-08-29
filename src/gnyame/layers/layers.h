/**
 * @file layers.h
 *
 * The three layers this demo is made of, and the world they share.
 *
 * A physics sandbox: noise generated terrain, and boxes that fall onto it wherever the mouse is
 * clicked. It exists to exercise the pieces end to end rather than to be a game — entities with
 * rigid bodies, the fixed timestep, per entity callbacks, deferred despawn, the 2D batch, a camera,
 * and a layer stack with something in each slot.
 *
 * The stack is pushed background first, so it draws behind everything, and the UI last, so it draws
 * on top of everything. Between them the game layer owns the world and the camera.
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
 *
 * The menus themselves do none of it. An item names an action, the layer switches on the action it
 * was handed back, and the shared code in between knows nothing about what any of them mean — which
 * is what lets one implementation serve both menus.
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
 *
 * On GNY_World rather than in a static inside layer_cube3d.c, so it survives a hot reload — the same
 * reason the menus keep their state there. Two entity handles and an orbit; the cube's transform is
 * not here, because that belongs to the solver.
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
     *
     * A property of the cube rather than a separate pool, because it changes nothing about how it is
     * simulated — a glass crate falls and collides exactly like a painted one. What it changes is the
     * *draw*: the material is different and the pass is different, so the pile is drawn in two runs.
     * */
    b8 glass;

    /**
     * How frosted this one is, in [0, 1]. Meaningless unless `glass`.
     *
     * Per cube rather than one setting for all of them, because the whole point of the knob is that it
     * varies — a scene where every pane is equally frosted shows nothing that a single fixed blur would
     * not have. It also costs a draw call per distinct value, since the material is per flush, which is
     * why there are three levels rather than twenty-four.
     * */
    f32 blur;
};

struct GNY_Cube3DScene {
    NYA_EntityHandle cube;

    /**
     * The two loaded models, as simulated bodies rather than decorations.
     *
     * They used to be drawn at fixed offsets with a spin applied by the draw call, which is a model on a
     * turntable — it showed that a mesh batches with the primitives and nothing else. As bodies they land
     * in the same pile the cubes do, and the interesting part is that neither of their *colliders* is a
     * mesh: see _gny_cube3d_models_attach for why a convex stand-in is the right answer and a triangle
     * mesh is not.
     *
     * The bodies cannot be attached at spawn. A model's size comes from its vertices, and the vertices
     * arrive a frame or two after nya_asset_load queues them, so the attach is deferred to the first
     * update that finds the mesh loaded.
     * */
    NYA_EntityHandle model;
    NYA_EntityHandle pill;

    /** The noise-generated ground everything in this scene stands on. */
    NYA_Terrain3D* terrain;

    /**
     * The pile, in a fixed array rather than found by querying for GNY_ENTITY_CUBE3D.
     *
     * A query would also return the draggable cube and would give back no size or colour, both of which
     * the draw needs — the renderer bakes the transform into vertices, so it has to be told how big the
     * box is. A handful of handles beside the sizes they belong to is the smaller thing.
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
     *
     * A second particle system rather than the world's `sparks`, because a system draws in one space
     * and one space only — that one is 2D and this one is 3D. See NYA_ParticleSpace.
     * */
    NYA_ParticleSystem* dust;

    /**
     * The fire and the smoke above it, as two systems rather than one.
     *
     * Two because they blend differently and a system draws in one mode: fire is *additive*, so
     * overlapping tongues brighten toward white the way light does, and smoke is alpha over the sorted
     * transparent pass, so near puffs layer over far ones. One system could not be both, and a single
     * additive smoke column reads as a searchlight.
     * */
    NYA_ParticleSystem* fire;
    NYA_ParticleSystem* smoke;

    /** How long since the plume last emitted, so it feeds continuously rather than once. */
    f32 plume_timer_s;
};

/**
 * What a menu row *is*, which decides what left and right do to it.
 *
 * Two kinds is one more than a menu strictly needs and one fewer than it would take to make this a
 * widget system. A volume row is the case that actually exists — settings a player edits without
 * leaving the menu — and giving it a kind keeps the alternative out: a submenu per slider, which is
 * three more layers for two numbers.
 * */
enum GNY_MenuItemKind {
    /** Chosen with confirm, does one thing. The ordinary row. */
    GNY_MENU_ITEM_KIND_ACTION = 0,

    /**
     * A volume, edited in place with left and right. `action` is ignored.
     *
     * Written straight through nya_settings_volume_set, so there is no pending state to apply and
     * nothing to cancel — the mix changes as the key is held, which is the only way to hear what is
     * being set.
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
 *
 * The items are a pointer to a literal array the layer owns, not a copy — they never change at
 * runtime, and a menu that could grow would need an allocator for no reason.
 * */
struct GNY_Menu {
    NYA_ConstCString title;

    /** What the title says underneath, in the dim colour. Optional. */
    NYA_ConstCString subtitle;

    const GNY_MenuItem* items;
    u32                 item_count;

    /**
     * The highlighted item.
     *
     * Moved by the arrow keys and by the mouse passing over an item, so the two agree rather than
     * each keeping their own idea of what is selected — which is what makes a menu jump when a hand
     * brushes the mouse after arrowing down.
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
 *
 * Recomputed from the window size on both the draw and the event path rather than stored when
 * drawing. A menu that hit tests against last frame's layout is wrong for exactly one frame after a
 * resize, which is the frame someone is most likely to be clicking in.
 * */
NYA_Rectf gny_menu_item_bounds(const NYA_Window* window, const GNY_Menu* menu, u32 index);

/**
 * Runs the menu's navigation against one event.
 *
 * Returns true when the event was consumed, which the caller should mirror into `was_handled` — a
 * menu is modal, and anything it does not swallow reaches the game underneath it. That is how a
 * click on the empty part of a menu used to spawn a crate behind it.
 *
 * `out_action` is GNY_MENU_ACTION_NONE unless an item was actually chosen this event.
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
 *
 * **Never call nya_layer_push or nya_layer_pop from a layer callback.** The event and update loops
 * both walk the layer stack with nya_array_foreach, which caches the array's `items` pointer — and
 * pushing can reallocate that array, so the loop carries on through freed memory. It is the same
 * hazard nya_entity_despawn_deferred exists for, and it takes the same answer: queue the change and
 * apply it once everyone has stopped walking.
 *
 * These go through nya_sim_defer, so they land at the barrier in nya_system_sim_apply_commands,
 * which runs after both loops have finished. Requesting two in one tick applies both, in order.
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
 *
 * What the game layer gates its held-key polling on. Consuming an event stops it reaching the layers
 * underneath, but polling reads the keyboard directly and never sees an event at all — so without
 * this, holding a direction key still pans the camera behind an open pause menu.
 *
 * Reads the layer stack rather than a flag beside it, so it cannot disagree with what is on screen.
 * */
b8 gny_modal_active(void);

/**
 * Everything the demo owns that is not an entity.
 *
 * Parked in NYA_App.state rather than in a file scope variable, because this library is unloaded
 * and reopened on every hot reload and a static here would be zeroed each time — the terrain would
 * regenerate and the camera would snap back on every rebuild. NYA_App lives in the executable, so a
 * pointer left there survives.
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
     *
     * Alongside the procedural terrain rather than replacing it, deliberately: the two are the two
     * ways a 2D world gets a floor, and having both in one scene is what shows that a tilemap's
     * colliders and a chain shape are the same thing to the solver. See layer_game.c.
     * */
    NYA_Tilemap* tilemap;

    /**
     * The camera entity. Read it as an NYA_Camera2DTopDown with gny_entity_camera_get.
     *
     * A handle rather than the camera itself, because the camera is an entity — its position and
     * zoom live on its transform and it pans itself in its own on_update. See entity_camera.c.
     *
     * Invalid at the main menu, where the game layer has not been pushed and nothing has created
     * one; gny_entity_camera_get answers with the identity camera there.
     * */
    NYA_EntityHandle camera;

    /**
     * The picture-in-picture camera, created the first time something is watched.
     *
     * Lazily rather than at startup, so a session that never follows anything never pays for a second
     * render target. gny_world_inset_camera is what creates it.
     * */
    NYA_EntityHandle inset_camera;

    /** Counters the UI reports. Cumulative since startup, not since the last clear. */
    u32 boxes_spawned;
    u32 boxes_lost;

    /**
     * Impacts the physics world has reported, total.
     *
     * Counted rather than shown live because a hit lasts one tick: the HUD redraws far more often
     * than an impact happens, so a per frame reading would sit at zero and flicker.
     * */
    /** Impacts and losses, totalled by the simulation observer at the end of each frame. See sim.h. */
    u32 hits;

    /**
     * Whether the background track has been started yet.
     *
     * Assets are queued and resolve at the end of a frame, so the track cannot be played from the
     * layer's on_create — nya_audio_play_music would find nothing loaded and return quietly, which
     * looks exactly like a broken file. The first tick that finds it loaded starts it, and this is
     * what stops every tick after that from starting it again.
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
     *
     * The second rule does not bite here because nothing in the game registers a binding — the `nya`
     * table is the engine's and lives in the host too.
     * */
    NYA_LuaVM* lua;

    /** Seconds since the script's tick hook last ran. It is a once-a-second hook, not a per-frame one. */
    f32 lua_tick_timer_s;

    /**
     * Whether the startup script has been run.
     *
     * Assets resolve at the end of a frame, so the script cannot be run from gny_world_create — it
     * would find nothing loaded. The first tick that finds it loaded runs it, and this stops every
     * tick after that from running it again.
     * */
    b8 lua_started;

    /**
     * Sparks thrown off by crate impacts.
     *
     * On the world rather than on the game layer because the sim observer emits into it, and that
     * runs at the frame barrier rather than from inside any layer — see sim.c.
     * */
    NYA_ParticleSystem* sparks;

    /*
     * ── Post processing ──
     */

    /**
     * The offscreen target the world is drawn into before the bloom pass reads it back.
     *
     * Zeroed until the first frame that needs it, and resized whenever the window is. Owned by the
     * game layer: created on demand in its render, destroyed in its on_destroy while the renderer is
     * still standing.
     * */
    /**
     * The post-processing chain the world is composited through.
     *
     * Was a single NYA_RenderTexture plus a hand written ensure/begin/pass/end sequence, duplicated
     * between the 2D camera and the 3D layer. nya_post_* is that sequence, in the engine, once.
     * */
    NYA_PostChain post;

    /** Toggled with `b`. Off draws the scene straight to the window with no second pass. */
    b8 bloom_enabled;

    /**
     * Seconds added to the clock before the day phase is taken from it.
     *
     * How the demo starts at a time other than midnight, and how a key could scrub the cycle. Signed on
     * purpose: a negative offset is a legal way to start at dusk, and gny_sky_phase wraps for it.
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
 *
 * What "back to the main menu" does, so that starting again builds a fresh world rather than
 * resuming the old one. Called from the screen change rather than from the game layer's on_destroy,
 * because on_destroy also fires during nya_app_deinit — which tears down the entity system before it
 * destroys the windows, so the entity calls here would be reading a table that no longer exists.
 * */
void gny_world_clear(void);

/**
 * The inset camera, creating it on first use.
 *
 * Bottom right, clear of the HUD panels on the left and the trace panel on the right. Its viewport is
 * fixed; a real game would size it to whatever it is inset into.
 * */
NYA_EntityHandle gny_world_inset_camera(void);

/**
 * Runs the startup script once it has loaded, then its optional per-second hook.
 *
 * Called from the game layer's on_update. Split out of the layer because the VM belongs to the world
 * rather than to any one screen — see GNY_World.lua.
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
 *
 * Pushed by gny_window_main_create and popped by "start", which pushes the game and the HUD in its
 * place — so the world does not exist at all until it is asked for, and going back to the menu takes
 * it away again.
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
 *
 * Pushed on top of the HUD rather than replacing it, so the numbers can be read at rest — which is
 * most of what a pause is for in a demo like this one. Stops the solver while it is up.
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
 *
 * Left button only: a right or middle click on the cube is not a drag, and taking every button
 * would mean a context menu could never be added without the cube spinning first.
 * */
void      gny_layer_cube3d_on_cube_click(NYA_Entity* entity, f32x3 world_point, u8 button);

/** Drops a fresh pile onto the terrain, replacing whatever is already lying on it. */
void      gny_layer_cube3d_cubes_drop(void);

/** Despawns the pile. The terrain and the draggable cube stay. */
void      gny_layer_cube3d_cubes_clear(void);

/**
 * Attaches a body to either model whose mesh has finished loading. A no-op once both have one.
 *
 * Takes the window because the bounds it fits the body to come from the renderer, which resolves a handle
 * against that window's registered geometry as well as against the asset system.
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
 *
 * Called once at startup and again on every regenerate. Despawns the previous terrain entity first,
 * which takes its chain body with it, so this is safe to call repeatedly.
 * */
void gny_terrain_generate(u64 seed);

/**
 * Draws the world — terrain and every entity that knows how to draw itself — through `camera`.
 *
 * Public because it is run once per camera rather than once per frame: gny_system_camera_render
 * calls it for each secondary view and again for the primary one.
 * */
void gny_world_draw(NYA_Window* window, NYA_Camera2DTopDown camera);

/** Creates or resizes the primary camera's offscreen target, which the bloom pass reads back. */
/**
 * Makes sure the bloom pipeline and its fragment shader are queued. Safe to call more than once.
 *
 * Both scenes composite through it, and an asset load keyed on a handle it already knows returns
 * immediately — so this is idempotent by construction rather than by a guard.
 * */
void gny_bloom_pipeline_ensure(NYA_Window* window);


/**
 * Where a point on screen is in the world, under the game layer's camera.
 *
 * Not nya_render2d_screen_to_world, which answers against the camera the *batch* currently has — and
 * outside a render pass that is no camera at all, so it would hand back the screen coordinates
 * unchanged. Input arrives between frames, which is exactly when that is true.
 * */
f32x2 gny_screen_to_world(const NYA_Window* window, f32x2 screen);
