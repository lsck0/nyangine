/**
 * The cell presenter: the same widgets as characters. That every kind of widget draws something a person would
 * recognise, that every size it hands the layout is a whole number of cells, that focus is readable with no colour
 * at all, and that the ASCII fallback is the same screen with a different table behind it.
 *
 * It runs headless and with no terminal: the presenter writes into the caller's grid and nothing else, which is the
 * property that makes a TUI checkable at all. See ui_present_cell.h.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** A whole number of cells either way, so nothing below is rounding away a half row. */
#define COLUMNS 100
#define ROWS    40

#define CELL_W ((f32)NYA_TERMINAL_CELL_WIDTH_PX)
#define CELL_H ((f32)NYA_TERMINAL_CELL_HEIGHT_PX)

static NYA_Window window = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = (u32)(COLUMNS * NYA_TERMINAL_CELL_WIDTH_PX),
    .screen_height = (u32)(ROWS * NYA_TERMINAL_CELL_HEIGHT_PX),
};

static NYA_UICells cells;
static NYA_UICells ascii;

/** Everything a widget below writes into, so a pass is a pure function of what the test set. */
static b8    state_fullscreen = false;
static f32   state_volume     = 0.5F;
static u32   state_quality    = 1;
static b8    state_open       = true;
static char  state_name[NYA_UI_TEXT_INPUT_MAX] = "ada";
static f32   state_series[4]                   = { 1.0F, 4.0F, 2.0F, 8.0F };

/**
 * The look this test asks for: every number already a whole cell, so the recorder and the cell presenter must
 * agree on all of them and any disagreement is the cell presenter rounding something it should not.
 * */
static NYA_UIStyle style(void) {
    return (NYA_UIStyle){
        .margin      = CELL_H,
        .padding     = CELL_H,
        .spacing     = CELL_H,
        .item_height = CELL_H,
        .scrim       = { 1.0F, 0.0F, 0.0F, 0.5F },
        .panel       = { 0.1F, 0.1F, 0.1F, 1.0F },
        .track       = { 0.2F, 0.2F, 0.2F, 1.0F },
        .accent      = { 0.3F, 0.6F, 0.9F, 1.0F },
        .text_dim    = { 0.5F, 0.5F, 0.5F, 1.0F },
        .button      = { .normal = { 0.2F, 0.2F, 0.2F, 1.0F }, .focused = { 0.3F, 0.3F, 0.4F, 1.0F }, .pressed = { 0.1F, 0.1F, 0.2F, 1.0F }, .disabled = { 0.1F, 0.1F, 0.1F, 1.0F } },
        .text        = { .normal = { 0.9F, 0.9F, 0.9F, 1.0F }, .focused = { 1.0F, 1.0F, 1.0F, 1.0F }, .pressed = { 0.8F, 0.8F, 0.9F, 1.0F }, .disabled = { 0.4F, 0.4F, 0.4F, 1.0F } },
    };
}

/* ONE WIDGET AT A TIME, STRAIGHT AT THE SEAM */

/** What a kind is expected to leave on the screen, and whether it leaves paper rather than characters. */
typedef struct {
    NYA_ConstCString expect;
    b8               paper;
} Expected;

static const NYA_UIPanel case_panel = { .title = "title" };
static const NYA_UIChart case_chart = { .values = state_series, .count = 4, .kind = NYA_UI_CHART_BAR };
static const NYA_UIIcon case_icon   = { .texture = "" };

/**
 * One widget of `kind`, filled the way the widget that declares it fills it. The switch covers the enum, so a kind
 * added to the seam is a compile error here rather than a blank space on a screen nobody looked at.
 * */
static Expected one(NYA_UIWidgetKind kind, NYA_UIWidgetDraw* draw) {
    *draw = (NYA_UIWidgetDraw){
        .kind    = kind,
        .rect    = { 0.0F, 0.0F, 20.0F * CELL_W, CELL_H },
        .label   = "ok",
        .opacity = 1.0F,
        .color   = { 0.9F, 0.9F, 0.9F, 1.0F },
        .clip    = { 0.0F, 0.0F, (f32)window.screen_width, (f32)window.screen_height },
        .text    = NYA_UI_TEXT_BODY,
    };

    switch (kind) {
        case NYA_UI_WIDGET_SCRIM:  draw->label = ""; return (Expected){ .paper = true };
        case NYA_UI_WIDGET_STRIPE: draw->label = ""; return (Expected){ .paper = true };

        case NYA_UI_WIDGET_PANEL: {
            draw->rect              = (NYA_Rectf){ 0.0F, 0.0F, 20.0F * CELL_W, 4.0F * CELL_H };
            draw->label             = "title";
            draw->as_panel.options  = &case_panel;
            draw->as_panel.bar      = CELL_H;

            return (Expected){ .expect = "┌ title ─" };
        }

        case NYA_UI_WIDGET_LABEL: {
            draw->label          = "plain";
            draw->as_label.room  = 20.0F * CELL_W;

            return (Expected){ .expect = "plain" };
        }

        // exactly its text plus the padding either side, which is the width the layout gives a button.
        case NYA_UI_WIDGET_BUTTON: {
            draw->rect = (NYA_Rectf){ 0.0F, 0.0F, 6.0F * CELL_W, CELL_H };

            return (Expected){ .expect = "[ ok ]" };
        }

        case NYA_UI_WIDGET_SELECTABLE: draw->as_choice.on = true; return (Expected){ .expect = "‣ ok" };
        case NYA_UI_WIDGET_TOGGLE:     draw->as_choice.on = true; return (Expected){ .expect = "[x] ok" };
        case NYA_UI_WIDGET_RADIO:      draw->as_choice.on = true; return (Expected){ .expect = "(•) ok" };

        case NYA_UI_WIDGET_SLIDER: {
            draw->as_slider = (typeof(draw->as_slider)){ .track = { 10.0F * CELL_W, 0.0F, 10.0F * CELL_W, CELL_H }, .t = 0.5F };

            return (Expected){ .expect = "[████░░░░]" };
        }

        case NYA_UI_WIDGET_DROPDOWN: {
            draw->as_dropdown = (typeof(draw->as_dropdown)){ .shown = "high", .open = false };

            return (Expected){ .expect = "high ▾" };
        }

        case NYA_UI_WIDGET_FIELD: {
            draw->as_field.field = (NYA_UIFieldDraw){ .box = { 8.0F * CELL_W, 0.0F, 10.0F * CELL_W, CELL_H }, .buffer = "ada", .composing = "" };

            return (Expected){ .expect = "[ada" };
        }

        case NYA_UI_WIDGET_COLOR_PICKER: {
            draw->rect       = (NYA_Rectf){ 0.0F, 0.0F, 20.0F * CELL_W, 6.0F * CELL_H };
            draw->as_picker  = (typeof(draw->as_picker)){
                .plane  = { 0.0F, CELL_H, 8.0F * CELL_W, 4.0F * CELL_H },
                .hue    = { 9.0F * CELL_W, CELL_H, CELL_W, 4.0F * CELL_H },
                .alpha  = { 0.0F, 5.0F * CELL_H, 8.0F * CELL_W, CELL_H },
                .swatch = { 11.0F * CELL_W, 0.0F, 2.0F * CELL_W, CELL_H },
                .hsv    = { 210.0F, 0.5F, 0.75F, 1.0F },
                .value  = { 0.3F, 0.5F, 0.9F, 1.0F },
                .field  = { .box = { 12.0F * CELL_W, 0.0F, 8.0F * CELL_W, CELL_H }, .buffer = "4d80e6", .composing = "" },
            };

            return (Expected){ .expect = "[4d80e6]" };
        }

        case NYA_UI_WIDGET_CHART: {
            draw->rect           = (NYA_Rectf){ 0.0F, 0.0F, 4.0F * CELL_W, 2.0F * CELL_H };
            draw->as_chart.chart = &case_chart;

            return (Expected){ .expect = "█" };
        }

        case NYA_UI_WIDGET_ICON: {
            draw->rect          = (NYA_Rectf){ 0.0F, 0.0F, CELL_W, CELL_H };
            draw->as_icon.icon  = &case_icon;

            return (Expected){ .expect = "▪" };
        }

        case NYA_UI_WIDGET_SECTION: {
            draw->label         = "more";
            draw->as_mark.mark  = NYA_UI_MARK_EXPANDED;

            return (Expected){ .expect = "▾ more" };
        }

        case NYA_UI_WIDGET_CHROME: {
            draw->rect         = (NYA_Rectf){ 0.0F, 0.0F, CELL_W, CELL_H };
            draw->label        = "";
            draw->as_mark      = (typeof(draw->as_mark)){ .mark = NYA_UI_MARK_CLOSE, .body = true };

            return (Expected){ .expect = "✕" };
        }

        case NYA_UI_WIDGET_GRIP: {
            draw->rect          = (NYA_Rectf){ 0.0F, 0.0F, CELL_W, CELL_H };
            draw->label         = "";
            draw->as_mark.mark  = NYA_UI_MARK_GRIP;

            return (Expected){ .expect = "◢" };
        }

        case NYA_UI_WIDGET_SCROLLBAR: {
            draw->rect  = (NYA_Rectf){ 0.0F, 0.0F, CELL_W, 3.0F * CELL_H };
            draw->label = "";

            return (Expected){ .expect = "█" };
        }

        case NYA_UI_WIDGET_RULE: {
            draw->rect  = (NYA_Rectf){ 0.0F, 0.0F, 4.0F * CELL_W, CELL_H };
            draw->label = "";

            return (Expected){ .expect = "────" };
        }

        case NYA_UI_WIDGET_UNDERLINE: {
            draw->rect  = (NYA_Rectf){ 0.0F, 0.0F, 4.0F * CELL_W, CELL_H };
            draw->label = "";

            return (Expected){ .expect = "━━━━" };
        }

        case NYA_UI_WIDGET_KIND_COUNT:
        default:                       nya_unreachable();
    }
}

/** One widget through the presenter, with the look built first, exactly as a pass would. */
static void present(NYA_UICells* grid, const NYA_UIWidgetDraw* draw) {
    const NYA_UIPresenter* presenter = nya_ui_cells_presenter(grid);
    NYA_UIStyle            look      = style();
    NYA_UILook             built     = { 0 };

    nya_ui_cells_reset(grid);

    presenter->look_build(presenter->state, 0, &look, 1.0F, &built);
    presenter->look_use(presenter->state, 0);
    presenter->draw(presenter->state, &window, draw);
}

/* A WHOLE TREE, AGAINST THE RECORDER */

/** The tree both presenters run. Not one line of it knows which one is installed. */
static void screen(NYA_UIPass pass) {
    NYA_ConstCString options[2] = { "low", "high" };

    NYA_UI* ui = nya_ui_begin(&window, pass);

    nya_ui_scrim(ui);

    if (nya_ui_panel_begin(ui, "settings", (NYA_UIPanel){ .width = nya_ui_fixed(40.0F * CELL_W), .title = "settings" })) {
        nya_ui_label(ui, "audio");

        (void)nya_ui_button(ui, "apply");
        (void)nya_ui_selectable(ui, "chosen", true);
        (void)nya_ui_toggle(ui, "fullscreen", &state_fullscreen);
        (void)nya_ui_slider(ui, "volume", &state_volume, 0.0F, 1.0F, 0.25F);
        (void)nya_ui_radio(ui, "high", &state_quality, 1);
        (void)nya_ui_dropdown(ui, "quality", options, nya_carray_length(options), &state_quality);
        (void)nya_ui_text_input(ui, "name", state_name, sizeof(state_name));

        if (nya_ui_section_begin(ui, "more", &state_open)) {
            nya_ui_label(ui, "inside");
            nya_ui_section_end(ui);
        }

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
}

/** Two draw passes, since a container laid out for the first time draws nothing until it has been measured. */
static void draw_twice(NYA_UICells* grid) {
    screen(NYA_UI_PASS_DRAW);

    if (grid != nullptr) nya_ui_cells_reset(grid);

    screen(NYA_UI_PASS_DRAW);
}

/**
 * Whether every size the layout *adds up* is the same on both presenters. Not every field: a corner radius, a
 * shadow and a focus pop are what the cell presenter deliberately zeroes, and none of them moves a widget.
 * */
static b8 looks_match(const NYA_UILook* a, const NYA_UILook* b) {
    if (a->margin != b->margin || a->padding != b->padding || a->spacing != b->spacing) return false;
    if (a->outline != b->outline) return false;
    if (a->item_height != b->item_height) return false;

    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) {
        if (a->line_heights[i] != b->line_heights[i]) return false;
    }

    return true;
}

/** Whether every edge of `rect` lands on a cell boundary. A widget one and a half cells wide is the bug. */
static b8 whole_cells(NYA_Rectf rect) {
    return fmodf(rect.x, CELL_W) == 0.0F && fmodf(rect.y, CELL_H) == 0.0F && fmodf(rect.width, CELL_W) == 0.0F && fmodf(rect.height, CELL_H) == 0.0F;
}

/** The first row of the grid with `attribute` anywhere along it. ROWS when no row has it. */
static u16 row_with(const NYA_UICells* grid, u8 attribute) {
    for (u16 row = 0; row < ROWS; row++) {
        for (u16 column = 0; column < COLUMNS; column++) {
            if ((nya_ui_cells_at(grid, column, row).attributes & attribute) != 0) return row;
        }
    }

    return ROWS;
}

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

    nya_ui_style_set(&window, style());

    nya_ui_cells_init(&cells, (NYA_UICellOptions){ 0 });
    nya_ui_cells_init(&ascii, (NYA_UICellOptions){ .ascii = true });

    defer nya_ui_cells_deinit(&cells);
    defer nya_ui_cells_deinit(&ascii);

    static char dump[8192];

    // ── Every kind of widget draws something a person would recognise, and none of them draws nothing.
    {
        for (u32 kind = 0; kind < NYA_UI_WIDGET_KIND_COUNT; kind++) {
            NYA_UIWidgetDraw draw     = { 0 };
            Expected         expected = one((NYA_UIWidgetKind)kind, &draw);
            NYA_ConstCString name     = nya_ui_widget_kind_name((NYA_UIWidgetKind)kind);

            present(&cells, &draw);
            (void)nya_ui_cells_write(&cells, dump, sizeof(dump));

            if (expected.paper) {
                nya_check(nya_ui_cells_at(&cells, 0, 0).background != 0, "%s leaves paper behind it", name);
                continue;
            }

            nya_check(expected.expect != nullptr && strstr(dump, expected.expect) != nullptr, "%s draws as \"%s\", got \"%s\"", name, expected.expect, dump);
        }
    }

    // ── And the ASCII table is the same screen with different characters: no multi-byte anywhere in it.
    {
        for (u32 kind = 0; kind < NYA_UI_WIDGET_KIND_COUNT; kind++) {
            NYA_UIWidgetDraw draw = { 0 };
            (void)one((NYA_UIWidgetKind)kind, &draw);

            present(&ascii, &draw);

            u32 wrote = nya_ui_cells_write(&ascii, dump, sizeof(dump));

            b8 plain = true;
            for (u32 i = 0; i < wrote; i++) plain &= (u8)dump[i] < 0x80U;

            nya_check(plain, "%s draws in ASCII alone when the grid was opened that way, got \"%s\"", nya_ui_widget_kind_name((NYA_UIWidgetKind)kind), dump);
        }

        NYA_UIWidgetDraw draw = { 0 };
        (void)one(NYA_UI_WIDGET_TOGGLE, &draw);
        present(&ascii, &draw);
        (void)nya_ui_cells_write(&ascii, dump, sizeof(dump));

        nya_check(strstr(dump, "[x] ok") != nullptr, "a toggle reads the same either way, got \"%s\"", dump);

        (void)one(NYA_UI_WIDGET_PANEL, &draw);
        present(&ascii, &draw);
        (void)nya_ui_cells_write(&ascii, dump, sizeof(dump));

        nya_check(strstr(dump, "+ title -") != nullptr, "and a frame falls back to the characters every terminal has, got \"%s\"", dump);
    }

    // ── Focus is two marks, and neither of them is a colour: reverse video, and the angles around the widget.
    {
        NYA_UIWidgetDraw draw = { 0 };
        (void)one(NYA_UI_WIDGET_BUTTON, &draw);

        present(&cells, &draw);
        nya_check((nya_ui_cells_at(&cells, 0, 0).attributes & NYA_TERMINAL_ATTRIBUTE_REVERSE) == 0, "a button at rest is not reversed");

        draw.state.focused = true;
        draw.state.focus   = 1.0F;
        present(&cells, &draw);

        for (u16 column = 0; column < 6; column++) {
            nya_check((nya_ui_cells_at(&cells, column, 0).attributes & NYA_TERMINAL_ATTRIBUTE_REVERSE) != 0, "a focused button is reversed across its whole row");
        }

        (void)nya_ui_cells_write(&cells, dump, sizeof(dump));
        nya_check(strstr(dump, "‹ ok ›") != nullptr, "and its delimiters say so again, for a terminal with no attributes, got \"%s\"", dump);

        // held and disabled are attributes for the same reason focus is: they have to read without colour.
        draw.state.held = true;
        present(&cells, &draw);
        nya_check((nya_ui_cells_at(&cells, 0, 0).attributes & NYA_TERMINAL_ATTRIBUTE_UNDERLINE) != 0, "a held widget is underlined");

        draw.state          = (NYA_UIWidgetState){ .disabled = true };
        present(&cells, &draw);
        nya_check((nya_ui_cells_at(&cells, 0, 0).attributes & NYA_TERMINAL_ATTRIBUTE_DIM) != 0, "a disabled one is dim");
    }

    // ── The same tree through the recorder and through this presenter: the same sizes, and every one of them whole.
    {
        static NYA_UIRecorder recorder;

        nya_ui_recorder_init(&recorder, (f32x2){ CELL_W, CELL_H });
        defer nya_ui_recorder_deinit(&recorder);

        nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));
        draw_twice(nullptr);
        nya_ui_recorder_reset(&recorder);
        screen(NYA_UI_PASS_DRAW);

        nya_ui_presenter_set(&window, nya_ui_cells_presenter(&cells));
        draw_twice(&cells);

        nya_check(looks_match(&recorder.looks[0], &cells.looks[0]), "a style already written in whole cells is the same look on both presenters");

        u32 count = nya_ui_recorder_count(&recorder);
        nya_check(count > 0, "the recorder saw the tree, got %u widgets", count);

        for (u32 i = 0; i < count; i++) {
            const NYA_UIWidgetDraw* widget = nya_ui_recorder_at(&recorder, i);

            nya_check(whole_cells(widget->rect), "%s \"%s\" lands on whole cells, got %f,%f %fx%f", nya_ui_widget_kind_name(widget->kind), widget->label,
                      (f64)widget->rect.x, (f64)widget->rect.y, (f64)widget->rect.width, (f64)widget->rect.height);
        }

        // and the screen is the widgets, not a picture of them.
        (void)nya_ui_cells_write(&cells, dump, sizeof(dump));

        // the first widget of the panel, so it is the one focus lands on and it wears both focus marks.
        nya_check(strstr(dump, "‹ apply ›") != nullptr, "the button reads as a focused button on the screen the tree drew:\n%s", dump);
        nya_check(strstr(dump, "[ ] fullscreen") != nullptr, "the toggle reads as a toggle:\n%s", dump);
        nya_check(strstr(dump, "‣ chosen") != nullptr, "the selectable says it is the chosen one:\n%s", dump);
        nya_check(strstr(dump, "settings") != nullptr && strstr(dump, "┌") != nullptr, "and the panel is a frame with its title in the top edge:\n%s", dump);
    }

    // ── Measurement is cells, not pixels: nothing it answers is a fraction of one.
    {
        const NYA_UIPresenter* presenter = nya_ui_cells_presenter(&cells);

        f32x2 measured = presenter->measure(presenter->state, NYA_UI_TEXT_BODY, "twelve chars", 0.0F, NYA_UI_OVERFLOW_VISIBLE);
        nya_check(measured.x == 12.0F * CELL_W && measured.y == CELL_H, "a line is as many cells as it has characters, got %fx%f", (f64)measured.x,
                  (f64)measured.y);

        // one code point, three bytes: a measurement in bytes would say three cells and draw one.
        measured = presenter->measure(presenter->state, NYA_UI_TEXT_BODY, "█", 0.0F, NYA_UI_OVERFLOW_VISIBLE);
        nya_check(measured.x == CELL_W, "a multi-byte character is one cell, got %f", (f64)measured.x);

        f32x2 wrapped = presenter->measure(presenter->state, NYA_UI_TEXT_BODY, "twelve chars", 5.0F * CELL_W, NYA_UI_OVERFLOW_WRAP);
        nya_check(wrapped.x == 5.0F * CELL_W && wrapped.y == 3.0F * CELL_H, "a wrapped line is whole rows of whole columns, got %fx%f", (f64)wrapped.x,
                  (f64)wrapped.y);

        f32 prefix = presenter->measure_bytes(presenter->state, NYA_UI_TEXT_BODY, "caret", 3);
        nya_check(prefix == 3.0F * CELL_W, "a caret sits on a cell boundary, got %f", (f64)prefix);

        // an odd style is rounded before the layout ever sees it, which is the promise the header makes.
        NYA_UIStyle odd   = style();
        NYA_UILook  built = { 0 };

        odd.padding     = 5.0F;
        odd.spacing     = 13.0F;
        odd.item_height = 3.0F;
        odd.radius      = 9.0F;

        presenter->look_build(presenter->state, 0, &odd, 1.0F, &built);

        nya_check(fmodf(built.padding, CELL_H) == 0.0F && fmodf(built.spacing, CELL_H) == 0.0F, "padding and spacing are whole rows, got %f and %f",
                  (f64)built.padding, (f64)built.spacing);
        nya_check(built.item_height == CELL_H, "a row is exactly one row tall, got %f", (f64)built.item_height);
        nya_check(built.radius == 0.0F && built.depth == 0.0F && built.pop == 0.0F, "and there is no corner, shadow or pop to round");
    }

    // ── Focus moving through a real tree marks the widget that has it, and only that one.
    {
        nya_ui_focus_reset(&window);
        tick();

        draw_twice(&cells);

        screen(NYA_UI_PASS_INPUT);
        tick();

        nya_ui_cells_reset(&cells);
        screen(NYA_UI_PASS_DRAW);

        u16 focused = row_with(&cells, NYA_TERMINAL_ATTRIBUTE_REVERSE);
        nya_check(focused < ROWS, "something on the screen carries the focus mark");

        tap(NYA_KEY_DOWN);
        screen(NYA_UI_PASS_INPUT);
        tick();

        nya_ui_cells_reset(&cells);
        screen(NYA_UI_PASS_DRAW);

        nya_check(row_with(&cells, NYA_TERMINAL_ATTRIBUTE_REVERSE) != focused, "and it moves down the panel with the arrow keys");
    }

    // ── The grid is the whole output: nothing reaches a terminal until it is asked to, and there is none here.
    {
        nya_ui_cells_present(&cells);

        nya_check(!nya_terminal_is_open(), "the test never opened a terminal, and presenting to none is not a crash");

        u32 wrote = nya_ui_cells_write(&cells, dump, sizeof(dump));
        nya_check(wrote > 0 && dump[wrote] == '\0', "the screen reads back as text, %u bytes", wrote);

        char cut[16];
        u32  short_write = nya_ui_cells_write(&cells, cut, sizeof(cut));
        nya_check(short_write < sizeof(cut) && cut[short_write] == '\0', "a buffer too small is cut rather than overrun");

        nya_ui_cells_reset(&cells);
        nya_check(nya_ui_cells_at(&cells, 0, 0).codepoint == 0, "and a reset grid is an empty screen");
    }

    // ── The window goes back to the shape presenter, so nothing after this test is drawn in cells.
    nya_ui_presenter_set(&window, nullptr);

    return nya_check_failures() == 0 ? 0 : 1;
}
