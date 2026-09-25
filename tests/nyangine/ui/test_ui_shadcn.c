/**
 * The small composable widget set: a card that frames a heading, a subtitle and a rule over its body; a badge that
 * fits its own text; a progress bar filled to a fraction; and a breadcrumb whose current crumb is plain text while
 * the rest are buttons. Each is built from the primitives that were already there, so it is checked the way the
 * primitives are: headless, through the recorder, reading the command stream a pass declared rather than a pixel.
 * The breadcrumb is driven with the pointer and the keyboard, since one of its crumbs is a link.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

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

/* A clock the test drives, so a spinner's frame and a toast's fade are exact rather than a race with the wall clock. */

static u64 g_clock_ns = 0;

static u64 test_clock(void) {
    return g_clock_ns;
}

static void advance_s(f64 seconds) {
    g_clock_ns += (u64)(seconds * 1'000'000'000.0);
}

static f32x2 center_of(NYA_Rectf rect) {
    return (f32x2){ rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };
}

/* The trees under test. Not a line of them knows a presenter is even installed. */

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

/** An avatar chip beside a full width button, so the fitted width can be told from the row's. */
static void avatar_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_avatar(ui, "AB");
        (void)nya_ui_button(ui, "a wide button");
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

/** One spinner, whichever frame the driven clock lands it on. */
static void spinner_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_spinner(ui);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

/** One skeleton block filling the row, twenty pixels tall. */
static void skeleton_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_skeleton(ui, 0.0F, 20.0F);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

/** One separator across the panel. */
static void separator_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_separator(ui);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

static u32                    accordion_open   = 0;
static const NYA_ConstCString ACCORDION[]      = { "one", "two", "three" };
static const NYA_ConstCString ACCORDION_BODY[] = { "body one", "body two", "body three" };

/** Three accordion folds sharing one open index, each with a body only shown while it is the open one. */
static void accordion_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        for (u32 i = 0; i < nya_carray_length(ACCORDION); i++) {
            if (nya_ui_accordion_begin(ui, ACCORDION[i], i, &accordion_open)) {
                nya_ui_label(ui, ACCORDION_BODY[i]);
                nya_ui_accordion_end(ui);
            }
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

static const NYA_Rectf TOOLTIP_TRIGGER = { 100.0F, 100.0F, 120.0F, 40.0F };

/** A tooltip over a fixed trigger rectangle. Shows only while the pointer rests on it. */
static void tooltip_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    nya_ui_tooltip(ui, "tip", TOOLTIP_TRIGGER, "hello tip");

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

static b8 dialog_open = true;

/** A modal dialog: a scrim, a titled centred panel, and one button for a body. */
static void dialog_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_dialog_begin(ui, "confirm", "Delete file?", &dialog_open)) {
        (void)nya_ui_button(ui, "delete");
        nya_ui_dialog_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

/** Posts one toast to the window, in a pass of its own. */
static void toast_post(NYA_ConstCString text) {
    NYA_UI* ui = nya_ui_begin(&window, NYA_UI_PASS_INPUT);
    nya_ui_toast(ui, text);
    nya_ui_end(ui);
    tick();
}

/** Draws and expires the toast stack. */
static void toast_screen(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);
    nya_ui_toasts(ui);
    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
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

    // a clock the test owns, so uptime is exactly g_clock_ns: a spinner's frame and a toast's fade are read at a time this chooses rather than raced against the wall clock.
    _NYA_APP_INSTANCE.time_source.now_ns = test_clock;

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

    // the whole set is driven through the recorder: no GPU, no face, and every rectangle is exact cell arithmetic, so a click reads the same geometry an input pass computes.
    static NYA_UIRecorder recorder;
    nya_ui_recorder_init(&recorder, (f32x2){ 8.0F, 16.0F });
    defer nya_ui_recorder_deinit(&recorder);

    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));

    // A card is a framed panel, a heading and a subtitle over a rule, then the body: built from panels and labels.
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

    // A badge fits its own text, so it does not fill the row the way a button does.
    {
        draw_twice(&recorder, badge_screen);

        const NYA_UIWidgetDraw* label  = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "beta");
        const NYA_UIWidgetDraw* button = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "a wide button");

        nya_check(label != nullptr, "a badge shows its text as a label");
        nya_check(button != nullptr, "and the reference button is there");
        nya_check(label->rect.width < button->rect.width, "a badge fits its text rather than filling the row, got %f against %f", (f64)label->rect.width,
                  (f64)button->rect.width);
    }

    // A progress bar is a track with the accent filled over a fraction of it, and only that fraction.
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

    // A breadcrumb: the current crumb is plain text, the rest are buttons, and a click navigates to one.
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

    // The buttons carry the keyboard: focus lands on the first crumb link and confirm navigates to it.
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

    // An avatar fits its initials rather than filling the row, so it reads as a chip beside a full width button.
    {
        draw_twice(&recorder, avatar_screen);

        const NYA_UIWidgetDraw* label  = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "AB");
        const NYA_UIWidgetDraw* button = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "a wide button");

        nya_check(label != nullptr, "an avatar shows its initials as a label");
        nya_check(button != nullptr, "and the reference button is there");
        nya_check(label->rect.width < button->rect.width, "an avatar fits its initials rather than filling the row, got %f against %f", (f64)label->rect.width,
                  (f64)button->rect.width);
    }

    // A spinner is a label in one of its frames, and the frame it shows follows the clock.
    {
        g_clock_ns = 0;
        draw_twice(&recorder, spinner_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "|") != nullptr, "at rest the spinner is on its first frame");

        // part of a second on is a frame further round, so the mark is turning rather than stuck.
        g_clock_ns = 0;
        advance_s(0.15);
        draw_twice(&recorder, spinner_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "/") != nullptr, "and a fraction of a second later it has stepped to the next frame");
    }

    // A skeleton is a track-coloured block: it fills the row across and stands at the height it was given.
    {
        draw_twice(&recorder, skeleton_screen);

        const NYA_UIWidgetDraw* block = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_STRIPE, nullptr);

        nya_check(block != nullptr, "a skeleton is a filled block");
        nya_check(block->rect.height == 20.0F, "at the height it was given, got %f", (f64)block->rect.height);
        nya_check(block->rect.width > 100.0F, "and filling the row across, got %f", (f64)block->rect.width);
    }

    // A separator is a thin dim rule spanning the container.
    {
        draw_twice(&recorder, separator_screen);

        const NYA_UIWidgetDraw* rule = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_UNDERLINE, nullptr);

        nya_check(rule != nullptr, "a separator draws a rule");
        nya_check(rule->rect.height <= 2.0F, "thin, got %f", (f64)rule->rect.height);
        nya_check(rule->rect.width > 100.0F, "and spanning the container, got %f", (f64)rule->rect.width);
    }

    // An accordion opens one fold at a time: the open fold's body shows and the others' do not, and opening a second closes the first.
    {
        accordion_open = 0;
        draw_twice(&recorder, accordion_screen);

        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SECTION, "one") != nullptr, "each fold has a section header");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "body one") != nullptr, "the open fold shows its body");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "body two") == nullptr, "and the folded ones hide theirs");

        // a click on the second header opens it and closes the first, since one index is shared.
        NYA_Rectf second = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SECTION, "two")->rect;

        click_at(center_of(second));
        accordion_screen(NYA_UI_PASS_INPUT);
        nya_check(accordion_open == 1, "clicking a fold opens it, got %u", accordion_open);

        draw_twice(&recorder, accordion_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "body two") != nullptr, "the fold clicked is now open");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "body one") == nullptr, "and the one that was open has closed");
    }

    // A tooltip shows only while the pointer rests on its trigger.
    {
        pointer_move(center_of(TOOLTIP_TRIGGER));
        draw_twice(&recorder, tooltip_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "hello tip") != nullptr, "a tooltip shows while the pointer is over the trigger");

        pointer_move((f32x2){ 400.0F, 400.0F });
        draw_twice(&recorder, tooltip_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "hello tip") == nullptr, "and hides once the pointer leaves it");
    }

    // A dialog is a scrim under a centred panel while it is open, and escape closes it.
    {
        dialog_open = true;
        draw_twice(&recorder, dialog_screen);

        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_SCRIM, nullptr) != nullptr, "an open dialog dims the window behind it");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "delete") != nullptr, "and shows the body it was opened for");

        // escape is the one step-back a modal always answers to.
        tap(NYA_KEY_ESCAPE);
        dialog_screen(NYA_UI_PASS_INPUT);
        nya_check(!dialog_open, "escape closes the dialog");

        draw_twice(&recorder, dialog_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_BUTTON, "delete") == nullptr, "and a closed dialog draws nothing");
    }

    // A toast is posted, shows, and leaves once it has run its course; a flood keeps only the newest few.
    {
        g_clock_ns = 0;
        toast_post("saved");
        draw_twice(&recorder, toast_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "saved") != nullptr, "a posted toast shows");

        // past its show time plus its fade it is expired and gone.
        advance_s((f64)NYA_UI_TOAST_SHOW_S + (f64)NYA_UI_TOAST_FADE_S + 0.1);
        draw_twice(&recorder, toast_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "saved") == nullptr, "and leaves once it has run its course");

        // more than the stack holds keeps the newest NYA_UI_TOASTS_MAX and drops the oldest.
        g_clock_ns = 0;
        for (u32 i = 0; i < NYA_UI_TOASTS_MAX + 2; i++) {
            char text[8];
            (void)snprintf(text, sizeof(text), "t%u", i);
            toast_post(text);
        }

        draw_twice(&recorder, toast_screen);
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "t0") == nullptr, "the oldest toast is dropped when the stack overflows");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "t5") != nullptr, "and the newest is kept");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
