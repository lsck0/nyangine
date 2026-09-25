/**
 * The context menu, headless: a right click inside the trigger opens a popup of actions where the pointer is,
 * picking one reports its index and closes the menu, a separator takes no click, and a press outside it, escape,
 * or the left-click variant all behave. Runs on the offscreen driver, so there is no display; the menu is a
 * float like a dropdown's list and is driven the same way, laying out over two draw passes before it is clicked.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define FACE "./assets/fonts/Aldrich.ttf"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/** The test style's frame, matching the widgets test: padding 10, gap 6, item height 40, inside the margin. */
#define FRAME 10.0F
#define GAP   6.0F
#define ITEM  40.0F

/* items[2] is null: a separator between the two groups. */
static NYA_ConstCString ITEMS[4] = { "Cut", "Copy", nullptr, "Delete" };

/** The trigger is the whole window, so a click anywhere opens the menu. */
static const NYA_Rectf TRIGGER = { 0.0F, 0.0F, 800.0F, 600.0F };

/** Where a right click opens the menu. Far enough from the edges that the float is not pushed to fit. */
static const f32x2 OPEN_AT = { 200.0F, 150.0F };

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&event);
}

static void button_at(NYA_MouseButton button, b8 down, f32x2 at) {
    NYA_Event event = { .type = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = down, .button = button, .x = at.x, .y = at.y } };
    nya_system_input_handle_event(&event);
}

/** A full press and release of `button` at `point`, the way a click of that button arrives. */
static void click(NYA_MouseButton button, f32x2 point) {
    pointer_move(point);
    button_at(button, true, point);
    button_at(button, false, point);
}

static void tap(NYA_Keycode keycode) {
    for (u32 i = 0; i < 2; i++) {
        NYA_Event event = { .type = i == 0 ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP, .as_key_event = { .is_down = i == 0, .key = keycode } };
        nya_system_input_handle_event(&event);
    }
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

/** The centre of item `index`'s row inside the float opened at OPEN_AT: padding, then a row per item and a gap. */
static f32x2 item_center(u32 index) {
    f32 y = OPEN_AT.y + FRAME;
    for (u32 i = 0; i < index; i++) {
        y += (ITEMS[i] == nullptr || ITEMS[i][0] == '\0') ? NYA_UI_SPACING : ITEM;
        y += GAP;
    }
    f32 own = (ITEMS[index] == nullptr || ITEMS[index][0] == '\0') ? NYA_UI_SPACING : ITEM;

    // near the left edge, so the point is inside the entry whatever its width came out to.
    return (f32x2){ OPEN_AT.x + FRAME + 8.0F, y + (own * 0.5F) };
}

/** One pass over a context menu named `id` opened by `button`; returns the item it reported this pass. */
static u32 run(NYA_UIPass pass, NYA_ConstCString id, NYA_MouseButton button) {
    NYA_UI* ui     = nya_ui_begin(&window, pass);
    u32     picked = nya_ui_context_menu(ui, id, TRIGGER, button, ITEMS, 4);
    nya_ui_end(ui);

    if (pass == NYA_UI_PASS_INPUT) tick();
    return picked;
}

/** Lays the open menu out over two draw passes, so the next input pass hits real item rectangles. */
static void settle(NYA_ConstCString id, NYA_MouseButton button) {
    for (u32 i = 0; i < 2; i++) (void)run(NYA_UI_PASS_DRAW, id, button);
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

    nya_font_default_set(nya_font(FACE, 20.0F));
    for (u32 i = 0; i < 32 && nya_font_metrics(NYA_FONT_NONE).line_height <= 0.0F; i++) nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
    nya_check(nya_font_metrics(NYA_FONT_NONE).line_height > 0.0F, "the face loads");

    nya_ui_style_set(&window, (NYA_UIStyle){ .body_size = 20.0F, .padding = FRAME, .spacing = GAP, .item_height = ITEM });

    // A right click opens the menu, and clicking an item reports its index and closes the menu.
    {
        // closed to begin with: a pass with nothing pressed picks nothing.
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "a closed menu picks nothing");

        // a left click while closed does nothing either: this menu opens on the right button.
        click(NYA_MOUSE_BUTTON_LEFT, OPEN_AT);
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "a left click does not open a right-click menu");

        // the right click opens it where the pointer is.
        click(NYA_MOUSE_BUTTON_RIGHT, OPEN_AT);
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "opening the menu picks nothing on its own");

        settle("edit", NYA_MOUSE_BUTTON_RIGHT);

        // a left click on the second item reports its index.
        click(NYA_MOUSE_BUTTON_LEFT, item_center(1));
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == 1, "clicking an item reports its index");

        // and the pick closed it: a click where that item was reports nothing now.
        settle("edit", NYA_MOUSE_BUTTON_RIGHT);
        click(NYA_MOUSE_BUTTON_LEFT, item_center(1));
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "picking an item closes the menu");
    }

    // A separator takes no click: the menu stays open and an item under it is still reachable.
    {
        click(NYA_MOUSE_BUTTON_RIGHT, OPEN_AT);
        (void)run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT);
        settle("edit", NYA_MOUSE_BUTTON_RIGHT);

        // item 2 is the separator; a click on it picks nothing and does not dismiss the menu.
        click(NYA_MOUSE_BUTTON_LEFT, item_center(2));
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "a separator reports nothing");

        settle("edit", NYA_MOUSE_BUTTON_RIGHT);
        click(NYA_MOUSE_BUTTON_LEFT, item_center(0));
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == 0, "the menu was still open, so the first item is picked");
    }

    // A press outside the menu dismisses it without picking.
    {
        click(NYA_MOUSE_BUTTON_RIGHT, OPEN_AT);
        (void)run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT);
        settle("edit", NYA_MOUSE_BUTTON_RIGHT);

        click(NYA_MOUSE_BUTTON_LEFT, (f32x2){ 700.0F, 520.0F });
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "a click outside the menu picks nothing");

        // and it is closed: the point the first item was at reports nothing.
        settle("edit", NYA_MOUSE_BUTTON_RIGHT);
        click(NYA_MOUSE_BUTTON_LEFT, item_center(0));
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "a click outside dismissed the menu");
    }

    // Escape dismisses it, the one step the module takes before a layer sees the cancel.
    {
        click(NYA_MOUSE_BUTTON_RIGHT, OPEN_AT);
        (void)run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT);
        settle("edit", NYA_MOUSE_BUTTON_RIGHT);

        tap(NYA_KEY_ESCAPE);
        (void)run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT);

        settle("edit", NYA_MOUSE_BUTTON_RIGHT);
        click(NYA_MOUSE_BUTTON_LEFT, item_center(0));
        nya_check(run(NYA_UI_PASS_INPUT, "edit", NYA_MOUSE_BUTTON_RIGHT) == NYA_UI_MENU_NONE, "escape dismissed the menu");
    }

    // The left-click variant opens on a left click, and selecting an item still works over the trigger.
    {
        click(NYA_MOUSE_BUTTON_LEFT, OPEN_AT);
        nya_check(run(NYA_UI_PASS_INPUT, "left", NYA_MOUSE_BUTTON_LEFT) == NYA_UI_MENU_NONE, "the opening left click picks nothing");

        settle("left", NYA_MOUSE_BUTTON_LEFT);

        // the trigger is the whole window, so this click is over it; the open menu must take it rather than reopen.
        click(NYA_MOUSE_BUTTON_LEFT, item_center(0));
        nya_check(run(NYA_UI_PASS_INPUT, "left", NYA_MOUSE_BUTTON_LEFT) == 0, "a left-click menu selects an item over its own trigger");
    }

    nya_log_info("PASSED: test_ui_context_menu");

    return nya_check_failures() == 0 ? 0 : 1;
}
