/**
 * Regression test for iterating the hook array while a hook grows it (core_event.c).
 *
 * _nya_event_notify_listeners walks the array with nya_array_foreach, which holds a pointer into
 * items. A hook that calls nya_event_hook_register for the event type currently being dispatched
 * pushes into that same array; once it is full the push reallocates, the old block is freed back to
 * the arena, and the loop's pointer is dangling for the rest of the dispatch.
 *
 * The array below is filled to exactly its capacity first, so the very first hook's registration is
 * the push that grows it. Registering from inside a handler is not exotic: the asset system's hot
 * reload path and the job system's completion handlers both do it.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** _NYA_ARRAY_DEFAULT_CAPACITY, which is what nya_array_create gives the hook array. */
#define HOOK_COUNT 16

static volatile s32 hooks_run = 0;

void bug_hook_that_registers(NYA_Event* event);
void bug_hook_plain(NYA_Event* event);

void bug_hook_plain(NYA_Event* event) {
  nya_unused(event);
  hooks_run++;
}

void bug_hook_that_registers(NYA_Event* event) {
  nya_unused(event);
  hooks_run++;

  // Once only, or this never terminates.
  static b8 registered = false;
  if (registered) return;
  registered = true;

  // The array is at length == capacity, so this is the push that reallocates it.
  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_QUIT,
      .fn         = nya_callback(bug_hook_plain),
  });
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());

  // The first one is the one that grows the array from inside the walk; the rest are there so the
  // walk still has somewhere to go afterwards.
  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_QUIT,
      .fn         = nya_callback(bug_hook_that_registers),
  });

  for (s32 i = 1; i < HOOK_COUNT; i++) {
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_QUIT,
        .fn         = nya_callback(bug_hook_plain),
    });
  }

  NYA_ArrayᐸNYA_EventHookᐳ* registered = nya_hmap_get(_NYA_APP_INSTANCE.event_system.immediate_event_hooks, (NYA_EventType)NYA_EVENT_QUIT);
  nya_assert(registered != nullptr);
  nya_assert(registered->length == HOOK_COUNT);
  nya_assert(registered->length == registered->capacity, "the array must be full, or the growth does not happen mid walk");

  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_QUIT });

  // Every hook that was registered before the dispatch has to have run. The one added during it may
  // or may not, which is why the bound is a floor rather than an equality.
  nya_assert(hooks_run >= HOOK_COUNT, "only %d of %d hooks ran; the walk lost its place", hooks_run, HOOK_COUNT);

  nya_system_events_deinit();
  nya_system_callback_deinit();
  SDL_Quit();

  nya_log_info("PASSED: growing the hook array during dispatch does not strand the walk");
  return 0;
}
