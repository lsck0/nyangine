/**
 * The presenter seam: one widget tree through two backends. That a pass declares the same widgets in the same order
 * whichever presenter is installed, that what the widgets return does not depend on it, that every size comes out of
 * the presenter's own measurement, and that a UI can be driven and read with no GPU and no font at all.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static void tap(NYA_Keycode keycode) {
    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = { .type = i == 0 ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP, .as_key_event = { .is_down = i == 0, .key = keycode } };
        nya_system_input_handle_event(&event);
    }
}

/** The end of an update tick: edges roll and the tick advances. */
static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

/** What one pass of the screen below decided. Nothing in here is a pixel, so both presenters must agree on it. */
typedef struct {
    b8  applied;
    b8  fullscreen;
    f32 volume;
    u32 quality;
} Result;

static b8  state_fullscreen = false;
static f32 state_volume     = 0.5F;
static u32 state_quality    = 0;

/**
 * The widget tree under test. Not one line of it knows which presenter it is running on, which is the whole claim
 * being checked: it is the same function in every case below.
 * */
static Result screen(NYA_UIPass pass) {
    Result   result = { 0 };
    NYA_UI*  ui     = nya_ui_begin(&window, pass);

    nya_ui_scrim(ui);

    if (nya_ui_panel_begin(ui, "settings", (NYA_UIPanel){ .width = nya_ui_fixed(320), .title = "settings" })) {
        nya_ui_label(ui, "audio");

        result.applied = nya_ui_button(ui, "apply");

        (void)nya_ui_toggle(ui, "fullscreen", &state_fullscreen);
        (void)nya_ui_slider(ui, "volume", &state_volume, 0.0F, 1.0F, 0.25F);
        (void)nya_ui_radio(ui, "low", &state_quality, 0);
        (void)nya_ui_radio(ui, "high", &state_quality, 1);

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);

    result.fullscreen = state_fullscreen;
    result.volume     = state_volume;
    result.quality    = state_quality;

    return result;
}

/**
 * Two draw passes, since a container laid out for the first time draws nothing until it has been measured, with
 * `recorder` emptied between them so what is left is one whole pass rather than a half one and a whole one.
 * */
static void draw_twice(NYA_UIRecorder* recorder) {
    (void)screen(NYA_UI_PASS_DRAW);

    if (recorder != nullptr) nya_ui_recorder_reset(recorder);

    (void)screen(NYA_UI_PASS_DRAW);
}

/** The widgets the screen above declares, in declaration order. */
static const NYA_UIWidgetKind expected[] = {
    NYA_UI_WIDGET_SCRIM, NYA_UI_WIDGET_PANEL,  NYA_UI_WIDGET_LABEL, NYA_UI_WIDGET_BUTTON,
    NYA_UI_WIDGET_TOGGLE, NYA_UI_WIDGET_SLIDER, NYA_UI_WIDGET_RADIO, NYA_UI_WIDGET_RADIO,
};

static b8 stream_matches(const NYA_UIRecorder* recorder) {
    if (nya_ui_recorder_count(recorder) != nya_carray_length(expected)) return false;

    for (u32 i = 0; i < nya_carray_length(expected); i++) {
        if (nya_ui_recorder_at(recorder, i)->kind != expected[i]) return false;
    }

    return true;
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
    nya_input_action_rebind(NYA_INPUT_ACTION_DOWN, NYA_KEY_DOWN);

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 8.0F, .spacing = 6.0F });

    static NYA_UIRecorder cells;
    static NYA_UIRecorder wider;

    nya_ui_recorder_init(&cells, (f32x2){ 8.0F, 16.0F });
    nya_ui_recorder_init(&wider, (f32x2){ 16.0F, 32.0F });

    defer nya_ui_recorder_deinit(&cells);
    defer nya_ui_recorder_deinit(&wider);

    // ── No font is registered yet, on purpose: a recorded pass measures in cells and needs neither a face nor a GPU.
    {
        nya_check(!nya_font_valid(nya_font_resolve(NYA_FONT_NONE)), "the test starts with no usable face");

        nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&cells));
        nya_check(nya_ui_presenter_get(&window) == nya_ui_recorder_presenter(&cells), "the window presents through the recorder");

        draw_twice(&cells);

        nya_check(stream_matches(&cells), "eight widgets, in the order they were declared, got %u", nya_ui_recorder_count(&cells));
        nya_check(strcmp(nya_ui_recorder_at(&cells, 1)->label, "settings") == 0, "a panel carries its title as its label");
        nya_check(nya_ui_recorder_find(&cells, NYA_UI_WIDGET_SLIDER, "volume") != nullptr, "the slider is there under its own label");
        nya_check(nya_ui_recorder_find(&cells, NYA_UI_WIDGET_BUTTON, "volume") == nullptr, "and nothing else answers to it");
    }

    // ── An input pass draws nothing, whatever the presenter is: only a draw pass declares widgets to anyone.
    {
        nya_ui_recorder_reset(&cells);
        (void)screen(NYA_UI_PASS_INPUT);
        tick();

        nya_check(nya_ui_recorder_count(&cells) == 0, "an input pass records nothing, got %u", nya_ui_recorder_count(&cells));
    }

    // ── Every size comes out of the presenter: the same tree in bigger cells is the same widgets, bigger.
    {
        draw_twice(&cells);
        NYA_Rectf small = nya_ui_recorder_find(&cells, NYA_UI_WIDGET_BUTTON, "apply")->rect;

        nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&wider));
        draw_twice(&wider);

        const NYA_UIWidgetDraw* big = nya_ui_recorder_find(&wider, NYA_UI_WIDGET_BUTTON, "apply");

        nya_check(stream_matches(&wider), "the widget stream does not depend on the cell");
        nya_check(big != nullptr && big->rect.height > small.height, "but a taller cell makes a taller row, got %f against %f", (f64)big->rect.height,
                  (f64)small.height);
        nya_check(big->rect.width == small.width, "while a fixed panel width stays what the caller asked for");
    }

    // ── The pass reads as text, which is what makes a recorded UI something a person can check.
    {
        char dump[2048];
        u32  wrote = nya_ui_recorder_write(&cells, dump, sizeof(dump));

        nya_check(wrote > 0 && dump[wrote] == '\0', "the dump is written and terminated, %u bytes", wrote);
        nya_check(strstr(dump, "fullscreen") != nullptr && strstr(dump, "slider") != nullptr, "and names every widget and its kind");

        char cut[16];
        u32  short_write = nya_ui_recorder_write(&cells, cut, sizeof(cut));

        nya_check(short_write < sizeof(cut) && cut[short_write] == '\0', "a buffer too small is cut rather than overrun");
    }

    // ── The same keys, through both presenters, decide the same thing. The face is loaded here and not before.
    {
        nya_font_default_set(nya_font(FACE, 20.0F));
        for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

        Result through[2] = { 0 };

        for (u32 i = 0; i < 2; i++) {
            state_fullscreen = false;
            state_volume     = 0.5F;
            state_quality    = 0;

            nya_ui_presenter_set(&window, i == 0 ? nya_ui_presenter_shape() : nya_ui_recorder_presenter(&cells));
            nya_ui_focus_reset(&window);
            tick();

            // the panel's first pass measures it, so focus has somewhere to land before the keys arrive.
            draw_twice(nullptr);

            // down twice from the button lands on the toggle, and confirm flips it.
            tap(NYA_KEY_DOWN);
            (void)screen(NYA_UI_PASS_INPUT);
            tick();

            tap(NYA_KEY_RETURN);
            through[i] = screen(NYA_UI_PASS_INPUT);
            tick();
        }

        nya_check(through[0].fullscreen == through[1].fullscreen && through[0].fullscreen, "the same keys flip the same toggle on both presenters");
        nya_check(through[0].volume == through[1].volume, "and leave the same value behind, got %f against %f", (f64)through[0].volume, (f64)through[1].volume);
        nya_check(through[0].quality == through[1].quality, "and the same choice");
    }

    // ── And the shape presenter is what a window goes back to when it is handed nothing.
    {
        nya_ui_presenter_set(&window, nullptr);
        nya_check(nya_ui_presenter_get(&window) == nya_ui_presenter_shape(), "null puts the shape presenter back");

        nya_ui_recorder_reset(&cells);
        draw_twice(nullptr);

        nya_check(nya_ui_recorder_count(&cells) == 0, "and the recorder sees nothing once it is off the window");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
