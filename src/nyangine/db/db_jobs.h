/**
 * @file db_jobs.h
 *
 * A background job queue that lives in a SQLite table, so the work a program still owes survives a
 * restart the way its data does: one executable, one database file, and the queue is a table in it.
 * A crash loses no enqueued job, a redeploy resumes mid-flight work, and a second process opening the
 * same file is a second worker with no broker in between.
 *
 * The queue is the same shape as db_blob.h's store: a thin thing over a NYA_Database, opened on
 * whichever database the caller already has, so a program that has storage has a queue without
 * dragging in http, a scheduler, or a message broker. It shares the app's file, its encryption key
 * and its transactions, or it has its own.
 *
 * Overview:
 *   nya_jobs_open / _close      create or migrate the queue's table in a database, then let go of it
 *   nya_job_enqueue             put a job on the queue: a kind, opaque payload bytes, and options
 *   nya_job_claim               atomically take the next due job, leased so a dead worker's is reclaimed
 *   nya_job_complete            mark a claimed job done
 *   nya_job_fail                a failed job retries with exponential backoff, or dead-letters
 *   nya_job_get                 read one job back by id, for a status page or a test
 *   nya_jobs_count              how many jobs are in a given state
 *   nya_jobs_stats              the count of every state in one pass
 *   nya_jobs_reap_expired       move jobs past their deadline to the expired state
 *
 * ```c
 * NYA_JobQueue* queue = nullptr;
 * NYA_TRY(nya_jobs_open(arena, database, &queue));
 * defer nya_jobs_close(queue);
 *
 * // A producer enqueues work; the payload is whatever bytes the handler will need.
 * s64 id = 0;
 * NYA_TRY(nya_job_enqueue(queue, "send_email", body, body_size, &id, .max_attempts = 5));
 *
 * // A worker loop claims, does the work, and reports the outcome.
 * NYA_QueuedJob job = { 0 };
 * b8      claimed = false;
 * NYA_TRY(nya_job_claim(queue, "worker-1", arena, &job, &claimed));
 * if (claimed) {
 *     NYA_Error work = do_the_work(job.kind, job.payload, job.payload_size);
 *     if (work.ok) NYA_TRY(nya_job_complete(queue, job.id));
 *     else         NYA_TRY(nya_job_fail(queue, job.id, true));   // true: worth retrying
 * }
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHY A LEASE, NOT A LOCK
 * ─────────────────────────────────────────────────────────
 *
 * A claim does not lock a job; it leases it. nya_job_claim stamps the job with the claiming worker and
 * a lease expiry, and a job whose lease has run out is claimable again by anyone — because the worker
 * that held it may have crashed, and a lock a dead process holds forever is how a queue wedges. The
 * lease is the visibility timeout: long enough that a live worker finishes or fails the job before it
 * expires, short enough that a dead worker's job is picked up again soon. A worker that is still alive
 * but slow will have its job stolen after the lease; that is the price of never wedging, and the fix is
 * a lease longer than the slowest honest job, not a lock.
 *
 * ─────────────────────────────────────────────────────────
 * THE CLAIM IS ATOMIC ACROSS WORKERS
 * ─────────────────────────────────────────────────────────
 *
 * Two workers must never run the same job. The claim is a single `UPDATE ... WHERE id = (SELECT ...)
 * RETURNING`: the row is selected and flipped to claimed in one statement, under SQLite's write lock,
 * so between two workers exactly one wins the row and the other's statement re-evaluates its subquery
 * against the already-flipped state and takes a different job or none. There is no read-then-write
 * window for two workers to slip through. See db_jobs.c.
 *
 * ─────────────────────────────────────────────────────────
 * BACKOFF, DEADLINES, AND UNIQUE JOBS
 * ─────────────────────────────────────────────────────────
 *
 * A failed retryable job is rescheduled `backoff_base * 2^(attempt - 1)` into the future, capped at
 * `backoff_cap`: monotonic in the attempt count and never past the cap. Optional jitter (a fraction of
 * the delay, off by default) spreads a herd of jobs that all failed at once, at the cost of the strict
 * monotonicity the cap-and-double schedule otherwise guarantees. After `max_attempts` the job stops
 * retrying and moves to the dead-letter state, where it stays for inspection rather than vanishing.
 *
 * A job may carry a deadline: a moment past which running it is pointless (a password reset email an
 * hour late, a cache warm for a page already gone). A job past its deadline is never claimed and is
 * moved to the expired state instead — nya_job_claim reaps expired jobs before it looks for one to run,
 * and nya_jobs_reap_expired does the same sweep on demand.
 *
 * A job may carry a `unique_key`: at most one job per key is active (pending or claimed) at a time, so
 * a duplicate enqueue while one is still outstanding is a no-op rather than a second copy — the "send
 * one welcome email per user", "rebuild this index once" case. Once a keyed job is done, dead or
 * expired the key is free again, so the next real request enqueues normally.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_clock_instant.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/db/db_sql.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** The table a queue uses when the caller names none. */
#define NYA_JOB_TABLE_DEFAULT "jobs"

/** Longest queue table name, terminator included; the same ceiling db_blob.h and db_orm.h hold one to. */
#define NYA_JOB_TABLE_MAX 64

/** The ceiling on retries a queue uses when the caller and the job both leave it unset: five attempts. */
#define NYA_JOB_DEFAULT_MAX_ATTEMPTS 5

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef struct NYA_JobQueue        NYA_JobQueue;
typedef struct NYA_JobQueueOptions NYA_JobQueueOptions;
typedef struct NYA_JobOptions      NYA_JobOptions;
typedef struct NYA_QueuedJob       NYA_QueuedJob;
typedef struct NYA_JobStats        NYA_JobStats;
typedef enum NYA_JobState          NYA_JobState;

/**
 * Where a job is in its life. Stored as the integer it is, so the same values a query filters on are
 * the ones the enum names; do not renumber without a migration.
 * */
enum NYA_JobState {
    /** Waiting to be claimed. `run_at` says the earliest it may be, so a scheduled job is pending too. */
    NYA_JOB_STATE_PENDING = 0,

    /** Leased to a worker. Claimable again once the lease expires, in case the worker died holding it. */
    NYA_JOB_STATE_CLAIMED = 1,

    /** Finished successfully. Kept as a tombstone rather than deleted, so a caller can see it was done. */
    NYA_JOB_STATE_DONE = 2,

    /** Out of attempts, or failed unrecoverably. The dead-letter state: kept for inspection, never run. */
    NYA_JOB_STATE_DEAD = 3,

    /** Past its deadline before it ran. Never claimed; moved here by the expiry sweep. */
    NYA_JOB_STATE_EXPIRED = 4,

    NYA_JOB_STATE_COUNT,
};

/** What nya_jobs_open takes besides the arena, the database, and where to put the queue. */
struct NYA_JobQueueOptions {
    /**
     * The table the jobs live in, created if it is not there. Null means NYA_JOB_TABLE_DEFAULT. An
     * identifier — letters, digits and underscore, starting with a letter or underscore — because it
     * cannot be a bound parameter; anything else is refused at open.
     * */
    NYA_ConstCString table;

    /** The retry ceiling a job that names none of its own inherits. Zero means NYA_JOB_DEFAULT_MAX_ATTEMPTS. */
    u32 default_max_attempts;

    /**
     * The first retry delay, doubled each further attempt. Zero or negative means one second. See the
     * backoff note in this file's block: the schedule is `backoff_base * 2^(attempt - 1)`, capped.
     * */
    NYA_Duration backoff_base;

    /** The ceiling the doubling delay is held to, so backoff never grows without bound. Zero means one hour. */
    NYA_Duration backoff_cap;

    /**
     * How long a claim's lease lasts before the job is claimable again. Zero or negative means thirty
     * seconds. Make it longer than the slowest honest job; see the lease note in this file's block.
     * */
    NYA_Duration lease;

    /**
     * The fraction of a computed backoff delay that jitter may shave off, in [0, 1]. Zero (the default)
     * is no jitter and a strictly monotonic schedule; a positive value trades that for spreading a herd
     * of jobs that failed together. Clamped into range at open. See the backoff note.
     * */
    f64 jitter;

    /**
     * Milliseconds a statement waits on another connection's write lock before giving up, for the case
     * of two processes sharing one file as two workers. Zero means a sensible default; negative means no
     * wait. Set on the caller's connection at open.
     * */
    s64 busy_timeout_ms;
};

/** What nya_job_enqueue takes besides the queue, the kind, and the payload. */
struct NYA_JobOptions {
    /**
     * The earliest moment the job may be claimed, for a delayed or scheduled job. A zeroed instant
     * (`.ns == 0`) means now, so a job with no schedule runs as soon as a worker is free.
     * */
    NYA_Instant run_at;

    /**
     * The moment past which the job is pointless and must not run: it is expired rather than claimed.
     * A zeroed instant (`.ns == 0`) means no deadline. See the deadline note in this file's block.
     * */
    NYA_Instant deadline;

    /** This job's retry ceiling. Zero means the queue's default_max_attempts. */
    u32 max_attempts;

    /**
     * At most one active (pending or claimed) job per key. A second enqueue under a key already active
     * is a no-op, unless `replace` is set. Null means the job is not deduplicated. See the unique note.
     * */
    NYA_ConstCString unique_key;

    /**
     * On a unique_key collision, replace the active job's payload and schedule instead of doing nothing —
     * but only while it is still pending, never while a worker holds it. Ignored without a unique_key.
     * */
    b8 replace;
};

/**
 * A claimed job, as nya_job_claim hands it back. Its `kind` and `payload` are copied into the arena the
 * claim was given and are valid until that arena dies. A value, so it is passed and returned by copy.
 * */
struct NYA_QueuedJob {
    s64 id;

    /** The job's type, the string the producer enqueued it under; the worker dispatches on it. */
    NYA_ConstCString kind;

    /** The opaque bytes the producer enqueued. Never null: a job with no payload has a zero-length one. */
    const u8* payload;
    u64       payload_size;

    /** How many times this job has been claimed, this claim included: one on its first run. */
    u32 attempts;

    /** The retry ceiling in force for this job. */
    u32 max_attempts;

    /** When the job became eligible to run. */
    NYA_Instant run_at;

    /** The job's deadline, or a zeroed instant (`.ns == 0`) when it has none. */
    NYA_Instant deadline;

    /** When the job was first enqueued. */
    NYA_Instant created;

    /** When this claim's lease runs out and the job is claimable again. */
    NYA_Instant lease_expiry;
};

/** The count of jobs in each state, as nya_jobs_stats fills it in one query. */
struct NYA_JobStats {
    u64 pending;
    u64 claimed;
    u64 done;
    u64 dead;
    u64 expired;

    /** Every row, whatever its state: the sum of the fields above. */
    u64 total;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/**
 * Opens a queue on `database`, creating or migrating its table and index. The queue is allocated in
 * `arena` and is valid until nya_jobs_close or the arena dies, whichever comes first.
 *
 * ```c
 * NYA_TRY(nya_jobs_open(arena, database, &queue));
 * NYA_TRY(nya_jobs_open(arena, database, &queue, .table = "emails", .default_max_attempts = 3));
 * ```
 * */
// Not named `table`, like nya_sql_open: a macro parameter is substituted after the dot and would take the caller's variable name.
#define nya_jobs_open(arena, database, out_queue, ...) \
    nya_jobs_open_with_options((arena), (database), (NYA_JobQueueOptions){ __VA_ARGS__ }, (out_queue))

/** What nya_jobs_open expands to. Refuses a table name that is not an identifier. */
NYA_API NYA_Error nya_jobs_open_with_options(
    NYA_Arena* arena, NYA_Database* database, NYA_JobQueueOptions options, OUT NYA_JobQueue** out_queue
) __attr_no_discard;

/** Lets go of the queue. Safe on null, so an unwind path does not need to check. Leaves the table. */
NYA_API void nya_jobs_close(NYA_JobQueue* queue);

/**
 * Puts a job on the queue and, when `out_id` is not null, writes its id there. `kind` is the handler
 * name a worker dispatches on; `payload`/`payload_size` are the opaque bytes it will need, and a
 * zero-length payload (a null pointer with size 0) is fine.
 *
 * With a `unique_key` set in the options, a job already active under that key makes this a no-op that
 * returns the existing job's id (or replaces it, with `.replace`); see the unique note in this file's
 * block. Without one, every call enqueues a new job.
 *
 * ```c
 * NYA_TRY(nya_job_enqueue(queue, "resize_image", bytes, size, &id));
 * NYA_TRY(nya_job_enqueue(queue, "welcome", bytes, size, &id, .unique_key = user_id, .max_attempts = 3));
 * NYA_TRY(nya_job_enqueue(queue, "report", bytes, size, &id, .run_at = midnight, .deadline = noon));
 * ```
 * */
// job_kind and the rest are named so nothing collides with an option field substituted after a dot.
#define nya_job_enqueue(queue, job_kind, job_payload, job_payload_size, out_id, ...) \
    nya_job_enqueue_with_options((queue), (job_kind), (job_payload), (job_payload_size), (out_id), (NYA_JobOptions){ __VA_ARGS__ })

/** What nya_job_enqueue expands to. */
NYA_API NYA_Error nya_job_enqueue_with_options(
    NYA_JobQueue* queue, NYA_ConstCString kind, const u8* payload, u64 payload_size, OUT s64* out_id, NYA_JobOptions options
) __attr_no_discard;

/**
 * Atomically claims the next due job for `worker_id` and, if there was one, writes it to `out_job` and
 * sets `*out_claimed` to true; when the queue has nothing to run, `*out_claimed` is false and the call
 * still returns NYA_OK — an empty queue is not an error a poller should treat as one.
 *
 * "Due" is: run_at reached, deadline not passed, and either pending or a claim whose lease has expired.
 * The claim is race-safe across workers (see this file's block). The job's kind and payload are copied
 * into `out_arena`. Jobs found past their deadline are reaped to the expired state first, so a claim
 * never returns one and never leaves one to block the queue.
 * */
NYA_API NYA_Error nya_job_claim(
    NYA_JobQueue* queue, NYA_ConstCString worker_id, NYA_Arena* out_arena, OUT NYA_QueuedJob* out_job, OUT b8* out_claimed
) __attr_no_discard;

/**
 * Marks a claimed job done. NYA_ERROR_NOT_FOUND when no job has that id or it was not in a state a
 * completion applies to, so a caller can tell a real completion from a no-op.
 * */
NYA_API NYA_Error nya_job_complete(NYA_JobQueue* queue, s64 job_id) __attr_no_discard;

/**
 * Reports a claimed job as failed. When `retryable` is true and the job still has attempts left, it is
 * rescheduled with exponential backoff (see this file's block); otherwise it moves to the dead-letter
 * state. NYA_ERROR_NOT_FOUND when no job has that id.
 * */
NYA_API NYA_Error nya_job_fail(NYA_JobQueue* queue, s64 job_id, b8 retryable) __attr_no_discard;

/**
 * Reads the job `job_id` names into `out_arena` and writes it to `out_job`. NYA_ERROR_NOT_FOUND when
 * there is no such job. For a status page or a test; the worker path uses nya_job_claim, which both
 * reads and leases in one step.
 * */
NYA_API NYA_Error nya_job_get(NYA_JobQueue* queue, s64 job_id, NYA_Arena* out_arena, OUT NYA_QueuedJob* out_job) __attr_no_discard;

/** Writes the number of jobs in `state` to `out_count`. */
NYA_API NYA_Error nya_jobs_count(NYA_JobQueue* queue, NYA_JobState state, OUT u64* out_count) __attr_no_discard;

/** Fills `out_stats` with the count of every state in a single query. */
NYA_API NYA_Error nya_jobs_stats(NYA_JobQueue* queue, OUT NYA_JobStats* out_stats) __attr_no_discard;

/**
 * Moves every pending or claimed job whose deadline has passed to the expired state, and writes how
 * many were moved to `out_reaped` when it is not null. nya_job_claim runs this itself before it looks
 * for work; a caller wanting the sweep without a claim (a maintenance tick) calls it directly.
 * */
NYA_API NYA_Error nya_jobs_reap_expired(NYA_JobQueue* queue, OUT u64* out_reaped) __attr_no_discard;
