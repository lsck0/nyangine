/**
 * @file screens.c
 *
 * Screen changes. See screens.h.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Runs at the barrier with the GNY_Screen gny_screen_request copied in. */
NYA_INTERNAL void _gny_screen_apply(void* data);

/** Pops the top layer only if it is `layer_id`, so a stale request cannot remove another screen's layer. */
NYA_INTERNAL b8 _gny_layer_pop_if(NYA_ConstCString layer_id);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCREENS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_screen_request(GNY_Screen screen) {
    nya_assert(screen > GNY_SCREEN_NONE && screen < GNY_SCREEN_COUNT);

    nya_sim_defer(_gny_screen_apply, &screen, sizeof(screen));
}

void _gny_screen_apply(void* data) {
    GNY_Screen screen = *(GNY_Screen*)data;

    switch (screen) {
        case GNY_SCREEN_START_GAME: {
            if (!_gny_layer_pop_if(GNY_LAYER_MAIN_MENU_ID)) return;

            // the game's on_create builds the world inside the push; the HUD pushed after draws over it.
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_GAME);
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_UI);
        } break;

        case GNY_SCREEN_PAUSE: {
            if (gny_modal_active()) return;

            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU);
            nya_physics2d_enabled_set(false);
        } break;

        case GNY_SCREEN_RESUME: {
            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            nya_physics2d_enabled_set(true);
        } break;

        case GNY_SCREEN_RESTART: {
            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            nya_physics2d_enabled_set(true);

            // crates first, or a pile resting on the old surface ends up inside the new one.
            gny_entity_box_destroy_all();
            gny_terrain_generate(gny_world()->terrain_seed + 1);
        } break;

        case GNY_SCREEN_MAIN_MENU: {
            // back from the 3D demo, whose on_destroy despawns its own entities.
            if (_gny_layer_pop_if(GNY_LAYER_CUBE3D_ID)) {
                nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
                return;
            }

            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            // while the entities are still up; from on_destroy it would also run after nya_app_deinit freed them.
            gny_world_clear();

            (void)_gny_layer_pop_if(GNY_LAYER_UI_ID);
            (void)_gny_layer_pop_if(GNY_LAYER_GAME_ID);

            nya_physics2d_enabled_set(true);
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
        } break;

        case GNY_SCREEN_CUBE3D: {
            if (!_gny_layer_pop_if(GNY_LAYER_MAIN_MENU_ID)) return;

            // no HUD layer: the demo draws its own text after nya_render3d_end.
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_CUBE3D);
        } break;

        case GNY_SCREEN_SOCIAL_PROMPT: {
            // pushed once: a second request while one is up replaces the name behind the same layer,
            // and two prompts stacked would each have to be answered.
            if (nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_SOCIAL_ID) != nullptr) return;

            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_SOCIAL);
        } break;

        case GNY_SCREEN_SOCIAL_DISMISS: {
            (void)_gny_layer_pop_if(GNY_LAYER_SOCIAL_ID);
        } break;

        case GNY_SCREEN_QUIT: {
            nya_app_get()->should_quit = true;
        } break;

        case GNY_SCREEN_NONE:
        case GNY_SCREEN_COUNT:
        default: nya_unreachable();
    }
}

b8 gny_modal_active(void) {
    return nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU_ID) != nullptr || nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU_ID) != nullptr
        || nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_SOCIAL_ID) != nullptr;
}

b8 _gny_layer_pop_if(NYA_ConstCString layer_id) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr || window->layer_stack->length == 0) return false;

    if (!nya_string_equals(window->layer_stack->items[window->layer_stack->length - 1].id, layer_id)) return false;

    (void)nya_layer_pop(GNY_WINDOW_MAIN);
    return true;
}
