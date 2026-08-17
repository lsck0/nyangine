/**
 * Per-device input routing: several people on one machine.
 *
 * Two things are being defended here.
 *
 * The first is that adding players changed nothing for a game that has none. The merged view is fed
 * by every device unconditionally, so `nya_input_key_pressed` answers exactly what it always did —
 * including after a device has been assigned to a player, which is the case a naive "route the event
 * to the player instead" implementation breaks, silently, in every menu.
 *
 * The second is that two players do not leak into each other: player 1's keys, mouse buttons,
 * modifiers and action chords must be player 1's alone, and an unclaimed slot must read as nothing
 * held rather than as whatever the last claimant left behind.
 *
 * Events are synthesised rather than pumped from SDL. That is the point — SDL only fills its device
 * id in where the platform supports it, so a test driven by real events would be testing the
 * platform, not the routing.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define PLAYER_ONE 0
#define PLAYER_TWO 1

/** Whatever a game's own action would be. Anything at or past NYA_INPUT_ACTION_USER works. */
#define ACTION_FIRE (NYA_INPUT_ACTION_USER + 0)

static NYA_InputSource keyboard(u32 id) {
  return (NYA_InputSource){ .kind = NYA_INPUT_DEVICE_KIND_KEYBOARD, .id = id };
}

static NYA_InputSource mouse(u32 id) {
  return (NYA_InputSource){ .kind = NYA_INPUT_DEVICE_KIND_MOUSE, .id = id };
}

static void press(NYA_InputSource source, NYA_Keycode key, NYA_KeyModFlag modifiers) {
  NYA_Event event = {
    .type = NYA_EVENT_KEY_DOWN,
    .as_key_event = { .source = source, .is_down = true, .key = key, .modifier_flags = modifiers },
  };

  nya_system_input_handle_event(&event);
}

static void release(NYA_InputSource source, NYA_Keycode key) {
  NYA_Event event = {
    .type = NYA_EVENT_KEY_UP,
    .as_key_event = { .source = source, .is_down = false, .key = key },
  };

  nya_system_input_handle_event(&event);
}

static void click(NYA_InputSource source, NYA_MouseButton button, b8 is_down) {
  NYA_Event event = {
    .type = is_down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP,
    .as_mouse_button_event = { .source = source, .is_down = is_down, .button = button },
  };

  nya_system_input_handle_event(&event);
}

static void move_mouse(NYA_InputSource source, f32 x, f32 y) {
  NYA_Event event = {
    .type = NYA_EVENT_MOUSE_MOVED,
    .as_mouse_moved_event = { .source = source, .x = x, .y = y, .delta_x = 1.0F, .delta_y = 2.0F },
  };

  nya_system_input_handle_event(&event);
}

/** What the app loop does at the end of a tick: drop the edges, keep what is held. */
static void end_frame(void) {
  NYA_Event event = { .type = NYA_EVENT_UPDATING_ENDED };
  _nya_system_event_on_update_ended_hook(&event);
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  // The input system registers an NYA_EVENT_UPDATING_ENDED hook, so the event system has to be up
  // before it rather than after.
  NYA_EXPECT(nya_system_events_init());
  defer nya_system_events_deinit();

  // Bindings live on the settings, which the input system reads through. See _nya_input_bindings_for.
  nya_system_settings_init();
  defer nya_system_settings_deinit();

  nya_system_input_init();
  defer nya_system_input_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an untouched system has no devices and no players
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_input_source_count() == 0);
    nya_assert(nya_input_source_last().kind == NYA_INPUT_DEVICE_KIND_NONE);
    nya_assert(nya_input_source_at(0).kind == NYA_INPUT_DEVICE_KIND_NONE, "past the end is NONE rather than garbage");

    // An unclaimed slot reads as nothing held, which is what lets a loop over the slots run without
    // a guard around every query.
    nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A));
    nya_assert(nya_input_modifiers_by(PLAYER_ONE) == NYA_KEYMOD_NONE);
    nya_assert(!nya_input_mouse_button_pressed_by(PLAYER_ONE, NYA_MOUSE_BUTTON_LEFT));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a device joins the roster the first time it produces an event
  // ─────────────────────────────────────────────────────────────────────────────
  {
    press(keyboard(1), NYA_KEY_A, NYA_KEYMOD_NONE);

    nya_assert(nya_input_source_count() == 1, "the first event from a device puts it on the roster");
    nya_assert(nya_input_source_at(0).kind == NYA_INPUT_DEVICE_KIND_KEYBOARD);
    nya_assert(nya_input_source_at(0).id == 1);

    NYA_InputSource last = nya_input_source_last();
    nya_assert(last.kind == NYA_INPUT_DEVICE_KIND_KEYBOARD && last.id == 1, "a join screen can name who just pressed something");

    // Seen, but nobody has claimed it. That distinction is the whole join flow.
    nya_assert(nya_input_source_player(keyboard(1)) == NYA_INPUT_PLAYER_NONE);
    nya_assert(nya_input_source_player(keyboard(9)) == NYA_INPUT_PLAYER_NONE, "a device never seen is unclaimed too");

    // Same id, different kind, is a different device — an id is only unique within a kind.
    press(mouse(1), NYA_KEY_UNKNOWN, NYA_KEYMOD_NONE);
    click(mouse(1), NYA_MOUSE_BUTTON_LEFT, true);
    nya_assert(nya_input_source_count() == 2, "keyboard 1 and mouse 1 are two devices");

    release(keyboard(1), NYA_KEY_A);
    click(mouse(1), NYA_MOUSE_BUTTON_LEFT, false);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the merged view still sees everything, whoever the device belongs to
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_input_source_assign(keyboard(1), PLAYER_ONE);
    nya_assert(nya_input_source_player(keyboard(1)) == PLAYER_ONE);

    press(keyboard(1), NYA_KEY_A, NYA_KEYMOD_NONE);

    // The single-player API, unchanged. This is the regression that would make every pause menu in
    // every game stop responding the moment a second player was added.
    nya_assert(nya_input_key_pressed(NYA_KEY_A), "a routed event still reaches the merged view");
    nya_assert(nya_input_key_just_pressed(NYA_KEY_A));
    nya_assert(nya_input_key_pressed_by(NYA_INPUT_PLAYER_ANY, NYA_KEY_A), "PLAYER_ANY is the merged view");
    nya_assert(nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A), "and the player who owns the device sees it too");

    release(keyboard(1), NYA_KEY_A);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: two players do not see each other's keys
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_input_source_assign(keyboard(2), PLAYER_TWO);

    press(keyboard(1), NYA_KEY_A, NYA_KEYMOD_NONE);
    press(keyboard(2), NYA_KEY_B, NYA_KEYMOD_NONE);

    nya_assert(nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A));
    nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_B), "player one does not see player two's key");

    nya_assert(nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_B));
    nya_assert(!nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_A));

    // Both, in the merged view, because that is what merged means.
    nya_assert(nya_input_key_pressed(NYA_KEY_A) && nya_input_key_pressed(NYA_KEY_B));

    // Releasing one player's key leaves the other's held.
    release(keyboard(1), NYA_KEY_A);

    nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A));
    nya_assert(nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_B), "one player letting go does not release the other");

    release(keyboard(2), NYA_KEY_B);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an unassigned device feeds only the merged view
  // ─────────────────────────────────────────────────────────────────────────────
  {
    press(keyboard(3), NYA_KEY_C, NYA_KEYMOD_NONE);

    nya_assert(nya_input_key_pressed(NYA_KEY_C), "a stranger's keys still work the single-player way");
    nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_C));
    nya_assert(!nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_C));

    release(keyboard(3), NYA_KEY_C);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: modifiers are per player, so one player's shift is not another's chord
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_input_action_bind(ACTION_FIRE, NYA_KEY_F, NYA_KEYMOD_CTRL);

    // Player two holds ctrl. Player one presses F with no modifier at all.
    press(keyboard(2), NYA_KEY_LCTRL, NYA_KEYMOD_LCTRL);
    press(keyboard(1), NYA_KEY_F, NYA_KEYMOD_NONE);

    nya_assert(nya_input_modifiers_by(PLAYER_TWO) == NYA_KEYMOD_LCTRL);
    nya_assert(nya_input_modifiers_by(PLAYER_ONE) == NYA_KEYMOD_NONE);

    /*
     * The whole reason modifiers are stored per player.
     *
     * Reading the merged modifier set here would have player two's ctrl completing player one's
     * chord — a player firing because somebody else on the couch was holding a key.
     */
    nya_assert(!nya_input_action_pressed_by(PLAYER_ONE, ACTION_FIRE), "player two's ctrl does not complete player one's chord");

    // And with their own ctrl down it fires.
    press(keyboard(1), NYA_KEY_LCTRL, NYA_KEYMOD_LCTRL);
    press(keyboard(1), NYA_KEY_F, NYA_KEYMOD_LCTRL);
    nya_assert(nya_input_action_pressed_by(PLAYER_ONE, ACTION_FIRE));

    release(keyboard(1), NYA_KEY_F);
    release(keyboard(1), NYA_KEY_LCTRL);
    release(keyboard(2), NYA_KEY_LCTRL);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: mouse state is per player too
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_input_source_assign(mouse(1), PLAYER_ONE);
    nya_input_source_assign(mouse(2), PLAYER_TWO);

    move_mouse(mouse(1), 100.0F, 200.0F);
    move_mouse(mouse(2), 300.0F, 400.0F);

    f32x2 one = nya_input_mouse_position_by(PLAYER_ONE);
    f32x2 two = nya_input_mouse_position_by(PLAYER_TWO);

    nya_assert(one.x == 100.0F && one.y == 200.0F);
    nya_assert(two.x == 300.0F && two.y == 400.0F, "two pointers, two positions");

    // The merged view holds whichever moved last, which is the right answer for a single cursor.
    f32x2 merged = nya_input_mouse_position();
    nya_assert(merged.x == 300.0F && merged.y == 400.0F);

    click(mouse(1), NYA_MOUSE_BUTTON_LEFT, true);

    nya_assert(nya_input_mouse_button_pressed_by(PLAYER_ONE, NYA_MOUSE_BUTTON_LEFT));
    nya_assert(!nya_input_mouse_button_pressed_by(PLAYER_TWO, NYA_MOUSE_BUTTON_LEFT));
    nya_assert(nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_LEFT));

    click(mouse(1), NYA_MOUSE_BUTTON_LEFT, false);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an out of range mouse button is ignored rather than written past the table
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A gaming mouse reports button numbers well past the five every mouse has. The three button
    // tables sit next to each other in NYA_InputState, so an unbounded write lands in the next one.
    click(mouse(1), (NYA_MouseButton)(NYA_MOUSE_BUTTON_COUNT + 4), true);

    nya_assert(!nya_input_mouse_button_pressed_by(PLAYER_ONE, NYA_MOUSE_BUTTON_LEFT), "a side button does not become a left click");
    nya_assert(!nya_input_mouse_button_pressed((NYA_MouseButton)(NYA_MOUSE_BUTTON_COUNT + 4)), "and querying it is refused rather than read out of bounds");

    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: releasing a device, and resetting every player
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_input_source_release(keyboard(1));
    nya_assert(nya_input_source_player(keyboard(1)) == NYA_INPUT_PLAYER_NONE);

    press(keyboard(1), NYA_KEY_A, NYA_KEYMOD_NONE);
    nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A), "a released device stops feeding its old slot");
    nya_assert(nya_input_key_pressed(NYA_KEY_A), "and keeps feeding the merged view");
    release(keyboard(1), NYA_KEY_A);
    end_frame();

    /*
     * A reset must tear the slots down, not merely unroute them.
     *
     * Player two is holding B when the lobby is returned to. If the state survived, the next person
     * assigned to slot two would start already holding it — walking left the moment they joined.
     */
    press(keyboard(2), NYA_KEY_B, NYA_KEYMOD_NONE);
    nya_assert(nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_B));

    nya_input_players_reset();

    nya_assert(nya_input_source_player(keyboard(2)) == NYA_INPUT_PLAYER_NONE, "every device is unclaimed");
    nya_assert(nya_input_source_count() > 0, "but the devices are still plugged in, so the roster survives");

    nya_input_source_assign(keyboard(4), PLAYER_TWO);
    nya_assert(!nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_B), "a reused slot does not inherit the last player's held keys");

    release(keyboard(2), NYA_KEY_B);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the roster is bounded, and a device past the cap still works
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Well past NYA_INPUT_MAX_SOURCES. Nothing may be evicted: doing so would unassign a player
    // mid-game over nothing more than somebody plugging in another device.
    for (u32 i = 0; i < NYA_INPUT_MAX_SOURCES + 8; i++) {
      press(keyboard(100 + i), NYA_KEY_Z, NYA_KEYMOD_NONE);
      release(keyboard(100 + i), NYA_KEY_Z);
    }

    nya_assert(nya_input_source_count() == NYA_INPUT_MAX_SOURCES, "the roster stops growing rather than overrunning");

    end_frame();

    // A device past the cap is unclaimed and unclaimable, but its input is not lost.
    press(keyboard(9999), NYA_KEY_Y, NYA_KEYMOD_NONE);
    nya_assert(nya_input_key_pressed(NYA_KEY_Y), "a device past the cap still drives the merged view");
    release(keyboard(9999), NYA_KEY_Y);
    end_frame();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: many lobby cycles, which is where the state used to accumulate
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Claim, use, reset, repeat.
     *
     * Player states were arena-allocated pointers, and an arena has no per-allocation free — so a
     * reset destroyed a slot's key tables and left the struct behind, and the next claim allocated
     * another. Every trip back to the lobby grew the input arena a little more, forever.
     *
     * The states are inline now, so there is nothing to accumulate. What this checks is the
     * behaviour that would have degraded: a slot claimed for the fiftieth time works exactly like
     * one claimed for the first, and carries nothing over from the last occupant.
     */
    for (u32 cycle = 0; cycle < 50; cycle++) {
      nya_input_source_assign(keyboard(1), PLAYER_ONE);
      nya_input_source_assign(keyboard(2), PLAYER_TWO);

      press(keyboard(1), NYA_KEY_A, NYA_KEYMOD_NONE);
      press(keyboard(2), NYA_KEY_B, NYA_KEYMOD_NONE);

      nya_assert(nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A), "cycle %u: player one's key", cycle);
      nya_assert(nya_input_key_pressed_by(PLAYER_TWO, NYA_KEY_B), "cycle %u: player two's key", cycle);
      nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_B), "cycle %u: still no crosstalk", cycle);

      // Deliberately without releasing: a player who leaves mid-press is the case that would leave
      // a key stuck down in a slot somebody else is about to be given.
      nya_input_players_reset();

      nya_assert(!nya_input_key_pressed_by(PLAYER_ONE, NYA_KEY_A), "cycle %u: a reset slot holds nothing", cycle);
    }

    // And the merged view is untouched by all of it — reset is about players, not about devices.
    nya_assert(nya_input_key_pressed(NYA_KEY_A), "the merged view still holds what was never released");

    release(keyboard(1), NYA_KEY_A);
    release(keyboard(2), NYA_KEY_B);
    end_frame();
  }

  nya_info("PASSED: test_input_sources (0 failures)");

  return EXIT_SUCCESS;
}
