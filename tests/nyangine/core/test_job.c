/**
 * The job system: submission, completion, and the concurrency limit.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** How many jobs have finished. Atomic because the scheduler's threads are what increment it. */
static atomic u32 completed = 0;

/** Highest number of jobs seen running at once, for the concurrency limit test. */
static atomic u32 running_now  = 0;
static atomic u32 running_peak = 0;

/** Does nothing but say it ran. */
static int job_noop(NYA_Job* job) {
  nya_unused(job);
  atomic_fetch_add(&completed, 1);
  return 0;
}

/**
 * Reads its input and writes a result, so the data plumbing is observable.
 * */
static int job_double(NYA_Job* job) {
  s32 input = *(s32*)job->in_data;

  if (job->out_data != nullptr) {
    s32* result = (s32*)*job->out_data;
    *result     = input * 2;

    if (job->out_size != nullptr) *job->out_size = sizeof(s32);
  }

  atomic_fetch_add(&completed, 1);
  return 0;
}

/** Occupies a slot long enough for the peak counter to mean something. */
static int job_occupy(NYA_Job* job) {
  nya_unused(job);

  u32 now = atomic_fetch_add(&running_now, 1) + 1;

  // Monotonic max. Compare exchange rather than a plain store, so two threads racing here cannot
  // lose the higher of the two values.
  u32 peak = atomic_load(&running_peak);
  while (now > peak) {
    if (atomic_compare_exchange_weak(&running_peak, &peak, now)) break;
  }

  SDL_Delay(5);

  atomic_fetch_sub(&running_now, 1);
  atomic_fetch_add(&completed, 1);
  return 0;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){
    .initialized = true,
    // The scheduler reads this to decide how many jobs may run at once.
    .options     = { _NYA_APP_DEFAULT_OPTIONS },
  };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // The event system too: a finished job dispatches through it, and without it the scheduler
  // pushes onto a null queue and faults inside core_event rather than anywhere near core_job.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  NYA_EXPECT(nya_system_job_init());

  /*
   * Torn down job system first, which means declaring its defer last.
   */
  defer nya_system_callback_deinit();
  defer nya_system_events_deinit();
  defer nya_system_job_deinit();

  // TEST: a single job runs and reports done
  {
    atomic_store(&completed, 0);

    NYA_JobHandle handle = nya_job_submit((NYA_Job){
      .priority = NYA_JOB_PRIORITY_NORMAL,
      .function = nya_callback(job_noop),
    });

    // Waiting rather than sleeping: the point of the API is that a caller can block until the work
    // is finished, and a sleep would make this test both slower and flakier.
    nya_job_wait(handle);

    nya_assert(atomic_load(&completed) == 1, "the job ran, got " FMTu32, atomic_load(&completed));
    nya_assert(nya_job_is_done(handle), "and the system agrees it is finished");
  }

  // TEST: input reaches the job and output comes back
  {
    atomic_store(&completed, 0);

    // Storage the submitter owns, which is what out_data points at. See job_double.
    s32   result    = 0;
    s32   input     = 21;
    void* out_data  = &result;
    u64   out_size  = 0;

    NYA_JobHandle handle = nya_job_submit((NYA_Job){
      .priority = NYA_JOB_PRIORITY_NORMAL,
      .function = nya_callback(job_double),
      .in_data  = &input,
      .in_size  = sizeof(input),
      .out_data = &out_data,
      .out_size = &out_size,
    });

    nya_job_wait(handle);

    nya_assert(out_size == sizeof(s32), "out_size was not set, got " FMTu64, out_size);
    nya_assert(result == 42, "expected 21 doubled, got %d", result);
  }

  // TEST: many jobs all run, none dropped
  {
    enum { COUNT = 64 };

    atomic_store(&completed, 0);

    NYA_JobHandle handles[COUNT];
    for (u32 i = 0; i < COUNT; i++) {
      handles[i] = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_NORMAL, .function = nya_callback(job_noop) });
    }

    // Waited on individually rather than sleeping for "long enough". More jobs than the concurrency
    // limit, so this also exercises the queue draining as slots free up.
    for (u32 i = 0; i < COUNT; i++) nya_job_wait(handles[i]);

    nya_assert(atomic_load(&completed) == COUNT, "expected " FMTu32 " jobs, got " FMTu32, (u32)COUNT, atomic_load(&completed));

    for (u32 i = 0; i < COUNT; i++) nya_assert(nya_job_is_done(handles[i]), "job " FMTu32 " is not marked done", i);
  }

  // TEST: every priority is accepted and runs
  {
    atomic_store(&completed, 0);

    /*
     * Deliberately not asserting execution *order*.
     */
    NYA_JobHandle low    = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_LOW, .function = nya_callback(job_noop) });
    NYA_JobHandle normal = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_NORMAL, .function = nya_callback(job_noop) });
    NYA_JobHandle high   = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_HIGH, .function = nya_callback(job_noop) });

    nya_job_wait(low);
    nya_job_wait(normal);
    nya_job_wait(high);

    nya_assert(atomic_load(&completed) == 3, "all three priorities ran, got " FMTu32, atomic_load(&completed));
  }

  // TEST: the concurrency limit is respected
  {
    atomic_store(&completed, 0);
    atomic_store(&running_now, 0);
    atomic_store(&running_peak, 0);

    u32 limit = nya_app_get()->options.max_concurrent_jobs;
    nya_assert(limit > 0, "the default options set no concurrency limit");

    enum { COUNT = 32 };
    NYA_JobHandle handles[COUNT];
    for (u32 i = 0; i < COUNT; i++) {
      handles[i] = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_NORMAL, .function = nya_callback(job_occupy) });
    }

    for (u32 i = 0; i < COUNT; i++) nya_job_wait(handles[i]);

    nya_assert(atomic_load(&completed) == COUNT, "every job ran, got " FMTu32, atomic_load(&completed));

    // A one sided assertion on purpose. Exceeding the limit is a real bug; not reaching it just
    // means the machine scheduled them sequentially, which is allowed and happens on a single core
    // CI box.
    u32 peak = atomic_load(&running_peak);
    nya_assert(peak <= limit, "%u jobs ran at once with a limit of %u", peak, limit);
    nya_assert(peak >= 1, "nothing ever ran");

    nya_log_info("job concurrency: peak %u of a limit of %u across " FMTu32 " jobs", peak, limit, (u32)COUNT);
  }

  // TEST: waiting on something already finished returns immediately
  {
    atomic_store(&completed, 0);

    NYA_JobHandle handle = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_NORMAL, .function = nya_callback(job_noop) });
    nya_job_wait(handle);
    nya_assert(nya_job_is_done(handle));

    // Waiting twice must not block forever waiting for something that already happened, which is
    // the failure mode of a wait implemented as "block until signalled" with no completed state.
    nya_job_wait(handle);
    nya_assert(nya_job_is_done(handle), "still done after a second wait");

    nya_assert(atomic_load(&completed) == 1, "and it did not run twice");
  }

  // TEST: identical submissions get distinct handles
  {
    /*
     * Two submissions of the same function with the same arguments must get distinct handles. That is the
     * ordinary case, and waiting on one handle must not return when a different identical job finishes.
     */
    enum { COUNT = 4 };

    atomic_store(&completed, 0);

    NYA_JobHandle handles[COUNT];
    for (u32 i = 0; i < COUNT; i++) {
      handles[i] = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_NORMAL, .function = nya_callback(job_noop) });
    }

    for (u32 i = 0; i < COUNT; i++) {
      nya_assert(handles[i] != 0, "a submitted job must not get the null handle");

      for (u32 j = i + 1; j < COUNT; j++) {
        nya_assert(handles[i] != handles[j], "jobs " FMTu32 " and " FMTu32 " share handle " FMTu64, i, j, handles[i]);
      }
    }

    for (u32 i = 0; i < COUNT; i++) nya_job_wait(handles[i]);
    nya_assert(atomic_load(&completed) == COUNT, "expected " FMTu32 ", got " FMTu32, (u32)COUNT, atomic_load(&completed));

    /*
     * And the same property once the slots have been recycled.
     */
    NYA_JobHandle recycled[COUNT];
    for (u32 i = 0; i < COUNT; i++) {
      recycled[i] = nya_job_submit((NYA_Job){ .priority = NYA_JOB_PRIORITY_NORMAL, .function = nya_callback(job_noop) });

      for (u32 j = 0; j < COUNT; j++) nya_assert(recycled[i] != handles[j], "a recycled slot reused an earlier job's handle");
    }

    for (u32 i = 0; i < COUNT; i++) nya_job_wait(recycled[i]);

    for (u32 i = 0; i < COUNT; i++) nya_assert(nya_job_is_done(handles[i]), "a finished job must stay finished once its slot is reused");
  }

  // TEST: waiting publishes the job's writes, not just its exit
  {
    /*
     * nya_job_wait has to be a synchronisation edge, not merely a liveness check.
     */
    enum { COUNT = 8 };

    atomic_store(&completed, 0);

    // Per job storage, allocated here rather than inside the jobs: several run at once and the
    // arena they would otherwise allocate from is not thread safe. See job_double.
    s32           inputs[COUNT];
    s32           results[COUNT]   = { 0 };
    void*         outputs[COUNT]   = { nullptr };
    u64           out_sizes[COUNT] = { 0 };
    NYA_JobHandle handles[COUNT];

    for (u32 i = 0; i < COUNT; i++) {
      inputs[i]  = (s32)i;
      results[i] = -1;
      outputs[i] = &results[i];
      handles[i] = nya_job_submit((NYA_Job){
        .priority = NYA_JOB_PRIORITY_NORMAL,
        .function = nya_callback(job_double),
        .in_data  = &inputs[i],
        .in_size  = sizeof(inputs[i]),
        .out_data = &outputs[i],
        .out_size = &out_sizes[i],
      });
    }

    // Read immediately after each wait, before the others are waited on, so a result that is only
    // visible some time later fails here rather than being hidden by the waits that follow.
    for (u32 i = 0; i < COUNT; i++) {
      nya_job_wait(handles[i]);

      nya_assert(out_sizes[i] == sizeof(s32), "job " FMTu32 " had not published when its wait returned", i);
      nya_assert(results[i] == (s32)(i * 2), "job " FMTu32 " gave %d, expected %d", i, results[i], (s32)(i * 2));
    }

    nya_assert(atomic_load(&completed) == COUNT, "expected " FMTu32 ", got " FMTu32, (u32)COUNT, atomic_load(&completed));
  }

  // TEST: an unknown handle is done rather than a hang
  {
    // A handle from a previous run, or a zeroed struct field. Neither corresponds to queued work,
    // so the honest answer is "there is nothing to wait for" rather than blocking forever.
    NYA_JobHandle never_submitted = 0;
    nya_assert(nya_job_is_done(never_submitted), "a handle that names no job is not pending");
    nya_job_wait(never_submitted);

    NYA_JobHandle far_future = 0xFFFFFFFF;
    nya_assert(nya_job_is_done(far_future));
    nya_job_wait(far_future);
  }

  printf("PASSED: test_job\n");
  return 0;
}
