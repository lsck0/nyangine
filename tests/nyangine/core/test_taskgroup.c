/**
 * The task group: fan work out onto the job pool, join it all at the wait, and carry the first
 * failure back — plus the bound, the cancellation flag, and groups nested inside a task.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Bumped by every task that runs, so the wait can prove each one did. */
static atomic u32 ran = 0;

/** Just says it ran. */
static NYA_Error task_tick(void* arg, NYA_TaskGroup* group) {
  nya_unused(arg);
  nya_unused(group);

  atomic_fetch_add(&ran, 1);
  return NYA_OK;
}

/** Fails with a nameable error, so the wait can be checked for that exact one. */
static NYA_Error task_fail(void* arg, NYA_TaskGroup* group) {
  nya_unused(arg);
  nya_unused(group);

  atomic_fetch_add(&ran, 1);
  return nya_error(NYA_ERROR_NOT_FOUND, "deliberately failing task");
}

/** Runs until the group is cancelled, then bows out — the polling half of cancellation. */
static NYA_Error task_until_cancelled(void* arg, NYA_TaskGroup* group) {
  atomic b8* saw_cancel = (atomic b8*)arg;

  while (!nya_taskgroup_is_cancelled(group)) SDL_Delay(1);

  atomic_store(saw_cancel, true);
  atomic_fetch_add(&ran, 1);
  return NYA_OK;
}

/** Opens a group of its own, fans a few ticks into it, and joins them: a group nested in a task. */
static NYA_Error task_nested(void* arg, NYA_TaskGroup* group) {
  nya_unused(arg);
  nya_unused(group);

  enum { INNER = 4 };

  NYA_Arena*     arena = nya_arena_create(.name = "nested_group");
  NYA_TaskGroup* inner = nullptr;
  NYA_TRY(nya_taskgroup_begin(arena, INNER, &inner));

  for (u32 i = 0; i < INNER; i++) NYA_TRY(nya_taskgroup_spawn(inner, task_tick, nullptr));

  NYA_Error inner_error = NYA_OK;
  nya_taskgroup_end(inner, &inner_error);

  nya_arena_destroy(arena);
  return inner_error;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){
    .initialized = true,
    .options     = { _NYA_APP_DEFAULT_OPTIONS },
  };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // The job pool the groups ride on, and the systems it in turn leans on.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  NYA_EXPECT(nya_system_job_init());

  defer nya_system_callback_deinit();
  defer nya_system_events_deinit();
  defer nya_system_job_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: N tasks all run, the count is exact, and the wait is clean
  // ─────────────────────────────────────────────────────────────────────────────
  {
    enum { COUNT = 64 };

    atomic_store(&ran, 0);

    NYA_Arena*     arena = nya_arena_create(.name = "fan_out");
    NYA_TaskGroup* group = nullptr;
    NYA_EXPECT(nya_taskgroup_begin(arena, COUNT, &group));

    for (u32 i = 0; i < COUNT; i++) NYA_EXPECT(nya_taskgroup_spawn(group, task_tick, nullptr));

    NYA_Error error = NYA_OK;
    nya_taskgroup_end(group, &error);

    nya_assert(error.ok, "no task failed, so the group should join clean");
    nya_assert(atomic_load(&ran) == COUNT, "every task should have run, got " FMTu32, atomic_load(&ran));

    // Waited, so nothing of the group is still running and its arena is safe to reclaim.
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a failing task surfaces its error from the wait
  // ─────────────────────────────────────────────────────────────────────────────
  {
    enum { COUNT = 16 };

    atomic_store(&ran, 0);

    NYA_Arena*     arena = nya_arena_create(.name = "one_fails");
    NYA_TaskGroup* group = nullptr;
    NYA_EXPECT(nya_taskgroup_begin(arena, COUNT, &group));

    // One failer among the ticks. The wait has to notice it wherever it lands in the finishing order.
    for (u32 i = 0; i < COUNT; i++) {
      NYA_TaskFn fn = i == COUNT / 2 ? task_fail : task_tick;
      NYA_EXPECT(nya_taskgroup_spawn(group, fn, nullptr));
    }

    NYA_Error error = NYA_OK;
    nya_taskgroup_end(group, &error);

    nya_assert(!error.ok, "the failing task should have surfaced an error");
    nya_assert(error.kind == NYA_ERROR_NOT_FOUND, "and it should be the one the task returned, got %s", NYA_ERRORKIND_NAME_MAP[error.kind]);
    nya_assert(atomic_load(&ran) == COUNT, "all tasks still ran, got " FMTu32, atomic_load(&ran));

    // A failure raises the flag too, so a caller can see the group was cancelled by its own task.
    nya_assert(nya_taskgroup_is_cancelled(group), "a failed task should have cancelled the group");

    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the bound is enforced
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*     arena = nya_arena_create(.name = "bounded");
    NYA_TaskGroup* group = nullptr;
    NYA_EXPECT(nya_taskgroup_begin(arena, 2, &group));

    nya_assert(nya_taskgroup_spawn(group, task_tick, nullptr).ok, "the first task fits");
    nya_assert(nya_taskgroup_spawn(group, task_tick, nullptr).ok, "the second fills the group");

    NYA_Error overflow = nya_taskgroup_spawn(group, task_tick, nullptr);
    nya_assert(!overflow.ok, "spawning past the bound should fail");
    nya_assert(overflow.kind == NYA_ERROR_OUT_OF_MEMORY, "and say the group is full, got %s", NYA_ERRORKIND_NAME_MAP[overflow.kind]);

    // A zero sized group is refused at begin rather than left to fail on the first spawn.
    NYA_TaskGroup* empty = nullptr;
    nya_assert(!nya_taskgroup_begin(arena, 0, &empty).ok, "a group with no room should be refused");

    NYA_Error error = NYA_OK;
    nya_taskgroup_end(group, &error);
    nya_assert(error.ok, "the two tasks that fit should join clean");

    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a polling task sees the cancellation flag and returns
  // ─────────────────────────────────────────────────────────────────────────────
  {
    enum { COUNT = 3 };

    atomic_store(&ran, 0);

    NYA_Arena*     arena = nya_arena_create(.name = "cancel");
    NYA_TaskGroup* group = nullptr;
    NYA_EXPECT(nya_taskgroup_begin(arena, COUNT, &group));

    // Each task blocks on the flag, so the wait below would hang forever if cancel did not reach them.
    atomic b8 saw_cancel[COUNT] = { 0 };
    for (u32 i = 0; i < COUNT; i++) NYA_EXPECT(nya_taskgroup_spawn(group, task_until_cancelled, &saw_cancel[i]));

    nya_assert(!nya_taskgroup_is_cancelled(group), "nothing has cancelled the group yet");
    nya_taskgroup_cancel(group);

    NYA_Error error = NYA_OK;
    nya_taskgroup_end(group, &error);

    nya_assert(error.ok, "the tasks returned NYA_OK once asked to stop");
    nya_assert(atomic_load(&ran) == COUNT, "every task should have observed the cancel and returned, got " FMTu32, atomic_load(&ran));
    for (u32 i = 0; i < COUNT; i++) nya_assert(atomic_load(&saw_cancel[i]), "task %u never saw the flag", i);

    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nested groups — a task opens and joins a group of its own
  // ─────────────────────────────────────────────────────────────────────────────
  {
    enum { OUTER = 3, INNER = 4 };

    atomic_store(&ran, 0);

    NYA_Arena*     arena = nya_arena_create(.name = "nested_outer");
    NYA_TaskGroup* group = nullptr;
    NYA_EXPECT(nya_taskgroup_begin(arena, OUTER, &group));

    for (u32 i = 0; i < OUTER; i++) NYA_EXPECT(nya_taskgroup_spawn(group, task_nested, nullptr));

    NYA_Error error = NYA_OK;
    nya_taskgroup_end(group, &error);

    nya_assert(error.ok, "no inner or outer task failed");
    nya_assert(atomic_load(&ran) == OUTER * INNER, "every nested tick should have run, got " FMTu32, atomic_load(&ran));

    nya_arena_destroy(arena);
  }

  printf("PASSED: test_taskgroup\n");
  return 0;
}
