/**
 * The immediate-mode UI's layout and look, headless: fixed, fitting and growing children and how rounding hands out
 * the pixels, min and max, containers inside containers, the scale, labels wrapping and shrinking to their room, focus
 * across the cells of a row, disabled widgets, pushed styles, skin insets as padding, and a panel past its height
 * scrolling by wheel and by focus with the pointer clipped to what shows.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

/** Wider than any window here, so it has to wrap or shrink to fit one. */
#define LONG_LINE "the quick brown fox jumps over the lazy dog and keeps on running"

/** The default margin between the window and a top level panel, and the test style's padding and gap. */
#define MARGIN 16.0F
#define FRAME  10.0F
#define GAP    6.0F

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/** Narrow enough that LONG_LINE does not fit. */
static NYA_Window narrow = { .handle = { .index = 2, .generation = 1 }, .screen_width = 300, .screen_height = 600 };

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

static void click(void) {
    f32x2 at = nya_input_mouse_position();

    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = { .type = i == 0 ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = i == 0, .button = NYA_MOUSE_BUTTON_LEFT, .x = at.x, .y = at.y } };
        nya_system_input_handle_event(&event);
    }
}

static void wheel(f32 amount) {
    NYA_Event event = { .type = NYA_EVENT_MOUSE_WHEEL_MOVED, .as_mouse_wheel_event = { .amount_y = amount } };
    nya_system_input_handle_event(&event);
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static NYA_UIPanel row(NYA_UISize children) {
    return (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .children = children, .frameless = true };
}

/** What a label took: the column it sat alone in, as a space filling that column reads it back. */
typedef struct {
    f32 width;
    f32 below;
} Taken;

/** A top left panel in `target` holding one label, with a space under it, drawn until a shrunk size has loaded. */
static Taken label_in(NYA_Window* target, NYA_ConstCString id, NYA_UIPanel panel) {
    Taken taken = { 0 };

    for (u32 pass = 0; pass < 32; pass++) {
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });

        NYA_UI* ui = nya_ui_begin(target, NYA_UI_PASS_DRAW);

        if (nya_ui_panel_begin(ui, id, panel)) {
            nya_ui_label(ui, LONG_LINE);
            NYA_Rectf under = nya_ui_space(ui, 0.0F, 1.0F);
            nya_ui_panel_end(ui);

            taken = (Taken){ under.width, under.y };
        }

        nya_ui_end(ui);
    }

    return taken;
}

/** A button, a row of a button beside a row of a button and a slider, and a button; which one activated, or -1. */
static s32 grid(f32* value) {
    s32     activated = -1;
    NYA_UI* ui        = nya_ui_begin(&window, NYA_UI_PASS_INPUT);

    if (nya_ui_panel_begin(ui, "grid", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        if (nya_ui_button(ui, "top")) activated = 0;

        if (nya_ui_panel_begin(ui, nullptr, row(nya_ui_grow(1)))) {
            if (nya_ui_button(ui, "left")) activated = 1;

            if (nya_ui_panel_begin(ui, nullptr, row(nya_ui_grow(1)))) {
                if (nya_ui_button(ui, "inner")) activated = 2;
                (void)nya_ui_slider(ui, "value", value, 0.0F, 1.0F, 0.5F);
                nya_ui_panel_end(ui);
            }

            nya_ui_panel_end(ui);
        }

        if (nya_ui_button(ui, "bottom")) activated = 4;
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    return activated;
}

/** A tap, the tick that reads it, and the activation a return right after reports. */
static s32 after(NYA_Keycode keycode, f32* value) {
    tap(keycode);
    (void)grid(value);
    tick();

    tap(NYA_KEY_RETURN);
    s32 activated = grid(value);
    tick();

    return activated;
}

/** Eight buttons in a panel no taller than 160; the top of the content, and which button activated or -1. */
static NYA_Rectf list_top = { 0 };

static s32 list(NYA_UIPass pass) {
    s32     activated = -1;
    NYA_UI* ui        = nya_ui_begin(&window, pass);

    NYA_UIPanel panel = { .offset = { 20.0F, 20.0F }, .width = nya_ui_fixed(300), .height = { .kind = NYA_UI_SIZE_FIT, .max = 160.0F } };

    if (nya_ui_panel_begin(ui, "list", panel)) {
        list_top = nya_ui_space(ui, 0.0F, 0.0F);

        for (u32 i = 0; i < 8; i++) {
            char label[8];
            (void)snprintf(label, sizeof(label), "b%u", i);

            if (nya_ui_button(ui, label)) activated = (s32)i;
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    return activated;
}

/** Two passes of a frameless row `width` wide holding `count` spaces, each sized by `sizes`, into `out`. */
static void distribute(NYA_UISize sizes[], f32 naturals[], u32 count, f32 width, f32 gap, NYA_Rectf out[]) {
    for (u32 pass = 0; pass < 2; pass++) {
        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

        NYA_UIPanel panel = { .width = nya_ui_fixed(width), .direction = NYA_UI_DIRECTION_ROW, .gap = gap, .frameless = true };

        if (nya_ui_panel_begin(ui, "distribute", panel)) {
            for (u32 i = 0; i < count; i++) {
                nya_ui_size(ui, sizes[i]);
                out[i] = nya_ui_space(ui, naturals[i], 10.0F);
            }

            nya_ui_panel_end(ui);
        }

        nya_ui_end(ui);
    }
}

/** Whether `count` rects sit edge to edge along x from `start` with `gap` between, ending at `end`. */
static b8 contiguous(const NYA_Rectf rects[], u32 count, f32 start, f32 gap, f32 end) {
    f32 at = start;

    for (u32 i = 0; i < count; i++) {
        if (rects[i].x != at) return false;
        at += rects[i].width + (i + 1 < count ? gap : 0.0F);
    }

    return at == end;
}

s32 main(void) {
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
    nya_input_action_rebind(NYA_INPUT_ACTION_UP, NYA_KEY_UP);
    nya_input_action_rebind(NYA_INPUT_ACTION_DOWN, NYA_KEY_DOWN);
    nya_input_action_rebind(NYA_INPUT_ACTION_LEFT, NYA_KEY_LEFT);
    nya_input_action_rebind(NYA_INPUT_ACTION_RIGHT, NYA_KEY_RIGHT);

    nya_font_default_set(nya_font(FACE, 20.0F));
    for (u32 i = 0; i < 32 && nya_font_metrics(nya_font(FACE, 20.0F)).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    NYA_UIStyle style = { .body_size = 20.0F, .padding = FRAME, .spacing = GAP, .item_height = 40.0F };
    nya_ui_style_set(&window, style);
    nya_ui_style_set(&narrow, style);

    f32 line = ceilf(nya_font_metrics(nya_font(FACE, 20.0F)).line_height);

    // The default look is flat: no outline, shadow or pop, and labels run past their room.
    {
        NYA_Window  other    = { .handle = { .index = 3, .generation = 1 }, .screen_width = 100, .screen_height = 100 };
        NYA_UIStyle defaults = nya_ui_style_get(&other);

        nya_check(defaults.outline == 0.0F && defaults.depth == 0.0F && defaults.pop == 0.0F, "a zeroed style has no outline, depth or pop");
        nya_check(defaults.overflow == NYA_UI_OVERFLOW_VISIBLE && defaults.panel_skin.texture[0] == '\0', "lets text run, and draws flat");
        nya_check(defaults.button.focused.a > 0.0F && defaults.text.disabled.a > 0.0F, "and has a colour for every state");
    }

    // Along a row: fixed takes its pixels, fit its content, and grow shares the rest by weight, to the pixel.
    {
        NYA_Rectf  rects[4];
        NYA_UISize mixed[4]    = { nya_ui_fixed(100), nya_ui_fit(), nya_ui_grow(1), nya_ui_grow(2) };
        f32        naturals[4] = { 0.0F, 50.0F, 0.0F, 0.0F };

        distribute(mixed, naturals, 4, 400.0F, GAP, rects);

        // 400 less 100, 50 and three gaps of 6 leaves 232: a third and two thirds, rounded so they add up.
        nya_check(rects[0].width == 100.0F && rects[1].width == 50.0F, "fixed and fit, got %f %f", (f64)rects[0].width, (f64)rects[1].width);
        nya_check(rects[2].width == 77.0F && rects[3].width == 155.0F, "grow by weight, got %f %f", (f64)rects[2].width, (f64)rects[3].width);
        nya_check(contiguous(rects, 4, MARGIN, GAP, MARGIN + 400.0F), "edge to edge, ending on the row's end");

        NYA_UISize thirds[3] = { nya_ui_grow(1), nya_ui_grow(1), nya_ui_grow(1) };
        f32        none[3]   = { 0 };

        distribute(thirds, none, 3, 100.0F, 1.0F, rects);
        nya_check(rects[0].width + rects[1].width + rects[2].width == 98.0F, "98 in thirds adds up to 98, got %f %f %f", (f64)rects[0].width, (f64)rects[1].width, (f64)rects[2].width);
        nya_check(contiguous(rects, 3, MARGIN, 1.0F, MARGIN + 100.0F), "with no pixel lost to rounding");
    }

    // Min and max bound any kind, and a bounded grow leaves its siblings their shares of what was left before.
    {
        NYA_Rectf  rects[3];
        NYA_UISize bounded[3]  = { { .kind = NYA_UI_SIZE_FIXED, .value = 40.0F, .min = 80.0F }, { .kind = NYA_UI_SIZE_GROW, .value = 1.0F, .max = 50.0F }, nya_ui_grow(1) };
        f32        naturals[3] = { 0 };

        distribute(bounded, naturals, 3, 400.0F, GAP, rects);
        nya_check(rects[0].width == 80.0F, "min lifts a fixed size, got %f", (f64)rects[0].width);
        nya_check(rects[1].width == 50.0F, "max caps a share, got %f", (f64)rects[1].width);

        Taken wide = label_in(&window, "min_width", (NYA_UIPanel){ .width = { .kind = NYA_UI_SIZE_FIT, .min = 700.0F }, .frameless = true });
        nya_check(wide.width == 700.0F, "min widens a fitting panel, got %f", (f64)wide.width);
    }

    // Nesting: a row inside a row splits its cell, a column inside a row stacks in its cell, and across a column fills.
    {
        NYA_Rectf cell = { 0 };
        NYA_Rectf half = { 0 };
        NYA_Rectf low  = { 0 };

        for (u32 pass = 0; pass < 3; pass++) {
            NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

            if (nya_ui_panel_begin(ui, "nested", (NYA_UIPanel){ .width = nya_ui_fixed(300) })) {
                if (nya_ui_panel_begin(ui, nullptr, row(nya_ui_grow(1)))) {
                    cell = nya_ui_space(ui, 0.0F, 20.0F);

                    if (nya_ui_panel_begin(ui, nullptr, row(nya_ui_grow(1)))) {
                        (void)nya_ui_space(ui, 0.0F, 30.0F);
                        half = nya_ui_space(ui, 0.0F, 30.0F);
                        nya_ui_panel_end(ui);
                    }

                    nya_ui_panel_end(ui);
                }

                low = nya_ui_space(ui, 0.0F, 1.0F);
                nya_ui_panel_end(ui);
            }

            nya_ui_end(ui);
        }

        f32 content = 300.0F - (FRAME * 2.0F);
        f32 outer   = roundf((content - GAP) * 0.5F);
        f32 inner   = roundf((content - outer - GAP - GAP) * 0.5F);

        nya_check(cell.width == outer, "the outer row halves the column, got %f", (f64)cell.width);
        nya_check(half.x == MARGIN + FRAME + outer + GAP + inner + GAP && half.width == content - outer - GAP - inner - GAP, "the inner row halves the second cell, got %f %f", (f64)half.x, (f64)half.width);
        nya_check(low.y == MARGIN + FRAME + 30.0F + GAP && low.width == content, "the tallest cell sets the row's height, and a space fills the column, got %f", (f64)low.y);
    }

    // Scale: the style's, or 1, and never the window's size. See ui.h for why the window is deliberately not in it.
    {
        struct {
            u32 height;
            f32 scale;
            f32 expected;
        } cases[] = {
            { 720,  0.0F,  1.0F  },
            { 1440, 0.0F,  1.0F  },
            { 1080, 0.0F,  1.0F  },
            { 600,  0.0F,  1.0F  },
            { 2160, 0.0F,  1.0F  },
            { 720,  1.25F, 1.25F },
            { 2160, 2.0F,  2.0F  },
            // under the floor, which is there so a mistyped setting cannot make the UI unreadable.
            { 720,  0.1F,  0.5F  },
        };

        for (u32 i = 0; i < nya_carray_length(cases); i++) {
            NYA_Window  sized  = { .handle = { .index = 4, .generation = 1 + i }, .screen_width = 1280, .screen_height = cases[i].height };
            NYA_UIStyle scaled = style;
            scaled.scale       = cases[i].scale;
            nya_ui_style_set(&sized, scaled);

            NYA_Rectf inside = { 0 };

            for (u32 pass = 0; pass < 2; pass++) {
                NYA_UI* ui = nya_ui_begin(&sized, NYA_UI_PASS_DRAW);

                if (nya_ui_panel_begin(ui, "scaled", (NYA_UIPanel){ .width = nya_ui_fixed(200) })) {
                    inside = nya_ui_space(ui, 0.0F, 10.0F);
                    nya_ui_panel_end(ui);
                }

                nya_ui_end(ui);
            }

            f32 s = cases[i].expected;
            nya_check(nya_ui_scale(&sized) == s, "a %u tall window at scale %f reads back %f, got %f", cases[i].height, (f64)cases[i].scale, (f64)s,
                      (f64)nya_ui_scale(&sized));
            nya_check(inside.x == roundf(MARGIN * s) + roundf(FRAME * s) && inside.width == roundf(200.0F * s) - (roundf(FRAME * s) * 2.0F) && inside.height == roundf(10.0F * s),
                      "margin, padding, sizes and spaces all scale at %f, got %f %f", (f64)s, (f64)inside.x, (f64)inside.width);
        }
    }

    // Overflow: visible runs past a panel held to the safe area, wrap breaks the line in the room, shrink drops a size.
    {
        f32 room = 300.0F - (MARGIN * 2.0F) - (FRAME * 2.0F);

        Taken visible = label_in(&narrow, "visible", (NYA_UIPanel){ 0 });
        nya_check(visible.width == room && visible.below == MARGIN + FRAME + line + GAP, "visible stays one line in a panel held to the room, got %f %f", (f64)visible.width,
                  (f64)visible.below);

        Taken wrapped = label_in(&narrow, "wrapped", (NYA_UIPanel){ .overflow = NYA_UI_OVERFLOW_WRAP });
        nya_check(wrapped.width <= room && wrapped.width > room * 0.5F, "wrap fills at most the room, got %f", (f64)wrapped.width);
        nya_check(wrapped.below >= MARGIN + FRAME + (line * 2.0F) + GAP, "on more than one line, got %f", (f64)wrapped.below);

        Taken shrunk = label_in(&narrow, "shrunk", (NYA_UIPanel){ .overflow = NYA_UI_OVERFLOW_SHRINK });
        nya_check(shrunk.width <= room && shrunk.width > room * 0.8F, "shrink fits the room closely, got %f", (f64)shrunk.width);
        nya_check(shrunk.below == MARGIN + FRAME + line + GAP, "and keeps one line of the column's height, got %f", (f64)shrunk.below);

        Taken offset = label_in(&narrow, "offset", (NYA_UIPanel){ .offset = { 30.0F, 0.0F }, .overflow = NYA_UI_OVERFLOW_WRAP });
        nya_check(offset.width <= room - 60.0F, "the offset comes off both sides, got %f", (f64)offset.width);

        Taken capped = label_in(&window, "capped", (NYA_UIPanel){ .width = { .kind = NYA_UI_SIZE_FIT, .max = 200.0F }, .overflow = NYA_UI_OVERFLOW_WRAP });
        nya_check(capped.width <= 200.0F - (FRAME * 2.0F), "a max width caps the room, got %f", (f64)capped.width);

        NYA_UIStyle wrapping = style;
        wrapping.overflow    = NYA_UI_OVERFLOW_WRAP;
        nya_ui_style_set(&narrow, wrapping);

        nya_check(label_in(&narrow, "inherits", (NYA_UIPanel){ 0 }).width <= room, "a panel inherits the style's overflow");
        nya_check(label_in(&narrow, "overrides", (NYA_UIPanel){ .overflow = NYA_UI_OVERFLOW_VISIBLE }).below == visible.below, "and its own overrides it");

        nya_ui_style_set(&narrow, style);
    }

    // Focus moves by lines up and down and by cells left and right; a slider keeps left and right.
    {
        f32 value = 0.0F;

        nya_ui_focus_reset(&window);
        (void)grid(&value);
        tick();
        (void)grid(&value);
        tick();

        nya_check(after(NYA_KEY_DOWN, &value) == 1, "down from the top lands on the row's first cell");
        nya_check(after(NYA_KEY_RIGHT, &value) == 2, "right moves into the inner row");
        nya_check(after(NYA_KEY_LEFT, &value) == 1, "left moves back");
        nya_check(after(NYA_KEY_LEFT, &value) == 1, "and stops at the first cell");
        nya_check(after(NYA_KEY_DOWN, &value) == 4, "down leaves the whole row");
        nya_check(after(NYA_KEY_UP, &value) == 1, "up comes back to its first cell");

        (void)after(NYA_KEY_RIGHT, &value);
        tap(NYA_KEY_RIGHT);
        (void)grid(&value);
        tick();
        nya_check(value == 0.0F, "the slider is not focused yet");

        tap(NYA_KEY_RIGHT);
        (void)grid(&value);
        tick();
        nya_check(value == 0.5F, "on the slider, right moves the value, got %f", (f64)value);

        nya_check(after(NYA_KEY_DOWN, &value) == 4, "a position past the next line's end lands on its last widget");
        nya_check(after(NYA_KEY_DOWN, &value) == 0, "and down from the last line wraps to the first");
    }

    // Disabled widgets draw but take no focus and never act; the keys step over them.
    {
        nya_ui_focus_reset(&window);

        s32 activated = -1;

        for (u32 round = 0; round < 3; round++) {
            if (round == 1) tap(NYA_KEY_DOWN);
            if (round == 2) tap(NYA_KEY_RETURN);

            NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_INPUT);

            if (nya_ui_panel_begin(ui, "disabled", (NYA_UIPanel){ .width = nya_ui_fixed(300) })) {
                if (nya_ui_button(ui, "first")) activated = 0;

                nya_ui_disabled_begin(ui);
                if (nya_ui_button(ui, "off")) activated = 1;
                nya_ui_disabled_end(ui);

                if (nya_ui_button(ui, "last")) activated = 2;
                nya_ui_panel_end(ui);
            }

            nya_ui_end(ui);
            tick();
        }

        nya_check(activated == 2, "down from the first skips the disabled one, got %d", activated);
    }

    // A pushed style applies until popped, and both must balance.
    {
        NYA_Rectf pushed = { 0 };
        NYA_Rectf popped = { 0 };

        NYA_UIStyle roomy = style;
        roomy.spacing     = 30.0F;

        for (u32 pass = 0; pass < 2; pass++) {
            NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

            if (nya_ui_panel_begin(ui, "pushed", (NYA_UIPanel){ .width = nya_ui_fixed(300), .frameless = true })) {
                (void)nya_ui_space(ui, 0.0F, 10.0F);

                nya_ui_style_push(ui, roomy);
                if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .frameless = true })) {
                    (void)nya_ui_space(ui, 0.0F, 10.0F);
                    pushed = nya_ui_space(ui, 0.0F, 10.0F);
                    nya_ui_panel_end(ui);
                }
                nya_ui_style_pop(ui);

                popped = nya_ui_space(ui, 0.0F, 10.0F);
                nya_ui_panel_end(ui);
            }

            if (pass == 1) nya_expect_crash(nya_ui_style_pop(ui));
            nya_ui_end(ui);
        }

        nya_check(pushed.y == MARGIN + 10.0F + GAP + 10.0F + 30.0F, "children of the pushed style get its gap, got %f", (f64)pushed.y);
        nya_check(popped.y == MARGIN + 10.0F + GAP + 50.0F + GAP, "and after the pop the window's gap is back, got %f", (f64)popped.y);
    }

    // A skinned panel pads by its skin's insets, scaled; a region with no texture stays flat and keeps the style's padding.
    {
        NYA_UIStyle skinned = style;
        (void)snprintf(skinned.panel_skin.texture, sizeof(skinned.panel_skin.texture), "%s", "./assets/ui/sheet.png");
        skinned.panel_skin.left   = 8.0F;
        skinned.panel_skin.top    = 4.0F;
        skinned.panel_skin.right  = 6.0F;
        skinned.panel_skin.bottom = 2.0F;
        skinned.scale             = 2.0F;

        NYA_Window sheet = { .handle = { .index = 5, .generation = 1 }, .screen_width = 800, .screen_height = 600 };
        nya_ui_style_set(&sheet, skinned);

        NYA_Rectf inside = { 0 };

        NYA_UI* ui = nya_ui_begin(&sheet, NYA_UI_PASS_INPUT);
        if (nya_ui_panel_begin(ui, "skinned", (NYA_UIPanel){ .width = nya_ui_fixed(100) })) {
            inside = nya_ui_space(ui, 0.0F, 1.0F);
            nya_ui_panel_end(ui);
        }
        nya_ui_end(ui);

        nya_check(inside.x == (MARGIN * 2.0F) + 16.0F && inside.y == (MARGIN * 2.0F) + 8.0F && inside.width == 200.0F - 28.0F, "the insets pad at twice their size, got %f %f %f", (f64)inside.x,
                  (f64)inside.y, (f64)inside.width);

        skinned.panel_skin.texture[0] = '\0';
        nya_ui_style_set(&sheet, skinned);

        ui = nya_ui_begin(&sheet, NYA_UI_PASS_INPUT);
        if (nya_ui_panel_begin(ui, "skinned", (NYA_UIPanel){ .width = nya_ui_fixed(100) })) {
            inside = nya_ui_space(ui, 0.0F, 1.0F);
            nya_ui_panel_end(ui);
        }
        nya_ui_end(ui);

        nya_check(inside.x == (MARGIN * 2.0F) + (FRAME * 2.0F), "without a texture the panel is flat and padded by the style, got %f", (f64)inside.x);
    }

    // Scrolling: the wheel over the panel scrolls it and clamps, the pointer only reaches what shows, and focus scrolls into view.
    {
        nya_ui_focus_reset(&window);

        for (u32 i = 0; i < 2; i++) (void)list(NYA_UI_PASS_DRAW);

        f32 top = list_top.y;
        nya_check(top == MARGIN + 20.0F + FRAME, "unscrolled, the content starts inside the frame, got %f", (f64)top);

        // 8 buttons of 40, each after a gap, in a view of 160 less the frame.
        f32 reach = (8.0F * (40.0F + GAP)) - (160.0F - (FRAME * 2.0F));

        pointer_move((f32x2){ 600.0F, 80.0F });
        wheel(-1.0F);
        (void)list(NYA_UI_PASS_INPUT);
        tick();
        (void)list(NYA_UI_PASS_DRAW);
        nya_check(list_top.y == top, "the wheel away from the panel does nothing");

        pointer_move((f32x2){ 100.0F, 80.0F });
        wheel(-1.0F);
        (void)list(NYA_UI_PASS_INPUT);
        tick();
        (void)list(NYA_UI_PASS_DRAW);
        nya_check(list_top.y == top - NYA_UI_SCROLL_STEP, "a notch down over it scrolls one step, got %f", (f64)list_top.y);

        wheel(-100.0F);
        (void)list(NYA_UI_PASS_INPUT);
        tick();
        (void)list(NYA_UI_PASS_DRAW);
        nya_check(fabsf(list_top.y - (top - reach)) < 0.01F, "and no further than the content goes, got %f", (f64)list_top.y);

        // b0 now sits above the view, so a click where it is falls outside the clip.
        pointer_move((f32x2){ 100.0F, list_top.y + 20.0F });
        (void)list(NYA_UI_PASS_INPUT);
        tick();
        click();
        nya_check(list(NYA_UI_PASS_INPUT) == -1, "a click on a widget scrolled out of view reaches nothing");
        tick();

        wheel(100.0F);
        (void)list(NYA_UI_PASS_INPUT);
        tick();

        // up from the first wraps to the last, which is below the view until the panel follows.
        nya_ui_focus_reset(&window);
        pointer_move((f32x2){ 700.0F, 500.0F });
        (void)list(NYA_UI_PASS_INPUT);
        tick();

        tap(NYA_KEY_UP);
        (void)list(NYA_UI_PASS_INPUT);
        tick();
        (void)list(NYA_UI_PASS_DRAW);
        (void)list(NYA_UI_PASS_DRAW);

        nya_check(fabsf(list_top.y - (top - reach)) < 0.01F, "focus on the last button scrolls to the end, got %f", (f64)list_top.y);

        tap(NYA_KEY_DOWN);
        (void)list(NYA_UI_PASS_INPUT);
        tick();
        (void)list(NYA_UI_PASS_DRAW);
        (void)list(NYA_UI_PASS_DRAW);

        nya_check(list_top.y + GAP == top, "and wrapping to the first scrolls back until it is at the top of the view, got %f", (f64)list_top.y);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
