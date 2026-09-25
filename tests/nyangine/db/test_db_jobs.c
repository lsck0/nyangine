/**
 * The db module's persistent job queue: a queue that lives in a SQLite table, so background work
 * survives a restart. The tests are deterministic — a simulated clock stands in for the wall clock, so
 * backoff, leases and deadlines advance exactly when the test says and a run replays the same every
 * time. Most tests use an in-memory database; the ones about two workers or about persistence across a
 * reopen use a temp file, since those are about a file two connections share.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/*
 * A clock the test drives by hand. Installed through nya_instant_source_set, it is what nya_instant_now
 * — and so the whole queue — reads, so every deadline and backoff delay is measured against a time the
 * test controls to the nanosecond.
 */
static s64 g_now_ns = 0;

static NYA_Instant test_clock(void* context) {
  (void)context;
  return (NYA_Instant){ .ns = g_now_ns };
}

static NYA_Instant at(s64 ns) {
  return (NYA_Instant){ .ns = ns };
}

/** A UTC instant from calendar parts, for the cron tests, which reason in wall-clock time. Asserts it is representable. */
static NYA_Instant utc(s32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second) {
  NYA_Instant instant = { 0 };
  b8          ok      = nya_instant_from_utc(
    (NYA_Date){ .year = year, .month = month, .day = day }, (NYA_TimeOfDay){ .hour = hour, .minute = minute, .second = second, .nanosecond = 0 }, &instant
  );
  nya_assert(ok, "the test date is within the representable range");
  return instant;
}

/** Removes a database file and the WAL/SHM sidecars a connection may leave beside it. */
static void remove_database(NYA_ConstCString path) {
  (void)remove(path);
  char sidecar[512];
  (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
  (void)remove(sidecar);
  (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
  (void)remove(sidecar);
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_jobs");
  defer      nya_arena_destroy(arena);

  nya_log_info("SQLite %s", nya_sql_version());

  // A large, nonzero base so `run_at == 0` (the "run now" sentinel) is never a real time we schedule, and so times before the base are still positive. Roughly 2023-11-14 in unix nanoseconds.
  g_now_ns = 1700000000LL * NYA_NS_PER_SECOND;
  nya_instant_source_set((NYA_InstantSource){ .now = test_clock });

  // TEST: enqueue -> claim -> complete round-trip
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    s64              id      = 0;
    NYA_ConstCString payload = "hello world";
    NYA_EXPECT(nya_job_enqueue(queue, "greet", (const u8*)payload, strlen(payload), &id));
    nya_assert(id > 0, "an enqueued job has an id");

    u64 pending = 0;
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_PENDING, &pending));
    nya_assert(pending == 1, "the job is pending");

    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "worker-1", arena, &job, &claimed));
    nya_assert(claimed, "a due job is claimed");
    nya_assert(job.id == id, "the claim returns the job we enqueued");
    nya_assert(nya_string_equals(job.kind, "greet"), "the kind round-trips");
    nya_assert(job.attempts == 1, "the first claim is attempt one");
    nya_assert(job.payload_size == strlen(payload), "the payload size round-trips");
    nya_assert(memcmp(job.payload, payload, job.payload_size) == 0, "the payload bytes round-trip");

    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.claimed == 1 && stats.pending == 0, "the claimed job left the pending pool");

    NYA_EXPECT(nya_job_complete(queue, job.id));
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.done == 1 && stats.claimed == 0, "the completed job is done");

    // Nothing left to run.
    NYA_QueuedJob none         = { 0 };
    b8            none_claimed = true;
    NYA_EXPECT(nya_job_claim(queue, "worker-1", arena, &none, &none_claimed));
    nya_assert(!none_claimed, "an empty queue claims nothing and is not an error");

    // Completing a job that is not in flight is a no-op reported as not found.
    NYA_Error again = nya_job_complete(queue, job.id);
    nya_assert(again.kind == NYA_ERROR_NOT_FOUND, "a second completion finds no claimed job");
  }

  // TEST: a failed job retries with exponential backoff, then dead-letters at max_attempts
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue, .backoff_base = nya_duration_from_s(1), .backoff_cap = nya_duration_from_s(3600), .default_max_attempts = 3, .lease = nya_duration_from_s(30)));
    defer nya_jobs_close(queue);

    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "flaky", (const u8*)"x", 1, &id));

    // Attempt one: claim, then fail. The reschedule is base * 2^0 = 1s into the future.
    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.attempts == 1, "attempt one claimed");

    s64 fail_time_1 = g_now_ns;
    NYA_EXPECT(nya_job_fail(queue, id, true));

    NYA_QueuedJob after1 = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &after1));
    s64 delay1 = after1.run_at.ns - fail_time_1;
    nya_assert(delay1 == NYA_NS_PER_SECOND, "the first backoff is one base");

    // Not due yet: the backoff must actually hold the job back.
    NYA_QueuedJob early         = { 0 };
    b8            early_claimed = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &early, &early_claimed));
    nya_assert(!early_claimed, "a backed-off job is not claimable before its run_at");

    // Attempt two: advance to its run_at, claim, fail. The reschedule is base * 2^1 = 2s.
    g_now_ns = after1.run_at.ns;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.attempts == 2, "attempt two claimed");

    s64 fail_time_2 = g_now_ns;
    NYA_EXPECT(nya_job_fail(queue, id, true));

    NYA_QueuedJob after2 = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &after2));
    s64 delay2 = after2.run_at.ns - fail_time_2;
    nya_assert(delay2 == 2 * NYA_NS_PER_SECOND, "the second backoff has doubled");
    nya_assert(delay2 > delay1, "backoff is monotonic across attempts");

    // Attempt three: advance, claim, fail. attempts (3) is not < max_attempts (3), so it dead-letters.
    g_now_ns = after2.run_at.ns;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.attempts == 3, "attempt three claimed");
    NYA_EXPECT(nya_job_fail(queue, id, true));

    u64 dead = 0;
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_DEAD, &dead));
    nya_assert(dead == 1, "the job dead-letters after its last attempt");

    // A dead job is never handed out again.
    NYA_QueuedJob revived      = { 0 };
    b8            revived_flag = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &revived, &revived_flag));
    nya_assert(!revived_flag, "a dead-lettered job is not claimable");
  }

  // TEST: a non-retryable failure dead-letters immediately, whatever the attempt count
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue, .default_max_attempts = 10));
    defer nya_jobs_close(queue);

    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "poison", (const u8*)"", 0, &id));

    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.attempts == 1, "claimed on the first try");
    nya_assert(job.payload_size == 0 && job.payload != nullptr, "an empty payload is empty bytes, not missing");

    // retryable = false: it dies now even with nine attempts left.
    NYA_EXPECT(nya_job_fail(queue, id, false));
    u64 dead = 0;
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_DEAD, &dead));
    nya_assert(dead == 1, "an unrecoverable failure dead-letters at once");
  }

  // TEST: a unique_key enqueued twice yields one job; the key frees when the job ends
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    s64 first = 0;
    s64 second = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "welcome", (const u8*)"a", 1, &first, .unique_key = "user-42"));
    NYA_EXPECT(nya_job_enqueue(queue, "welcome", (const u8*)"b", 1, &second, .unique_key = "user-42"));
    nya_assert(first == second, "a duplicate enqueue returns the same job id");

    u64 pending = 0;
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_PENDING, &pending));
    nya_assert(pending == 1, "a unique key is queued exactly once");

    // The no-op did not overwrite the payload of the job already there.
    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.payload_size == 1 && job.payload[0] == 'a', "the first enqueue's payload is the one kept");

    // While it is claimed (active, not done) the key is still taken.
    s64 dup = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "welcome", (const u8*)"c", 1, &dup, .unique_key = "user-42"));
    nya_assert(dup == job.id, "a claimed job still holds its unique key");
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_PENDING, &pending));
    nya_assert(pending == 0, "no second job appeared while the first was in flight");

    // Once it is done, the key is free and the next request enqueues a fresh job.
    NYA_EXPECT(nya_job_complete(queue, job.id));
    s64 fresh = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "welcome", (const u8*)"d", 1, &fresh, .unique_key = "user-42"));
    nya_assert(fresh != job.id && fresh > 0, "a finished key can be enqueued again");
  }

  // TEST: replace overwrites a still-pending unique job instead of a no-op
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "rebuild", (const u8*)"old", 3, &id, .unique_key = "index"));
    NYA_EXPECT(nya_job_enqueue(queue, "rebuild", (const u8*)"new", 3, &id, .unique_key = "index", .replace = true));

    u64 pending = 0;
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_PENDING, &pending));
    nya_assert(pending == 1, "replace still leaves exactly one job");

    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.payload_size == 3 && memcmp(job.payload, "new", 3) == 0, "replace swapped in the new payload");
  }

  // TEST: a scheduled (future run_at) job is not claimed early
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    s64 run_at = g_now_ns + 60 * NYA_NS_PER_SECOND;
    s64 id     = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "later", (const u8*)"", 0, &id, .run_at = at(run_at)));

    NYA_QueuedJob job     = { 0 };
    b8            claimed = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(!claimed, "a job scheduled for the future is not claimed now");

    g_now_ns = run_at;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.id == id, "once its time comes it is claimable");
  }

  // TEST: a claimed job is not double-claimed, across two workers on two connections
  {
    NYA_ConstCString path = "./_test_jobs_race.db";
    remove_database(path);
    defer remove_database(path);

    // Two connections to one file: two independent workers, exactly the Phase 3 shape. SQLite's write lock serialises the claims, and the claim's WHERE re-checks state, so the second worker's UPDATE finds the row already claimed and takes a different one — the two never carry the same job away.
    NYA_Database* db_a = nullptr;
    NYA_Database* db_b = nullptr;
    NYA_EXPECT(nya_sql_open(arena, path, &db_a));
    defer nya_sql_close(db_a);
    NYA_EXPECT(nya_sql_open(arena, path, &db_b));
    defer nya_sql_close(db_b);

    NYA_JobQueue* queue_a = nullptr;
    NYA_JobQueue* queue_b = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db_a, &queue_a));
    defer nya_jobs_close(queue_a);
    NYA_EXPECT(nya_jobs_open(arena, db_b, &queue_b));
    defer nya_jobs_close(queue_b);

    // One job, two workers racing for it: exactly one wins.
    s64 only = 0;
    NYA_EXPECT(nya_job_enqueue(queue_a, "solo", (const u8*)"", 0, &only));

    NYA_QueuedJob job_a  = { 0 };
    NYA_QueuedJob job_b  = { 0 };
    b8            got_a  = false;
    b8            got_b  = false;
    NYA_EXPECT(nya_job_claim(queue_a, "worker-a", arena, &job_a, &got_a));
    NYA_EXPECT(nya_job_claim(queue_b, "worker-b", arena, &job_b, &got_b));
    nya_assert(got_a && !got_b, "worker A took the only job; worker B got nothing");
    nya_assert(job_a.id == only, "and it was the job we enqueued");

    // Two jobs, two workers: each gets a distinct one, and a third claim finds none.
    s64 j1 = 0;
    s64 j2 = 0;
    NYA_EXPECT(nya_job_complete(queue_a, job_a.id));
    NYA_EXPECT(nya_job_enqueue(queue_a, "pair", (const u8*)"", 0, &j1));
    NYA_EXPECT(nya_job_enqueue(queue_a, "pair", (const u8*)"", 0, &j2));

    NYA_EXPECT(nya_job_claim(queue_a, "worker-a", arena, &job_a, &got_a));
    NYA_EXPECT(nya_job_claim(queue_b, "worker-b", arena, &job_b, &got_b));
    nya_assert(got_a && got_b, "both workers found work");
    nya_assert(job_a.id != job_b.id, "and never the same job");

    NYA_QueuedJob none = { 0 };
    b8            more = true;
    NYA_EXPECT(nya_job_claim(queue_a, "worker-a", arena, &none, &more));
    nya_assert(!more, "with both jobs claimed the next claim finds nothing");
  }

  // TEST: a lease-expired claim is reclaimable (a crashed worker's job comes back)
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue, .lease = nya_duration_from_s(10)));
    defer nya_jobs_close(queue);

    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "leased", (const u8*)"", 0, &id));

    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "worker-1", arena, &job, &claimed));
    nya_assert(claimed && job.attempts == 1, "worker-1 holds the lease");
    nya_assert(job.lease_expiry.ns == g_now_ns + 10 * NYA_NS_PER_SECOND, "the lease runs ten seconds");

    // While the lease is live, nobody else can take it.
    NYA_QueuedJob steal      = { 0 };
    b8            stole      = true;
    NYA_EXPECT(nya_job_claim(queue, "worker-2", arena, &steal, &stole));
    nya_assert(!stole, "a live lease keeps the job to its holder");

    // worker-1 "crashes": it never completes. Past the lease, worker-2 reclaims it.
    g_now_ns += 11 * NYA_NS_PER_SECOND;
    NYA_EXPECT(nya_job_claim(queue, "worker-2", arena, &steal, &stole));
    nya_assert(stole && steal.id == id, "an expired lease is reclaimable");
    nya_assert(steal.attempts == 2, "the reclaim counts as another attempt");
  }

  // TEST: a job past its deadline is expired, not run
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    // Due now, but with a deadline five seconds out.
    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "perishable", (const u8*)"", 0, &id, .deadline = at(g_now_ns + 5 * NYA_NS_PER_SECOND)));

    // Past the deadline before anyone ran it: the claim reaps it to expired and hands back nothing.
    g_now_ns += 6 * NYA_NS_PER_SECOND;
    NYA_QueuedJob job     = { 0 };
    b8            claimed = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(!claimed, "a job past its deadline is never claimed");

    u64 expired = 0;
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_EXPIRED, &expired));
    nya_assert(expired == 1, "and it is marked expired");

    // The explicit sweep does the same job for a maintenance tick with no claim.
    s64 id2 = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "perishable", (const u8*)"", 0, &id2, .deadline = at(g_now_ns + 1 * NYA_NS_PER_SECOND)));
    g_now_ns += 2 * NYA_NS_PER_SECOND;

    u64 reaped = 0;
    NYA_EXPECT(nya_jobs_reap_expired(queue, &reaped));
    nya_assert(reaped == 1, "reap_expired moves the newly-lapsed job");
    NYA_EXPECT(nya_jobs_count(queue, NYA_JOB_STATE_EXPIRED, &expired));
    nya_assert(expired == 2, "both perishable jobs are expired now");
  }

  // TEST: jobs survive close + reopen (the whole point of a persistent queue)
  {
    NYA_ConstCString path = "./_test_jobs_persist.db";
    remove_database(path);
    defer remove_database(path);

    s64 pending_id = 0;

    // Session one: enqueue two jobs, finish one, then close everything as a restart would.
    {
      NYA_Database* db = nullptr;
      NYA_EXPECT(nya_sql_open(arena, path, &db));
      defer nya_sql_close(db);

      NYA_JobQueue* queue = nullptr;
      NYA_EXPECT(nya_jobs_open(arena, db, &queue));
      defer nya_jobs_close(queue);

      // The throwaway is enqueued first, so it has the lower id and the claim (which orders by run_at then id) takes it rather than the one we want to see survive.
      s64 throwaway_id = 0;
      NYA_EXPECT(nya_job_enqueue(queue, "keep", (const u8*)"gone", 4, &throwaway_id));
      NYA_EXPECT(nya_job_enqueue(queue, "keep", (const u8*)"survive", 7, &pending_id));

      NYA_QueuedJob job     = { 0 };
      b8            claimed = false;
      NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
      nya_assert(claimed && job.id == throwaway_id, "the claim took the first-enqueued job");
      NYA_EXPECT(nya_job_complete(queue, job.id));  // one is done before the "restart"
    }

    // Session two: a fresh connection to the same file sees exactly what session one left.
    {
      NYA_Database* db = nullptr;
      NYA_EXPECT(nya_sql_open(arena, path, &db));
      defer nya_sql_close(db);

      NYA_JobQueue* queue = nullptr;
      NYA_EXPECT(nya_jobs_open(arena, db, &queue));
      defer nya_jobs_close(queue);

      NYA_JobStats stats = { 0 };
      NYA_EXPECT(nya_jobs_stats(queue, &stats));
      nya_assert(stats.done == 1, "the completed job persisted as done");
      nya_assert(stats.pending == 1, "the unfinished job persisted as pending");
      nya_assert(stats.total == 2, "and nothing else");

      // The pending job is claimable in the new session with its payload intact.
      NYA_QueuedJob job     = { 0 };
      b8            claimed = false;
      NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
      nya_assert(claimed && job.id == pending_id, "the survivor is the pending job");
      nya_assert(job.payload_size == 7 && memcmp(job.payload, "survive", 7) == 0, "its payload survived the reopen");
    }
  }

  // TEST: a recurring job reschedules itself on completion instead of finishing, and keeps one row
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue, .default_max_attempts = 3, .lease = nya_duration_from_s(30)));
    defer nya_jobs_close(queue);

    // A heartbeat every ten seconds, keyed so it is a single schedule that a duplicate enqueue never forks.
    s64 enqueued_at = g_now_ns;
    s64 id          = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "beat", (const u8*)"tick", 4, &id, .recur = nya_duration_from_s(10), .unique_key = "hb"));

    // First firing: claim carries the interval back, complete reschedules rather than finishing.
    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.id == id && job.attempts == 1, "the recurring job is claimed");
    nya_assert(job.recur.ns == 10 * NYA_NS_PER_SECOND, "the claim carries the recurrence interval");
    NYA_EXPECT(nya_job_complete(queue, id));

    // It is pending again, not done: one row, its next run one interval on, its attempts reset.
    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.pending == 1 && stats.done == 0 && stats.total == 1, "a completed recurring job is pending again, not a done tombstone");

    NYA_QueuedJob next = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &next));
    nya_assert(next.run_at.ns == enqueued_at + 10 * NYA_NS_PER_SECOND, "the next run is one interval past the last");
    nya_assert(next.attempts == 0, "the reschedule resets the attempt count for the next firing");

    // Not due until its next run_at: the reschedule actually holds it back.
    NYA_QueuedJob early         = { 0 };
    b8            early_claimed = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &early, &early_claimed));
    nya_assert(!early_claimed, "a rescheduled recurring job is not claimable before its next run");

    // A duplicate enqueue under the same key while it waits is still a no-op: one schedule, one row.
    s64 dup = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "beat", (const u8*)"tick", 4, &dup, .recur = nya_duration_from_s(10), .unique_key = "hb"));
    nya_assert(dup == id, "a duplicate enqueue keeps the one recurring row");

    // Second firing on time: claim, complete, and the next run is two intervals from the first.
    g_now_ns = next.run_at.ns;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.id == id && job.attempts == 1, "the recurring job fires again on its next run");
    NYA_EXPECT(nya_job_complete(queue, id));

    NYA_QueuedJob third = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &third));
    nya_assert(third.run_at.ns == enqueued_at + 20 * NYA_NS_PER_SECOND, "each firing advances the schedule by one interval");
  }

  // TEST: a recurring job run late catches up to the interval grid, firing once, not a burst
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    s64 base = g_now_ns;
    s64 id   = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "roll", (const u8*)"", 0, &id, .recur = nya_duration_from_s(10)));

    // Claim on time, but the worker (or the whole queue) is down and the completion lands 55s late.
    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed, "claimed the first firing");

    g_now_ns = base + 55 * NYA_NS_PER_SECOND;
    NYA_EXPECT(nya_job_complete(queue, id));

    // The next run is the first grid tick strictly after now (base + 60s), not base + 10s and not a backlog of five missed firings.
    NYA_QueuedJob next = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &next));
    nya_assert(next.run_at.ns == base + 60 * NYA_NS_PER_SECOND, "a late completion lands on the next grid tick after now");
    nya_assert(next.run_at.ns > g_now_ns, "and strictly in the future, so it does not immediately re-fire");

    // Exactly one job is due at the catch-up tick, not one per missed interval.
    g_now_ns = next.run_at.ns;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.id == id, "the caught-up job is claimable at its tick");
    NYA_QueuedJob none = { 0 };
    b8            more = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &none, &more));
    nya_assert(!more, "there is no backlog of missed firings queued behind it");
  }

  // TEST: a recurring job that fails to death stops recurring and stays for inspection
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue, .default_max_attempts = 1));
    defer nya_jobs_close(queue);

    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "doomed", (const u8*)"", 0, &id, .recur = nya_duration_from_s(10)));

    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.attempts == 1, "claimed the only allowed attempt");

    // Out of attempts: it dead-letters rather than rescheduling, so a broken schedule stops instead of firing blind.
    NYA_EXPECT(nya_job_fail(queue, id, true));

    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.dead == 1 && stats.pending == 0, "a recurring job out of attempts dead-letters and does not reschedule");

    NYA_QueuedJob revived      = { 0 };
    b8            revived_flag = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &revived, &revived_flag));
    nya_assert(!revived_flag, "the dead recurring job is never handed out again");
  }

  // TEST: cron specs parse, and malformed ones are refused rather than crashing
  {
    NYA_Cron cron = { 0 };

    // Every shape the grammar accepts: star, single, range, step, list, and the 7 = Sunday spelling.
    NYA_EXPECT(nya_cron_parse("* * * * *", &cron));
    NYA_EXPECT(nya_cron_parse("30 2 * * *", &cron));
    NYA_EXPECT(nya_cron_parse("*/15 9-17 * * 1-5", &cron));
    NYA_EXPECT(nya_cron_parse("0,15,30,45 * 1,15 1,6,12 *", &cron));
    NYA_EXPECT(nya_cron_parse("5/15 * * * *", &cron));  // n/step: 5, 20, 35, 50
    nya_assert((cron.minute & (1ULL << 5)) && (cron.minute & (1ULL << 20)) && (cron.minute & (1ULL << 50)), "n/step spans from n to the field's top");
    NYA_EXPECT(nya_cron_parse("  0   0 * * *  ", &cron));  // surrounding and repeated whitespace is tolerated

    NYA_EXPECT(nya_cron_parse("0 0 * * 7", &cron));
    nya_assert((cron.day_of_week & 1ULL) && !(cron.day_of_week & (1ULL << 7)), "day-of-week 7 folds onto Sunday = 0");

    // Malformed specs: each is an error, none panics.
    nya_assert(nya_cron_parse("* * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "four fields is too few");
    nya_assert(nya_cron_parse("* * * * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "six fields is too many");
    nya_assert(nya_cron_parse("60 * * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "minute 60 is out of range");
    nya_assert(nya_cron_parse("* 24 * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "hour 24 is out of range");
    nya_assert(nya_cron_parse("* * 0 * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "day-of-month 0 is out of range");
    nya_assert(nya_cron_parse("* * * 13 *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "month 13 is out of range");
    nya_assert(nya_cron_parse("* * * * 8", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "day-of-week 8 is out of range");
    nya_assert(nya_cron_parse("5-1 * * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "a reversed range is rejected");
    nya_assert(nya_cron_parse("*/0 * * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "a zero step is rejected");
    nya_assert(nya_cron_parse("x * * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "a non-number is rejected");
    nya_assert(nya_cron_parse("1, * * * *", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "a trailing comma is rejected");
    nya_assert(nya_cron_parse("", &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "an empty spec is rejected");
    nya_assert(nya_cron_parse(nullptr, &cron).kind == NYA_ERROR_INVALID_ARGUMENT, "a null spec is rejected");
  }

  // TEST: next-fire computation for representative specs, all UTC
  {
    NYA_Cron every_minute = { 0 };
    NYA_Cron hourly       = { 0 };
    NYA_Cron daily        = { 0 };
    NYA_Cron quarter      = { 0 };
    NYA_Cron weekday      = { 0 };
    NYA_EXPECT(nya_cron_parse("* * * * *", &every_minute));
    NYA_EXPECT(nya_cron_parse("0 * * * *", &hourly));
    NYA_EXPECT(nya_cron_parse("30 2 * * *", &daily));
    NYA_EXPECT(nya_cron_parse("*/15 * * * *", &quarter));
    NYA_EXPECT(nya_cron_parse("0 9 * * 1-5", &weekday));

    NYA_Instant next = { 0 };

    NYA_EXPECT(nya_cron_next(every_minute, utc(2024, 3, 10, 8, 15, 30), &next));
    nya_assert(next.ns == utc(2024, 3, 10, 8, 16, 0).ns, "every-minute rounds up to the next whole minute");

    NYA_EXPECT(nya_cron_next(hourly, utc(2024, 3, 10, 8, 15, 0), &next));
    nya_assert(next.ns == utc(2024, 3, 10, 9, 0, 0).ns, "hourly fires at the top of the next hour");

    NYA_EXPECT(nya_cron_next(daily, utc(2024, 3, 10, 8, 15, 0), &next));
    nya_assert(next.ns == utc(2024, 3, 11, 2, 30, 0).ns, "a daily 02:30 job after 08:15 is tomorrow at 02:30");

    NYA_EXPECT(nya_cron_next(daily, utc(2024, 3, 10, 1, 0, 0), &next));
    nya_assert(next.ns == utc(2024, 3, 10, 2, 30, 0).ns, "a daily 02:30 job seen before 02:30 fires today");

    NYA_EXPECT(nya_cron_next(quarter, utc(2024, 3, 10, 8, 15, 0), &next));
    nya_assert(next.ns == utc(2024, 3, 10, 8, 30, 0).ns, "*/15 seen exactly at :15 fires at :30, strictly after");

    NYA_EXPECT(nya_cron_next(quarter, utc(2024, 3, 10, 8, 47, 0), &next));
    nya_assert(next.ns == utc(2024, 3, 10, 9, 0, 0).ns, "*/15 after :47 rolls into the next hour at :00");

    // 2024-03-08 is a Friday; the next weekday-09:00 firing skips the weekend to Monday the 11th.
    NYA_EXPECT(nya_cron_next(weekday, utc(2024, 3, 8, 10, 0, 0), &next));
    nya_assert(next.ns == utc(2024, 3, 11, 9, 0, 0).ns, "a weekday 09:00 job on Friday afternoon next fires Monday");

    // A spec that can never match is bounded, not an infinite loop.
    NYA_Cron never = { 0 };
    NYA_EXPECT(nya_cron_parse("0 0 30 2 *", &never));  // February the 30th
    NYA_Instant sink = { 0 };
    nya_assert(nya_cron_next(never, utc(2024, 1, 1, 0, 0, 0), &sink).kind == NYA_ERROR_NOT_FOUND, "an impossible date is reported after the horizon, never looped forever");
  }

  // TEST: a cron job first fires at its next matching instant and reschedules to the following one on completion
  {
    g_now_ns = utc(2024, 3, 10, 8, 15, 0).ns;

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_JobQueue* queue = nullptr;
    NYA_EXPECT(nya_jobs_open(arena, db, &queue));
    defer nya_jobs_close(queue);

    // Rotate a key every day at 02:30 UTC, keyed so a duplicate enqueue never forks the schedule.
    s64 id = 0;
    NYA_EXPECT(nya_job_enqueue(queue, "rotate", (const u8*)"", 0, &id, .cron = "30 2 * * *", .unique_key = "rot"));

    // The first firing is the next 02:30 after now (08:15 on the 10th), i.e. the 11th, not now.
    NYA_QueuedJob first = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &first));
    nya_assert(first.run_at.ns == utc(2024, 3, 11, 2, 30, 0).ns, "a fresh cron job first fires at its next matching instant, not immediately");
    nya_assert(first.cron != nullptr && nya_string_equals(first.cron, "30 2 * * *"), "the stored cron spec round-trips");

    // Not claimable before its first instant.
    NYA_QueuedJob early         = { 0 };
    b8            early_claimed = true;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &early, &early_claimed));
    nya_assert(!early_claimed, "a cron job is not claimable before its next matching instant");

    // At the instant: claim carries the spec, completion reschedules rather than finishing.
    g_now_ns = first.run_at.ns;
    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_EXPECT(nya_job_claim(queue, "w", arena, &job, &claimed));
    nya_assert(claimed && job.id == id, "the cron job is claimed at its matching instant");
    nya_assert(job.cron != nullptr && nya_string_equals(job.cron, "30 2 * * *"), "the claim carries the cron spec");
    NYA_EXPECT(nya_job_complete(queue, id));

    // One row, pending again, advanced to the next day's 02:30.
    NYA_JobStats stats = { 0 };
    NYA_EXPECT(nya_jobs_stats(queue, &stats));
    nya_assert(stats.pending == 1 && stats.done == 0 && stats.total == 1, "a completed cron job is pending again, one row, not a done tombstone");

    NYA_QueuedJob next = { 0 };
    NYA_EXPECT(nya_job_get(queue, id, arena, &next));
    nya_assert(next.run_at.ns == utc(2024, 3, 12, 2, 30, 0).ns, "a completed cron job advances to its next matching instant");
    nya_assert(next.attempts == 0, "the reschedule resets the attempt count for the next firing");

    // Restore the module clock for anything that runs after this block.
    g_now_ns = 1700000000LL * NYA_NS_PER_SECOND;
  }

  printf("PASSED: test_db_jobs\n");
  return 0;
}
