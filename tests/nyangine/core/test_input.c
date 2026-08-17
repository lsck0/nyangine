/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_input_init();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Initial state - no keys pressed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(!nya_input_key_pressed(NYA_KEY_A), "No key should be pressed initially");
    nya_assert(!nya_input_key_just_pressed(NYA_KEY_A));
    nya_assert(!nya_input_key_just_released(NYA_KEY_A));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Initial state - no mouse buttons pressed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(!nya_input_mouse_button_pressed(1));
    nya_assert(!nya_input_mouse_button_just_pressed(1));
    nya_assert(!nya_input_mouse_button_just_released(1));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Key down sets pressed and just_pressed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event event = {
      .type         = NYA_EVENT_KEY_DOWN,
      .as_key_event = { .key = NYA_KEY_A, .is_down = true },
    };
    nya_system_input_handle_event(&event);

    nya_assert(nya_input_key_pressed(NYA_KEY_A), "Key A should be pressed");
    nya_assert(nya_input_key_just_pressed(NYA_KEY_A), "Key A should be just pressed");
    nya_assert(!nya_input_key_just_released(NYA_KEY_A));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Key up clears pressed, sets just_released
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event event = {
      .type         = NYA_EVENT_KEY_UP,
      .as_key_event = { .key = NYA_KEY_A, .is_down = false },
    };
    nya_system_input_handle_event(&event);

    nya_assert(!nya_input_key_pressed(NYA_KEY_A), "Key A should not be pressed");
    nya_assert(nya_input_key_just_released(NYA_KEY_A), "Key A should be just released");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Update ended clears just_pressed and just_released
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event key_down = {
      .type         = NYA_EVENT_KEY_DOWN,
      .as_key_event = { .key = NYA_KEY_B, .is_down = true },
    };
    nya_system_input_handle_event(&key_down);
    nya_assert(nya_input_key_just_pressed(NYA_KEY_B));

    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    nya_assert(!nya_input_key_just_pressed(NYA_KEY_B), "just_pressed should be cleared");
    nya_assert(nya_input_key_pressed(NYA_KEY_B), "pressed should persist across frames");
    nya_assert(!nya_input_key_just_released(NYA_KEY_A), "just_released should be cleared");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple keys tracked simultaneously
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    NYA_Event key_w = { .type = NYA_EVENT_KEY_DOWN, .as_key_event = { .key = NYA_KEY_W, .is_down = true } };
    NYA_Event key_s = { .type = NYA_EVENT_KEY_DOWN, .as_key_event = { .key = NYA_KEY_S, .is_down = true } };
    nya_system_input_handle_event(&key_w);
    nya_system_input_handle_event(&key_s);

    nya_assert(nya_input_key_pressed(NYA_KEY_W));
    nya_assert(nya_input_key_pressed(NYA_KEY_S));
    nya_assert(!nya_input_key_pressed(NYA_KEY_D), "D should not be pressed");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Holding a key doesn't re-trigger just_pressed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    // W is already held from previous test
    NYA_Event key_w_repeat = {
      .type         = NYA_EVENT_KEY_DOWN,
      .as_key_event = { .key = NYA_KEY_W, .is_down = true, .is_repeat = true },
    };
    nya_system_input_handle_event(&key_w_repeat);

    nya_assert(nya_input_key_pressed(NYA_KEY_W), "W should still be pressed");
    nya_assert(!nya_input_key_just_pressed(NYA_KEY_W), "Repeat should not set just_pressed");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Mouse button down/up
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    NYA_Event mouse_down = {
      .type                  = NYA_EVENT_MOUSE_BUTTON_DOWN,
      .as_mouse_button_event = { .button = 1, .is_down = true, .x = 100.0F, .y = 200.0F },
    };
    nya_system_input_handle_event(&mouse_down);

    nya_assert(nya_input_mouse_button_pressed(1));
    nya_assert(nya_input_mouse_button_just_pressed(1));
    nya_assert(!nya_input_mouse_button_just_released(1));

    NYA_Event mouse_up = {
      .type                  = NYA_EVENT_MOUSE_BUTTON_UP,
      .as_mouse_button_event = { .button = 1, .is_down = false },
    };
    nya_system_input_handle_event(&mouse_up);

    nya_assert(!nya_input_mouse_button_pressed(1), "Mouse button should be released");
    nya_assert(nya_input_mouse_button_just_released(1));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Mouse movement updates position and delta
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    NYA_Event mouse_move = {
      .type                  = NYA_EVENT_MOUSE_MOVED,
      .as_mouse_moved_event  = { .x = 150.0F, .y = 250.0F, .delta_x = 10.0F, .delta_y = 20.0F },
    };
    nya_system_input_handle_event(&mouse_move);

    f32x2 pos = nya_input_mouse_position();
    nya_assert(pos[0] == 150.0F && pos[1] == 250.0F, "Mouse position should be updated");

    f32x2 delta = nya_input_mouse_position_delta();
    nya_assert(delta[0] == 10.0F && delta[1] == 20.0F, "Mouse delta should be updated");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Mouse delta accumulates within a frame
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    NYA_Event move1 = {
      .type                 = NYA_EVENT_MOUSE_MOVED,
      .as_mouse_moved_event = { .x = 100.0F, .y = 100.0F, .delta_x = 5.0F, .delta_y = 3.0F },
    };
    NYA_Event move2 = {
      .type                 = NYA_EVENT_MOUSE_MOVED,
      .as_mouse_moved_event = { .x = 105.0F, .y = 103.0F, .delta_x = 7.0F, .delta_y = 2.0F },
    };
    nya_system_input_handle_event(&move1);
    nya_system_input_handle_event(&move2);

    f32x2 delta = nya_input_mouse_position_delta();
    nya_assert(delta[0] == 12.0F && delta[1] == 5.0F, "Delta should accumulate: 5+7=12, 3+2=5");

    f32x2 pos = nya_input_mouse_position();
    nya_assert(pos[0] == 105.0F && pos[1] == 103.0F, "Position should be the latest value");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Mouse wheel scroll
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    NYA_Event wheel = {
      .type                  = NYA_EVENT_MOUSE_WHEEL_MOVED,
      .as_mouse_wheel_event  = { .amount_x = 0.0F, .amount_y = 3.0F },
    };
    nya_system_input_handle_event(&wheel);

    f32x2 scroll = nya_input_mouse_wheel_scroll();
    nya_assert(scroll[0] == 0.0F && scroll[1] == 3.0F, "Wheel delta should be set");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Mouse wheel accumulates within a frame
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    NYA_Event wheel1 = {
      .type                  = NYA_EVENT_MOUSE_WHEEL_MOVED,
      .as_mouse_wheel_event  = { .amount_x = 1.0F, .amount_y = 2.0F },
    };
    NYA_Event wheel2 = {
      .type                  = NYA_EVENT_MOUSE_WHEEL_MOVED,
      .as_mouse_wheel_event  = { .amount_x = 3.0F, .amount_y = 4.0F },
    };
    nya_system_input_handle_event(&wheel1);
    nya_system_input_handle_event(&wheel2);

    f32x2 scroll = nya_input_mouse_wheel_scroll();
    nya_assert(scroll[0] == 4.0F && scroll[1] == 6.0F, "Wheel should accumulate: 1+3=4, 2+4=6");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Update ended resets mouse deltas
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event mouse_move = {
      .type                 = NYA_EVENT_MOUSE_MOVED,
      .as_mouse_moved_event = { .x = 50.0F, .y = 50.0F, .delta_x = 99.0F, .delta_y = 99.0F },
    };
    nya_system_input_handle_event(&mouse_move);

    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    f32x2 delta = nya_input_mouse_position_delta();
    nya_assert(delta[0] == 0.0F && delta[1] == 0.0F, "Mouse delta should reset after update");

    f32x2 scroll = nya_input_mouse_wheel_scroll();
    nya_assert(scroll[0] == 0.0F && scroll[1] == 0.0F, "Wheel delta should reset after update");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Update ended resets mouse button just_pressed/just_released
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event mouse_down = {
      .type                  = NYA_EVENT_MOUSE_BUTTON_DOWN,
      .as_mouse_button_event = { .button = 2, .is_down = true },
    };
    nya_system_input_handle_event(&mouse_down);
    nya_assert(nya_input_mouse_button_just_pressed(2));

    NYA_Event update_ended = { .type = NYA_EVENT_UPDATING_ENDED };
    _nya_system_event_on_update_ended_hook(&update_ended);

    nya_assert(!nya_input_mouse_button_just_pressed(2), "just_pressed should clear");
    nya_assert(nya_input_mouse_button_pressed(2), "pressed should persist");
  }

  nya_system_input_deinit();
  nya_system_events_deinit();
  nya_system_callback_deinit();
  SDL_Quit();
  _NYA_APP_INSTANCE = (NYA_App){ 0 };

  printf("PASSED: test_input\n");
  return 0;
}
