/**
 * The UI's multi-line code editor, headless through the recorder: text builds into the line structure a gutter
 * numbers, typing goes in at the caret, enter splits a line and backspace at its start joins it to the one above,
 * a selection round trips through the clipboard with its newlines, the view shows the right window of lines when it
 * is scrolled, and the arrows walk the caret between lines and along them. The presenter is the recorder, so every
 * line and every number is exact arithmetic on a cell grid rather than anything a screen had to show.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static NYA_UIRecorder    recorder;
static char              src[1024] = "";
static NYA_UICodeEditor  editor    = { 0 };
static b8                changed   = false;

/* SYNTHETIC INPUT */

static void key(NYA_Keycode keycode, b8 down, NYA_KeyModFlag modifiers) {
    NYA_Event event = { .type = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP, .as_key_event = { .is_down = down, .key = keycode, .modifier_flags = modifiers } };
    nya_system_input_handle_event(&event);
}

static void tap(NYA_Keycode keycode) {
    key(keycode, true, NYA_KEYMOD_NONE);
    key(keycode, false, NYA_KEYMOD_NONE);
}

static void type(NYA_ConstCString text) {
    NYA_Event event = { .type = NYA_EVENT_TEXT_INPUT, .as_text_input_event = { .text = text } };
    nya_system_input_handle_event(&event);
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

/* THE EDITOR UNDER TEST */

/** One pass over an editor that fills a fixed panel at the top left, tall enough to show a dozen lines. */
static void pass(NYA_UIPass p) {
    NYA_UI* ui = nya_ui_begin(&window, p);

    if (nya_ui_panel_begin(ui, "pane", (NYA_UIPanel){ .anchor = NYA_UI_ANCHOR_TOP_LEFT, .width = nya_ui_fixed(400), .height = nya_ui_fixed(320) })) {
        changed = nya_ui_code_editor(ui, "src", src, sizeof(src), &editor);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

static void draw(void) {
    nya_ui_recorder_reset(&recorder);
    pass(NYA_UI_PASS_DRAW);
}

static void input(void) {
    pass(NYA_UI_PASS_INPUT);
    tick();
}

/** Whether a label with exactly `text` was drawn this pass. */
static b8 has_label(NYA_ConstCString text) {
    return nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, text) != nullptr;
}

/** The rectangle of the label with exactly `text`, or a zeroed one when it was not drawn. */
static NYA_Rectf label_rect(NYA_ConstCString text) {
    const NYA_UIWidgetDraw* widget = nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, text);
    return widget != nullptr ? widget->rect : (NYA_Rectf){ 0 };
}

/** Puts `start` in the buffer and takes the keyboard, the caret at the text's end. Focus is by confirm, so no click. */
static void editing_start(NYA_ConstCString start) {
    nya_ui_focus_reset(&window);
    (void)snprintf(src, sizeof(src), "%s", start);
    editor = (NYA_UICodeEditor){ 0 };

    input();               // settle the grow layout and take focus for the only widget
    input();
    tap(NYA_KEY_RETURN);   // confirm starts typing; the editor is not editing yet, so this focuses rather than splitting
    input();
    nya_assert(editor.focused, "the editor took the keyboard");
}

/** A press carrying `modifiers`, the pass that reads it, then control released so the next pass types again. */
static b8 press_with(NYA_Keycode keycode, NYA_KeyModFlag modifiers) {
    key(keycode, true, modifiers);
    key(keycode, false, modifiers);

    input();
    key(NYA_KEY_LCTRL, false, NYA_KEYMOD_NONE);

    return changed;
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
    nya_system_window_init(); // starting text input looks the handle up

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

    // the recorder measures in cells, so every line and number is exact and needs no font.
    nya_ui_recorder_init(&recorder, NYA_UI_RECORD_CELL);
    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));
    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 8.0F, .spacing = 0.0F });

    // TEST: the text builds into the expected lines, each with its number in the gutter.
    {
        editor = (NYA_UICodeEditor){ 0 };
        (void)snprintf(src, sizeof(src), "abc\ndef\nghi");

        input();
        input();
        draw();

        nya_check(editor.lines == 3, "three lines of text are three lines, got %u", editor.lines);

        nya_check(has_label("abc") && has_label("def") && has_label("ghi"), "every line is drawn as its own label");
        nya_check(has_label("1") && has_label("2") && has_label("3"), "every line has its number in the gutter");

        // the gutter sits left of the text: line one's number starts before line one's text.
        nya_check(label_rect("1").x < label_rect("abc").x, "the gutter is left of the text");

        // the numbers step down the lines, one row apart.
        nya_check(label_rect("2").y > label_rect("1").y && label_rect("3").y > label_rect("2").y, "the numbers step down the lines");
    }

    // TEST: typing goes in at the caret, wherever it is.
    {
        editing_start("");

        type("ab");
        input();
        nya_check(nya_string_equals(src, "ab"), "typing into an empty editor appends, got '%s'", src);

        tap(NYA_KEY_HOME);
        input();
        type("Z");
        input();
        nya_check(nya_string_equals(src, "Zab"), "home then typing inserts at the line's start, got '%s'", src);
        nya_check(editor.cursor_column == 1, "the caret sits after what was typed, at column %u", editor.cursor_column);
    }

    // TEST: enter splits the line at the caret.
    {
        editing_start("");

        type("ab");
        input();
        tap(NYA_KEY_RETURN);
        input();
        type("cd");
        input();

        nya_check(nya_string_equals(src, "ab\ncd"), "enter puts a newline in at the caret, got '%s'", src);
        nya_check(editor.lines == 2, "the buffer is two lines now, got %u", editor.lines);
        nya_check(editor.cursor_line == 1 && editor.cursor_column == 2, "the caret is on the second line, at %u:%u", editor.cursor_line, editor.cursor_column);
    }

    // TEST: backspace at the start of a line joins it to the one above.
    {
        editing_start("ab\ncd");

        // the caret opens at the end, on the second line; home takes it to that line's start, over the newline.
        tap(NYA_KEY_HOME);
        input();
        nya_check(editor.cursor_line == 1 && editor.cursor_column == 0, "home is at the second line's start, at %u:%u", editor.cursor_line, editor.cursor_column);

        tap(NYA_KEY_BACKSPACE);
        input();
        nya_check(nya_string_equals(src, "abcd"), "backspace over the line start joins the lines, got '%s'", src);
        nya_check(editor.lines == 1, "one line now, got %u", editor.lines);
        nya_check(editor.cursor_line == 0 && editor.cursor_column == 2, "the caret sits where the join happened, at %u:%u", editor.cursor_line, editor.cursor_column);
    }

    // TEST: cursor navigation walks the lines and their columns.
    {
        editing_start("abc\ndefg\nhi");

        // the caret opens at the end: the third line, column two.
        nya_check(editor.cursor_line == 2 && editor.cursor_column == 2, "the caret opens at the end, at %u:%u", editor.cursor_line, editor.cursor_column);

        tap(NYA_KEY_UP);
        input();
        nya_check(editor.cursor_line == 1 && editor.cursor_column == 2, "up keeps the column on a line long enough for it, at %u:%u", editor.cursor_line, editor.cursor_column);

        tap(NYA_KEY_END);
        input();
        nya_check(editor.cursor_line == 1 && editor.cursor_column == 4, "end goes to the line's end, at %u:%u", editor.cursor_line, editor.cursor_column);

        tap(NYA_KEY_UP);
        input();
        nya_check(editor.cursor_line == 0 && editor.cursor_column == 3, "up onto a shorter line lands at its end, at %u:%u", editor.cursor_line, editor.cursor_column);

        tap(NYA_KEY_DOWN);
        input();
        nya_check(editor.cursor_line == 1 && editor.cursor_column == 4, "down keeps the column the run aimed for, at %u:%u", editor.cursor_line, editor.cursor_column);

        tap(NYA_KEY_LEFT);
        input();
        nya_check(editor.cursor_line == 1 && editor.cursor_column == 3, "left steps back one column, at %u:%u", editor.cursor_line, editor.cursor_column);
    }

    // TEST: a selection round trips through the clipboard, newlines kept.
    {
        // headless SDL may have no clipboard at all, and then there is nothing here to test.
        b8 clipboard = nya_clipboard_text_set("probe").ok;

        if (clipboard) {
            editing_start("a\nb");

            nya_check(!press_with(NYA_KEY_A, NYA_KEYMOD_LCTRL), "select all changes no text");
            nya_check(!press_with(NYA_KEY_C, NYA_KEYMOD_LCTRL) && nya_string_equals(src, "a\nb"), "copy leaves the text alone, got '%s'", src);

            tap(NYA_KEY_END);
            input();

            nya_check(press_with(NYA_KEY_V, NYA_KEYMOD_LCTRL) && nya_string_equals(src, "a\nba\nb"), "paste keeps the newline, got '%s'", src);
            nya_check(editor.lines == 3, "the pasted newline made a third line, got %u", editor.lines);

            // cut takes the whole selection, and paste puts it straight back.
            (void)press_with(NYA_KEY_A, NYA_KEYMOD_LCTRL);
            nya_check(press_with(NYA_KEY_X, NYA_KEYMOD_LCTRL) && src[0] == '\0', "cut empties the editor, got '%s'", src);
            nya_check(press_with(NYA_KEY_V, NYA_KEYMOD_LCTRL) && nya_string_equals(src, "a\nba\nb"), "and paste restores it, got '%s'", src);
        }
    }

    // TEST: scrolling the view shows the right window of lines.
    {
        editor  = (NYA_UICodeEditor){ 0 };
        u32 pos = 0;
        for (u32 i = 0; i < 50; i++) pos += (u32)snprintf(src + pos, sizeof(src) - pos, "L%02u\n", i);

        input();
        input();
        draw();

        nya_check(editor.lines == 51, "fifty lines and a trailing empty one, got %u", editor.lines);
        nya_check(editor.first_line == 0 && editor.visible_lines > 1, "the top of an unscrolled view is the first line, %u lines fit", editor.visible_lines);
        nya_check(has_label("1"), "the first line's number shows at the top");

        // scroll ten lines down; the record cell is sixteen pixels tall.
        editor.scroll.y = 10.0F * 16.0F;

        input();
        draw();

        nya_check(editor.first_line == 10, "scrolled ten rows, the window starts at line eleven, first is %u", editor.first_line);
        nya_check(has_label("11"), "line eleven's number is in view");
        nya_check(!has_label("1"), "the first line has scrolled out of view");
    }

    nya_ui_recorder_deinit(&recorder);

    return nya_check_failures() == 0 ? 0 : 1;
}
