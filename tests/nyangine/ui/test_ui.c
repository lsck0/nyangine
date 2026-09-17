/**
 * The immediate-mode UI, headless: layout, focus navigation and its wrap and repeat, activation by key and pointer,
 * sliders and toggles, id stability, and the fixed tables refusing past their capacity.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static void key(NYA_Keycode keycode, b8 down) {
    NYA_Event event = { .type = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP, .as_key_event = { .is_down = down, .key = keycode } };
    nya_system_input_handle_event(&event);
}

static void tap(NYA_Keycode keycode) {
    key(keycode, true);
    key(keycode, false);
}

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&event);
}

static void pointer_button(NYA_MouseButton button, b8 down) {
    f32x2     at    = nya_input_mouse_position();
    NYA_Event event = { .type = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = down, .button = button, .x = at.x, .y = at.y } };
    nya_system_input_handle_event(&event);
}

/** The end of an update tick: edges roll and the tick advances. */
static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static f32x2 center_of(NYA_Rectf rect) {
    return (f32x2){ rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };
}

/**
 * Where `menu` puts its second button: the panel's offset, then its frame of padding and outline, then one button
 * and a gap down, as wide as the panel inside its frame.
 * */
#define SECOND ((NYA_Rectf){ 20.0F + 12.0F, 30.0F + 12.0F + 40.0F + 6.0F, 300.0F - 24.0F, 40.0F })

/** And its slider, alone at the top left of a 400 wide panel. */
#define SLIDER ((NYA_Rectf){ 12.0F, 12.0F, 400.0F - 24.0F, 40.0F })

/** Three buttons in a panel; which one activated this pass, or -1. */
static s32 menu(NYA_UIPass pass, NYA_ConstCString labels[3]) {
    s32     activated = -1;
    NYA_UI* ui        = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "menu", (NYA_UIPanel){ .offset = { 20.0F, 30.0F }, .width = 300.0F })) {
        for (u32 i = 0; i < 3; i++) {
            if (nya_ui_button(ui, labels[i])) activated = (s32)i;
        }
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    return activated;
}

/** An input pass over a slider and a toggle. Whether either changed. */
static b8 options(f32* value, b8* on) {
    b8      changed = false;
    NYA_UI* ui      = nya_ui_begin(&window, NYA_UI_PASS_INPUT);

    if (nya_ui_panel_begin(ui, "options", (NYA_UIPanel){ .width = 400.0F })) {
        changed = nya_ui_slider(ui, "volume", value, 0.0F, 1.0F, 0.25F);
        changed = nya_ui_toggle(ui, "fullscreen", on) || changed;
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    return changed;
}

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_asset_deinit();
    defer nya_world_destroy(world);

    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);
    nya_input_action_rebind(NYA_INPUT_ACTION_UP, NYA_KEY_UP);
    nya_input_action_rebind(NYA_INPUT_ACTION_DOWN, NYA_KEY_DOWN);
    nya_input_action_rebind(NYA_INPUT_ACTION_LEFT, NYA_KEY_LEFT);
    nya_input_action_rebind(NYA_INPUT_ACTION_RIGHT, NYA_KEY_RIGHT);

    nya_font_default_set(nya_font(FACE, 20.0F));
    for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    NYA_UIStyle style = { .padding = 10.0F, .spacing = 6.0F, .outline = 2.0F, .item_height = 40.0F };
    nya_ui_style_set(&window, style);

    NYA_ConstCString abc[3] = { "a", "b", "c" };

    // ── A zeroed style is the default look, and a set one reads back with its zeros filled in.
    {
        NYA_Window other = { .handle = { .index = 2, .generation = 1 }, .screen_width = 100, .screen_height = 100 };

        NYA_UIStyle defaults = nya_ui_style_get(&other);
        nya_check(defaults.padding == NYA_UI_PADDING && defaults.radius == NYA_UI_RADIUS && defaults.panel.a == NYA_UI_PANEL.a, "zero takes the defaults");

        NYA_UIStyle got = nya_ui_style_get(&window);
        nya_check(got.padding == 10.0F && got.item_height == 40.0F && got.depth == NYA_UI_DEPTH, "set fields stay and the rest default");

        other.handle.generation = 2;
        nya_ui_style_set(&other, style);
        other.handle.generation = 3;
        nya_check(nya_ui_style_get(&other).padding == NYA_UI_PADDING, "a new window in the same slot starts over");
    }

    // ── Layout: a top left panel stacks full width buttons inside its frame, a pass later it can centre, and rows split it.
    {
        // the pointer finds the second button exactly where the frame, the first button and the gap put it.
        pointer_move((f32x2){ SECOND.x + 1.0F, SECOND.y + 1.0F });
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 1, "a click just inside the second button's corner is on it");
        tick();

        pointer_move((f32x2){ SECOND.x + 1.0F, SECOND.y - 3.0F });
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == -1, "and one in the gap above it is on nothing");
        tick();

        f32 frame = 10.0F + 2.0F;

        for (u32 pass = 0; pass < 2; pass++) {
            NYA_UI*   ui    = nya_ui_begin(&window, NYA_UI_PASS_DRAW);
            NYA_Rectf inner = { 0 };

            if (nya_ui_panel_begin(ui, "centred", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_CENTER, .width = 300.0F })) {
                inner = nya_ui_space(ui, 0.0F, 50.0F);
                nya_ui_panel_end(ui);
            }

            nya_ui_end(ui);

            f32 height = (frame * 2.0F) + 50.0F;
            if (pass == 1) {
                nya_check(inner.x == 250.0F + frame && inner.y == ((600.0F - height) * 0.5F) + frame && inner.width == 300.0F - (frame * 2.0F),
                          "centred from its measured size, got %f %f %f", (f64)inner.x, (f64)inner.y, (f64)inner.width);
            }
        }

        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);
        if (nya_ui_panel_begin(ui, "row", (NYA_UIPanel){ .width = 300.0F })) {
            nya_ui_row_begin(ui, 2);
            (void)nya_ui_button(ui, "left");
            NYA_Rectf right = nya_ui_space(ui, 0.0F, 40.0F);
            nya_ui_row_end(ui);
            NYA_Rectf under = nya_ui_space(ui, 0.0F, 10.0F);
            nya_ui_panel_end(ui);

            f32 cell = (276.0F - 6.0F) * 0.5F;
            nya_check(right.x == frame + cell + 6.0F && right.width == cell && right.y == frame, "a row splits the width into cells, got %f %f", (f64)right.x, (f64)right.width);
            nya_check(under.y == frame + 40.0F + 6.0F && under.x == frame, "and what follows goes under the row, got %f", (f64)under.y);
        }
        nya_ui_end(ui);
    }

    // ── Focus starts on the first widget, moves with up and down, and wraps at both ends.
    {
        nya_ui_focus_reset(&window);
        tick();

        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 0, "the first pass can already confirm the first button");
        tick();

        tap(NYA_KEY_UP);
        (void)menu(NYA_UI_PASS_INPUT, abc);
        tick();
        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 2, "up on the first wraps to the last");
        tick();

        tap(NYA_KEY_DOWN);
        (void)menu(NYA_UI_PASS_INPUT, abc);
        tick();
        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 0, "down on the last wraps to the first");
        tick();
    }

    // ── A press acts once: not in a draw pass, not again after the tick that saw it, and cancel reads the same way.
    {
        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_DRAW, abc) == -1, "a draw pass never activates");
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 0, "the tick after it does");
        tick();
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == -1, "and the next tick has nothing left to act on");

        tap(NYA_KEY_ESCAPE);

        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);
        nya_check(!nya_ui_cancelled(ui), "cancel is not read while drawing");
        nya_ui_end(ui);

        ui = nya_ui_begin(&window, NYA_UI_PASS_INPUT);
        nya_check(nya_ui_cancelled(ui), "but is in the input pass");
        nya_ui_end(ui);
        tick();
    }

    // ── A held direction repeats after a delay, once per tick however many passes read it.
    {
        nya_ui_focus_reset(&window);
        nya_app_get()->frame_stats.delta_time_s = 0.1F;

        key(NYA_KEY_DOWN, true);
        (void)menu(NYA_UI_PASS_INPUT, abc);
        tick();

        for (u32 i = 0; i < 4; i++) {
            (void)menu(NYA_UI_PASS_INPUT, abc);
            (void)menu(NYA_UI_PASS_INPUT, abc);
            tick();
        }

        tap(NYA_KEY_RETURN);
        key(NYA_KEY_DOWN, true);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 1, "half a second held has moved only once");
        tick();

        tap(NYA_KEY_RETURN);
        key(NYA_KEY_DOWN, true);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 2, "then it repeats");
        tick();

        key(NYA_KEY_DOWN, false);
        nya_app_get()->frame_stats.delta_time_s = 0.0F;
        tick();
    }

    // ── Ids: relabelled rows keep focus by position, and reordered ones keep it by label.
    {
        nya_ui_focus_reset(&window);
        tap(NYA_KEY_DOWN);
        (void)menu(NYA_UI_PASS_INPUT, abc);
        tick();

        NYA_ConstCString xyz[3] = { "x", "y", "z" };
        (void)menu(NYA_UI_PASS_DRAW, xyz);
        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_INPUT, xyz) == 1, "new labels in the same places keep the focused position");
        tick();

        NYA_ConstCString zxy[3] = { "z", "x", "y" };
        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_INPUT, zxy) == 2, "a moved label takes its focus with it");
        tick();

        // the same label in two panels is two widgets, so this does not assert.
        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_INPUT);
        for (u32 i = 0; i < 2; i++) {
            if (nya_ui_panel_begin(ui, i == 0 ? "one" : "two", (NYA_UIPanel){ .width = 100.0F })) {
                (void)nya_ui_button(ui, "same");
                nya_ui_panel_end(ui);
            }
        }
        nya_ui_end(ui);
    }

    // ── The pointer: moving focuses, press and release on the same widget activates, anything else does not.
    {
        pointer_move(center_of(SECOND));
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == -1, "a press alone does nothing");
        tick();

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 1, "the release over it activates");
        tick();

        tap(NYA_KEY_RETURN);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == 1, "and the keys carry on from where the pointer left focus");
        tick();

        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        (void)menu(NYA_UI_PASS_INPUT, abc);
        tick();

        pointer_move((f32x2){ 700.0F, 500.0F });
        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == -1, "released somewhere else, nothing activates");
        tick();

        pointer_move(center_of(SECOND));
        pointer_button(NYA_MOUSE_BUTTON_RIGHT, true);
        pointer_button(NYA_MOUSE_BUTTON_RIGHT, false);
        nya_check(menu(NYA_UI_PASS_INPUT, abc) == -1, "a right click is not a click");
        tick();
    }

    // ── Sliders step and clamp, toggles flip on confirm and take a side from left and right.
    {
        nya_ui_focus_reset(&window);

        f32 value = 0.9F;
        b8  on    = false;

        struct {
            NYA_Keycode key;
            f32         value;
            b8          on;
            b8          changed;
        } steps[] = {
            { NYA_KEY_RIGHT,  1.0F,  false, true  },
            { NYA_KEY_RIGHT,  1.0F,  false, false },
            { NYA_KEY_LEFT,   0.75F, false, true  },
            { NYA_KEY_DOWN,   0.75F, false, false },
            { NYA_KEY_RETURN, 0.75F, true,  true  },
            { NYA_KEY_LEFT,   0.75F, false, true  },
            { NYA_KEY_LEFT,   0.75F, false, false },
        };

        for (u32 i = 0; i < nya_carray_length(steps); i++) {
            tap(steps[i].key);
            b8 changed = options(&value, &on);
            tick();

            nya_check(fabsf(value - steps[i].value) < 1e-5F && on == steps[i].on && changed == steps[i].changed, "step %u: got %f %d %d", i, (f64)value,
                      on, changed);
        }
    }

    // ── A press on a slider's label only focuses it; one on its track grabs the knob, which follows the pointer off the row.
    {
        f32 value = 0.5F;
        b8  on    = false;

        (void)options(&value, &on);
        tick();

        NYA_Rectf slider = SLIDER;
        f32       middle = slider.y + (slider.height * 0.5F);

        pointer_move((f32x2){ slider.x + 4.0F, middle });
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        (void)options(&value, &on);
        tick();
        nya_check(value == 0.5F, "the label end leaves the value, got %f", (f64)value);

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        (void)options(&value, &on);
        tick();

        pointer_move((f32x2){ slider.x + slider.width - 60.0F, middle });
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        (void)options(&value, &on);
        tick();

        f32 grabbed = value;

        pointer_move((f32x2){ slider.x + slider.width + 80.0F, middle - 60.0F });
        (void)options(&value, &on);
        tick();

        nya_check(grabbed > 0.5F && grabbed < 1.0F && fmodf(grabbed, 0.25F) == 0.0F, "the track snaps the pointer to a step, got %f", (f64)grabbed);
        nya_check(value == 1.0F, "and dragging past the end clamps, got %f", (f64)value);

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        tick();
    }

    // ── Capacity: a widget past the table is refused and left out of the focus cycle, and so is a panel.
    {
        nya_ui_focus_reset(&window);

        s32 activated = -1;

        for (u32 round = 0; round < 3; round++) {
            if (round == 1) tap(NYA_KEY_UP);
            if (round == 2) tap(NYA_KEY_RETURN);

            NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_INPUT);
            for (u32 i = 0; i <= NYA_UI_WIDGETS_MAX; i++) {
                char label[16];
                (void)snprintf(label, sizeof(label), "w%u", i);

                if (nya_ui_button(ui, label)) activated = (s32)i;
            }
            nya_ui_end(ui);
            tick();
        }

        nya_check(activated == NYA_UI_WIDGETS_MAX - 1, "up from the first lands on the last one kept, got %d", activated);

        b8 refused = false;

        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);
        for (u32 i = 0; i <= NYA_UI_PANELS_MAX; i++) {
            char id[16];
            (void)snprintf(id, sizeof(id), "p%u", i);

            if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ 0 })) {
                refused = i == NYA_UI_PANELS_MAX;
                continue;
            }
            nya_ui_panel_end(ui);
        }
        nya_ui_end(ui);
        nya_check(refused, "one pass cannot hold more panels than the table");

        ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);
        b8 reused = nya_ui_panel_begin(ui, "fresh", (NYA_UIPanel){ 0 });
        if (reused) nya_ui_panel_end(ui);
        nya_ui_end(ui);
        nya_check(reused, "the next pass takes over the stalest slot");
    }

    // ── A modal layer takes keys and clicks but lets the pointer's motion through.
    {
        NYA_Event down  = { .type = NYA_EVENT_KEY_DOWN };
        NYA_Event click = { .type = NYA_EVENT_MOUSE_BUTTON_DOWN };
        NYA_Event moved = { .type = NYA_EVENT_MOUSE_MOVED };

        nya_check(nya_ui_modal_event(&down) && down.was_handled, "a key stops at the modal layer");
        nya_check(nya_ui_modal_event(&click) && click.was_handled, "and so does a click");
        nya_check(!nya_ui_modal_event(&moved) && !moved.was_handled, "but hover still reaches the layers below");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
