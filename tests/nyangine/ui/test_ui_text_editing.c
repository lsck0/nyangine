/**
 * Editing in the UI's one line field, headless: shift with the arrows selects and typing replaces the selection,
 * control moves and deletes by word, control with A selects everything, the clipboard round trips through copy, cut
 * and paste, and a click past the end of the text puts the caret after it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/** The field is the only row in a 400 wide panel at the top left, inside the default margin of 16. */
#define FIELD ((NYA_Rectf){ 26.0F, 26.0F, 380.0F, 40.0F })

static char text[64] = "";

static void key(NYA_Keycode keycode, b8 down, NYA_KeyModFlag modifiers) {
    NYA_Event event = {
        .type         = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP,
        .as_key_event = { .is_down = down, .key = keycode, .modifier_flags = modifiers },
    };

    nya_system_input_handle_event(&event);
}

/** A press and release carrying `modifiers`, so the modifier is still held when the pass reads it. */
static void tap_with(NYA_Keycode keycode, NYA_KeyModFlag modifiers) {
    key(keycode, true, modifiers);
    key(keycode, false, modifiers);
}

static void tap(NYA_Keycode keycode) {
    tap_with(keycode, NYA_KEYMOD_NONE);
}

/** One input pass over the field, and the tick after it. Whether the text changed. */
static b8 field(void);

/**
 * A modified press and the pass that reads it, then the modifier released. The UI reads the modifiers during the
 * pass, not when the key arrives, so they have to outlive the key and be cleared afterwards; a field that still
 * thinks control is down drops everything typed into it.
 * */
static b8 press_with(NYA_Keycode keycode, NYA_KeyModFlag modifiers) {
    tap_with(keycode, modifiers);

    b8 changed = field();
    key(NYA_KEY_LCTRL, false, NYA_KEYMOD_NONE);

    return changed;
}

static void type(NYA_ConstCString typed) {
    NYA_Event event = { .type = NYA_EVENT_TEXT_INPUT, .as_text_input_event = { .text = typed } };
    nya_system_input_handle_event(&event);
}

static void click_at(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event moved = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&moved);

    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = {
            .type                  = i == 0 ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP,
            .as_mouse_button_event = { .is_down = i == 0, .button = NYA_MOUSE_BUTTON_LEFT, .x = point.x, .y = point.y },
        };

        nya_system_input_handle_event(&event);
    }
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static b8 field(void) {
    b8      changed = false;
    NYA_UI* ui      = nya_ui_begin(&window, NYA_UI_PASS_INPUT);

    if (nya_ui_panel_begin(ui, "form", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        changed = nya_ui_text_input(ui, "name", text, sizeof(text));
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    tick();

    return changed;
}

/** Starts typing from scratch with `start` in the buffer and the caret at its end. */
static void editing_start(NYA_ConstCString start) {
    nya_ui_focus_reset(&window);
    (void)snprintf(text, sizeof(text), "%s", start);

    (void)field();
    tap(NYA_KEY_RETURN);
    (void)field();

    tap(NYA_KEY_END);
    (void)field();
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

    // no window opens, but starting text input looks the handle up.
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
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);
    nya_input_action_rebind(NYA_INPUT_ACTION_UP, NYA_KEY_UP);
    nya_input_action_rebind(NYA_INPUT_ACTION_DOWN, NYA_KEY_DOWN);
    nya_input_action_rebind(NYA_INPUT_ACTION_LEFT, NYA_KEY_LEFT);
    nya_input_action_rebind(NYA_INPUT_ACTION_RIGHT, NYA_KEY_RIGHT);

    nya_font_default_set(nya_font(FACE, 20.0F));
    for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 10.0F, .spacing = 6.0F, .item_height = 40.0F });

    // ── Shift with the arrows selects, and what goes in next replaces the selection rather than joining it.
    {
        editing_start("hello world");

        for (u32 i = 0; i < 5; i++) (void)press_with(NYA_KEY_LEFT, NYA_KEYMOD_LSHIFT);

        type("there");
        (void)field();
        nya_check(nya_string_equals(text, "hello there"), "five shifted lefts select the last word and typing replaces it, got '%s'", text);

        // an unshifted move over a selection collapses to its near edge instead of stepping from the caret.
        (void)press_with(NYA_KEY_LEFT, NYA_KEYMOD_LSHIFT);
        tap(NYA_KEY_LEFT);
        (void)field();
        type("X");
        (void)field();
        nya_check(nya_string_equals(text, "hello therXe"), "an unshifted arrow drops the selection and keeps the text, got '%s'", text);
    }

    // ── Control moves and deletes by word, and backspace over a selection takes the selection.
    {
        editing_start("one two three");

        (void)press_with(NYA_KEY_BACKSPACE, NYA_KEYMOD_LCTRL);
        nya_check(nya_string_equals(text, "one two "), "control backspace takes the word before the caret, got '%s'", text);

        (void)press_with(NYA_KEY_LEFT, NYA_KEYMOD_LCTRL);
        type("|");
        (void)field();
        nya_check(nya_string_equals(text, "one |two "), "control left lands at the start of the word, got '%s'", text);

        (void)press_with(NYA_KEY_DELETE, NYA_KEYMOD_LCTRL);
        nya_check(nya_string_equals(text, "one | "), "control delete takes the word after it, got '%s'", text);
    }

    // ── Control with A selects everything, so one backspace empties the field.
    {
        editing_start("wipe me");

        (void)press_with(NYA_KEY_A, NYA_KEYMOD_LCTRL);

        tap(NYA_KEY_BACKSPACE);
        (void)field();
        nya_check(text[0] == '\0', "select all then backspace empties it, got '%s'", text);

        // and control with A on an empty field is not a delete waiting to happen.
        nya_check(!press_with(NYA_KEY_A, NYA_KEYMOD_LCTRL), "select all on an empty field changes nothing");
    }

    // ── The clipboard: copy keeps the text, cut removes it, and paste puts it back at the caret.
    {
        // headless SDL may have no clipboard at all, and then there is nothing here to test.
        b8 clipboard = nya_clipboard_text_set("probe").ok;

        if (clipboard) {
            editing_start("copy me");

            (void)press_with(NYA_KEY_A, NYA_KEYMOD_LCTRL);
            nya_check(!press_with(NYA_KEY_C, NYA_KEYMOD_LCTRL) && nya_string_equals(text, "copy me"), "copy leaves the text alone, got '%s'", text);

            (void)press_with(NYA_KEY_A, NYA_KEYMOD_LCTRL);
            nya_check(press_with(NYA_KEY_X, NYA_KEYMOD_LCTRL) && text[0] == '\0', "cut takes it, got '%s'", text);

            nya_check(press_with(NYA_KEY_V, NYA_KEYMOD_LCTRL) && nya_string_equals(text, "copy me"), "and paste puts it back, got '%s'", text);

            // twice over, so the paste lands at the caret rather than replacing the line.
            (void)press_with(NYA_KEY_V, NYA_KEYMOD_LCTRL);
            nya_check(nya_string_equals(text, "copy mecopy me"), "a second paste appends at the caret, got '%s'", text);
        }
    }

    // ── A click past the end of the text puts the caret after it, not where the box ends.
    {
        nya_ui_focus_reset(&window);
        (void)snprintf(text, sizeof(text), "%s", "ab");
        (void)field();

        click_at((f32x2){ FIELD.x + FIELD.width - 20.0F, FIELD.y + (FIELD.height * 0.5F) });
        (void)field();
        nya_check(nya_ui_typing(&window), "a click on the field starts typing");

        type("c");
        (void)field();
        nya_check(nya_string_equals(text, "abc"), "and the caret sits after the last character, got '%s'", text);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
