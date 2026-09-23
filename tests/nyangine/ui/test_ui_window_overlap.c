/**
 * Two windows on top of each other, headless.
 *
 * A window is a panel with chrome, and the chrome is what makes overlapping windows behave or not: a
 * bar under another window is not a handle, a grip under one is not a grip, and a close button under
 * one is not a button. What this pins is that every one of those refuses, that a click on what is
 * visible of the lower window brings it forward, and that the order the two draw in agrees with the
 * order the pointer is resolved in — back to front drawing and front to back hit testing disagreeing is
 * exactly what "the windows do not behave when they overlap" looks like.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

#define MARGIN 16.0F
#define TITLE  30.0F
#define WIDTH  360.0F

/**
 * The two windows overlap in their middles and nowhere else: the upper one is offset down and right, so
 * each keeps a strip of itself and its own title bar clear of the other. Sharing a corner would make
 * every coordinate in this file ambiguous, which is a test that lies rather than a test that fails.
 * */
#define UNDER_HEIGHT 420.0F
#define OVER_HEIGHT  240.0F
#define OVER_OFFSET  140.0F

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static NYA_UIWindowState UNDER = { .open = true };
static NYA_UIWindowState OVER  = { .open = true };

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED,
                        .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };

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

/** What one pass over both windows saw. */
typedef struct {
    b8 under_hit;
    b8 over_hit;

    /** Where each window's one button was placed, read from the pass rather than worked out here. */
    NYA_Rectf under_button;
    NYA_Rectf over_button;

    NYA_Rectf under_bounds;
    NYA_Rectf over_bounds;

    /** How many panels each draws over, which is what says who is in front. */
    s32 under_layer;
    s32 over_layer;
} Scene;

/** Reads back where a window was laid out and how many standing panels it draws over. */
static void panel_state(NYA_ConstCString id, OUT NYA_Rectf* out_bounds, OUT s32* out_rank) {
    u64 key = _nya_ui_id((u64)window.handle.index + 1, id);

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        if (_nya_ui.panels[i].id != key) continue;

        *out_bounds = _nya_ui.panels[i].bounds;
        *out_rank   = (s32)_nya_ui_panel_rank(_nya_ui_context(&window), i);
    }
}

/** Both windows, the lower declared first and anchored at the same corner. */
static Scene scene(NYA_UIPass pass) {
    Scene   out = { 0 };
    NYA_UI* ui  = nya_ui_begin(&window, pass);

    NYA_UIWindow under = {
        .panel    = { .width = nya_ui_fixed(WIDTH), .height = nya_ui_fixed(UNDER_HEIGHT) },
        .title    = "under",
        .close    = true,
        .collapse = true,
        .resize   = true,
    };

    if (nya_ui_window_begin(ui, "under", under, &UNDER)) {
        NYA_Rectf mark = nya_ui_space(ui, 0.0F, 0.0F);

        out.under_hit    = nya_ui_button(ui, "under button");
        out.under_button = (NYA_Rectf){ mark.x, mark.y + mark.height + 6.0F, mark.width, 40.0F };

        nya_ui_window_end(ui);
    }

    NYA_UIWindow over = {
        .panel    = { .width = nya_ui_fixed(WIDTH), .height = nya_ui_fixed(OVER_HEIGHT), .offset = { OVER_OFFSET, OVER_OFFSET } },
        .title    = "over",
        .close    = true,
        .collapse = true,
        .resize   = true,
    };

    if (nya_ui_window_begin(ui, "over", over, &OVER)) {
        NYA_Rectf mark = nya_ui_space(ui, 0.0F, 0.0F);

        out.over_hit    = nya_ui_button(ui, "over button");
        out.over_button = (NYA_Rectf){ mark.x, mark.y + mark.height + 6.0F, mark.width, 40.0F };

        nya_ui_window_end(ui);
    }

    nya_ui_end(ui);

    panel_state("under", &out.under_bounds, &out.under_layer);
    panel_state("over", &out.over_bounds, &out.over_layer);

    if (pass == NYA_UI_PASS_INPUT) tick();

    return out;
}

/** A press and its release in one tick, then one input pass. */
static Scene click(f32x2 at) {
    pointer_move(at);
    pointer_button(true);
    pointer_button(false);

    return scene(NYA_UI_PASS_INPUT);
}

/** A press that is still held, for a drag. */
static Scene press(f32x2 at) {
    pointer_move(at);
    pointer_button(true);

    return scene(NYA_UI_PASS_INPUT);
}

/** Moving with the button down, which is what drags a window by its bar. */
static Scene drag(f32x2 to) {
    pointer_move(to);

    return scene(NYA_UI_PASS_INPUT);
}

static Scene release(void) {
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

    nya_font_default_set(nya_font(FACE, 20.0F));

    // the title size is a glyph atlas of its own, and the chrome is square to its line height.
    for (u32 i = 0; i < 32 && nya_font_metrics(nya_font(FACE, TITLE)).line_height <= 0.0F; i++) {
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    }

    nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 20.0F, .title_size = TITLE, .padding = 10.0F, .spacing = 6.0F, .item_height = 40.0F });

    // one draw pass so both windows have bounds before anything is clicked.
    Scene first = scene(NYA_UI_PASS_DRAW);
    nya_check(first.under_bounds.width > 0.0F && first.over_bounds.width > 0.0F, "both windows were laid out");

    // ── the one declared last is in front, and draws over the other ──────────────
    nya_check(first.over_layer > first.under_layer, "the window declared last draws in front, got %d over %d", first.over_layer, first.under_layer);

    // ── a widget under another window refuses the pointer, and the one above takes it ──
    {
        Scene now = scene(NYA_UI_PASS_DRAW);

        // the upper window's own button, which sits over the lower window's body.
        f32x2 shared = { now.over_button.x + 20.0F, now.over_button.y + now.over_button.height * 0.5F };

        nya_check(nya_rect_contains(now.under_bounds, shared) && nya_rect_contains(now.over_bounds, shared), "the point clicked is in both");

        Scene hit = click(shared);

        nya_check(hit.over_hit, "the button in the window in front took the click");
        nya_check(!hit.under_hit, "and the one under it did not");
    }

    // ── a bar under another window is not a handle ───────────────────────────────
    {
        Scene before = scene(NYA_UI_PASS_DRAW);

        // the upper window's bar where the lower one is behind it: grabbing here must move the upper.
        f32x2 bar = { before.over_bounds.x + WIDTH * 0.5F, before.over_bounds.y + TITLE * 0.5F };
        nya_check(nya_rect_contains(before.under_bounds, bar), "the bar grabbed has the other window behind it");

        (void)press(bar);
        Scene dragged = drag((f32x2){ bar.x + 30.0F, bar.y + 20.0F });
        (void)release();

        nya_check(dragged.over_bounds.x > before.over_bounds.x, "the window in front moved");
        nya_check(dragged.under_bounds.x == before.under_bounds.x && dragged.under_bounds.y == before.under_bounds.y,
                  "and the one behind it stayed put, at %.1f,%.1f", (f64)dragged.under_bounds.x, (f64)dragged.under_bounds.y);
    }

    // ── a close button under another window is not a button ──────────────────────
    {
        Scene now = scene(NYA_UI_PASS_DRAW);

        // the lower window's close X, which the upper window covers.
        f32x2 close = { now.under_bounds.x + WIDTH - TITLE * 0.5F, now.under_bounds.y + TITLE * 0.5F };

        if (nya_rect_contains(now.over_bounds, close)) {
            (void)click(close);

            nya_check(UNDER.open, "a close button under another window closed it");
        }
    }

    // ── clicking what is visible of the lower window brings it forward, for good ──
    {
        Scene now = scene(NYA_UI_PASS_DRAW);

        // the lower window's own bar, which nothing covers: the upper one is offset past it.
        f32x2 bar = { now.under_bounds.x + 30.0F, now.under_bounds.y + TITLE * 0.5F };
        nya_check(!nya_rect_contains(now.over_bounds, bar), "the bar clicked is clear of the other window");

        Scene raised = click(bar);
        nya_check(raised.under_layer > raised.over_layer, "the clicked window came forward, got %d over %d", raised.under_layer, raised.over_layer);

        Scene after = scene(NYA_UI_PASS_DRAW);
        nya_check(after.under_layer > after.over_layer, "and stayed in front");

        // which is the whole point: the raised window's own button now takes the pointer even where the
        // other window is drawn over that spot.
        f32x2 own = { after.under_button.x + 20.0F, after.under_button.y + after.under_button.height * 0.5F };

        Scene hit = click(own);

        nya_check(hit.under_hit, "the raised window's button took the click");
        nya_check(!hit.over_hit, "and the window now behind refused it");
    }

    // ── the click that brings a window forward is spent on that and nothing else ──
    {
        // the upper window is in front again after the drag above; put the lower one behind it first.
        Scene now = scene(NYA_UI_PASS_DRAW);

        if (now.under_layer > now.over_layer) {
            f32x2 bar = { now.over_bounds.x + WIDTH - 120.0F, now.over_bounds.y + TITLE * 0.5F };
            (void)click(bar);

            now = scene(NYA_UI_PASS_DRAW);
        }

        nya_check(now.over_layer > now.under_layer, "the lower window is behind before this case");

        // its own button, clear of the window in front: one click brings the window forward and the
        // button does not take it, because somebody aiming at a window they cannot see is aiming at the
        // window.
        f32x2 own = { now.under_button.x + 20.0F, now.under_button.y + now.under_button.height * 0.5F };
        nya_check(!nya_rect_contains(now.over_bounds, own), "the button clicked is clear of the window in front");

        Scene raising = click(own);

        nya_check(raising.under_layer > raising.over_layer, "the click brought the window forward");
        nya_check(!raising.under_hit, "and was not also a press on the button under the pointer");

        // the second click is a click on the button, because by then nothing is being raised.
        Scene second = click(own);
        nya_check(second.under_hit, "the next click activates it");
    }

    // ── grabbing a window's bar brings it forward as it is grabbed ───────────────
    {
        Scene now = scene(NYA_UI_PASS_DRAW);
        nya_check(now.under_layer > now.over_layer, "the lower window is still the raised one");

        // the upper window's bar, clear of the other window and clear of its own chrome: the collapse
        // chevron and the close X live in the right of the bar, and pressing one is a button rather
        // than the start of a drag.
        f32x2 bar = { now.over_bounds.x + WIDTH - 120.0F, now.over_bounds.y + TITLE * 0.5F };
        nya_check(!nya_rect_contains(now.under_bounds, bar), "the bar grabbed is clear of the other window");

        Scene grabbed = press(bar);
        nya_check(grabbed.over_layer > grabbed.under_layer, "grabbing a bar brings its window forward, got %d over %d", grabbed.over_layer,
                  grabbed.under_layer);

        Scene moved = drag((f32x2){ bar.x + 25.0F, bar.y + 15.0F });
        nya_check(moved.over_bounds.x > grabbed.over_bounds.x, "and it moved with the pointer, from %.1f to %.1f", (f64)grabbed.over_bounds.x,
                  (f64)moved.over_bounds.x);
        nya_check(moved.over_layer > moved.under_layer, "and is still in front while it moves");

        (void)release();
    }

    // ── the strip a window is dragged by is the bar that is drawn ────────────────
    {
        /*
         * What gnyame does: a big title, a hamburger, and a panel declared before the windows. The bar a
         * person sees is the strip to grab, so the two have to be the same rectangle — a grip that
         * starts at the window's top edge and stops short of the bar's bottom is a window that does not
         * move when it is grabbed by the part of the bar below the mismatch.
         */
        nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 22.0F, .title_size = 44.0F, .padding = 14.0F, .spacing = 6.0F, .item_height = 40.0F });

        for (u32 i = 0; i < 32 && nya_font_metrics(nya_font(FACE, 44.0F)).line_height <= 0.0F; i++) {
            nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        }

        UNDER = (NYA_UIWindowState){ .open = true };
        OVER  = (NYA_UIWindowState){ .open = true };

        Scene now = scene(NYA_UI_PASS_DRAW);

        NYA_UIStyle style = nya_ui_style_get(&window);
        NYA_UILook  look  = { 0 };
        nya_ui_look_scale(&style, 1.0F, &look);

        f32 line = ceilf(nya_font_metrics(nya_font(FACE, 44.0F)).line_height);
        f32 bar  = roundf(line + (look.padding * 0.5F));

        // the bottom of the drawn bar, which is the part a person aims at when the title is tall.
        f32x2 low = { now.over_bounds.x + WIDTH * 0.5F, now.over_bounds.y + look.padding + bar - 4.0F };

        pointer_move(low);
        pointer_button(true);
        (void)scene(NYA_UI_PASS_INPUT);

        pointer_move((f32x2){ low.x + 40.0F, low.y + 20.0F });
        Scene moved = scene(NYA_UI_PASS_INPUT);

        pointer_button(false);
        (void)scene(NYA_UI_PASS_INPUT);

        nya_check(moved.over_bounds.x > now.over_bounds.x, "the bottom of the bar drags the window, from %.1f to %.1f", (f64)now.over_bounds.x,
                  (f64)moved.over_bounds.x);
    }

    printf("PASSED: ui window overlap\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
