/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static volatile s32        deferred_hook_count  = 0;
static volatile s32        immediate_hook_count = 0;
static volatile NYA_EventType last_hook_event_type = NYA_EVENT_INVALID;

void test_deferred_hook(NYA_Event* event) {
  deferred_hook_count++;
  last_hook_event_type = event->type;
}

void test_immediate_hook(NYA_Event* event) {
  immediate_hook_count++;
  last_hook_event_type = event->type;
}

void test_handled_hook(NYA_Event* event) {
  event->was_handled = true;
}

b8 test_condition_true(NYA_Event* event) {
  nya_unused(event);
  return true;
}

b8 test_condition_false(NYA_Event* event) {
  nya_unused(event);
  return false;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Dispatch and poll an event
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_QUIT });

    NYA_Event polled;
    b8 ok = nya_system_event_poll(&polled);
    nya_assert(ok, "Should poll an event");
    nya_assert(polled.type == NYA_EVENT_QUIT, "Should be QUIT event");
    nya_assert(polled.timestamp > 0, "Should have a timestamp");

    ok = nya_system_event_poll(&polled);
    nya_assert(!ok, "Queue should be empty");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple events dispatched and polled in order
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEY_DOWN });
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEY_UP });
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_MOUSE_MOVED });

    NYA_Event e;
    b8 ok;

    ok = nya_system_event_poll(&e);
    nya_assert(ok && e.type == NYA_EVENT_KEY_DOWN);

    ok = nya_system_event_poll(&e);
    nya_assert(ok && e.type == NYA_EVENT_KEY_UP);

    ok = nya_system_event_poll(&e);
    nya_assert(ok && e.type == NYA_EVENT_MOUSE_MOVED);

    ok = nya_system_event_poll(&e);
    nya_assert(!ok, "Queue should be empty");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Event data preserved through dispatch and poll
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Event dispatched = {
      .type         = NYA_EVENT_KEY_DOWN,
      .as_key_event = {
        .key       = NYA_KEY_A,
        .is_down   = true,
        .is_repeat = false,
      },
    };
    nya_event_dispatch(dispatched);

    NYA_Event polled;
    b8 ok = nya_system_event_poll(&polled);
    nya_assert(ok);
    nya_assert(polled.type == NYA_EVENT_KEY_DOWN);
    nya_assert(polled.as_key_event.key == NYA_KEY_A);
    nya_assert(polled.as_key_event.is_down == true);
    nya_assert(polled.as_key_event.is_repeat == false);

    while (nya_system_event_poll(&polled)) {}
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Immediate hooks fire on dispatch
  // ─────────────────────────────────────────────────────────────────────────────
  {
    immediate_hook_count = 0;
    last_hook_event_type = NYA_EVENT_INVALID;

    NYA_EventHook hook = {
      .event_type = NYA_EVENT_CLIPBOARD_UPDATE,
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .fn         = nya_callback(test_immediate_hook),
    };
    nya_event_hook_register(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_CLIPBOARD_UPDATE });
    nya_assert(immediate_hook_count == 1, "Immediate hook should fire on dispatch");
    nya_assert(last_hook_event_type == NYA_EVENT_CLIPBOARD_UPDATE);

    NYA_Event e;
    while (nya_system_event_poll(&e)) {}

    nya_event_hook_unregister(hook);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Deferred hooks fire on poll
  // ─────────────────────────────────────────────────────────────────────────────
  {
    deferred_hook_count  = 0;
    last_hook_event_type = NYA_EVENT_INVALID;

    NYA_EventHook hook = {
      .event_type = NYA_EVENT_KEYMAP_CHANGED,
      .hook_type  = NYA_EVENT_HOOK_TYPE_DEFERRED,
      .fn         = nya_callback(test_deferred_hook),
    };
    nya_event_hook_register(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEYMAP_CHANGED });
    nya_assert(deferred_hook_count == 0, "Deferred hook should not fire on dispatch");

    NYA_Event e;
    b8 ok = nya_system_event_poll(&e);
    nya_assert(ok);
    nya_assert(deferred_hook_count == 1, "Deferred hook should fire on poll");

    while (nya_system_event_poll(&e)) {}

    nya_event_hook_unregister(hook);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: One-shot hook fires once then is removed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    deferred_hook_count = 0;

    NYA_EventHook hook = {
      .event_type = NYA_EVENT_KEYMAP_CHANGED,
      .hook_type  = NYA_EVENT_HOOK_TYPE_DEFERRED,
      .fn         = nya_callback(test_deferred_hook),
      .one_shot   = true,
    };
    nya_event_hook_register(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEYMAP_CHANGED });
    NYA_Event e;
    while (nya_system_event_poll(&e)) {}
    nya_assert(deferred_hook_count == 1, "One-shot hook should fire once");

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEYMAP_CHANGED });
    while (nya_system_event_poll(&e)) {}
    nya_assert(deferred_hook_count == 1, "One-shot hook should not fire again");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Hook with false condition does not fire
  // ─────────────────────────────────────────────────────────────────────────────
  {
    deferred_hook_count = 0;

    NYA_EventHook hook = {
      .event_type   = NYA_EVENT_KEYMAP_CHANGED,
      .hook_type    = NYA_EVENT_HOOK_TYPE_DEFERRED,
      .fn           = nya_callback(test_deferred_hook),
      .condition_fn = nya_callback(test_condition_false),
    };
    nya_event_hook_register(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEYMAP_CHANGED });
    NYA_Event e;
    while (nya_system_event_poll(&e)) {}
    nya_assert(deferred_hook_count == 0, "Hook with false condition should not fire");

    nya_event_hook_unregister(hook);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Hook with true condition fires
  // ─────────────────────────────────────────────────────────────────────────────
  {
    deferred_hook_count = 0;

    NYA_EventHook hook = {
      .event_type   = NYA_EVENT_KEYMAP_CHANGED,
      .hook_type    = NYA_EVENT_HOOK_TYPE_DEFERRED,
      .fn           = nya_callback(test_deferred_hook),
      .condition_fn = nya_callback(test_condition_true),
    };
    nya_event_hook_register(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEYMAP_CHANGED });
    NYA_Event e;
    while (nya_system_event_poll(&e)) {}
    nya_assert(deferred_hook_count == 1, "Hook with true condition should fire");

    nya_event_hook_unregister(hook);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Handled event stops propagation to later deferred hooks
  // ─────────────────────────────────────────────────────────────────────────────
  {
    deferred_hook_count = 0;

    NYA_EventHook hook_handle = {
      .event_type = NYA_EVENT_KEYMAP_CHANGED,
      .hook_type  = NYA_EVENT_HOOK_TYPE_DEFERRED,
      .fn         = nya_callback(test_handled_hook),
    };
    NYA_EventHook hook_count = {
      .event_type = NYA_EVENT_KEYMAP_CHANGED,
      .hook_type  = NYA_EVENT_HOOK_TYPE_DEFERRED,
      .fn         = nya_callback(test_deferred_hook),
    };
    nya_event_hook_register(hook_handle);
    nya_event_hook_register(hook_count);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEYMAP_CHANGED });
    NYA_Event e;
    while (nya_system_event_poll(&e)) {}
    nya_assert(deferred_hook_count == 0, "Second hook should not fire after first marks handled");

    nya_event_hook_unregister(hook_handle);
    nya_event_hook_unregister(hook_count);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Hook only fires for matching event type
  // ─────────────────────────────────────────────────────────────────────────────
  {
    immediate_hook_count = 0;

    NYA_EventHook hook = {
      .event_type = NYA_EVENT_CLIPBOARD_UPDATE,
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .fn         = nya_callback(test_immediate_hook),
    };
    nya_event_hook_register(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_KEY_DOWN });
    nya_assert(immediate_hook_count == 0, "Hook should not fire for non-matching event");

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_CLIPBOARD_UPDATE });
    nya_assert(immediate_hook_count == 1, "Hook should fire for matching event");

    NYA_Event e;
    while (nya_system_event_poll(&e)) {}

    nya_event_hook_unregister(hook);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Unregistered hook does not fire
  // ─────────────────────────────────────────────────────────────────────────────
  {
    immediate_hook_count = 0;

    NYA_EventHook hook = {
      .event_type = NYA_EVENT_CLIPBOARD_UPDATE,
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .fn         = nya_callback(test_immediate_hook),
    };
    nya_event_hook_register(hook);
    nya_event_hook_unregister(hook);

    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_CLIPBOARD_UPDATE });
    nya_assert(immediate_hook_count == 0, "Unregistered hook should not fire");

    NYA_Event e;
    while (nya_system_event_poll(&e)) {}
  }

  nya_system_events_deinit();
  nya_system_callback_deinit();
  SDL_Quit();
  _NYA_APP_INSTANCE = (NYA_App){ 0 };

  printf("PASSED: test_event\n");
  return 0;
}
