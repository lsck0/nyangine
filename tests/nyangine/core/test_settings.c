/**
 * Settings, and the input actions that read their bindings out of it.
 *
 * The binding table and the volume mix are plain data, so both are testable without a window. The
 * key *state* behind an action query is not — it comes from platform events — so the tests below
 * drive the input system by handing it NYA_Events directly, which is exactly what the platform
 * layer does.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

enum {
  ACTION_JUMP = NYA_INPUT_ACTION_USER,
  ACTION_FIRE,
  ACTION_SAVE,
};

/** Pretends the platform reported a key going down or up, with the given modifiers held. */
static void feed_key(NYA_Keycode key, b8 down, NYA_KeyModFlag modifiers) {
  NYA_Event event = {
    .type = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP,
    .as_key_event = { .key = key, .is_down = down, .modifier_flags = modifiers },
  };
  nya_system_input_handle_event(&event);
}

/** The end-of-update hook that clears the one-frame edges. */
static void end_frame(void) {
  NYA_Event event = { .type = NYA_EVENT_UPDATING_ENDED };
  _nya_system_event_on_update_ended_hook(&event);
}

s32 main(void) {
  /*
   * The systems this needs, rather than nya_app_init.
   *
   * A full init opens a window and captures the integrity baseline, neither of which a headless
   * test can do — the same reason test_input and test_event build up by hand. Settings depends on
   * nothing and input needs the event and callback systems for its end-of-update hook.
   */
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_settings_init();
  nya_system_callback_init();
  nya_system_events_init();
  nya_system_input_init();

  defer nya_system_input_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();
  defer nya_system_settings_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: volumes start at full and clamp
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: volumes\n");
  {
    for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) {
      nya_assert(nya_settings_volume(channel) == 1.0F, "channel %u did not start at 1.0", channel);
    }

    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, 0.25F);
    nya_assert(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) == 0.25F);

    // Out of range is clamped rather than trusted, so a dragged slider cannot hand the mixer a
    // negative or a value above unity.
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_SOUND, 2.5F);
    nya_assert(nya_settings_volume(NYA_VOLUME_CHANNEL_SOUND) == 1.0F);
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_SOUND, -3.0F);
    nya_assert(nya_settings_volume(NYA_VOLUME_CHANNEL_SOUND) == 0.0F);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: master scales the others, and does not scale itself
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: effective volume\n");
  {
    nya_settings_reset();
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MASTER, 0.5F);
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, 0.5F);

    nya_assert(nya_settings_volume_effective(NYA_VOLUME_CHANNEL_MUSIC) == 0.25F);

    // Master through the effective path is master itself, not master squared.
    nya_assert(nya_settings_volume_effective(NYA_VOLUME_CHANNEL_MASTER) == 0.5F);

    // A muted master silences everything without disturbing the per channel levels.
    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MASTER, 0.0F);
    nya_assert(nya_settings_volume_effective(NYA_VOLUME_CHANNEL_MUSIC) == 0.0F);
    nya_assert(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) == 0.5F);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: binding, rebinding and unbinding
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: binding\n");
  {
    nya_settings_reset();

    nya_assert(nya_input_action_bound(ACTION_JUMP) == false);

    nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);
    nya_assert(nya_input_action_bound(ACTION_JUMP) == true);

    // A second key is an alternative, not a replacement.
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_W);
    nya_assert(nya_settings()->bindings[ACTION_JUMP][0].key == NYA_KEY_SPACE);
    nya_assert(nya_settings()->bindings[ACTION_JUMP][1].key == NYA_KEY_W);

    // Binding a key already on the action updates its modifiers in place rather than consuming
    // the other slot.
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE, NYA_KEYMOD_SHIFT);
    nya_assert(nya_settings()->bindings[ACTION_JUMP][0].modifiers == NYA_KEYMOD_SHIFT);
    nya_assert(nya_settings()->bindings[ACTION_JUMP][1].key == NYA_KEY_W);

    nya_input_action_unbind(ACTION_JUMP);
    nya_assert(nya_input_action_bound(ACTION_JUMP) == false);

    // The unbound action is not a binding target.
    nya_expect_crash(nya_input_action_bind(NYA_INPUT_ACTION_NONE, NYA_KEY_A));
    nya_expect_crash((void)nya_input_action_pressed(NYA_INPUT_ACTION_MAX));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: rebinding replaces, the way a settings screen expects
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: rebinding\n");
  {
    nya_settings_reset();

    // bind accumulates: this is the primary and the secondary.
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_W);
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_SPACE);
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_W);

    // rebind replaces the lot, which is what "press a key for Jump" means.
    nya_input_action_rebind(ACTION_JUMP, NYA_KEY_UP);
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_UP);
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_UNKNOWN, "rebind left the old secondary behind");

    // Calling it repeatedly, as a live rebinding menu would, never accumulates.
    for (u32 i = 0; i < 5; i++) nya_input_action_rebind(ACTION_JUMP, NYA_KEY_DOWN);
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_DOWN);
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_UNKNOWN);

    // Per slot, for a menu with a primary and a secondary column.
    nya_input_action_set(ACTION_JUMP, 1, NYA_KEY_SPACE, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_DOWN);
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_SPACE);

    nya_input_action_set(ACTION_JUMP, 0, NYA_KEY_UNKNOWN, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_UNKNOWN);
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_SPACE, "clearing one slot disturbed the other");

    // Clearing a slot must not leave its modifiers behind for the next binding to inherit.
    nya_input_action_set(ACTION_JUMP, 0, NYA_KEY_A, NYA_KEYMOD_CTRL);
    nya_input_action_set(ACTION_JUMP, 0, NYA_KEY_UNKNOWN, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).modifiers == NYA_KEYMOD_NONE);

    nya_expect_crash(nya_input_action_set(ACTION_JUMP, NYA_INPUT_BINDINGS_PER_ACTION, NYA_KEY_A, NYA_KEYMOD_NONE));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: pressed / just_pressed / just_released over a frame
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: action edges\n");
  {
    nya_settings_reset();
    end_frame();
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);

    nya_assert(nya_input_action_pressed(ACTION_JUMP) == false);

    feed_key(NYA_KEY_SPACE, true, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_just_pressed(ACTION_JUMP) == true);
    nya_assert(nya_input_action_pressed(ACTION_JUMP) == true);
    nya_assert(nya_input_action_just_released(ACTION_JUMP) == false);

    // just_pressed is an edge: it lasts exactly one frame, held is what persists.
    end_frame();
    nya_assert(nya_input_action_just_pressed(ACTION_JUMP) == false);
    nya_assert(nya_input_action_pressed(ACTION_JUMP) == true);

    feed_key(NYA_KEY_SPACE, false, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_just_released(ACTION_JUMP) == true);
    nya_assert(nya_input_action_pressed(ACTION_JUMP) == false);

    end_frame();
    nya_assert(nya_input_action_just_released(ACTION_JUMP) == false);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a chord needs its modifier
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: chords\n");
  {
    nya_settings_reset();
    end_frame();
    nya_input_action_bind(ACTION_SAVE, NYA_KEY_S, NYA_KEYMOD_CTRL);

    // The key alone is not the chord.
    feed_key(NYA_KEY_S, true, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == false);
    feed_key(NYA_KEY_S, false, NYA_KEYMOD_NONE);
    end_frame();

    // With Ctrl held it fires, from either side of the keyboard.
    feed_key(NYA_KEY_S, true, NYA_KEYMOD_LCTRL);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == true);
    feed_key(NYA_KEY_S, false, NYA_KEYMOD_LCTRL);
    end_frame();

    feed_key(NYA_KEY_S, true, NYA_KEYMOD_RCTRL);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == true);
    feed_key(NYA_KEY_S, false, NYA_KEYMOD_RCTRL);
    end_frame();
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an unwanted modifier suppresses a plain binding
  //
  // This is the reason the match is exact rather than "at least these": otherwise a bare W would
  // fire in the middle of typing Ctrl+W.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: extra modifiers suppress\n");
  {
    nya_settings_reset();
    end_frame();
    nya_input_action_bind(ACTION_FIRE, NYA_KEY_W);
    nya_input_action_bind(ACTION_SAVE, NYA_KEY_W, NYA_KEYMOD_CTRL);

    // Ctrl+W is the chord, and only the chord.
    feed_key(NYA_KEY_W, true, NYA_KEYMOD_LCTRL);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == true);
    nya_assert(nya_input_action_just_pressed(ACTION_FIRE) == false, "a bare binding fired while a modifier was held");
    feed_key(NYA_KEY_W, false, NYA_KEYMOD_LCTRL);
    end_frame();

    // W on its own is the plain one, and only that.
    feed_key(NYA_KEY_W, true, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_just_pressed(ACTION_FIRE) == true);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == false);
    feed_key(NYA_KEY_W, false, NYA_KEYMOD_NONE);
    end_frame();

    // A modifier the binding did not ask for is still an extra, even alongside one it did.
    feed_key(NYA_KEY_W, true, NYA_KEYMOD_LCTRL | NYA_KEYMOD_LSHIFT);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == false, "Ctrl+Shift+W matched a Ctrl+W binding");
    feed_key(NYA_KEY_W, false, NYA_KEYMOD_NONE);
    end_frame();
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: lock keys are keyboard state, not part of a chord
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: lock keys ignored\n");
  {
    nya_settings_reset();
    end_frame();
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);

    // Caps Lock being on must not stop the game responding to Space.
    feed_key(NYA_KEY_SPACE, true, NYA_KEYMOD_CAPS | NYA_KEYMOD_NUM);
    nya_assert(nya_input_action_just_pressed(ACTION_JUMP) == true, "a lock key suppressed a plain binding");
    feed_key(NYA_KEY_SPACE, false, NYA_KEYMOD_CAPS);
    end_frame();
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: release fires even when the modifier was let go first
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: release ignores modifiers\n");
  {
    nya_settings_reset();
    end_frame();
    nya_input_action_bind(ACTION_SAVE, NYA_KEY_S, NYA_KEYMOD_CTRL);

    feed_key(NYA_KEY_S, true, NYA_KEYMOD_LCTRL);
    nya_assert(nya_input_action_just_pressed(ACTION_SAVE) == true);
    end_frame();

    // Ctrl comes up first, which is how people actually end a chord.
    feed_key(NYA_KEY_S, false, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_just_released(ACTION_SAVE) == true, "the release was lost because the modifier went first");
    end_frame();
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: either binding satisfies the action
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: alternative bindings\n");
  {
    nya_settings_reset();
    end_frame();
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_W);

    feed_key(NYA_KEY_W, true, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_pressed(ACTION_JUMP) == true, "the secondary binding did not fire");
    feed_key(NYA_KEY_W, false, NYA_KEYMOD_NONE);
    end_frame();

    feed_key(NYA_KEY_SPACE, true, NYA_KEYMOD_NONE);
    nya_assert(nya_input_action_pressed(ACTION_JUMP) == true, "the primary binding did not fire");
    feed_key(NYA_KEY_SPACE, false, NYA_KEYMOD_NONE);
    end_frame();
    printf("  PASSED\n");
  }

  printf("PASSED: test_settings\n");
  return 0;
}
