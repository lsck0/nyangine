/**
 * @file db_jobworker.h
 *
 * The runtime that runs db_jobs.h's queue: a pool of threads, each on its own connection to the same
 * database file, that between them claim due jobs, dispatch each to the handler its kind names, and
 * report the outcome back to the queue. The queue is the store of work that survives a restart; this
 * is the part that actually does it, in the same process, with no broker and no second executable —
 * the one-binary shape the engine is built around.
 *
 * Overview:
 *   nya_jobworker_register        map a job kind to the function that runs it
 *   nya_jobworker_unregister_all  forget every mapping, for a test between cases
 *   nya_jobworker_start           bring up a pool of workers against a queue
 *   nya_jobworker_stop            drain in-flight work, join every worker, let the pool go
 *
 * ```c
 * static NYA_JobOutcome send_email(const NYA_QueuedJob* job, void* context) {
 *     return deliver(job->payload, job->payload_size) ? NYA_JOB_OUTCOME_COMPLETE : NYA_JOB_OUTCOME_RETRY;
 * }
 *
 * NYA_TRY(nya_jobworker_register("send_email", send_email, nullptr));
 *
 * NYA_JobWorkerPool* pool = nullptr;
 * NYA_TRY(nya_jobworker_start(queue, 4, &pool));       // four workers, four connections
 * defer nya_jobworker_stop(pool);                       // finishes what is in flight, then joins
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * ONE CONNECTION PER WORKER, NOT ONE SHARED
 * ─────────────────────────────────────────────────────────
 *
 * db_sql.h opens a connection in SQLite's NOMUTEX mode: fast, and unsafe to touch from two threads at
 * once. So a worker does not share the queue it was handed; the pool opens each worker its own
 * connection to the same file and its own queue over it, cloned from the tuning of the queue passed to
 * start. The workers never coordinate in this process's memory — they coordinate through the file, the
 * exact way two separate processes sharing it would, because nya_job_claim's atomic claim is what keeps
 * two of them from ever carrying off the same job. What start needs from the caller beyond the queue is
 * the encryption key, when the file has one: a connection is opened with its key and never hands it
 * back, so a fresh connection cannot recover it from the old and the caller has to supply it again.
 *
 * ─────────────────────────────────────────────────────────
 * A STOP DRAINS, IT DOES NOT ABANDON
 * ─────────────────────────────────────────────────────────
 *
 * nya_jobworker_stop raises a flag and wakes every worker. A worker checks the flag only between jobs,
 * never inside one: a job it has already claimed is always carried through to a complete or a fail
 * before the worker looks at the flag again, so a stop never leaves a claimed job in limbo for its
 * lease to reclaim. Stop then joins every thread — it returns once the pool is genuinely idle and every
 * connection closed, not merely once the flag is up. It is safe on null and safe to call twice.
 *
 * ─────────────────────────────────────────────────────────
 * A FAILING HANDLER, AND A KIND WITH NO HANDLER
 * ─────────────────────────────────────────────────────────
 *
 * A handler says how it went by its return: complete, retry, or fail. Retry takes the queue's backoff
 * path — rescheduled with exponential backoff, dead-lettered once its attempts run out — and fail
 * dead-letters it at once. A job whose kind no handler is registered for is not a crash and not a
 * wedge: the worker dead-letters it (a non-retryable fail, since a retry would find no handler either)
 * and moves on, leaving the row in the dead state for a human to look at.
 *
 * ─────────────────────────────────────────────────────────
 * NO BUSY-SPIN
 * ─────────────────────────────────────────────────────────
 *
 * A worker that claims a job loops straight back to claim the next, so a full queue drains at speed. A
 * worker that finds nothing waits on the pool's wake semaphore for up to the poll interval before it
 * looks again — it sleeps, it does not spin — and a stop posts that semaphore so the wait ends at once
 * rather than idling out the interval.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_clock_instant.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/db/db_jobs.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Longest job kind a handler may be registered under, terminator included. Matches the queue's own. */
#define NYA_JOBWORKER_KIND_MAX 64

/** How many distinct kinds the handler table holds. A bounded table: registration past it is refused. */
#define NYA_JOBWORKER_HANDLERS_MAX 64

/** The most workers one pool may run. A bound, so the worker array is a fixed span and never a guess. */
#define NYA_JOBWORKER_MAX 256

/** The poll interval a pool uses when the caller names none: a tenth of a second. */
#define NYA_JOBWORKER_DEFAULT_POLL_MS 100

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_JobWorkerPool    NYA_JobWorkerPool;
typedef struct NYA_JobWorkerOptions NYA_JobWorkerOptions;
typedef enum NYA_JobOutcome         NYA_JobOutcome;

/**
 * How a handler's run went, which decides what the worker reports to the queue. The value a handler
 * returns is the whole of its contract: there is no throwing to catch in C, so a handler that cannot
 * do its work says so by returning retry or fail rather than by some other channel.
 * */
enum NYA_JobOutcome {
    /** The work is done. The worker calls nya_job_complete and the job becomes a done tombstone. */
    NYA_JOB_OUTCOME_COMPLETE = 0,

    /** The work failed but is worth another go. The worker fails it retryable: backoff, then retry. */
    NYA_JOB_OUTCOME_RETRY = 1,

    /** The work failed and retrying is pointless. The worker fails it non-retryable: dead-lettered now. */
    NYA_JOB_OUTCOME_FAIL = 2,
};

/**
 * What runs a job. `job` is the claimed job — its kind, payload and attempt count — valid only for the
 * length of the call. `context` is whatever was registered alongside the handler, the seam for the
 * state the work needs (a mailer, a connection pool, a counter). Runs on a worker thread, so it stands
 * by base_thread.h's rules for what a background thread may touch.
 * */
typedef NYA_JobOutcome (*NYA_JobHandlerFn)(const NYA_QueuedJob* job, void* context);

/** What nya_jobworker_start takes besides the queue, the worker count, and where to put the pool. */
struct NYA_JobWorkerOptions {
    /**
     * The key each worker's own connection opens the file under, or null for a database in the clear.
     * A connection never hands its key back, so the pool cannot read it off the queue it was given and
     * the caller supplies it again here. Exactly NYA_SQL_KEY_SIZE bytes when set. See this file's block.
     * */
    const u8* key;
    u32       key_size;

    /**
     * How long an idle worker waits before it looks for work again, in milliseconds. Zero means
     * NYA_JOBWORKER_DEFAULT_POLL_MS. A stop wakes a waiting worker at once regardless, so this is the
     * ceiling on how stale a newly-enqueued job can sit while the pool is otherwise idle, not stop latency.
     * */
    u32 poll_interval_ms;

    /**
     * How often a worker sweeps the queue for jobs past their deadline. Zero means thirty seconds; a
     * negative span turns the periodic sweep off, since nya_job_claim already reaps before every claim.
     * */
    NYA_Duration reap_interval;

    /** Passed to each worker's queue as its busy_timeout: how long a claim waits on the write lock. */
    s64 busy_timeout_ms;

    /** The label a worker's id and thread name are built from: "<name>-<n>". Null means "jobworker". */
    NYA_ConstCString worker_name;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Registers `handler` as what runs jobs of kind `kind`, with `context` handed back to it on every run.
 * The table is process-wide and bounded (NYA_JOBWORKER_HANDLERS_MAX kinds); registering a kind already
 * in it replaces its handler. NYA_ERROR_INVALID_ARGUMENT for an empty or over-long kind or a null
 * handler, NYA_ERROR_OUT_OF_MEMORY when the table is full. Thread-safe against the workers' lookups.
 * */
NYA_API NYA_Error nya_jobworker_register(NYA_ConstCString kind, NYA_JobHandlerFn handler, void* context) __attr_no_discard;

/** Forgets every registered handler. For a test that wants a clean table between cases. Thread-safe. */
NYA_API void nya_jobworker_unregister_all(void);

/**
 * Starts `worker_count` workers against `queue`, each on its own connection to the queue's database,
 * and writes the pool to `out_pool`. The workers begin claiming at once. `worker_count` must be between
 * one and NYA_JOBWORKER_MAX. On any failure nothing is left running and `*out_pool` is null.
 *
 * ```c
 * NYA_TRY(nya_jobworker_start(queue, 4, &pool));
 * NYA_TRY(nya_jobworker_start(queue, 4, &pool, .poll_interval_ms = 50, .key = key.bytes, .key_size = 32));
 * ```
 * */
// worker_pool_count and the rest are named so nothing collides with an option field after a dot.
#define nya_jobworker_start(queue, worker_pool_count, out_pool, ...) \
    nya_jobworker_start_with_options((queue), (worker_pool_count), (NYA_JobWorkerOptions){ __VA_ARGS__ }, (out_pool))

/** What nya_jobworker_start expands to. */
NYA_API NYA_Error nya_jobworker_start_with_options(
    NYA_JobQueue* queue, u32 worker_count, NYA_JobWorkerOptions options, OUT NYA_JobWorkerPool** out_pool
) __attr_no_discard;

/**
 * Stops the pool gracefully: signals every worker, waits for each to finish the job it is on and exit,
 * closes their connections and lets the pool's memory go. Returns only once the pool is fully idle.
 * Safe on null, and safe to call more than once. See the drain note in this file's block.
 * */
NYA_API void nya_jobworker_stop(NYA_JobWorkerPool* pool);
