#include "nyangine/nyangine.h"

// Compiled as part of db.c, after db_sql.c and db_jobs.c: it opens a fresh connection per worker on the
// public db_sql.h surface and drives db_jobs.h's public claim/complete/fail, and it reads two fields
// those files keep private — the queue's database and its tuning — to clone a worker's queue from the
// one the caller handed start without making the caller repeat itself. See db.c for the include order.

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One handler in the process-wide table: a kind, the function it runs, and the context it is handed. */
typedef struct _NYA_JobHandlerEntry {
    char             kind[NYA_JOBWORKER_KIND_MAX];
    NYA_JobHandlerFn handler;
    void*            context;
    b8               used;
} _NYA_JobHandlerEntry;

/** One worker: its connection, the queue over it, the arena its claims are copied into, and its thread. */
typedef struct NYA_JobWorker {
    NYA_JobWorkerPool* pool;
    NYA_Database*      database;
    NYA_JobQueue*      queue;

    /** Claimed jobs are copied here and the whole arena is freed after each, so it never grows unbounded. */
    NYA_Arena* job_arena;

    NYA_Thread* thread;
    char        worker_id[NYA_JOBWORKER_KIND_MAX];
} NYA_JobWorker;

struct NYA_JobWorkerPool {
    NYA_Arena*     arena;
    NYA_Semaphore* wake;

    /** Raised by stop, read between jobs by every worker. The one thing shared in memory across threads. */
    atomic b8 stopping;

    /** Guards a second stop from touching freed memory. Only the stopping thread reads or writes it. */
    b8 stopped;

    u32 worker_count;
    u32 poll_interval_ms;
    s64 reap_interval_ns;

    /** A copy of the queue's database path, so a worker's connection outlives the string the caller passed. */
    const char* database_path;

    NYA_JobWorker* workers;
};

/** The bounded, process-wide handler table and the spinlock that guards it against the workers' lookups. */
NYA_INTERNAL _NYA_JobHandlerEntry _nya_jobworker_handlers[NYA_JOBWORKER_HANDLERS_MAX];
NYA_INTERNAL atomic u32           _nya_jobworker_registry_lock = 0;

/** Takes the registry spinlock. The critical sections are a bounded scan, microseconds, never a wait on work. */
NYA_INTERNAL void _nya_jobworker_lock(void);

/** Releases the registry spinlock. */
NYA_INTERNAL void _nya_jobworker_unlock(void);

/** Looks a kind up in the handler table, copying its handler and context out. False when none is registered. */
NYA_INTERNAL b8 _nya_jobworker_lookup(NYA_ConstCString kind, OUT NYA_JobHandlerFn* out_handler, OUT void** out_context) __attr_no_discard;

/** One worker's whole life: claim, dispatch, resolve, sleep when idle, until the pool is told to stop. */
NYA_INTERNAL void _nya_jobworker_run(void* data);

/** How many times a complete or a fail is retried when it loses a lock race, before the lease takes over. */
#define _NYA_JOBWORKER_RESOLVE_RETRIES 5

/** Completes or fails a job, retrying past a lock-contention timeout so a busy database does not leave a
 *  finished job claimed for its lease to re-run — the belt to the atomic claim's braces on exactly-once. */
NYA_INTERNAL NYA_Error _nya_jobworker_resolve(NYA_JobWorker* worker, s64 job_id, b8 complete, b8 retryable) __attr_no_discard;

/** Runs one claimed job to a complete or a fail. Split out so the loop reads as claim / run / repeat. */
NYA_INTERNAL void _nya_jobworker_dispatch(NYA_JobWorker* worker, const NYA_QueuedJob* job);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE HANDLER TABLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_jobworker_lock(void) {
    while (atomic_exchange_explicit(&_nya_jobworker_registry_lock, 1U, memory_order_acquire) != 0U) {
        // Held by another thread; take it the moment it drops. Registration is rare and lookups brief.
    }
}

void _nya_jobworker_unlock(void) {
    atomic_store_explicit(&_nya_jobworker_registry_lock, 0U, memory_order_release);
}

NYA_Error nya_jobworker_register(NYA_ConstCString kind, NYA_JobHandlerFn handler, void* context) {
    if (kind == nullptr || kind[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a job handler needs a non-empty kind");
    if (handler == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' was registered with a null handler", kind);
    if (strlen(kind) >= NYA_JOBWORKER_KIND_MAX) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "job kind '%s' is longer than NYA_JOBWORKER_KIND_MAX", kind);
    }

    _nya_jobworker_lock();
    defer _nya_jobworker_unlock();

    // A kind already in the table has its handler replaced; otherwise the first free slot takes it.
    s32 slot = -1;
    for (s32 i = 0; i < (s32)NYA_JOBWORKER_HANDLERS_MAX; i++) {
        if (_nya_jobworker_handlers[i].used && strcmp(_nya_jobworker_handlers[i].kind, kind) == 0) {
            slot = i;
            break;
        }
        if (!_nya_jobworker_handlers[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the job handler table is full at %d kinds", NYA_JOBWORKER_HANDLERS_MAX);

    _NYA_JobHandlerEntry* entry = &_nya_jobworker_handlers[slot];
    (void)snprintf(entry->kind, sizeof(entry->kind), "%s", kind);
    entry->handler = handler;
    entry->context = context;
    entry->used    = true;
    return NYA_OK;
}

void nya_jobworker_unregister_all(void) {
    _nya_jobworker_lock();
    defer _nya_jobworker_unlock();

    for (u32 i = 0; i < NYA_JOBWORKER_HANDLERS_MAX; i++) _nya_jobworker_handlers[i] = (_NYA_JobHandlerEntry){ 0 };
}

b8 _nya_jobworker_lookup(NYA_ConstCString kind, OUT NYA_JobHandlerFn* out_handler, OUT void** out_context) {
    _nya_jobworker_lock();
    defer _nya_jobworker_unlock();

    for (u32 i = 0; i < NYA_JOBWORKER_HANDLERS_MAX; i++) {
        if (_nya_jobworker_handlers[i].used && strcmp(_nya_jobworker_handlers[i].kind, kind) == 0) {
            *out_handler = _nya_jobworker_handlers[i].handler;
            *out_context = _nya_jobworker_handlers[i].context;
            return true;
        }
    }
    return false;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE WORKER LOOP
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_jobworker_resolve(NYA_JobWorker* worker, s64 job_id, b8 complete, b8 retryable) {
    for (u32 attempt = 0;; attempt++) {
        NYA_Error result = complete ? nya_job_complete(worker->queue, job_id) : nya_job_fail(worker->queue, job_id, retryable);

        // Anything but a lock-contention timeout is the final answer — success, or a fault the caller logs.
        // On a timeout the write never landed, so retrying it is safe; past the cap we give up and let the
        // lease reclaim the job rather than block this worker forever.
        if (result.ok || result.kind != NYA_ERROR_TIMEOUT || attempt >= _NYA_JOBWORKER_RESOLVE_RETRIES) return result;
        nya_os_time_sleep_ms(1);
    }
}

void _nya_jobworker_dispatch(NYA_JobWorker* worker, const NYA_QueuedJob* job) {
    NYA_JobHandlerFn handler = nullptr;
    void*            context = nullptr;

    // A kind with no handler is dead-lettered, never crashed and never left to block the queue: a retry
    // would only meet the same missing handler, so the fail is non-retryable and the row stays for a look.
    if (!_nya_jobworker_lookup(job->kind, &handler, &context)) {
        nya_log_warn("job " FMTs64 " of kind '%s' has no registered handler; dead-lettering it", job->id, job->kind);
        NYA_Error dead = _nya_jobworker_resolve(worker, job->id, false, false);
        if (!dead.ok) nya_log_error("could not dead-letter unhandled job " FMTs64 ": %s", job->id, dead.message);
        return;
    }

    NYA_JobOutcome outcome = handler(job, context);

    NYA_Error resolution;
    switch (outcome) {
        case NYA_JOB_OUTCOME_COMPLETE: resolution = _nya_jobworker_resolve(worker, job->id, true, false); break;
        case NYA_JOB_OUTCOME_RETRY:    resolution = _nya_jobworker_resolve(worker, job->id, false, true); break;
        // Fail dead-letters. A handler that returned something out of range is treated the same rather than
        // trusted, so a bug shows up as a stuck job to inspect and never as a silent loss.
        case NYA_JOB_OUTCOME_FAIL:
        default:                       resolution = _nya_jobworker_resolve(worker, job->id, false, false); break;
    }
    if (!resolution.ok) nya_log_error("could not resolve job " FMTs64 " after its handler ran: %s", job->id, resolution.message);
}

void _nya_jobworker_run(void* data) {
    NYA_JobWorker*     worker = data;
    NYA_JobWorkerPool* pool   = worker->pool;

    u64 last_reap_ns = nya_clock_get_monotonic_ns();

    // The flag is only ever read here, between jobs — never inside a claimed one — so a stop can end the
    // loop but can never strand a job that has already been claimed. That is the whole of the drain.
    while (!atomic_load_explicit(&pool->stopping, memory_order_acquire)) {
        // A periodic sweep for jobs past their deadline. nya_job_claim reaps before every claim already,
        // so this only matters while the pool is otherwise idle; it is gated so it is not run every poll.
        if (pool->reap_interval_ns > 0) {
            u64 now_ns = nya_clock_get_monotonic_ns();
            if (now_ns - last_reap_ns >= (u64)pool->reap_interval_ns) {
                u64       reaped = 0;
                NYA_Error swept  = nya_jobs_reap_expired(worker->queue, &reaped);
                if (!swept.ok) nya_log_warn("%s could not reap expired jobs: %s", worker->worker_id, swept.message);
                last_reap_ns = now_ns;
            }
        }

        NYA_QueuedJob job     = { 0 };
        b8            claimed = false;
        NYA_Error     claim   = nya_job_claim(worker->queue, worker->worker_id, worker->job_arena, &job, &claimed);

        if (!claim.ok) {
            // A claim that errors is not a reason to spin: reclaim the arena and wait like an idle worker
            // before trying again, so a database that is briefly unhappy does not peg a core. A timeout is
            // the ordinary shape of contention — another worker held the write lock — and expected while
            // several of them race for the same file, so it is quiet; anything else is a genuine fault.
            if (claim.kind == NYA_ERROR_TIMEOUT) {
                nya_log_debug("%s lost a claim race and will retry: %s", worker->worker_id, claim.message);
            } else {
                nya_log_error("%s could not claim a job: %s", worker->worker_id, claim.message);
            }
            nya_arena_free_all(worker->job_arena);
            (void)nya_semaphore_wait_timeout(pool->wake, pool->poll_interval_ms);
            continue;
        }

        if (!claimed) {
            // Nothing due. Sleep on the wake semaphore until the poll interval runs out or a stop posts
            // it — a real wait, not a spin. Then loop and look again.
            (void)nya_semaphore_wait_timeout(pool->wake, pool->poll_interval_ms);
            continue;
        }

        _nya_jobworker_dispatch(worker, &job);

        // The claim copied the job's kind and payload into this arena; give it all back before the next
        // claim so a long-lived worker's memory stays flat. Then loop straight on to drain a full queue.
        nya_arena_free_all(worker->job_arena);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STARTING AND STOPPING THE POOL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_jobworker_start_with_options(
    NYA_JobQueue* queue, u32 worker_count, NYA_JobWorkerOptions options, OUT NYA_JobWorkerPool** out_pool
) {
    nya_assert(queue != nullptr);
    nya_assert(out_pool != nullptr);

    *out_pool = nullptr;

    if (worker_count == 0 || worker_count > NYA_JOBWORKER_MAX) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a pool runs between 1 and %d workers, not %u", NYA_JOBWORKER_MAX, worker_count);
    }

    NYA_ConstCString name = options.worker_name != nullptr ? options.worker_name : "jobworker";

    NYA_Arena*         arena = nya_arena_create(.name = "jobworker_pool");
    NYA_JobWorkerPool* pool  = nya_arena_alloc(arena, sizeof(NYA_JobWorkerPool));
    *pool                    = (NYA_JobWorkerPool){
                           .arena            = arena,
                           .stopping         = false,
                           .stopped          = false,
                           .worker_count     = worker_count,
                           .poll_interval_ms = options.poll_interval_ms != 0 ? options.poll_interval_ms : NYA_JOBWORKER_DEFAULT_POLL_MS,
                           .reap_interval_ns = options.reap_interval.ns == 0 ? 30 * NYA_NS_PER_SECOND : options.reap_interval.ns,
    };
    pool->workers = nya_arena_alloc(arena, sizeof(NYA_JobWorker) * worker_count);
    memset(pool->workers, 0, sizeof(NYA_JobWorker) * worker_count);

    // A copy of the path in the pool's own memory: the caller's string need only live for this call, but
    // a worker connection opens against it here and lives until stop. The key it does not copy — a caller
    // that gave one to the queue gives it here too, since a connection never reveals the key it holds.
    const char* source_path = queue->database->path;
    u64         path_size   = strlen(source_path) + 1;
    char*       path_copy   = nya_arena_alloc(arena, path_size);
    memcpy(path_copy, source_path, path_size);
    pool->database_path = path_copy;

    NYA_Error semaphore = nya_semaphore_create(arena, 0, &pool->wake);
    if (!semaphore.ok) {
        nya_jobworker_stop(pool);
        return semaphore;
    }

    // Each worker's queue is the caller's, cloned onto a private connection: same table, same tuning, so
    // the backoff, lease and deadline behaviour a worker sees is the behaviour the caller configured.
    NYA_JobQueueOptions queue_options = {
        .table                = queue->table,
        .default_max_attempts = queue->default_max_attempts,
        .backoff_base         = { .ns = queue->backoff_base_ns },
        .backoff_cap          = { .ns = queue->backoff_cap_ns },
        .lease                = { .ns = queue->lease_ns },
        .jitter               = queue->jitter,
        .busy_timeout_ms      = options.busy_timeout_ms,
    };

    // Open every connection before starting any thread: a failure here tears down cleanly with nothing
    // running yet, rather than mid-flight.
    for (u32 i = 0; i < worker_count; i++) {
        NYA_JobWorker* worker = &pool->workers[i];
        worker->pool          = pool;
        (void)snprintf(worker->worker_id, sizeof(worker->worker_id), "%s-%u", name, i);

        NYA_Error opened = nya_sql_open_with_options(
            arena, (NYA_SqlOptions){ .path = pool->database_path, .key = options.key, .key_size = options.key_size }, &worker->database
        );
        if (!opened.ok) {
            nya_jobworker_stop(pool);
            return opened;
        }

        NYA_Error queue_opened = nya_jobs_open_with_options(arena, worker->database, queue_options, &worker->queue);
        if (!queue_opened.ok) {
            nya_jobworker_stop(pool);
            return queue_opened;
        }

        worker->job_arena = nya_arena_create(.name = "jobworker_job");
    }

    // Everyone has a connection: start the threads. They begin claiming at once.
    for (u32 i = 0; i < worker_count; i++) {
        NYA_JobWorker* worker = &pool->workers[i];
        NYA_Error      spawn  = nya_thread_spawn(arena, _nya_jobworker_run, worker, worker->worker_id, &worker->thread);
        if (!spawn.ok) {
            // Some threads are already running; stop signals and joins them, then closes every connection.
            nya_jobworker_stop(pool);
            return spawn;
        }
    }

    *out_pool = pool;
    return NYA_OK;
}

void nya_jobworker_stop(NYA_JobWorkerPool* pool) {
    if (pool == nullptr) return;
    if (pool->stopped) return;  // Idempotent: a second stop, and the defer'd stop after an explicit one, are no-ops.
    pool->stopped = true;

    // Raise the flag, then wake every worker so an idle one ends its wait now instead of idling out its
    // poll interval. A worker inside a job finishes it first; the flag is only seen between jobs.
    atomic_store_explicit(&pool->stopping, true, memory_order_release);
    if (pool->wake != nullptr) {
        for (u32 i = 0; i < pool->worker_count; i++) nya_semaphore_post(pool->wake);
    }

    // Join every thread that was started. Returns only once each worker has left its loop for good.
    for (u32 i = 0; i < pool->worker_count; i++) {
        if (pool->workers[i].thread != nullptr) nya_thread_join(pool->workers[i].thread);
    }

    // No worker runs now, so tearing their connections and arenas down races with nothing.
    for (u32 i = 0; i < pool->worker_count; i++) {
        NYA_JobWorker* worker = &pool->workers[i];
        if (worker->queue != nullptr) nya_jobs_close(worker->queue);
        if (worker->database != nullptr) nya_sql_close(worker->database);
        if (worker->job_arena != nullptr) nya_arena_destroy(worker->job_arena);
    }

    if (pool->wake != nullptr) nya_semaphore_destroy(pool->wake);

    // Last, since it frees the pool struct, the worker array and the connections' own allocations.
    nya_arena_destroy(pool->arena);
}
