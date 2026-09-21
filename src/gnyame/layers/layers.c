#include "gnyame/gnyame.h"

void gny_layers_init(void) {
    GNY_LAYER_BACKGROUND = nya_layer_of(gny_layer_background, GNY_LAYER_BACKGROUND_ID);
    GNY_LAYER_MAIN_MENU  = nya_layer_of(gny_layer_main_menu, GNY_LAYER_MAIN_MENU_ID);
    GNY_LAYER_PAUSE_MENU = nya_layer_of(gny_layer_pause_menu, GNY_LAYER_PAUSE_MENU_ID);
    GNY_LAYER_GAME       = nya_layer_of(gny_layer_game, GNY_LAYER_GAME_ID);
    GNY_LAYER_UI         = nya_layer_of(gny_layer_ui, GNY_LAYER_UI_ID);
    GNY_LAYER_CUBE3D     = nya_layer_of(gny_layer_cube3d, GNY_LAYER_CUBE3D_ID);
    GNY_LAYER_SOCIAL     = nya_layer_of(gny_layer_social, GNY_LAYER_SOCIAL_ID);
}
