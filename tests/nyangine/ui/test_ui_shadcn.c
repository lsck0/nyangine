/**
 * The small composable widget set: a card that frames a heading, a subtitle and a rule over its body; a badge that
 * fits its own text; a progress bar filled to a fraction; and a breadcrumb whose current crumb is plain text while
 * the rest are buttons. Each is built from the primitives that were already there, so it is checked the way the
 * primitives are: headless, through the recorder, reading the command stream a pass declared rather than a pixel.
 * The breadcrumb is driven with the pointer and the keyboard, since one of its crumbs is a link.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
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

static void tap(NYA_Keycode keycode) {
    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = { .type = i == 0 ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP, .as_key_event = { .is_down = i == 0, .key = keycode } };
        nya_system_input_handle_event(&event);
    }
}

static void click_at(f32x2 point) {
    pointer_move(point);
    pointer_button(true);
    pointer_button(false);
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static f32x2 center_of(NYA_Rectf rect) {
    return (f32x2){ rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };
}

/* ── The trees under test. Not a line of them knows a presenter is even installed. ── */

/** A card with a heading and a subtitle, one button for a body. */
static void card_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        if (nya_ui_card_begin(ui, "card", "Session", "since you signed in")) {
            (void)nya_ui_button(ui, "sign out");
            nya_ui_card_end(ui);
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

/** A badge beside a full width button, so the two widths can be told apart. */
static void badge_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_badge(ui, "beta");
        (void)nya_ui_button(ui, "a wide button");
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

static f32 fraction = 0.5F;

/** One progress bar, at whatever the global fraction is. */
static void progress_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_progress(ui, fraction);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

static u32                    crumb    = 0;
static const NYA_ConstCString CRUMBS[] = { "home", "docs", "ui" };

/** A breadcrumb trail. Reports whether the pass changed the current crumb. */
static b8 crumb_screen(NYA_UIPass pass) {
    b8      changed = false;
    NYA_UI* ui      = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        changed = nya_ui_breadcrumb(ui, "crumbs", CRUMBS, nya_carray_length(CRUMBS), &crumb);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();

    return changed;
}

/** A draw pass over the trail, discarding what it returns, so it fits the draw_twice signature. */
static void crumb_draw(NYA_UIPass pass) {
    (void)crumb_screen(pass);
}

/** Two draw passes, so the pass that is read is a whole one laid out from measurements, not a first blank one. */
static void draw_twice(NYA_UIRecorder* recorder, void (*screen)(NYA_UIPass)) {
    screen(NYA_UI_PASS_DRAW);
    nya_ui_recorder_reset(recorder);
    screen(NYA_UI_PASS_DRAW);
}

/** The index of the first recorded widget of `kind` with `label`, or -1. */
static s32 index_of(const NYA_UIRecorder* recorder, NYA_UIWidgetKind kind, NYA_ConstCString label) {
    for (u32 i = 0; i < nya_ui_recorder_count(recorder); i++) {
        const NYA_UIWidgetDraw* widget = nya_ui_recorder_at(recorder, i);

        if (widget->kind == kind && (label == nullptr || strcmp(widget->label, label) == 0)) return (s32)i;
    }

    return -1;
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
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);
    nya_input_action_rebind(NYA_INPUT_ACTION_UP, NYA_KEY_UP);
    nya_input_action_rebind(NYA_INPUT_ACTION_DOWN, NYA_KEY_DOWN);
    nya_input_action_rebind(NYA_INPUT_ACTION_LEFT, NYA_KEY_LEFT);
    nya_input_action_rebind(NYA_INPUT_ACTION_RIGHT, NYA_KEY_RIGHT);

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 8.0F, .spacing = 6.0F, .item_height = 32.0F });

    // the whole set is driven through the recorder: no GPU, no face, and every rectangle is exact cell arithmetic,
    // so a click reads the same geometry an input pass computes.
    static NYA_UIRecorder recorder;
    nya_ui_recorder_init(&recorder, (f32x2){ 8.0F, 16.0F });
    defer nya_ui_recorder_deinit(&recorder);

    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));

    // ── A card is a framed panel, a heading and a subtitle over a rule, then the body: built from panels and labels.
    {
        draw_twice(&recorder, card_screen);

        s32 panel = index_of(&recorder, NYA_UI_WIDGET_PANEL, "");
        s32 title = index_of(&recorder, NYA_UI_WIDGET_LABEL, "Session");
        s32 sub   = index_of(&recorder, NYA_UI_WIDGET_LABEL, "since you signed in");
        s32 rule  = index_of(&recorder, NYA_UI_WIDGET_RULE, nullptr);
        s32 body  = index_of(&recorder, NYA_UI_WIDGET_BUTTON, "sign out");

        nya_check(panel >= 0, "a card opens a panel");
        nya_check(title >= 0 && sub >= 0, "and carries a heading and a subtitle as labels");
        nya_check(rule >= 0, "and a rule sets the header apart");
        nya_check(body >= 0, "and the body it was opened for is inside it");

        // the heading, then the subtitle, then the rule, then the body: the order that makes it read as a card.
        nya_check(title < sub && sub < rule && rule < body, "the header comes before the body it introduces, got %d %d %d %d", title, sub, rule, body);
    }

    // ── A badge fits its own text, so it does not fill the row the way a button does.
    {
        draw_twice(&recorder, badge_screen);

        const NYA_UIWidgetDraw* label  = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "beta");
        const NYA_UIWidgetDraw* button = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "a wide button");

        nya_check(label != nullptr, "a badge shows its text as a label");
        nya_check(button != nullptr, "and the reference button is there");
        nya_check(label->rect.width < button->rect.width, "a badge fits its text rather than filling the row, got %f against %f", (f64)label->rect.width,
                  (f64)button->rect.width);
    }

    // ── A progress bar is a track with the accent filled over a fraction of it, and only that fraction.
    {
        fraction = 0.5F;
        draw_twice(&recorder, progress_screen);

        const NYA_UIWidgetDraw* track = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_STRIPE, nullptr);
        const NYA_UIWidgetDraw* fill  = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_UNDERLINE, nullptr);

        nya_check(track != nullptr && fill != nullptr, "a progress bar is a track and a fill");
        nya_check(fill->rect.width == roundf(track->rect.width * 0.5F), "half filled is half the track wide, got %f of %f", (f64)fill->rect.width,
                  (f64)track->rect.width);
        nya_check(fill->rect.x == track->rect.x && fill->rect.y == track->rect.y, "and the fill sits on the track");

        // empty draws no fill at all, and full fills the whole track, so the ends are exact rather than nearly so.
        fraction = 0.0F;
        draw_twice(&recorder, progress_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_UNDERLINE, nullptr) == nullptr, "an empty bar draws no fill");

        fraction = 1.0F;
        draw_twice(&recorder, progress_screen);
        const NYA_UIWidgetDraw* full_track = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_STRIPE, nullptr);
        const NYA_UIWidgetDraw* full_fill  = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_UNDERLINE, nullptr);
        nya_check(full_fill != nullptr && full_fill->rect.width == full_track->rect.width, "a full bar fills the whole track");
    }

    // ── A breadcrumb: the current crumb is plain text, the rest are buttons, and a click navigates to one.
    {
        crumb = 0;
        draw_twice(&recorder, crumb_draw);

        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "home") != nullptr, "the current crumb is plain text");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "home") == nullptr, "and nothing clickable answers to it");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "docs") != nullptr, "the crumbs before and after it are buttons");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "/") != nullptr, "with a separator between them");

        // a click on another crumb navigates there, which is the whole point of a trail.
        NYA_Rectf docs = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "docs")->rect;

        click_at(center_of(docs));
        b8 changed = crumb_screen(NYA_UI_PASS_INPUT);
        nya_check(changed && crumb == 1, "clicking a crumb navigates to it, got %u", crumb);

        // and now the crumb it went to is the plain one, while the one it came from turned into a link.
        draw_twice(&recorder, crumb_draw);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "docs") != nullptr, "the crumb it is at is now plain text");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "home") != nullptr, "and the one it left is a link now");
    }

    // ── The buttons carry the keyboard: focus lands on the first crumb link and confirm navigates to it.
    {
        crumb = 0;
        nya_ui_focus_reset(&window);
        draw_twice(&recorder, crumb_draw);

        // focus settles on the first focusable widget, which is the first crumb that is a link rather than the page.
        (void)crumb_screen(NYA_UI_PASS_INPUT);

        tap(NYA_KEY_RETURN);
        b8 changed = crumb_screen(NYA_UI_PASS_INPUT);
        nya_check(changed && crumb == 1, "confirm on the focused crumb navigates to it, got %u", crumb);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
