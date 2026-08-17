/**
 * Regression test for the lock order inversion between the job and event systems (core_job.c).
 *
 * Two orders used to exist:
 *
 *   - _nya_job_scheduler took job_active_mutex then job_queue_mutex, and dispatched
 *     NYA_EVENT_JOB_STARTED and NYA_EVENT_JOB_COMPLETED from inside that critical section.
 *     nya_event_dispatch takes event_queue_mutex, so the scheduler's order was
 *     job_active -> job_queue -> event_queue.
 *   - nya_event_dispatch holds event_queue_mutex across every immediate hook. A hook that calls
 *     nya_job_submit takes job_queue_mutex, so any other thread's order was
 *     event_queue -> job_queue.
 *
 * Together those deadlock: the scheduler waits on event_queue holding job_queue, while the thread
 * dispatching waits on job_queue holding event_queue.
 *
 * The fix collects what a scheduler pass reaped and started, releases both job mutexes, and only
 * then dispatches.
 *
 * **This test is a race, and it is a watchdog rather than an assertion.** It cannot prove the
 * absence of a deadlock; it drives the two threads into their respective windows as hard as it can
 * and fails loudly if they lock up. On the unfixed code it hangs within a second or so, which is
 * why the watchdog exists — a hung test is worse than a failing one, so the watchdog turns it into
 * a clean non-zero exit.
 **/
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** How long the whole exercise gets before the watchdog calls it a deadlock. */
#define WATCHDOG_TIMEOUT_MS 15000

/** How many times the main thread dispatches an event whose hook submits a job. */
#define DISPATCH_ROUNDS 400

static atomic u32 jobs_completed  = 0;
static atomic u32 jobs_submitted  = 0;
static atomic b8  finished        = false;
static atomic u32 watchdog_stage  = 0;

static int job_noop(NYA_Job* job) {
  nya_unused(job);
  atomic_fetch_add(&jobs_completed, 1);
  return 0;
}

/**
 * Submits a job from inside an immediate hook, which is the second half of the inversion.
 *
 * Immediate hooks run with event_queue_mutex held, so this is the thread that wants job_queue_mutex
 * while holding event_queue_mutex.
 * */
void hook_submits_a_job(NYA_Event* event);
void hook_submits_a_job(NYA_Event* event) {
  nya_unused(event);

  (void)nya_job_submit((NYA_Job){
      .priority = NYA_JOB_PRIORITY_NORMAL,
      .function = nya_callback(job_noop),
  });

  atomic_fetch_add(&jobs_submitted, 1);
}

/** Kills the process rather than letting the suite hang forever on a deadlock. */
static int watchdog(void* data) {
  nya_unused(data);

  u64 waited = 0;
  while (waited < WATCHDOG_TIMEOUT_MS) {
    if (atomic_load(&finished)) return 0;
    SDL_Delay(25);
    waited += 25;
  }

  fprintf(
    stderr,
    "\nDEADLOCK: no progress for %d ms at stage %u (submitted %u, completed %u).\n"
    "The job scheduler and the event system have inverted lock orders.\n",
    WATCHDOG_TIMEOUT_MS,
    atomic_load(&watchdog_stage),
    atomic_load(&jobs_submitted),
    atomic_load(&jobs_completed)
  );
  fflush(stderr);

  // _exit rather than exit: the atexit handlers would try to take the very mutexes that are stuck.
  _exit(1);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){
    .initialized = true,
    .options     = { _NYA_APP_DEFAULT_OPTIONS },
  };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  NYA_EXPECT(nya_system_job_init());

  SDL_Thread* guard = SDL_CreateThread(watchdog, "watchdog", nullptr);
  nya_assert(guard != nullptr, "SDL_CreateThread failed for the watchdog: %s", SDL_GetError());

  printf("TEST: submitting a job from an immediate event hook\n");
  {
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(hook_submits_a_job),
    });

    /*
     * Seeded with work first, so the scheduler is already churning — reaping finished threads and
     * starting new ones, which is exactly when it holds both job mutexes and wants to dispatch.
     * Dispatching into that from this thread is the other half of the inversion.
     */
    for (u32 i = 0; i < 32; i++) {
      (void)nya_job_submit((NYA_Job){
          .priority = NYA_JOB_PRIORITY_NORMAL,
          .function = nya_callback(job_noop),
      });
      atomic_fetch_add(&jobs_submitted, 1);
    }

    atomic_store(&watchdog_stage, 1);

    for (u32 round = 0; round < DISPATCH_ROUNDS; round++) {
      nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
      atomic_store(&watchdog_stage, 2);
    }
  }
  printf("  dispatched " FMTu32 " rounds, submitted %u jobs\n", (u32)DISPATCH_ROUNDS, atomic_load(&jobs_submitted));

  printf("TEST: every submitted job runs to completion\n");
  {
    atomic_store(&watchdog_stage, 3);

    // Drained by the scheduler, so this is a poll rather than a join. The watchdog is what bounds it.
    u32 expected = atomic_load(&jobs_submitted);
    while (atomic_load(&jobs_completed) < expected) SDL_Delay(2);

    nya_assert(
      atomic_load(&jobs_completed) == expected,
      "completed %u of %u jobs",
      atomic_load(&jobs_completed),
      expected
    );
  }
  printf("  all %u jobs completed\n", atomic_load(&jobs_completed));

  atomic_store(&finished, true);
  SDL_WaitThread(guard, nullptr);

  nya_system_job_deinit();
  nya_system_events_deinit();
  nya_system_callback_deinit();
  SDL_Quit();

  printf("PASSED: the job scheduler dispatches with its mutexes released\n");
  return 0;
}
