#include "gnyame/gnyame.h"

void gny_layers_init(void) {
    GNY_LAYER_MENU = (NYA_Layer){
        .id         = GNY_LAYER_MENU_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_menu_on_create),
        .on_destroy = nya_callback(gny_layer_menu_on_destroy),
        .on_event   = nya_callback(gny_layer_menu_on_event),
        .on_update  = nya_callback(gny_layer_menu_on_update),
        .on_render  = nya_callback(gny_layer_menu_on_render),
    };

    GNY_LAYER_GAME = (NYA_Layer){
        .id         = GNY_LAYER_GAME_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_game_on_create),
        .on_destroy = nya_callback(gny_layer_game_on_destroy),
        .on_event   = nya_callback(gny_layer_game_on_event),
        .on_update  = nya_callback(gny_layer_game_on_update),
        .on_render  = nya_callback(gny_layer_game_on_render),
    };

    GNY_LAYER_UI = (NYA_Layer){
        .id         = GNY_LAYER_UI_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_ui_on_create),
        .on_destroy = nya_callback(gny_layer_ui_on_destroy),
        .on_event   = nya_callback(gny_layer_ui_on_event),
        .on_update  = nya_callback(gny_layer_ui_on_update),
        .on_render  = nya_callback(gny_layer_ui_on_render),
    };
}
