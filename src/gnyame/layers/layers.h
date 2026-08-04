#pragma once

// nyangine is a different subsystem, so its umbrella. Not gnyame.h: that is this subsystem's own
// umbrella and it includes this file, so naming it here is a cycle that only #pragma once hides.
#include "nyangine/nyangine.h"

void gny_layers_init(void);

void*     GNY_LAYER_MENU_ID = "gny_layer_menu";
NYA_Layer GNY_LAYER_MENU;
void      gny_layer_menu_on_create(NYA_Window* window);
void      gny_layer_menu_on_destroy(NYA_Window* window);
void      gny_layer_menu_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_menu_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_menu_on_render(NYA_Window* window);

void*     GNY_LAYER_GAME_ID = "gny_layer_game";
NYA_Layer GNY_LAYER_GAME;
void      gny_layer_game_on_create(NYA_Window* window);
void      gny_layer_game_on_destroy(NYA_Window* window);
void      gny_layer_game_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_game_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_game_on_render(NYA_Window* window);

void*     GNY_LAYER_UI_ID = "gny_layer_ui";
NYA_Layer GNY_LAYER_UI;
void      gny_layer_ui_on_create(NYA_Window* window);
void      gny_layer_ui_on_destroy(NYA_Window* window);
void      gny_layer_ui_on_event(NYA_Window* window, NYA_Event* event);
void      gny_layer_ui_on_update(NYA_Window* window, f32 delta_time_s);
void      gny_layer_ui_on_render(NYA_Window* window);
