/**
 * nya_ui_pointer_over: whether an input pass found the pointer over a top level titled or draggable panel,
 * which is how a scene drawn under a non-modal UI knows a click was the panel's and not the world's.
 *
 * Headless, synthetic pointer, recorder presenter — the same setup as test_ui_interaction. The bug this
 * guards against was a real one: gnyame's 3D feature panel was only ever drawn, so a click on it also
 * grabbed a cube behind it; the fix pairs the draw pass with an input pass and reads this flag.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static NYA_Window   window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };
static NYA_UIRecorder recorder;

/* Which panel the pass draws, so one helper serves the titled, draggable and frameless cases. */
typedef enum { PANEL_TITLED, PANEL_DRAGGABLE, PANEL_FRAMELESS } PanelKind;
static PanelKind panel_kind;

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = {
        .type                 = NYA_EVENT_MOUSE_MOVED,
        .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y },
    };
    nya_system_input_handle_event(&event);
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
}

static NYA_UIPanel panel_spec(void) {
    switch (panel_kind) {
        case PANEL_DRAGGABLE: return (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_TOP_LEFT, .width = nya_ui_fixed(300), .draggable = true };
        case PANEL_FRAMELESS: return (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_TOP_LEFT, .width = nya_ui_fixed(300), .frameless = true };
        case PANEL_TITLED:
        default:              return (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_TOP_LEFT, .width = nya_ui_fixed(300), .title = "Panel" };
    }
}

static void panel_of(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "panel", panel_spec())) {
        (void)nya_ui_button(ui, "press me");
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/** A pass pair so the panel measures and takes its place before a rectangle is read from it. */
static void settle(void) {
    panel_of(NYA_UI_PASS_DRAW);
    panel_of(NYA_UI_PASS_INPUT);
    tick();
}

/** The button's recorded rectangle, a point known to be inside whichever panel drew it. */
static NYA_Rectf button_rect(void) {
    const NYA_UIWidgetDraw* widget = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "press me");
    nya_assert(widget != nullptr, "no button was drawn");
    return widget->rect;
}

/** Draw pass (records rectangles) then an input pass with the pointer where it is now; returns the flag. */
static b8 over_after(f32x2 point) {
    nya_ui_recorder_reset(&recorder);
    panel_of(NYA_UI_PASS_DRAW);

    pointer_move(point);
    panel_of(NYA_UI_PASS_INPUT);
    b8 over = nya_ui_pointer_over(&window);
    tick();
    return over;
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

    nya_ui_recorder_init(&recorder, NYA_UI_RECORD_CELL);
    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));
    defer nya_ui_recorder_deinit(&recorder);

    f32x2 far_corner = { (f32)window.screen_width - 5.0F, (f32)window.screen_height - 5.0F };

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a titled panel swallows a pointer inside it, and does not claim one outside.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        panel_kind = PANEL_TITLED;
        settle();

        nya_ui_recorder_reset(&recorder);
        panel_of(NYA_UI_PASS_DRAW);
        f32x2 inside = (f32x2){ button_rect().x + 4.0F, button_rect().y + 4.0F };

        nya_check(over_after(inside), "a titled panel claims a pointer inside it");
        nya_check(!over_after(far_corner), "and claims nothing in the far corner outside it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a draggable panel does the same (a click on it is a drag or a widget, never the world).
    // ─────────────────────────────────────────────────────────────────────────────
    {
        panel_kind = PANEL_DRAGGABLE;
        settle();

        nya_ui_recorder_reset(&recorder);
        panel_of(NYA_UI_PASS_DRAW);
        f32x2 inside = (f32x2){ button_rect().x + 4.0F, button_rect().y + 4.0F };

        nya_check(over_after(inside), "a draggable panel claims a pointer inside it");
        nya_check(!over_after(far_corner), "and nothing outside it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a frameless HUD takes no clicks, so it never claims the pointer.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        panel_kind = PANEL_FRAMELESS;
        settle();

        nya_ui_recorder_reset(&recorder);
        panel_of(NYA_UI_PASS_DRAW);
        f32x2 inside = (f32x2){ button_rect().x + 4.0F, button_rect().y + 4.0F };

        nya_check(!over_after(inside), "a frameless overlay does not swallow a pointer over it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the flag is cleared each input pass, so a pass with no panel claims nothing.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        panel_kind = PANEL_TITLED;
        (void)over_after((f32x2){ 10.0F, 10.0F });   // leaves the flag set for a pointer over the panel

        NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_INPUT);
        nya_ui_end(ui);
        nya_check(!nya_ui_pointer_over(&window), "an input pass that drew no panel clears the flag");
        tick();
    }

    printf("test_ui_pointer_over: all passed\n");
    return 0;
}
