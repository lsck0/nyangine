/**
 * @file layers.h
 *
 * Every layer the game pushes. A layer is five hooks named `<prefix>_on_create/_on_destroy/_on_event/
 * _on_update/_on_render` and an id; nya_layer_of builds one from the prefix. Layers are pushed and
 * popped by screen changes, see screens.h.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds the layer values. Called at startup and again after a code reload. */
void gny_layers_init(void);

/**
 * The title screen. The only thing over the background at startup.
 * */
NYA_ConstCString GNY_LAYER_MAIN_MENU_ID = "gny_layer_main_menu";
NYA_Layer        GNY_LAYER_MAIN_MENU;
void             gny_layer_main_menu_on_create(NYA_Window* window);
void             gny_layer_main_menu_on_destroy(NYA_Window* window);
void             gny_layer_main_menu_on_event(NYA_Window* window, NYA_Event* event);
void             gny_layer_main_menu_on_update(NYA_Window* window, f32 delta_time_s);
void             gny_layer_main_menu_on_render(NYA_Window* window);

/**
 * The pause menu, over a stopped world.
 * */
NYA_ConstCString GNY_LAYER_PAUSE_MENU_ID = "gny_layer_pause_menu";
NYA_Layer        GNY_LAYER_PAUSE_MENU;
void             gny_layer_pause_menu_on_create(NYA_Window* window);
void             gny_layer_pause_menu_on_destroy(NYA_Window* window);
void             gny_layer_pause_menu_on_event(NYA_Window* window, NYA_Event* event);
void             gny_layer_pause_menu_on_update(NYA_Window* window, f32 delta_time_s);
void             gny_layer_pause_menu_on_render(NYA_Window* window);

/** The 3D demo. See layer_cube3d.h. */
NYA_ConstCString GNY_LAYER_CUBE3D_ID = "gny_layer_cube3d";
NYA_Layer        GNY_LAYER_CUBE3D;
void             gny_layer_cube3d_on_create(NYA_Window* window);
void             gny_layer_cube3d_on_destroy(NYA_Window* window);
void             gny_layer_cube3d_on_event(NYA_Window* window, NYA_Event* event);
void             gny_layer_cube3d_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/**
 * Grabs the cube. Registered as its on_click, so the ground declines simply by having none.
 * */
void             gny_layer_cube3d_on_cube_click(NYA_Entity* entity, f32x3 world_point, u8 button);

/** Drops a fresh pile onto the terrain, replacing whatever is already lying on it. */
void             gny_layer_cube3d_cubes_drop(void);

/** Despawns the pile. The terrain and the draggable cube stay. */
void             gny_layer_cube3d_cubes_clear(void);

/**
 * Attaches a body to either model whose mesh has finished loading. A no-op once both have one.
 * */
void             gny_layer_cube3d_models_attach(NYA_Window* window);
void             gny_layer_cube3d_on_update(NYA_Window* window, f32 delta_time_s);
void             gny_layer_cube3d_on_render(NYA_Window* window);

/** Sky, parallax hills and drifting motes, behind everything. */
NYA_ConstCString GNY_LAYER_BACKGROUND_ID = "gny_layer_background";
NYA_Layer        GNY_LAYER_BACKGROUND;
void             gny_layer_background_on_create(NYA_Window* window);
void             gny_layer_background_on_destroy(NYA_Window* window);
void             gny_layer_background_on_event(NYA_Window* window, NYA_Event* event);
void             gny_layer_background_on_update(NYA_Window* window, f32 delta_time_s);
void             gny_layer_background_on_render(NYA_Window* window);

/** The terrain, the crates, the camera and the input that spawns things. */
NYA_ConstCString GNY_LAYER_GAME_ID = "gny_layer_game";
NYA_Layer        GNY_LAYER_GAME;
void             gny_layer_game_on_create(NYA_Window* window);
void             gny_layer_game_on_destroy(NYA_Window* window);
void             gny_layer_game_on_event(NYA_Window* window, NYA_Event* event);
void             gny_layer_game_on_update(NYA_Window* window, f32 delta_time_s);
void             gny_layer_game_on_render(NYA_Window* window);

/** Screen space only: the counters, the frame cost and the key bindings. */
NYA_ConstCString GNY_LAYER_UI_ID = "gny_layer_ui";
NYA_Layer        GNY_LAYER_UI;
void             gny_layer_ui_on_create(NYA_Window* window);
void             gny_layer_ui_on_destroy(NYA_Window* window);
void             gny_layer_ui_on_event(NYA_Window* window, NYA_Event* event);
void             gny_layer_ui_on_update(NYA_Window* window, f32 delta_time_s);
void             gny_layer_ui_on_render(NYA_Window* window);
