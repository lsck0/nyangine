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

#include <stdlib.h>

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

  /*
   * Both variables, because the save root is resolved per platform: Linux reads XDG_DATA_HOME and
   * Windows reads APPDATA. Pointed at a scratch directory so the round-trip test does not write into
   * the developer's real ~/.local/share, which is where it would otherwise land — a test that edits
   * the machine it runs on is a test nobody can run twice.
   */
  NYA_Arena*  scratch_arena = nya_arena_create(.name = "test_settings_scratch");
  NYA_String* temp_root     = nullptr;
  NYA_EXPECT(nya_filesystem_temp_directory(scratch_arena, &temp_root));

  NYA_String* save_home = nya_path_join(scratch_arena, nya_string_to_cstring(scratch_arena, temp_root), "nyangine-test-settings");
  NYA_CString save_home_cstring = nya_string_to_cstring(scratch_arena, save_home);

  // Not NYA_EXPECT: on a clean checkout there is nothing there to delete, and NOT_FOUND is the
  // successful outcome of "make sure this is gone".
  (void)nya_filesystem_delete_recursive(save_home_cstring);

  setenv("XDG_DATA_HOME", save_home_cstring, 1);
  setenv("APPDATA", save_home_cstring, 1);

  NYA_EXPECT(nya_system_save_init());
  nya_system_settings_init();
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_input_init();

  defer nya_arena_destroy(scratch_arena);
  defer nya_system_save_deinit();
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: settings round-trip through a file, by name rather than by number
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_settings_reset();

    // The game's actions have to be named before they can be persisted. An unnamed action is skipped
    // on write, which is the behaviour the last assertion in this block checks.
    nya_input_action_name_set(ACTION_JUMP, "jump");
    nya_input_action_name_set(ACTION_FIRE, "fire");

    nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, 0.25F);
    nya_input_action_rebind(ACTION_JUMP, NYA_KEY_SPACE);
    nya_input_action_bind(ACTION_JUMP, NYA_KEY_W);
    nya_input_action_rebind(ACTION_FIRE, NYA_KEY_S, NYA_KEYMOD_CTRL);

    // ACTION_SAVE is deliberately bound and deliberately unnamed.
    nya_input_action_rebind(ACTION_SAVE, NYA_KEY_F5);

    NYA_EXPECT(nya_settings_save());
    nya_assert(nya_save_exists(NYA_SETTINGS_FILE), "saving settings writes the file");

    // Everything back to defaults, so a successful load is the only thing that could restore it.
    nya_settings_reset();
    nya_assert(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) == 1.0F, "reset put the volume back");
    nya_assert(!nya_input_action_bound(ACTION_JUMP), "reset cleared the bindings");

    NYA_EXPECT(nya_settings_load());

    nya_assert(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) == 0.25F, "the volume came back, got %f",
               (f64)nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC));

    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_SPACE, "the primary binding came back");
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_W, "and so did the alternative");

    // The modifier has to survive as well, or a Ctrl+S binding loads as a bare S and fires on every
    // typed letter.
    nya_assert(nya_input_action_get(ACTION_FIRE, 0).key == NYA_KEY_S, "the chorded binding's key came back");
    nya_assert(nya_input_action_get(ACTION_FIRE, 0).modifiers == NYA_KEYMOD_CTRL, "and its modifier came with it");

    nya_assert(!nya_input_action_bound(ACTION_SAVE), "an unnamed action is not persisted");

    // Loading twice must not append duplicates into the second slot, which is what makes
    // nya_settings_from_object replace rather than add.
    NYA_EXPECT(nya_settings_load());
    nya_assert(nya_input_action_get(ACTION_JUMP, 0).key == NYA_KEY_SPACE, "a second load is idempotent");
    nya_assert(nya_input_action_get(ACTION_JUMP, 1).key == NYA_KEY_W, "on both slots");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the settings file is text a human can read and edit
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena* arena = nya_arena_create(.name = "test_settings_readback");
    defer nya_arena_destroy(arena);

    NYA_String* path = nya_save_path(arena, NYA_SETTINGS_FILE);
    nya_assert(path != nullptr, "the save root resolved");

    NYA_String* contents = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(nya_string_to_cstring(arena, path), contents));

    // The point of choosing the native format over anything binary: these are the strings a player
    // would look for if they opened the file to change a key.
    nya_assert(nya_string_contains(contents, "jump"), "the action is named in the file");
    nya_assert(nya_string_contains(contents, "Space"), "and its key is spelled out");
    nya_assert(nya_string_contains(contents, "Ctrl+S"), "chords are one editable token");
    nya_assert(nya_string_contains(contents, "music"), "volume channels are named too");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a save path cannot escape the save root
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena* arena = nya_arena_create(.name = "test_settings_escape");
    defer nya_arena_destroy(arena);

    nya_assert(nya_save_path(arena, "../outside.nya") == nullptr, "a parent segment is refused");
    nya_assert(nya_save_path(arena, "saves/../../outside.nya") == nullptr, "and so is one in the middle");
    nya_assert(nya_save_path(arena, "/etc/passwd") == nullptr, "an absolute path is not relative to anything");

    // A dot in a filename is not a parent segment, and refusing those would be a rule nobody could
    // predict from the outside.
    nya_assert(nya_save_path(arena, "saves/..config.nya") != nullptr, "a leading dot-dot in a name is fine");
    nya_assert(nya_save_path(arena, "saves/slot..nya") != nullptr, "and so is one in the middle of a name");

    printf("  PASSED\n");
  }

  printf("PASSED: test_settings\n");
  return 0;
}
