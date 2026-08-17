/**
 * Regression test for the hook map rehashing during a dispatch (core_event.c).
 *
 * test_bug_event_hook_realloc.c covers the case where a hook grows the *array* it is being walked
 * from. This is the other allocation in that walk: the array itself lives by value inside
 * `immediate_event_hooks`, and _nya_event_notify_listeners holds a pointer into that map's `values`
 * block across every hook call.
 *
 * nya_hmap_set rehashes when (length + 1) / capacity crosses 0.75, which allocates a new keys,
 * values and occupied triple and frees the old one back to the arena. So a hook that registers
 * another hook for an event type *nothing has hooked yet* — an insert rather than an update — can
 * free the block the walk is reading, and the next `hook_array->length` in the loop condition is a
 * use after free.
 *
 * Reachable rather than theoretical: the map starts at capacity 64 and NYA_EVENT_COUNT is 63, so
 * the threshold of 48 distinct hooked event types is inside what an application registers. The asset
 * system's hot reload path and the job system's completion handlers both register from inside a
 * handler, which is the other half of what this needs.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** How many hooks NYA_EVENT_QUIT carries, so the walk still has work to do after the rehash. */
#define QUIT_HOOK_COUNT 8

static volatile s32 hooks_run = 0;

void bug_hook_plain(NYA_Event* event);
void bug_hook_that_registers_a_new_type(NYA_Event* event);

void bug_hook_plain(NYA_Event* event) {
  nya_unused(event);
  hooks_run++;
}

void bug_hook_that_registers_a_new_type(NYA_Event* event) {
  nya_unused(event);
  hooks_run++;

  // Once only, or the map grows without bound.
  static b8 registered = false;
  if (registered) return;
  registered = true;

  // A type no hook has claimed, so this is an insert, and the insert is what crosses the load
  // factor and rehashes the map out from under the walk.
  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_KEYMAP_CHANGED,
      .fn         = nya_callback(bug_hook_plain),
  });
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());

  NYA_HMapᐸNYA_EventTypeˏNYA_ArrayᐸNYA_EventHookᐳᐳ* hooks = _NYA_APP_INSTANCE.event_system.immediate_event_hooks;

  // One insert short of the load factor, using event types that are neither the one dispatched nor
  // the one the handler claims.
  u64           threshold = (u64)((f32)hooks->capacity * _NYA_HASHMAP_LOAD_FACTOR);
  NYA_EventType filler    = (NYA_EventType)(NYA_EVENT_INVALID + 1);

  while (hooks->length + 1 < threshold) {
    nya_assert(filler < NYA_EVENT_COUNT, "not enough event types to reach the load factor");

    if (filler != NYA_EVENT_QUIT && filler != NYA_EVENT_KEYMAP_CHANGED) {
      nya_event_hook_register((NYA_EventHook){
          .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
          .event_type = filler,
          .fn         = nya_callback(bug_hook_plain),
      });
    }
    filler = (NYA_EventType)(filler + 1);
  }

  // NYA_EVENT_QUIT takes the last slot below the threshold. Its first hook is the one that inserts
  // a new key mid dispatch; the rest are there so the walk continues afterwards.
  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_QUIT,
      .fn         = nya_callback(bug_hook_that_registers_a_new_type),
  });
  for (s32 i = 1; i < QUIT_HOOK_COUNT; i++) {
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_QUIT,
        .fn         = nya_callback(bug_hook_plain),
    });
  }

  u64 capacity_before = hooks->capacity;
  nya_assert(hooks->length == threshold, "expected the map to sit at the threshold, it is at " FMTu64, hooks->length);

  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_QUIT });

  // The rehash has to have happened, or this test is no longer exercising anything. `hooks` is still
  // the right pointer to ask: a rehash swaps the keys, values and occupied blocks but never moves
  // the NYA_HMap struct itself.
  nya_assert(hooks->capacity > capacity_before, "the map did not rehash; the test no longer reproduces the bug");

  // Every hook registered before the dispatch must have run. The one added during it may or may not.
  nya_assert(hooks_run >= QUIT_HOOK_COUNT, "only %d of %d hooks ran; the walk lost its place", hooks_run, QUIT_HOOK_COUNT);

  nya_system_events_deinit();
  nya_system_callback_deinit();
  SDL_Quit();

  printf("PASSED: rehashing the hook map during a dispatch does not strand the walk\n");
  return 0;
}
