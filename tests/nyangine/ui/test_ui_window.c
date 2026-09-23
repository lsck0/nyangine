/**
 * A window's chrome, headless: the close button writes the caller's flag and nothing in the UI can write it back,
 * the chevron folds the body away and shrinks the window to its bar, the hamburger's list floats over the widget
 * under it and takes the click that widget would have had, and the corner grip resizes without moving the window.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

/** The default margin between the window and a top level panel, and the test style's frame, gap and item height. */
#define MARGIN 16.0F
#define FRAME  10.0F
#define GAP    6.0F
#define ITEM   40.0F

/** The title size the chrome is square to, and the window's width. */
#define TITLE 30.0F
#define WIDTH 400.0F

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static NYA_UIWindowState state = { .open = true };

static const NYA_ConstCString MENU[] = { "reset", "shut" };

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

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

/** What one pass over the window saw. */
typedef struct {
    /** Whether the body ran at all, which is what a closed or folded window answers no to. */
    b8 body_seen;

    /** Whether the one button in the body was activated. */
    b8 button_hit;

    /** Where the window was laid out, read back from its container state. */
    NYA_Rectf bounds;
} Scene;

static Scene scene(NYA_UIPass pass) {
    Scene   out = { 0 };
    NYA_UI* ui  = nya_ui_begin(&window, pass);

    NYA_UIWindow tools = {
        .panel      = { .width = nya_ui_fixed(WIDTH) },
        .title      = "tools",
        .close      = true,
        .collapse   = true,
        .resize     = true,
        .menu       = MENU,
        .menu_count = nya_carray_length(MENU),
    };

    if (nya_ui_window_begin(ui, "tools", tools, &state)) {
        out.body_seen  = true;
        out.button_hit = nya_ui_button(ui, "apply");

        nya_ui_window_end(ui);
    }

    nya_ui_end(ui);

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        if (_nya_ui.panels[i].id == _nya_ui_id((u64)window.handle.index + 1, "tools")) out.bounds = _nya_ui.panels[i].bounds;
    }

    if (pass == NYA_UI_PASS_INPUT) tick();

    return out;
}

/** A press and its release in one tick, which is how a click arrives, then one input pass over the window. */
static Scene click_scene(f32x2 at) {
    pointer_move(at);
    pointer_button(true);
    pointer_button(false);

    return scene(NYA_UI_PASS_INPUT);
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

    nya_font_default_set(nya_font(FACE, 20.0F));

    // the title size is a glyph atlas of its own, and the chrome is square to its line height.
    for (u32 i = 0; i < 32 && nya_font_metrics(nya_font(FACE, TITLE)).line_height <= 0.0F; i++) {
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    }

    f32 side = ceilf(nya_font_metrics(nya_font(FACE, TITLE)).line_height);
    nya_check(side > 0.0F, "the title face loads");

    nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 20.0F, .title_size = TITLE, .padding = FRAME, .spacing = GAP, .item_height = ITEM });

    // a container lays out without drawing until it has been measured, so nothing is placed until the second pass.
    Scene laid = { 0 };
    for (u32 pass = 0; pass < 3; pass++) laid = scene(NYA_UI_PASS_DRAW);

    nya_check(laid.body_seen, "an open window runs its body");
    nya_check(laid.bounds.width == WIDTH, "and is as wide as it asked to be, got %f", (f64)laid.bounds.width);

    // the bar the panel reserved, and the chrome in it: the hamburger at the left, then the chevron and the X from
    // the right. Everything here is what nya_ui_window_begin places, written out so a move is caught.
    f32 bar_y  = laid.bounds.y + FRAME;
    f32 bar_to = laid.bounds.x + laid.bounds.width - FRAME;

    f32x2 menu     = { laid.bounds.x + FRAME + (side * 0.5F), bar_y + (side * 0.5F) };
    f32x2 collapse = { bar_to - (side * 1.5F), bar_y + (side * 0.5F) };
    f32x2 close    = { bar_to - (side * 0.5F), bar_y + (side * 0.5F) };

    f32 expanded_height = laid.bounds.height;

    // ── The close button writes the caller's flag, and only the caller can write it back.
    {
        Scene closed = click_scene(close);
        nya_check(!state.open, "the close button closes the window");
        nya_check(!closed.body_seen, "and its body stops running from that pass on");

        for (u32 pass = 0; pass < 2; pass++) {
            Scene gone = scene(NYA_UI_PASS_DRAW);
            nya_check(!gone.body_seen && !state.open, "a closed window stays closed on its own");
        }

        state.open = true;
        Scene back = scene(NYA_UI_PASS_DRAW);
        nya_check(back.body_seen, "and shows again when the caller says so");
    }

    // ── The chevron folds the body away and leaves the title bar, and the window shrinks to it.
    {
        Scene folded = click_scene(collapse);
        nya_check(state.collapsed, "the chevron collapses the window");
        nya_check(!folded.body_seen, "and its body stops running");

        Scene shrunk = scene(NYA_UI_PASS_DRAW);
        nya_check(shrunk.bounds.height < expanded_height, "a collapsed window is shorter than an open one, got %f against %f",
                  (f64)shrunk.bounds.height, (f64)expanded_height);
        nya_check(shrunk.bounds.height >= side, "and is still its title bar, got %f", (f64)shrunk.bounds.height);

        Scene opened = click_scene(collapse);
        nya_check(!state.collapsed && opened.body_seen, "and the same button opens it again");
    }

    for (u32 pass = 0; pass < 2; pass++) laid = scene(NYA_UI_PASS_DRAW);

    // ── The hamburger's list floats over the body and takes the click the widget under it would have had.
    {
        nya_check(state.menu_picked == NYA_UI_MENU_NONE, "a pass that picked nothing reports nothing");

        Scene shown = click_scene(menu);
        nya_check(state.menu_picked == NYA_UI_MENU_NONE, "opening the menu picks nothing on its own");
        nya_check(!shown.button_hit, "and the press that opened it activated nothing");

        // twice, so the open list is measured before it is clicked.
        for (u32 pass = 0; pass < 2; pass++) (void)scene(NYA_UI_PASS_DRAW);

        // the list hangs from the bottom of the button; its first option is one frame and half an item into it,
        // which is over the body's only widget.
        f32x2 first = { menu.x, bar_y + side + FRAME + (ITEM * 0.5F) };

        Scene picked = click_scene(first);
        nya_check(state.menu_picked == 0, "clicking the first item reports its index, got %u", state.menu_picked);
        nya_check(!picked.button_hit, "and the widget the list hangs over never sees the click");

        (void)scene(NYA_UI_PASS_INPUT);
        nya_check(state.menu_picked == NYA_UI_MENU_NONE, "a pick lasts the one pass that made it");

        // the same point reaches the widget once the list has closed, which is what makes that a covering test.
        for (u32 pass = 0; pass < 2; pass++) (void)scene(NYA_UI_PASS_DRAW);

        Scene freed = click_scene(first);
        nya_check(freed.button_hit, "and reaches it once the list is gone");
    }

    // ── The corner grip resizes the window where it stands.
    {
        for (u32 pass = 0; pass < 2; pass++) laid = scene(NYA_UI_PASS_DRAW);

        f32   grip   = NYA_UI_GRIP;
        f32x2 corner = { laid.bounds.x + laid.bounds.width - (grip * 0.5F), laid.bounds.y + laid.bounds.height - (grip * 0.5F) };

        pointer_move(corner);
        pointer_button(true);
        (void)scene(NYA_UI_PASS_INPUT);

        nya_check(state.size.x > 0.0F, "taking the grip records the size the window already had, got %f", (f64)state.size.x);

        f32 held = state.size.x;

        pointer_move((f32x2){ corner.x + 40.0F, corner.y + 20.0F });
        (void)scene(NYA_UI_PASS_INPUT);

        nya_check(state.size.x == held + 40.0F, "dragging it right widens the window by that much, got %f against %f", (f64)state.size.x,
                  (f64)held + 40.0);

        pointer_button(false);
        (void)scene(NYA_UI_PASS_INPUT);

        f32 released = state.size.x;

        pointer_move((f32x2){ corner.x + 200.0F, corner.y });
        (void)scene(NYA_UI_PASS_INPUT);

        nya_check(state.size.x == released, "and letting go stops it following the pointer");

        Scene resized = scene(NYA_UI_PASS_DRAW);
        nya_check(resized.bounds.width == released, "the window is laid out at what the grip left, got %f against %f", (f64)resized.bounds.width,
                  (f64)released);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a big title with every piece of chrome: the bar holds it, and the bar drags.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        // what gnyame's own style does: a title twice the body size.
        nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 22.0F, .title_size = 44.0F, .padding = FRAME, .spacing = GAP, .item_height = ITEM });

        for (u32 i = 0; i < 32 && nya_font_metrics(nya_font(FACE, 44.0F)).line_height <= 0.0F; i++) {
            nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        }

        state = (NYA_UIWindowState){ .open = true };

        Scene big = scene(NYA_UI_PASS_DRAW);
        nya_check(big.body_seen, "the window with a big title is up");

        NYA_UIStyle style = nya_ui_style_get(&window);
        NYA_UILook  look  = { 0 };
        nya_ui_look_scale(&style, 1.0F, &look);
        look.line_heights[NYA_UI_TEXT_TITLE] = ceilf(nya_font_metrics(nya_font(FACE, 44.0F)).line_height);

        // the bar is drawn around its text rather than tight to it.
        f32 bar = roundf(look.line_heights[NYA_UI_TEXT_TITLE] + look.padding);
        nya_check(bar > look.line_heights[NYA_UI_TEXT_TITLE], "the bar is taller than the line it holds");

        // the bar drags from a point that is bar and nothing else: past the hamburger, before the chevron.
        f32x2 grip = { big.bounds.x + FRAME + bar + 10.0F, big.bounds.y + FRAME + bar * 0.5F };

        pointer_move(grip);
        pointer_button(true);
        (void)scene(NYA_UI_PASS_INPUT);

        pointer_move((f32x2){ grip.x + 40.0F, grip.y + 25.0F });
        Scene moved = scene(NYA_UI_PASS_INPUT);

        pointer_button(false);
        (void)scene(NYA_UI_PASS_INPUT);

        nya_check(moved.bounds.x > big.bounds.x, "the window moved with the pointer, from %.1f to %.1f", (f64)big.bounds.x, (f64)moved.bounds.x);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
