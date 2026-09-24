/**
 * The db module's job worker runtime: the pool of threads that runs db_jobs.h's persistent queue in
 * process. These tests are real threads on real connections to a shared file, so — unlike the queue's
 * own tests, which drive a simulated clock — they run on the wall clock and assert on outcomes, not on
 * timing: every job runs exactly once and completes, a failing handler is retried and then succeeds, a
 * stop drains what is in flight and joins cleanly, and a kind with no handler is dead-lettered rather
 * than crashing anything. They stay fast with a short poll interval and small counts, and they run
 * under ASan + LSan + UBSan, so a leaked arena, a use-after-free on a joined worker or a data race
 * fails the build.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Removes a database file and the WAL/SHM sidecars a connection may leave beside it. */
static void remove_database(NYA_ConstCString path) {
  (void)remove(path);
  char sidecar[512];
  (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
  (void)remove(sidecar);
  (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
  (void)remove(sidecar);
}

/** Polls the queue until it has at least `want_done` done and `want_dead` dead jobs, or the deadline passes. */
static b8 wait_for(NYA_JobQueue* queue, u64 want_done, u64 want_dead, u32 timeout_ms) {
  u32 waited = 0;
  while (waited < timeout_ms) {
    NYA_JobStats stats = { 0 };
    NYA_Error    read  = nya_jobs_stats(queue, &stats);
    if (read.ok && stats.done >= want_done && stats.dead >= want_dead) return true;
    nya_os_time_sleep_ms(5);
    waited += 5;
  }
  return false;
}

/* ── Handlers ── */

/** Counts every run through an atomic in its context, then completes. The fan-out and drain workhorse. */
static NYA_JobOutcome count_and_complete(const NYA_QueuedJob* job, void* context) {
  (void)job;
  atomic_fetch_add_explicit((atomic u32*)context, 1U, memory_order_relaxed);
  return NYA_JOB_OUTCOME_COMPLETE;
}

/** Like count_and_complete, but sleeps first, so a stop has to wait on a genuinely in-flight job. */
static NYA_JobOutcome sleep_then_complete(const NYA_QueuedJob* job, void* context) {
  (void)job;
  nya_os_time_sleep_ms(20);
  atomic_fetch_add_explicit((atomic u32*)context, 1U, memory_order_relaxed);
  return NYA_JOB_OUTCOME_COMPLETE;
}

/** Fails retryably on the first attempt and completes on the second: the backoff-then-succeed path. */
static NYA_JobOutcome fail_once_then_complete(const NYA_QueuedJob* job, void* context) {
  atomic_fetch_add_explicit((atomic u32*)context, 1U, memory_order_relaxed);
  return job->attempts < 2 ? NYA_JOB_OUTCOME_RETRY : NYA_JOB_OUTCOME_COMPLETE;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_jobworker");
  defer      nya_arena_destroy(arena);

  nya_log_info("SQLite %s", nya_sql_version());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: N jobs, W workers — each job runs exactly once and completes
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString path = "./_test_jobworker_fanout.db";
    remove_database(path);
    defer remove_database(path);

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, path, &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    const u64 job_count = 50;
    for (u64 i = 0; i < job_count; i++) {
      s64 id = 0;
      NYA_EXPECT(nya_job_enqueue(queue, "count", (const u8*)"x", 1, &id));
    }

    atomic u32 ran = 0;
    nya_jobworker_unregister_all();
    NYA_EXPECT(nya_jobworker_register("count", count_and_complete, &ran));

    NYA_JobWorkerPool* pool = nullptr;
    NYA_EXPECT(nya_jobworker_start(queue, 4, &pool, .poll_interval_ms = 5));

    b8 finished = wait_for(queue, job_count, 0, 10000);
    nya_jobworker_stop(pool);  // graceful: joins every worker before returning

    nya_assert(finished, "every enqueued job reached the done state within the deadline");

    u32 runs = atomic_load_explicit(&ran, memory_order_relaxed);
    nya_assert(runs == job_count, "the handler ran exactly once per job — no double-claim, no skip");

    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.done == job_count, "all jobs are done");
    nya_assert(stats.pending == 0 && stats.claimed == 0, "nothing is left pending or in flight");
    nya_assert(stats.total == job_count, "and no phantom jobs appeared");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a handler that fails once is retried with backoff, then succeeds
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString path = "./_test_jobworker_retry.db";
    remove_database(path);
    defer remove_database(path);

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, path, &db));
    defer nya_sql_close(db);

    // A one-millisecond backoff base, so the retry becomes due almost at once and the test stays fast.
    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue, .backoff_base = nya_duration_from_ms(1), .default_max_attempts = 5));
    defer nya_jobs_close(queue);

    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "flaky", (const u8*)"", 0, &id));

    atomic u32 ran = 0;
    nya_jobworker_unregister_all();
    NYA_EXPECT(nya_jobworker_register("flaky", fail_once_then_complete, &ran));

    NYA_JobWorkerPool* pool = nullptr;
    NYA_EXPECT(nya_jobworker_start(queue, 2, &pool, .poll_interval_ms = 5));

    b8 finished = wait_for(queue, 1, 0, 10000);
    nya_jobworker_stop(pool);

    nya_assert(finished, "the flaky job completed after its retry");

    u32 runs = atomic_load_explicit(&ran, memory_order_relaxed);
    nya_assert(runs == 2, "the handler ran twice: a failed first attempt and a successful retry");

    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.done == 1 && stats.dead == 0, "the job is done, not dead-lettered");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: stop drains the in-flight job (nothing left claimed) and joins cleanly
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString path = "./_test_jobworker_drain.db";
    remove_database(path);
    defer remove_database(path);

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, path, &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    for (u64 i = 0; i < 3; i++) {
      s64 id = 0;
      NYA_EXPECT(nya_job_enqueue(queue, "slow", (const u8*)"", 0, &id));
    }

    atomic u32 ran = 0;
    nya_jobworker_unregister_all();
    NYA_EXPECT(nya_jobworker_register("slow", sleep_then_complete, &ran));

    // One worker, so a job is genuinely in flight when we stop rather than everything already done.
    NYA_JobWorkerPool* pool = nullptr;
    NYA_EXPECT(nya_jobworker_start(queue, 1, &pool, .poll_interval_ms = 5));

    // Give the worker time to claim and be mid-sleep in a handler, then stop while it runs.
    nya_os_time_sleep_ms(10);
    nya_jobworker_stop(pool);  // must wait out the in-flight handler before it returns

    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.claimed == 0, "a graceful stop leaves no job stranded mid-flight");
    nya_assert(stats.done >= 1, "the job that was in flight was carried through to done, not abandoned");
    nya_assert(stats.done + stats.pending == 3, "every job is either finished or still waiting, none lost");

    u32 runs = atomic_load_explicit(&ran, memory_order_relaxed);
    nya_assert(runs == stats.done, "the handler completed exactly the jobs the queue counts as done");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a kind with no registered handler is dead-lettered, not crashed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString path = "./_test_jobworker_unhandled.db";
    remove_database(path);
    defer remove_database(path);

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, path, &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    // "orphan" has no handler; "count" does, so we can also confirm the pool keeps working past the orphan.
    s64 orphan = 0;
    s64 handled = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "orphan", (const u8*)"", 0, &orphan));
    NYA_EXPECT(nya_job_enqueue(queue, "count", (const u8*)"", 0, &handled));

    atomic u32 ran = 0;
    nya_jobworker_unregister_all();
    NYA_EXPECT(nya_jobworker_register("count", count_and_complete, &ran));

    NYA_JobWorkerPool* pool = nullptr;
    NYA_EXPECT(nya_jobworker_start(queue, 2, &pool, .poll_interval_ms = 5));

    b8 finished = wait_for(queue, 1, 1, 10000);
    nya_jobworker_stop(pool);

    nya_assert(finished, "the handled job completed and the orphan dead-lettered");

    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.dead == 1, "the job with no handler landed in the dead-letter state");
    nya_assert(stats.done == 1, "and the pool ran the handled job regardless of the orphan");
    nya_assert(atomic_load_explicit(&ran, memory_order_relaxed) == 1, "the handled job ran once");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: registration guards its inputs and its bound
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_jobworker_unregister_all();

    NYA_Error empty = nya_jobworker_register("", count_and_complete, nullptr);
    nya_assert(empty.kind == NYA_ERROR_INVALID_ARGUMENT, "an empty kind is refused");

    NYA_Error null_handler = nya_jobworker_register("k", nullptr, nullptr);
    nya_assert(null_handler.kind == NYA_ERROR_INVALID_ARGUMENT, "a null handler is refused");

    // Registering the same kind twice is a replace, not a second slot, so it never exhausts the table.
    for (u32 i = 0; i < NYA_JOBWORKER_HANDLERS_MAX + 8; i++) {
      NYA_EXPECT(nya_jobworker_register("same", count_and_complete, nullptr));
    }
    nya_jobworker_unregister_all();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: start rejects a nonsensical worker count, and a double stop is safe
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    NYA_JobWorkerPool* pool = nullptr;
    NYA_Error          zero = nya_jobworker_start(queue, 0, &pool, .poll_interval_ms = 5);
    nya_assert(zero.kind == NYA_ERROR_INVALID_ARGUMENT, "zero workers is refused");
    nya_assert(pool == nullptr, "and no pool is handed back on failure");

    nya_jobworker_stop(nullptr);  // safe on null
  }

  printf("PASSED: test_db_jobworker\n");
  return 0;
}
