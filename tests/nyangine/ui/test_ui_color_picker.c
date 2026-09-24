/**
 * The colour picker and panel appearance, headless: dragging the field, hue and alpha sets those parts and keeps the
 * part a press grabbed, the keys turn the hue, a grey keeps the hue it was dragged from, the hex field takes whole
 * colours only, and a panel slides in on the wall clock when it shows again, and not at all with no duration.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/**
 * Where the picker's parts land in a 300 wide panel at the default margin of 16, with the test style's padding 10,
 * spacing 6 and item height 40: the field under the label row, the hue bar beside it, the alpha bar under both, and
 * the hex box after the swatch, 40% across.
 * */
#define PLANE ((NYA_Rectf){ 36.0F, 72.0F, 242.0F, 96.0F })
#define HUE   ((NYA_Rectf){ 284.0F, 72.0F, 12.0F, 96.0F })
#define ALPHA ((NYA_Rectf){ 36.0F, 174.0F, 260.0F, 12.0F })
#define HEX   ((NYA_Rectf){ 174.0F, 31.0F, 122.0F, 30.0F })

static NYA_Color tint = { 1.0F, 0.0F, 0.0F, 1.0F };

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

static void pointer_button(b8 down) {
    f32x2     at    = nya_input_mouse_position();
    NYA_Event event = { .type = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = down, .button = NYA_MOUSE_BUTTON_LEFT, .x = at.x, .y = at.y } };
    nya_system_input_handle_event(&event);
}

static void type(NYA_ConstCString text) {
    NYA_Event event = { .type = NYA_EVENT_TEXT_INPUT, .as_text_input_event = { .text = text } };
    nya_system_input_handle_event(&event);
}

/** One input pass over a panel holding the picker, and the tick after it. Whether the colour changed. */
static b8 picker(void) {
    b8      changed = false;
    NYA_UI* ui      = nya_ui_begin(&window, NYA_UI_PASS_INPUT);

    if (nya_ui_panel_begin(ui, "picker", (NYA_UIPanel){ .width = nya_ui_fixed(300) })) {
        changed = nya_ui_color_picker(ui, "tint", &tint);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;

    return changed;
}

static f32x2 at(NYA_Rectf rect, f32 across, f32 down) {
    return (f32x2){ rect.x + (rect.width * across), rect.y + (rect.height * down) };
}

static b8 near(f32 a, f32 b) {
    return fabsf(a - b) < 0.02F;
}

/** Moves the wall clock the UI reads on by `seconds`. */
static void wait(f64 seconds) {
    nya_app_get()->frame_stats.started_ns -= (u64)(seconds * 1e9);
}

/** Where a top level panel's content starts down the window after one draw pass. */
static f32 appear_top(void) {
    f32     top = 0.0F;
    NYA_UI* ui  = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

    if (nya_ui_panel_begin(ui, "appearing", (NYA_UIPanel){ .width = nya_ui_fixed(100) })) {
        top = nya_ui_space(ui, 0.0F, 10.0F).y;
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    return top;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .frame_stats = { .started_ns = nya_clock_get_monotonic_ns() } };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();
    nya_system_window_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_window_deinit();
    defer nya_world_destroy(world);

    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);
    nya_input_action_rebind(NYA_INPUT_ACTION_LEFT, NYA_KEY_LEFT);
    nya_input_action_rebind(NYA_INPUT_ACTION_RIGHT, NYA_KEY_RIGHT);

    nya_font_default_set(nya_font(FACE, 20.0F));
    for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 20.0F, .padding = 10.0F, .spacing = 6.0F, .item_height = 40.0F });

    // Untouched, the colour stays exactly as it was.
    {
        (void)picker();
        nya_check(!picker() && tint.r == 1.0F && tint.g == 0.0F, "a pass with no input leaves it alone");
    }

    // The field sets saturation across and value down; the press keeps the field while it is dragged past it.
    {
        pointer_move(at(PLANE, 0.5F, 0.25F));
        pointer_button(true);
        nya_check(picker(), "a press on the field sets the colour");

        NYA_ColorHSV hsv = nya_color_to_hsv(tint);
        nya_check(near(hsv.s, 0.5F) && near(hsv.v, 0.75F) && near(hsv.h, 0.0F), "half saturated, three quarters bright, still red, got %f %f %f", (f64)hsv.s, (f64)hsv.v,
                  (f64)hsv.h);

        pointer_move(at(HUE, 0.5F, 0.5F));
        (void)picker();
        hsv = nya_color_to_hsv(tint);
        nya_check(near(hsv.s, 1.0F) && near(hsv.h, 0.0F), "dragged over the hue bar, the field keeps the pointer, got %f %f", (f64)hsv.s, (f64)hsv.h);

        pointer_button(false);
        (void)picker();
    }

    // The hue bar sets the hue top to bottom, the alpha bar the alpha left to right, and the keys turn the hue.
    {
        tint = (NYA_Color){ 1.0F, 0.0F, 0.0F, 1.0F };

        pointer_move(at(HUE, 0.5F, 1.0F / 3.0F));
        pointer_button(true);
        (void)picker();
        pointer_button(false);
        (void)picker();
        nya_check(near(nya_color_to_hsv(tint).h / 360.0F, 1.0F / 3.0F), "a third down is green, got %f", (f64)nya_color_to_hsv(tint).h);

        pointer_move(at(ALPHA, 0.25F, 0.5F));
        pointer_button(true);
        (void)picker();
        pointer_button(false);
        (void)picker();
        nya_check(near(tint.a, 0.25F), "a quarter across the alpha bar, got %f", (f64)tint.a);

        f32 hue = nya_color_to_hsv(tint).h;
        tap(NYA_KEY_RIGHT);
        (void)picker();
        nya_check(near(nya_color_to_hsv(tint).h, hue + 10.0F), "right turns the hue, got %f", (f64)nya_color_to_hsv(tint).h);
    }

    // A grey has no hue, so the one it had is kept for the next drag across the field.
    {
        tint = nya_color_from_hsv((NYA_ColorHSV){ 200.0F, 1.0F, 1.0F, 1.0F });
        (void)picker();

        pointer_move(at(PLANE, 0.0F, 0.5F));
        pointer_button(true);
        (void)picker();

        pointer_move(at(PLANE, 1.0F, 0.0F));
        (void)picker();
        pointer_button(false);
        (void)picker();

        nya_check(near(nya_color_to_hsv(tint).h, 200.0F), "through white and back, still that hue, got %f", (f64)nya_color_to_hsv(tint).h);
    }

    // The hex field: a click starts typing, partial digits wait, whole ones set the colour with or without alpha.
    {
        pointer_move(at(HEX, 0.5F, 0.5F));
        pointer_button(true);
        (void)picker();
        pointer_button(false);
        (void)picker();
        nya_check(nya_ui_typing(&window), "a click on the hex starts typing");

        NYA_Color held = tint;

        // end first: a click puts the caret where it landed, which is in the middle of the digits here.
        tap(NYA_KEY_END);
        (void)picker();

        for (u32 i = 0; i < 9; i++) {
            tap(NYA_KEY_BACKSPACE);
            (void)picker();
        }

        type("#12");
        nya_check(!picker() && tint.r == held.r && tint.a == held.a, "three digits are not a colour yet");

        type("abef");
        nya_check(picker() && tint.r == 0x12 / 255.0F && tint.g == 0xAB / 255.0F && tint.b == 0xEF / 255.0F && tint.a == 1.0F, "six are, got %f %f %f", (f64)tint.r,
                  (f64)tint.g, (f64)tint.b);

        type("80");
        nya_check(picker() && near(tint.a, 0x80 / 255.0F), "and eight carry alpha, got %f", (f64)tint.a);

        type("zz");
        nya_check(!picker(), "past eight, nothing");

        tap(NYA_KEY_RETURN);
        (void)picker();
        (void)picker();
        nya_check(!nya_ui_typing(&window), "return stops typing");
    }

    // Appearing: a panel slides up into place over the duration, again after being gone, and not at all without one.
    {
        NYA_UIStyle style = nya_ui_style_get(&window);

        f32 still = appear_top();
        wait(1.0);
        nya_check(appear_top() == still, "no duration, no slide");

        style.appear_s = 0.2F;
        style.easing   = NYA_EASE_LINEAR;
        nya_ui_style_set(&window, style);

        wait(1.0);
        f32 start = appear_top();
        nya_check(start == still + NYA_UI_APPEAR_OFFSET, "back after a second, it starts below, got %f", (f64)start);

        wait(0.1);
        f32 half = appear_top();
        nya_check(fabsf(half - (still + (NYA_UI_APPEAR_OFFSET * 0.5F))) <= 1.0F, "halfway through, halfway up, got %f", (f64)half);

        wait(0.15);
        nya_check(appear_top() == still, "and in place once it is over");

        wait(0.1);
        nya_check(appear_top() == still, "a gap shorter than NYA_UI_APPEAR_GAP_S does not restart it");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
