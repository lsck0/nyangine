/**
 * The UI's one line text field, headless: typing starts on confirm or a click and stops on return, cancel, a click
 * elsewhere or the field leaving the UI; text goes in at the caret, which moves and erases by character, never past
 * the buffer; and while typing the menu keys neither move focus nor close anything.
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

static void type(NYA_ConstCString text) {
    NYA_Event event = { .type = NYA_EVENT_TEXT_INPUT, .as_text_input_event = { .text = text } };
    nya_system_input_handle_event(&event);
}

static void click_at(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event moved = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&moved);

    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = { .type = i == 0 ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = i == 0, .button = NYA_MOUSE_BUTTON_LEFT, .x = point.x, .y = point.y } };
        nya_system_input_handle_event(&event);
    }
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static char name[8] = "";

typedef struct {
    b8 changed;
    b8 pressed;
    b8 cancelled;
} Form;

/** A field for `name` above a button, both full width in a 400 wide panel at the top left, inside the margin. */
#define FIELD  ((NYA_Rectf){ 26.0F, 26.0F, 380.0F, 40.0F })
#define BUTTON ((NYA_Rectf){ 26.0F, 72.0F, 380.0F, 40.0F })

/** One input pass over the form, and the tick after it; `with_field` false leaves the field out. */
static Form form(b8 with_field) {
    Form    result = { 0 };
    NYA_UI* ui     = nya_ui_begin(&window, NYA_UI_PASS_INPUT);

    if (nya_ui_panel_begin(ui, "form", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        if (with_field) result.changed = nya_ui_text_input(ui, "name", name, sizeof(name));
        result.pressed = nya_ui_button(ui, "ok");
        nya_ui_panel_end(ui);
    }

    result.cancelled = nya_ui_cancelled(ui);
    nya_ui_end(ui);
    tick();

    return result;
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

    // as a game binds them: letters and space drive the menu too.
    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);
    nya_input_action_bind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_SPACE);
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);
    nya_input_action_rebind(NYA_INPUT_ACTION_UP, NYA_KEY_UP);
    nya_input_action_rebind(NYA_INPUT_ACTION_DOWN, NYA_KEY_DOWN);
    nya_input_action_bind(NYA_INPUT_ACTION_DOWN, NYA_KEY_S);
    nya_input_action_rebind(NYA_INPUT_ACTION_LEFT, NYA_KEY_LEFT);
    nya_input_action_rebind(NYA_INPUT_ACTION_RIGHT, NYA_KEY_RIGHT);

    nya_font_default_set(nya_font(FACE, 20.0F));
    for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 10.0F, .spacing = 6.0F, .item_height = 40.0F });

    // ── Focused, the field ignores typing until confirm starts it.
    {
        (void)form(true);

        type("x");
        nya_check(!form(true).changed && name[0] == '\0', "text before the field is started goes nowhere");

        tap(NYA_KEY_RETURN);
        nya_check(!form(true).changed && nya_ui_typing(&window), "confirm starts typing but types nothing");

        type("ab");
        Form typed = form(true);
        nya_check(typed.changed && nya_string_equals(name, "ab") && nya_ui_typing(&window), "typed text goes in, got '%s'", name);
    }

    // ── The caret: text goes in where it is, the arrows move it by character, backspace and delete take one, home and end jump.
    {
        tap(NYA_KEY_LEFT);
        (void)form(true);
        type("é");
        (void)form(true);
        nya_check(nya_string_equals(name, "aéb"), "inserted before the last character, got '%s'", name);

        tap(NYA_KEY_BACKSPACE);
        (void)form(true);
        nya_check(nya_string_equals(name, "ab"), "backspace takes the whole two byte character, got '%s'", name);

        tap(NYA_KEY_HOME);
        (void)form(true);
        tap(NYA_KEY_DELETE);
        (void)form(true);
        nya_check(nya_string_equals(name, "b"), "home then delete takes the first, got '%s'", name);

        tap(NYA_KEY_END);
        (void)form(true);
        type("c");
        (void)form(true);
        nya_check(nya_string_equals(name, "bc"), "end puts the caret after the last, got '%s'", name);

        tap(NYA_KEY_RIGHT);
        (void)form(true);
        tap(NYA_KEY_DELETE);
        nya_check(!form(true).changed, "right and delete at the end do nothing");
    }

    // ── The buffer: text past its capacity is cut, and never inside a character.
    {
        type("ééé");
        (void)form(true);
        nya_check(nya_string_equals(name, "bcéé"), "seven bytes of room take two of the three, got '%s'", name);

        type("é");
        nya_check(!form(true).changed, "a two byte character does not go in the one byte left");

        type("z");
        nya_check(form(true).changed && nya_string_equals(name, "bcééz"), "a one byte one does, got '%s'", name);
    }

    // ── While typing, the menu keys type: space and S neither confirm, stop, nor move focus.
    {
        name[0] = '\0';

        key(NYA_KEY_SPACE, true);
        type(" ");
        key(NYA_KEY_SPACE, false);
        Form spaced = form(true);
        nya_check(spaced.changed && !spaced.pressed && nya_string_equals(name, " ") && nya_ui_typing(&window), "space types a space, got '%s'", name);

        key(NYA_KEY_S, true);
        type("s");
        key(NYA_KEY_S, false);
        (void)form(true);
        tap(NYA_KEY_DOWN);
        (void)form(true);

        nya_check(nya_string_equals(name, " s") && nya_ui_typing(&window), "s types, and down does not leave the field, got '%s'", name);
    }

    // ── Stopping: return and cancel stop typing without acting, and the pass that stopped still reports typing.
    {
        tap(NYA_KEY_RETURN);
        Form returned = form(true);
        nya_check(!returned.pressed && nya_ui_typing(&window), "return stops, and the pass that saw it still counts as typing");
        nya_check(!form(true).changed && !nya_ui_typing(&window), "the next pass does not");

        type("q");
        nya_check(!form(true).changed, "stopped, typing goes nowhere");

        tap(NYA_KEY_RETURN);
        (void)form(true);
        tap(NYA_KEY_ESCAPE);
        Form escaped = form(true);
        nya_check(!escaped.cancelled && nya_ui_typing(&window), "cancel only stops typing, and the menu does not see it");

        tap(NYA_KEY_ESCAPE);
        nya_check(form(true).cancelled, "a second cancel is the menu's");
    }

    // ── The pointer: a click on the field starts typing, one elsewhere stops it, and a field that leaves the UI stops too.
    {
        f32x2 field  = { FIELD.x + 200.0F, FIELD.y + 20.0F };
        f32x2 button = { BUTTON.x + 200.0F, BUTTON.y + 20.0F };

        click_at(field);
        (void)form(true);
        nya_check(nya_ui_typing(&window), "a click on the field starts typing");

        click_at(button);
        Form clicked = form(true);
        nya_check(clicked.pressed, "a click elsewhere lands where it is aimed");
        (void)form(true);
        nya_check(!nya_ui_typing(&window), "and has stopped typing");

        click_at(field);
        (void)form(true);
        (void)form(false);
        (void)form(false);
        nya_check(!nya_ui_typing(&window), "a field no longer declared stops typing");

        nya_ui_focus_reset(&window);
        click_at(field);
        (void)form(true);
        nya_ui_focus_reset(&window);
        nya_check(!nya_ui_typing(&window), "and so does a focus reset");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
