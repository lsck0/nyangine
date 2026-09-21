/**
 * The immediate-mode UI's z order, headless: two overlapping top level panels, a widget under a higher one refusing
 * the pointer, a click on chrome claiming it instead of falling through, a click raising a panel for good, and an
 * explicit z beating both declaration order and every raise. The ranks the panels draw in are checked with them,
 * since back to front drawing and front to back hit testing have to agree on one order.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

/** The default margin between the window and a top level panel, and the test style's gap and item height. */
#define MARGIN 16.0F
#define GAP    6.0F
#define ITEM   40.0F

/**
 * Both panels are anchored at the same corner and are the same width, so everything in them lines up across; the
 * lower one is taller, which leaves a strip of it nothing covers for the raise to be clicked in.
 * */
#define PANEL_WIDTH  400.0F
#define UNDER_HEIGHT 420.0F
#define OVER_HEIGHT  300.0F

/** The upper panel's padding, which with its title is the chrome a click must not fall through. */
#define OVER_PADDING 40.0F

/** Pushes the lower panel's second button down to where the upper panel's sits, whatever its title measures. */
#define UNDER_SPACER 24.0F

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&event);
}

/** Press and release in one tick, which is how a click arrives and why the decision cannot wait for a barrier. */
static void click(void) {
    f32x2 at = nya_input_mouse_position();

    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = { .type = i == 0 ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP,
                            .as_mouse_button_event = { .is_down = i == 0, .button = NYA_MOUSE_BUTTON_LEFT, .x = at.x, .y = at.y } };
        nya_system_input_handle_event(&event);
    }
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static f32x2 center_of(NYA_Rectf rect) {
    return (f32x2){ rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };
}

/** Where the button declared after the marker `mark` sits: one gap below it, an item high, as wide as its column. */
static NYA_Rectf button_after(NYA_Rectf mark) {
    return (NYA_Rectf){ mark.x, mark.y + mark.height + GAP, mark.width, ITEM };
}

/** What one pass over the two panels placed, and which of the three buttons it activated. */
typedef struct {
    /** The lower panel's first button, which the upper panel's padding and title cover. */
    NYA_Rectf under;

    /** The lower panel's second button and the upper panel's only one, which land on each other. */
    NYA_Rectf shared;
    NYA_Rectf over;

    /** The strip of the lower panel below the upper one, which nothing covers. */
    NYA_Rectf free;

    s32 activated;
} Scene;

enum {
    ACTIVATED_NONE = -1,
    ACTIVATED_UNDER,
    ACTIVATED_SHARED,
    ACTIVATED_OVER,
};

/**
 * The lower panel, then the upper one over it, each at the given z. Declared low first, so declaration order alone
 * puts the upper one on top.
 * */
static Scene scene(NYA_UIPass pass, s32 z_under, s32 z_over) {
    Scene   out = { .activated = ACTIVATED_NONE };
    NYA_UI* ui  = nya_ui_begin(&window, pass);

    NYA_UIPanel under = { .width = nya_ui_fixed(PANEL_WIDTH), .height = nya_ui_fixed(UNDER_HEIGHT), .z = z_under };

    if (nya_ui_panel_begin(ui, "under", under)) {
        if (nya_ui_button(ui, "buried")) out.activated = ACTIVATED_UNDER;

        NYA_Rectf mark = nya_ui_space(ui, 0.0F, UNDER_SPACER);
        if (nya_ui_button(ui, "shared")) out.activated = ACTIVATED_SHARED;

        out.under  = (NYA_Rectf){ mark.x, mark.y - GAP - ITEM, mark.width, ITEM };
        out.shared = button_after(mark);
        out.free   = (NYA_Rectf){ MARGIN, MARGIN + OVER_HEIGHT, PANEL_WIDTH, UNDER_HEIGHT - OVER_HEIGHT };

        nya_ui_panel_end(ui);
    }

    NYA_UIPanel over = {
        .width   = nya_ui_fixed(PANEL_WIDTH),
        .height  = nya_ui_fixed(OVER_HEIGHT),
        .padding = OVER_PADDING,
        .title   = "over",
        .z       = z_over,
    };

    if (nya_ui_panel_begin(ui, "over", over)) {
        NYA_Rectf mark = nya_ui_space(ui, 0.0F, 0.0F);
        if (nya_ui_button(ui, "on top")) out.activated = ACTIVATED_OVER;

        out.over = button_after(mark);

        nya_ui_panel_end(ui);
    }

    nya_ui_end(ui);

    return out;
}

/** One input pass over the scene, with the click already queued. */
static s32 click_scene(f32x2 at, s32 z_under, s32 z_over) {
    pointer_move(at);
    click();

    s32 activated = scene(NYA_UI_PASS_INPUT, z_under, z_over).activated;
    tick();

    return activated;
}

/** The container slot `id` was claimed by, or U32_MAX. */
static u32 slot_of(NYA_ConstCString id) {
    u64 key = _nya_ui_id((u64)window.handle.index + 1, id);

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        if (_nya_ui.panels[i].id == key) return i;
    }

    return U32_MAX;
}

/** How many standing panels `id` draws over, which is what puts its geometry in front of theirs. */
static u32 rank_of(NYA_ConstCString id) {
    u32 slot = slot_of(id);
    nya_check(slot != U32_MAX, "the '%s' panel kept its slot", id);

    return _nya_ui_panel_rank(_nya_ui_context(&window), slot);
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
    for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 10.0F, .spacing = GAP, .outline = 2.0F, .item_height = ITEM });

    // a container lays out without drawing until it has been measured, occlusion reads the pass before it, and the
    // title size is an atlas of its own that the frames in between are what load.
    Scene placed = { 0 };

    for (u32 pass = 0; pass < 32; pass++) {
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        placed = scene(NYA_UI_PASS_DRAW, 0, 0);
    }

    NYA_Rectf both = nya_rect_intersection(placed.shared, placed.over);

    nya_check(both.width > 0.0F && both.height > 0.0F, "the two panels' buttons land on each other, got %f %f and %f %f", (f64)placed.shared.y,
              (f64)placed.shared.height, (f64)placed.over.y, (f64)placed.over.height);
    // the marker sits at the upper panel's content origin, so everything above it is padding, title and background.
    nya_check(placed.under.y + placed.under.height < placed.over.y - GAP, "and the buried button is above the upper panel's content, in its chrome");

    f32x2 overlap = center_of(both);
    f32x2 chrome  = center_of(placed.under);
    f32x2 free    = center_of(placed.free);

    // ── A widget under a higher panel never takes the pointer; the one in the panel above it does.
    {
        nya_check(rank_of("over") > rank_of("under"), "the panel declared last draws over the one before it");

        s32 activated = click_scene(overlap, 0, 0);
        nya_check(activated == ACTIVATED_OVER, "a click where both buttons are goes to the upper panel's, got %d", activated);
    }

    // ── A click on the upper panel's chrome claims the pointer rather than falling through to the button under it.
    {
        // hover first, from the last block's point: a covered widget must not take focus either, or the next
        // confirm would activate it with the pointer nowhere near.
        pointer_move(chrome);
        (void)scene(NYA_UI_PASS_INPUT, 0, 0);
        tick();

        u64 buried = _nya_ui_id(_nya_ui_id((u64)window.handle.index + 1, "under"), "buried");
        nya_check(_nya_ui_context(&window)->focus != buried, "hover under a panel does not move focus there");

        s32 activated = click_scene(chrome, 0, 0);
        nya_check(activated == ACTIVATED_NONE, "a click on the title and padding of a panel activates nothing under it, got %d", activated);
    }

    // ── Clicking a panel raises it, and it stays raised in the passes after.
    {
        s32 activated = click_scene(free, 0, 0);
        nya_check(activated == ACTIVATED_NONE, "the strip below the upper panel holds no widget");
        nya_check(rank_of("under") > rank_of("over"), "but clicking it raised the lower panel over the other");

        activated = click_scene(overlap, 0, 0);
        nya_check(activated == ACTIVATED_SHARED, "so the click where both buttons are now goes to the raised one, got %d", activated);

        // the raise outlives the pass that made it: nothing re-declares an order.
        (void)scene(NYA_UI_PASS_DRAW, 0, 0);
        nya_check(rank_of("under") > rank_of("over"), "and it is still over after a pass that clicked nothing");
    }

    // ── An explicit z beats declaration order and every raise inside it.
    {
        for (u32 pass = 0; pass < 2; pass++) (void)scene(NYA_UI_PASS_DRAW, 0, 1);
        nya_check(rank_of("over") > rank_of("under"), "a higher z is over a panel raised above it");

        s32 activated = click_scene(overlap, 0, 1);
        nya_check(activated == ACTIVATED_OVER, "and takes the click back, got %d", activated);

        // raising only sorts inside a z, so the strip below cannot lift the lower panel past the upper one.
        activated = click_scene(free, 0, 1);
        nya_check(activated == ACTIVATED_NONE, "the strip still holds no widget");
        nya_check(rank_of("over") > rank_of("under"), "and a raise cannot climb out of its z");

        activated = click_scene(overlap, 0, 1);
        nya_check(activated == ACTIVATED_OVER, "so the upper panel keeps the click, got %d", activated);

        // the other way round: the z under it loses however recently it was clicked.
        for (u32 pass = 0; pass < 2; pass++) (void)scene(NYA_UI_PASS_DRAW, 1, 0);
        activated = click_scene(overlap, 1, 0);
        nya_check(activated == ACTIVATED_SHARED, "a z above the panel declared last takes the click, got %d", activated);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
