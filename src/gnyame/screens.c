/**
 * @file screens.c
 *
 * Screen changes and the shared menu widget. See screens.h.
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

/** What one menu action does, whether a key event or a gamepad press carried it. */
NYA_INTERNAL void _gny_menu_apply(GNY_Menu* menu, NYA_InputAction action);

/** One row's label, with the value and arrows a volume row shows. */
NYA_INTERNAL NYA_ConstCString _gny_menu_row_label(const GNY_MenuItem* item, b8 highlighted, OUT char* buffer, u64 capacity);

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

        case GNY_SCREEN_QUIT: {
            nya_app_get()->should_quit = true;
        } break;

        case GNY_SCREEN_NONE:
        case GNY_SCREEN_COUNT:
        default: nya_unreachable();
    }
}

b8 gny_modal_active(void) {
    return nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU_ID) != nullptr || nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU_ID) != nullptr;
}

b8 _gny_layer_pop_if(NYA_ConstCString layer_id) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr || window->layer_stack->length == 0) return false;

    if (!nya_string_equals(window->layer_stack->items[window->layer_stack->length - 1].id, layer_id)) return false;

    (void)nya_layer_pop(GNY_WINDOW_MAIN);
    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MENUS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Rectf gny_menu_item_bounds(const NYA_Window* window, const GNY_Menu* menu, u32 index) {
    nya_assert(menu->item_count > 0 && index < menu->item_count);

    f32 title_height = nya_font_height(nya_font_named("menu_title"), menu->title);
    if (menu->subtitle != nullptr) title_height += nya_font_height(nya_font_named("menu"), menu->subtitle);

    f32 panel_height = (GNY_MENU_PADDING * 2.0F) + title_height + GNY_MENU_TITLE_GAP + (GNY_MENU_ITEM_HEIGHT * (f32)menu->item_count);
    f32 panel_x      = ((f32)window->screen_width - GNY_MENU_WIDTH) * 0.5F;
    f32 panel_y      = ((f32)window->screen_height - panel_height) * 0.5F;

    return (NYA_Rectf){
        .x      = panel_x + GNY_MENU_PADDING,
        .y      = panel_y + GNY_MENU_PADDING + title_height + GNY_MENU_TITLE_GAP + (GNY_MENU_ITEM_HEIGHT * (f32)index),
        .width  = GNY_MENU_WIDTH - (GNY_MENU_PADDING * 2.0F),
        .height = GNY_MENU_ITEM_HEIGHT,
    };
}

b8 gny_menu_handle_event(const NYA_Window* window, GNY_Menu* menu, const NYA_Event* event) {
    nya_assert(window != nullptr && menu != nullptr && event != nullptr);

    if (menu->item_count == 0) return false;

    switch (event->type) {
        case NYA_EVENT_KEY_DOWN: {
            const NYA_KeyEvent* key = &event->as_key_event;

            if (nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, key->key, key->modifier_flags)) {
                if (!key->is_repeat) _gny_menu_apply(menu, NYA_INPUT_ACTION_CANCEL);
                return true;
            }

            const NYA_InputAction actions[] = { NYA_INPUT_ACTION_UP, NYA_INPUT_ACTION_DOWN, NYA_INPUT_ACTION_LEFT, NYA_INPUT_ACTION_RIGHT,
                                                NYA_INPUT_ACTION_CONFIRM };

            for (u32 i = 0; i < nya_carray_length(actions); i++) {
                if (!nya_input_action_matches(actions[i], key->key, key->modifier_flags)) continue;

                _gny_menu_apply(menu, actions[i]);
                break;
            }

            // every key is swallowed: the menu is modal.
            return true;
        }

        case NYA_EVENT_MOUSE_MOVED: {
            f32x2 point = { event->as_mouse_moved_event.x, event->as_mouse_moved_event.y };

            // hover moves the same `selected` as the keys, so the two never disagree.
            for (u32 i = 0; i < menu->item_count; i++) {
                if (nya_rect_contains(gny_menu_item_bounds(window, menu, i), point)) menu->selected = i;
            }

            // not consumed, since layers below may track the mouse.
            return false;
        }

        case NYA_EVENT_MOUSE_BUTTON_DOWN: {
            const NYA_MouseButtonEvent* mouse = &event->as_mouse_button_event;
            if (mouse->button != NYA_MOUSE_BUTTON_LEFT) return true;

            for (u32 i = 0; i < menu->item_count; i++) {
                if (!nya_rect_contains(gny_menu_item_bounds(window, menu, i), (f32x2){ mouse->x, mouse->y })) continue;

                menu->selected = i;

                // a click selects a volume row; the keys move its value.
                if (menu->items[i].kind == GNY_MENU_ITEM_KIND_SCREEN) gny_screen_request(menu->items[i].screen);
            }

            // consumed even off a row, so a click on the panel cannot drop a crate behind it.
            return true;
        }

        default: return false;
    }
}

void gny_menu_update(GNY_Menu* menu) {
    nya_assert(menu != nullptr);

    if (menu->item_count == 0) return;

    const NYA_InputAction actions[] = { NYA_INPUT_ACTION_UP,    NYA_INPUT_ACTION_DOWN,    NYA_INPUT_ACTION_LEFT, NYA_INPUT_ACTION_RIGHT,
                                        NYA_INPUT_ACTION_CONFIRM, NYA_INPUT_ACTION_CANCEL, NYA_INPUT_ACTION_PAUSE };

    u32 held = 0;
    for (u32 i = 0; i < nya_carray_length(actions); i++) {
        if (gny_action_pad_held(actions[i])) held |= 1U << i;
    }

    u32 pressed    = held & ~menu->pad_held;
    menu->pad_held = held;

    for (u32 i = 0; i < nya_carray_length(actions); i++) {
        if ((pressed & (1U << i)) == 0) continue;

        _gny_menu_apply(menu, actions[i] == NYA_INPUT_ACTION_PAUSE ? NYA_INPUT_ACTION_CANCEL : actions[i]);
    }
}

void _gny_menu_apply(GNY_Menu* menu, NYA_InputAction action) {
    const GNY_MenuItem* selected = &menu->items[menu->selected];

    switch (action) {
        case NYA_INPUT_ACTION_CANCEL: {
            if (menu->on_cancel != GNY_SCREEN_NONE) gny_screen_request(menu->on_cancel);
        } break;

        // adding item_count - 1 wraps upward without an unsigned 0 - 1.
        case NYA_INPUT_ACTION_UP: menu->selected = (menu->selected + menu->item_count - 1) % menu->item_count; break;
        case NYA_INPUT_ACTION_DOWN: menu->selected = (menu->selected + 1) % menu->item_count; break;

        // nya_settings_volume_set clamps.
        case NYA_INPUT_ACTION_LEFT:
        case NYA_INPUT_ACTION_RIGHT: {
            if (selected->kind != GNY_MENU_ITEM_KIND_VOLUME) break;

            f32 step = action == NYA_INPUT_ACTION_LEFT ? -GNY_VOLUME_STEP : GNY_VOLUME_STEP;
            nya_settings_volume_set(selected->channel, nya_settings_volume(selected->channel) + step);
        } break;

        case NYA_INPUT_ACTION_CONFIRM: {
            if (selected->kind == GNY_MENU_ITEM_KIND_SCREEN) gny_screen_request(selected->screen);
        } break;

        default: nya_unreachable();
    }
}

void gny_menu_draw(NYA_Window* window, const GNY_Menu* menu) {
    nya_assert(window != nullptr && menu != nullptr && menu->item_count > 0);

    nya_render2d_rect(window, 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height, GNY_MENU_SCRIM);

    // the frame is derived from the row bounds, so frame and hit targets agree.
    NYA_Rectf first = gny_menu_item_bounds(window, menu, 0);
    NYA_Rectf last  = gny_menu_item_bounds(window, menu, menu->item_count - 1);

    // registered in world.c; the title is a distance field.
    NYA_Font title_font = nya_font_named("menu_title");
    NYA_Font item_font  = nya_font_named("menu");

    f32x2 title_size    = nya_font_measure(title_font, menu->title);
    f32x2 subtitle_size = menu->subtitle != nullptr ? nya_font_measure(item_font, menu->subtitle) : f32x2_zero;

    f32 panel_x      = first.x - GNY_MENU_PADDING;
    f32 panel_width  = first.width + (GNY_MENU_PADDING * 2.0F);
    f32 panel_y      = first.y - GNY_MENU_TITLE_GAP - title_size.y - subtitle_size.y - GNY_MENU_PADDING;
    f32 panel_height = (last.y + last.height + GNY_MENU_PADDING) - panel_y;

    nya_render2d_rect(window, panel_x, panel_y, panel_width, panel_height, GNY_MENU_PANEL);
    nya_render2d_rect_outline(window, panel_x, panel_y, panel_width, panel_height, 1.0F, GNY_MENU_BORDER);

    f32 text_y = panel_y + GNY_MENU_PADDING;
    nya_font_draw(window, title_font, menu->title, panel_x + ((panel_width - title_size.x) * 0.5F), text_y, GNY_MENU_TITLE);

    if (menu->subtitle != nullptr) {
        nya_font_draw(window, item_font, menu->subtitle, panel_x + ((panel_width - subtitle_size.x) * 0.5F), text_y + title_size.y, GNY_MENU_SUBTITLE);
    }

    for (u32 i = 0; i < menu->item_count; i++) {
        NYA_Rectf bounds      = gny_menu_item_bounds(window, menu, i);
        b8        highlighted = i == menu->selected;

        if (highlighted) nya_render2d_rect(window, bounds.x, bounds.y, bounds.width, bounds.height, GNY_MENU_HIGHLIGHT);

        char             buffer[64];
        NYA_ConstCString label = _gny_menu_row_label(&menu->items[i], highlighted, buffer, sizeof(buffer));
        f32x2            size  = nya_font_measure(item_font, label);

        nya_font_draw(window, item_font, label, bounds.x + ((bounds.width - size.x) * 0.5F), bounds.y + ((bounds.height - size.y) * 0.5F),
                      highlighted ? GNY_MENU_ITEM_ON : GNY_MENU_ITEM);
    }
}

NYA_ConstCString _gny_menu_row_label(const GNY_MenuItem* item, b8 highlighted, OUT char* buffer, u64 capacity) {
    nya_assert(item->label != nullptr);

    if (item->kind != GNY_MENU_ITEM_KIND_VOLUME) return item->label();

    f32 percent = nya_settings_volume(item->channel) * 100.0F;
    (void)snprintf(buffer, (size_t)capacity, highlighted ? "< %s  %3.0f%% >" : "%s  %3.0f%%", item->label(), (f64)percent);

    return buffer;
}
