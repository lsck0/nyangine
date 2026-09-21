/**
 * The widgets added on top of the original set, headless: radio buttons own their variable, tabs and dropdowns pick
 * one of a row, a dropdown only shows its list while it is open, a table's cells line up by column, a chart takes
 * the room it asks for, an opacity group nests and balances, and a draggable panel follows the pointer and stays
 * inside the safe area.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/** The test style's frame: padding 10 and no outline, inside the default margin of 16. */
#define MARGIN 16.0F
#define FRAME  10.0F
#define GAP    6.0F
#define ITEM   40.0F

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

/* ── What each pass reports back, so the checks read the layout rather than guessing at it. ── */

static u32 choice  = 0;
static u32 tab     = 0;
static u32 option  = 0;
static f32 samples[4] = { 0.0F, 1.0F, 2.0F, 1.0F };

static NYA_ConstCString LABELS[3] = { "one", "two", "three" };

typedef struct {
    NYA_Rectf first;
    NYA_Rectf second;
    NYA_Rectf row;
    NYA_Rectf chart;
    b8        changed;

    /** Where the button after the dropdown starts, and whether it was activated. An open list hangs over it. */
    NYA_Rectf under;
    b8        under_hit;
} Taken;

/** A panel with a radio pair, a tab strip, a dropdown and a button under it; `pass` picks input or draw. */
static Taken menu(NYA_UIPass pass) {
    Taken   taken = { 0 };
    NYA_UI* ui    = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "menu", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        taken.first  = nya_ui_space(ui, 0.0F, 0.0F);
        taken.changed = nya_ui_radio(ui, "first", &choice, 0);
        taken.changed = nya_ui_radio(ui, "second", &choice, 1) || taken.changed;

        taken.changed = nya_ui_tabs(ui, "pages", LABELS, 2, &tab) || taken.changed;
        taken.changed = nya_ui_dropdown(ui, "pick", LABELS, 3, &option) || taken.changed;

        // the marker is where the button starts, so the checks read the layout rather than guessing at it.
        taken.under     = nya_ui_space(ui, 0.0F, 0.0F);
        taken.under_hit = nya_ui_button(ui, "under");

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();

    return taken;
}

/** A two column table whose first column is `first_width` wide, reporting where its one row lands. */
static Taken table(f32 first_width) {
    const f32        widths[]  = { first_width, 0.0F };
    NYA_ConstCString headers[] = { "name", "value" };

    Taken   taken = { 0 };
    NYA_UI* ui    = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

    if (nya_ui_panel_begin(ui, "outer", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        taken.chart = nya_ui_space(ui, 0.0F, 0.0F);

        if (nya_ui_table_begin(ui, "rows", (NYA_UITable){ .widths = widths, .columns = 2, .headers = headers, .striped = true })) {
            if (nya_ui_table_row_begin(ui)) {
                taken.first  = nya_ui_space(ui, 0.0F, 20.0F);
                taken.second = nya_ui_space(ui, 0.0F, 20.0F);
                nya_ui_table_row_end(ui);
            }

            nya_ui_table_end(ui);
        }

        taken.row = nya_ui_space(ui, 0.0F, 5.0F);

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);

    return taken;
}

/** A panel with a chart in it, reporting the room the chart left behind. */
static NYA_Rectf chart_below(f32 height) {
    NYA_Rectf below = { 0 };
    NYA_UI*   ui    = nya_ui_begin(&window, NYA_UI_PASS_DRAW);

    if (nya_ui_panel_begin(ui, "plot", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
        nya_ui_chart(ui, "frames", (NYA_UIChart){ .values = samples, .count = 4, .height = height });
        below = nya_ui_space(ui, 0.0F, 1.0F);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);

    return below;
}

/** A draggable panel anchored top left, reporting where it ended up. */
static NYA_Rectf dragged(NYA_UIPass pass) {
    NYA_Rectf inside = { 0 };
    NYA_UI*   ui     = nya_ui_begin(&window, pass);

    if (nya_ui_panel_begin(ui, "movable", (NYA_UIPanel){ .width = nya_ui_fixed(200), .title = "drag me", .draggable = true })) {
        inside = nya_ui_space(ui, 0.0F, 20.0F);
        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();

    return inside;
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

    // the body size matches the face loaded above, so the look has a line height to measure a chart against.
    nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 20.0F, .padding = FRAME, .spacing = GAP, .item_height = ITEM });

    // ── A radio owns its variable: it writes `value` and reports only the pass that changed it.
    {
        // twice, so the second pass lays out from the first one's measurements.
        (void)menu(NYA_UI_PASS_DRAW);
        Taken laid = menu(NYA_UI_PASS_DRAW);

        NYA_Rectf second = { laid.first.x, laid.first.y + ITEM + GAP, laid.first.width, ITEM };

        click_at(center_of(second));
        Taken picked = menu(NYA_UI_PASS_INPUT);
        nya_check(picked.changed && choice == 1, "clicking the second radio writes its value, got %u", choice);

        click_at(center_of(second));
        Taken again = menu(NYA_UI_PASS_INPUT);
        nya_check(!again.changed && choice == 1, "and clicking it again reports no change");
    }

    // ── Tabs pick one of a row, and a dropdown only offers its options while it is open.
    {
        (void)menu(NYA_UI_PASS_DRAW);
        Taken laid = menu(NYA_UI_PASS_DRAW);

        // the tab strip is the third row, its two cells splitting the panel.
        f32       width = laid.first.width;
        NYA_Rectf tabs  = { laid.first.x, laid.first.y + ((ITEM + GAP) * 2.0F), width, ITEM };
        f32       cell  = roundf((width - GAP) * 0.5F);

        click_at((f32x2){ tabs.x + cell + GAP + (cell * 0.5F), tabs.y + (ITEM * 0.5F) });
        (void)menu(NYA_UI_PASS_INPUT);
        nya_check(tab == 1, "clicking the second tab selects it, got %u", tab);

        // the closed dropdown is the fourth row, and the button is the fifth.
        NYA_Rectf closed = { laid.first.x, laid.first.y + ((ITEM + GAP) * 3.0F), width, ITEM };
        NYA_Rectf shut   = laid.under;

        click_at(center_of(closed));
        (void)menu(NYA_UI_PASS_INPUT);
        nya_check(option == 0, "opening the list picks nothing on its own, got %u", option);

        // twice, so the open list is measured before it is clicked.
        (void)menu(NYA_UI_PASS_DRAW);
        Taken opened = menu(NYA_UI_PASS_DRAW);

        // ── The list floats: it takes no room, so nothing under it moved, and it hangs over the button instead.
        nya_check(opened.under.y == shut.y, "an open list does not push the button down, got %f against %f", (f64)opened.under.y, (f64)shut.y);

        // the list hangs from the bottom of the row, which is one gap above the marker, and is a framed column: its
        // own padding, then one option per item height. The button starts one gap under the marker.
        f32 first_option = shut.y - GAP + FRAME + (ITEM * 0.5F);

        f32x2 second_option = { closed.x + (width * 0.5F), first_option + ITEM + GAP };
        f32x2 over_button   = { closed.x + (width * 0.5F), shut.y + GAP + (ITEM * 0.5F) };

        // ── A click where the list covers the button goes to the list, and the button never sees it. The same point
        //    activates the button once the list is gone, which is what makes this a covering test and not a miss.
        click_at(over_button);
        Taken covered = menu(NYA_UI_PASS_INPUT);
        nya_check(!covered.under_hit, "a click on the list does not fall through to the button under it");

        click_at(second_option);
        (void)menu(NYA_UI_PASS_INPUT);
        nya_check(option == 1, "clicking the second option picks it, got %u", option);

        // and picking closes the list, so the room under the dropdown is the button's again.
        click_at(over_button);
        Taken freed = menu(NYA_UI_PASS_INPUT);
        nya_check(option == 1, "a click where the list was does not reach it once closed, got %u", option);
        nya_check(freed.under_hit, "and reaches the button instead");
    }

    // ── A table sizes its cells by its columns, so one row lines up with the next, and never sizes anything else.
    {
        for (u32 pass = 0; pass < 2; pass++) (void)table(120.0F);
        Taken laid = table(120.0F);

        f32 content = 400.0F - (FRAME * 2.0F);

        nya_check(laid.first.width == 120.0F, "a fixed column takes its width, got %f", (f64)laid.first.width);
        nya_check(laid.second.x == laid.first.x + 120.0F + GAP, "the next cell follows it, got %f", (f64)laid.second.x);
        nya_check(laid.second.width == content - 120.0F - GAP, "and a zero column grows into the rest, got %f", (f64)laid.second.width);
        nya_check(laid.row.y > laid.second.y, "what follows the table is under it, got %f", (f64)laid.row.y);

        /*
         * The header row and the rule under it are the table's own children, not cells, so a column width must not
         * become either one's height. It did: the widths were read for every child of the table, which made the
         * rule as tall as the second column and drew a bar over gnyame's counters.
         */
        f32 above = laid.first.y - laid.chart.y;

        for (u32 pass = 0; pass < 2; pass++) (void)table(300.0F);
        Taken wider = table(300.0F);

        nya_check(wider.first.y - wider.chart.y == above, "the headers are as tall whatever the columns are wide, got %f and %f",
                  (f64)(wider.first.y - wider.chart.y), (f64)above);
        nya_check(above < 120.0F, "and the rule under them is a rule, not a column width, got %f", (f64)above);
    }

    // ── A chart takes the height it asks for, and the default when it asks for none.
    {
        (void)chart_below(60.0F);
        NYA_Rectf below = chart_below(60.0F);
        nya_check(below.y == MARGIN + FRAME + 60.0F + GAP, "a chart takes the height it asks for, got %f", (f64)below.y);

        (void)chart_below(0.0F);
        NYA_Rectf fallback = chart_below(0.0F);
        nya_check(fallback.y > MARGIN + FRAME + GAP, "and a default one takes some, got %f", (f64)fallback.y);
    }

    // ── Opacity groups nest and balance, and leave the layout alone.
    {
        NYA_UI*   ui    = nya_ui_begin(&window, NYA_UI_PASS_DRAW);
        NYA_Rectf plain = { 0 };
        NYA_Rectf faded = { 0 };

        if (nya_ui_panel_begin(ui, "fade", (NYA_UIPanel){ .width = nya_ui_fixed(400) })) {
            plain = nya_ui_space(ui, 0.0F, 10.0F);

            nya_ui_opacity_begin(ui, 0.5F);
            nya_ui_opacity_begin(ui, 0.5F);
            faded = nya_ui_space(ui, 0.0F, 10.0F);
            nya_ui_opacity_end(ui);
            nya_ui_opacity_end(ui);

            nya_ui_panel_end(ui);
        }

        nya_ui_end(ui);

        nya_check(faded.y == plain.y + 10.0F + GAP && faded.height == plain.height, "a faded child takes the same room as a plain one");
    }

    // ── A draggable panel follows the pointer by its title, and stays inside the safe area.
    {
        (void)dragged(NYA_UI_PASS_DRAW);
        NYA_Rectf resting = dragged(NYA_UI_PASS_DRAW);

        // the title strip is the top of the panel, above the content.
        pointer_move((f32x2){ resting.x + 20.0F, MARGIN + 4.0F });
        pointer_button(true);
        (void)dragged(NYA_UI_PASS_INPUT);

        pointer_move((f32x2){ resting.x + 120.0F, MARGIN + 54.0F });
        NYA_Rectf moved = dragged(NYA_UI_PASS_INPUT);

        nya_check(moved.x == resting.x + 100.0F && moved.y == resting.y + 50.0F, "the panel follows the pointer, got %f %f against %f %f", (f64)moved.x,
                  (f64)moved.y, (f64)resting.x, (f64)resting.y);

        // far off the right edge, where the clamp has to pull it back.
        pointer_move((f32x2){ 4000.0F, 4000.0F });
        NYA_Rectf pushed = dragged(NYA_UI_PASS_INPUT);

        nya_check(pushed.x < (f32)window.screen_width && pushed.y < (f32)window.screen_height, "and never leaves the window, got %f %f", (f64)pushed.x, (f64)pushed.y);

        pointer_button(false);
        (void)dragged(NYA_UI_PASS_INPUT);

        pointer_move((f32x2){ 10.0F, 10.0F });
        NYA_Rectf released = dragged(NYA_UI_PASS_INPUT);

        nya_check(released.x == pushed.x && released.y == pushed.y, "a released panel stays where it was left");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
