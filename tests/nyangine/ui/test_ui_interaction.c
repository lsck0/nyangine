/**
 * What the value widgets do when a pointer reaches them, headless: a button activates on a click and
 * not on a hover, a toggle flips the b8 it owns, a slider takes the value the pointer is over, a
 * selectable reports the click and shows that it is the chosen one — and a disabled widget does none of
 * it. The pointer is synthetic and the presenter is the recorder, so a click is exact arithmetic against
 * a rectangle the layout computed rather than anything a screen had to show.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static NYA_UIRecorder recorder;

/*
 * ─────────────────────────────────────────────────────────
 * SYNTHETIC POINTER
 * ─────────────────────────────────────────────────────────
 */

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = {
        .type                 = NYA_EVENT_MOUSE_MOVED,
        .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y },
    };
    nya_system_input_handle_event(&event);
}

static void pointer_button(b8 down) {
    f32x2     at    = nya_input_mouse_position();
    NYA_Event event = {
        .type                  = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP,
        .as_mouse_button_event = { .is_down = down, .button = NYA_MOUSE_BUTTON_LEFT, .x = at.x, .y = at.y },
    };
    nya_system_input_handle_event(&event);
}

/** Ends the input frame, so this pass's presses do not linger into the next. */
static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
}

static f32x2 center_of(NYA_Rectf rect) {
    return (f32x2){ rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };
}

/** The rectangle a widget of `kind` with `label` was drawn at last recorded pass. */
static NYA_Rectf widget_rect(NYA_UIWidgetKind kind, NYA_ConstCString label) {
    const NYA_UIWidgetDraw* widget = nya_ui_recorder_find(&recorder, kind, label);
    nya_assert(widget != nullptr, "no %s labelled '%s' was drawn", nya_ui_widget_kind_name(kind), label);

    return widget->rect;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE COMPONENTS UNDER TEST
 * ─────────────────────────────────────────────────────────
 */

/* what each pass wrote and read, so a check reads the outcome rather than guessing. */
static b8  out_button    = false;
static b8  state_toggle  = false;
static b8  out_toggle    = false;
static f32 state_volume  = 0.5F;
static b8  out_slider    = false;
static u32 state_choice  = 0;
static b8  out_selected  = false;
static b8  make_disabled = false;

static void panel_of(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "panel", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_TOP_LEFT, .width = nya_ui_fixed(300) })) {
        if (make_disabled) nya_ui_disabled_begin(ui);

        out_button   = nya_ui_button(ui, "press me");
        out_toggle   = nya_ui_toggle(ui, "flag", &state_toggle);
        out_slider   = nya_ui_slider(ui, "volume", &state_volume, 0.0F, 1.0F, 0.0F);
        out_selected = nya_ui_selectable(ui, "pick", state_choice == 1);

        if (make_disabled) nya_ui_disabled_end(ui);

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/** Draws the panel to record its rectangles, then runs an input pass with the pointer where it is now. */
static void draw(void) {
    nya_ui_recorder_reset(&recorder);
    panel_of(NYA_UI_PASS_DRAW);
}

static void input(void) {
    panel_of(NYA_UI_PASS_INPUT);
    tick();
}

/** A whole click on a rectangle's centre: move, press, release, then an input pass to apply it. */
static void click(NYA_Rectf rect) {
    pointer_move(center_of(rect));
    pointer_button(true);
    pointer_button(false);
    input();
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());
    defer SDL_Quit();

    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();

    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_asset_deinit();

    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);

    // the recorder measures in cells, so every rectangle a check clicks is exact and needs no font.
    nya_ui_recorder_init(&recorder, NYA_UI_RECORD_CELL);
    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));

    // TEST: a button activates on a click, once, and not on a hover.
    {
        input(); // settle: no pointer over anything yet
        draw();

        NYA_Rectf button = widget_rect(NYA_UI_WIDGET_BUTTON, "press me");

        // a hover is not a click.
        pointer_move(center_of(button));
        input();
        nya_check(!out_button, "hovering a button does not activate it");

        click(button);
        nya_check(out_button, "clicking it does");

        // the same pointer resting on it, no new press, is not a second activation.
        input();
        nya_check(!out_button, "and it does not activate again while the pointer just rests on it");
    }

    // TEST: a toggle owns its b8 and flips it on each click.
    {
        state_toggle = false;
        draw();

        NYA_Rectf toggle = widget_rect(NYA_UI_WIDGET_TOGGLE, "flag");

        click(toggle);
        nya_check(state_toggle, "a click turns the flag on");
        nya_check(out_toggle, "and the toggle reports the change");

        click(toggle);
        nya_check(!state_toggle, "another click turns it off again");
    }

    // TEST: a slider takes the value the pointer is over, and clamps to its bounds.
    {
        state_volume = 0.5F;
        draw();

        const NYA_UIWidgetDraw* slider = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SLIDER, "volume");
        nya_assert(slider != nullptr, "the slider was drawn");

        NYA_Rectf track = slider->as_slider.track;

        // press at the far right of the track: the value goes to the top of its range.
        pointer_move((f32x2){ track.x + track.width, track.y + (track.height * 0.5F) });
        pointer_button(true);
        input();
        nya_check(state_volume > 0.95F, "dragging to the right end reaches the maximum, got %.3f", (f64)state_volume);

        // still held, drag to the far left: it follows to the bottom.
        pointer_move((f32x2){ track.x - 100.0F, track.y + (track.height * 0.5F) });
        input();
        nya_check(state_volume < 0.05F, "dragging past the left end clamps to the minimum, got %.3f", (f64)state_volume);

        pointer_button(false);
        input();
    }

    // TEST: a selectable reports its click and shows that it is chosen.
    {
        state_choice = 0;
        draw();

        NYA_Rectf pick = widget_rect(NYA_UI_WIDGET_SELECTABLE, "pick");

        const NYA_UIWidgetDraw* before = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SELECTABLE, "pick");
        nya_check(!before->as_choice.on, "the selectable is not chosen to begin with");

        click(pick);
        nya_check(out_selected, "clicking it reports the activation");

        // a program flips its own selection on that activation; here the test is the program.
        state_choice = 1;
        draw();

        const NYA_UIWidgetDraw* after = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SELECTABLE, "pick");
        nya_check(after->as_choice.on, "and once chosen it draws as chosen");
    }

    // TEST: a disabled widget does nothing, whatever the pointer does to it.
    {
        make_disabled = true;
        state_toggle  = false;
        state_volume  = 0.5F;

        input();
        draw();

        // the widgets are drawn disabled: the recorder sees the state, and a click changes nothing.
        const NYA_UIWidgetDraw* button = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "press me");
        nya_check(button->state.disabled, "a disabled button draws as disabled");

        click(widget_rect(NYA_UI_WIDGET_BUTTON, "press me"));
        nya_check(!out_button, "and a click on it does not activate");

        click(widget_rect(NYA_UI_WIDGET_TOGGLE, "flag"));
        nya_check(!state_toggle, "a disabled toggle does not flip");

        NYA_Rectf track = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SLIDER, "volume")->as_slider.track;
        pointer_move((f32x2){ track.x + track.width, track.y + (track.height * 0.5F) });
        pointer_button(true);
        input();
        pointer_button(false);
        input();
        nya_check(fabsf(state_volume - 0.5F) < 0.001F, "a disabled slider does not move, got %.3f", (f64)state_volume);

        make_disabled = false;
    }

    nya_ui_recorder_deinit(&recorder);

    return nya_check_failures() == 0 ? 0 : 1;
}
