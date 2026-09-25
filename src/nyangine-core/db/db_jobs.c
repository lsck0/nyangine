#include "nyangine-core/nyangine.h"

// Compiled as part of db.c after db_sql.c: builds every statement on the public db_sql.h surface (bound values, never interpolated data). See db.c.

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

struct NYA_JobQueue {
    NYA_Database* database;
    NYA_Arena*    arena;

    /** Validated at open, so it is interpolated into a statement's table name as a checked identifier. */
    char table[NYA_JOB_TABLE_MAX];

    u32 default_max_attempts;
    s64 backoff_base_ns;
    s64 backoff_cap_ns;
    s64 lease_ns;
    f64 jitter;
};

/** Whether `name` is an identifier SQLite can carry as a table name: [A-Za-z_][A-Za-z0-9_]*, bounded. */
NYA_INTERNAL b8 _nya_jobs_table_valid(NYA_ConstCString name) __attr_no_discard;

/** The wall clock in nanoseconds since the epoch, read through the seam a test can replace. */
NYA_INTERNAL s64 _nya_jobs_now(void) __attr_no_discard;

/** splitmix64, so jitter has a spread that depends only on its inputs and pulls in no global RNG. */
NYA_INTERNAL u64 _nya_jobs_hash(u64 x) __attr_no_discard;

/** The backoff delay in ns for a job that has been tried `attempts` times: base * 2^(attempt-1), capped. */
NYA_INTERNAL s64 _nya_jobs_backoff_ns(const NYA_JobQueue* queue, u32 attempts, s64 job_id, s64 now) __attr_no_discard;

/** Reads a result row — the columns nya_job_claim and nya_job_get select — into `out_job`, copying into `out_arena`. */
NYA_INTERNAL void _nya_jobs_row_read(NYA_Object* row, NYA_Arena* out_arena, OUT NYA_QueuedJob* out_job);

/** The expiry sweep both nya_job_claim and nya_jobs_reap_expired share: past-deadline jobs to expired. */
NYA_INTERNAL NYA_Error _nya_jobs_reap(NYA_JobQueue* queue, s64 now, OUT u64* out_reaped) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_jobs_open_with_options(NYA_Arena* arena, NYA_Database* database, NYA_JobQueueOptions options, OUT NYA_JobQueue** out_queue) {
    nya_assert(arena != nullptr);
    nya_assert(database != nullptr);
    nya_assert(out_queue != nullptr);

    *out_queue = nullptr;

    NYA_ConstCString table = options.table != nullptr ? options.table : NYA_JOB_TABLE_DEFAULT;
    if (!_nya_jobs_table_valid(table)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a table identifier", table);

    NYA_JobQueue* queue = nya_arena_alloc(arena, sizeof(NYA_JobQueue));
    *queue              = (NYA_JobQueue){
                     .database             = database,
                     .arena                = arena,
                     .default_max_attempts = options.default_max_attempts != 0 ? options.default_max_attempts : NYA_JOB_DEFAULT_MAX_ATTEMPTS,
                     .backoff_base_ns      = options.backoff_base.ns > 0 ? options.backoff_base.ns : NYA_NS_PER_SECOND,
                     .backoff_cap_ns       = options.backoff_cap.ns > 0 ? options.backoff_cap.ns : NYA_NS_PER_HOUR,
                     .lease_ns             = options.lease.ns > 0 ? options.lease.ns : 30 * NYA_NS_PER_SECOND,
                     .jitter               = options.jitter < 0.0 ? 0.0 : (options.jitter > 1.0 ? 1.0 : options.jitter),
    };
    (void)snprintf(queue->table, sizeof(queue->table), "%s", table);

    // A base above the cap would double straight past it; hold it to the cap so the first delay is never longer than the ones after.
    if (queue->backoff_base_ns > queue->backoff_cap_ns) queue->backoff_base_ns = queue->backoff_cap_ns;

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_open");
    defer      nya_arena_destroy(scratch);

    // Two processes sharing one file are two workers; a claim meeting the other's write lock waits this long. Our own number, so it is interpolated.
    s64         busy_ms   = options.busy_timeout_ms == 0 ? 5000 : (options.busy_timeout_ms < 0 ? 0 : options.busy_timeout_ms);
    NYA_String* busy_sql  = nya_string_sprintf(scratch, "PRAGMA busy_timeout = " FMTs64, busy_ms);
    NYA_TRY(nya_sql_exec(database, nya_string_to_cstring(scratch, busy_sql)));

    // The schema. Callers' data is bound in the statements below; the only interpolated things here are the validated table name and this file's own state integers.
    NYA_String* create = nya_string_sprintf(
        scratch,
        "CREATE TABLE IF NOT EXISTS %s ("
        "id INTEGER PRIMARY KEY, "
        "kind TEXT NOT NULL, "
        "payload BLOB NOT NULL, "
        "state INTEGER NOT NULL, "
        "attempts INTEGER NOT NULL DEFAULT 0, "
        "max_attempts INTEGER NOT NULL, "
        "run_at INTEGER NOT NULL, "
        "deadline INTEGER NOT NULL DEFAULT 0, "
        "lease_expiry INTEGER NOT NULL DEFAULT 0, "
        "worker TEXT, "
        "unique_key TEXT, "
        "created INTEGER NOT NULL, "
        "updated INTEGER NOT NULL, "
        "last_error TEXT"
        ")",
        queue->table
    );
    NYA_TRY(nya_sql_exec(database, nya_string_to_cstring(scratch, create)));

    // The claim reads by (state, run_at), so an index on that pair keeps a busy queue from scanning.
    NYA_String* due_index = nya_string_sprintf(scratch, "CREATE INDEX IF NOT EXISTS %s_due ON %s (state, run_at)", queue->table, queue->table);
    NYA_TRY(nya_sql_exec(database, nya_string_to_cstring(scratch, due_index)));

    // The unique-jobs guarantee: a partial index of at most one row per key while active (pending/claimed), making a duplicate enqueue a no-op until the job leaves those states.
    NYA_String* unique_index = nya_string_sprintf(
        scratch,
        "CREATE UNIQUE INDEX IF NOT EXISTS %s_unique ON %s (unique_key) WHERE unique_key IS NOT NULL AND state IN (%d, %d)",
        queue->table,
        queue->table,
        NYA_JOB_STATE_PENDING,
        NYA_JOB_STATE_CLAIMED
    );
    NYA_TRY(nya_sql_exec(database, nya_string_to_cstring(scratch, unique_index)));

    *out_queue = queue;
    return NYA_OK;
}

void nya_jobs_close(NYA_JobQueue* queue) {
    // Nothing to free (arena memory, caller's connection); the handle is cleared so a use after close trips rather than reads a live queue.
    if (queue == nullptr) return;
    *queue = (NYA_JobQueue){ 0 };
}

NYA_Error nya_job_enqueue_with_options(
    NYA_JobQueue* queue, NYA_ConstCString kind, const u8* payload, u64 payload_size, OUT s64* out_id, NYA_JobOptions options
) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "enqueue on a closed queue");
    nya_assert(payload != nullptr || payload_size == 0, "null payload with a non-zero size");

    if (out_id != nullptr) *out_id = 0;
    if (kind == nullptr || kind[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a job needs a kind");

    s64 now          = _nya_jobs_now();
    s64 run_at       = options.run_at.ns != 0 ? options.run_at.ns : now;
    s64 deadline     = options.deadline.ns;  // zero means none
    u32 max_attempts = options.max_attempts != 0 ? options.max_attempts : queue->default_max_attempts;

    // A zero-length payload binds a non-null pointer so SQLite stores an empty blob rather than the forbidden NULL, which would come back null, not empty.
    const u8* data = payload != nullptr ? payload : (const u8*)"";

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_enqueue");
    defer      nya_arena_destroy(scratch);

    if (options.unique_key == nullptr) {
        // No key: every call is a new row. RETURNING is not needed — an INSERT sets last_insert_id.
        NYA_String* sql = nya_string_sprintf(
            scratch,
            "INSERT INTO %s (kind, payload, state, attempts, max_attempts, run_at, deadline, lease_expiry, worker, unique_key, created, updated, last_error) "
            "VALUES (?, ?, %d, 0, ?, ?, ?, 0, NULL, NULL, ?, ?, NULL)",
            queue->table,
            NYA_JOB_STATE_PENDING
        );

        NYA_SqlValue values[] = {
            nya_sql_text(kind),        nya_sql_blob(data, payload_size), nya_sql_s64((s64)max_attempts),
            nya_sql_s64(run_at),       nya_sql_s64(deadline),            nya_sql_s64(now),
            nya_sql_s64(now),
        };
        NYA_SqlResult result = { 0 };
        NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values), &result));

        if (out_id != nullptr) *out_id = result.last_insert_id;
        return NYA_OK;
    }

    // Keyed: an active job under this key wins; `replace` overwrites payload and schedule but only while pending, never while a worker holds it. The conflict target matches the partial index.
    NYA_String* conflict_sql = nullptr;
    if (options.replace) {
        conflict_sql = nya_string_sprintf(
            scratch,
            "DO UPDATE SET payload = excluded.payload, max_attempts = excluded.max_attempts, "
            "run_at = excluded.run_at, deadline = excluded.deadline, updated = excluded.updated, "
            "attempts = 0, last_error = NULL WHERE %s.state = %d",
            queue->table,
            NYA_JOB_STATE_PENDING
        );
    } else {
        conflict_sql = nya_string_from(scratch, "DO NOTHING");
    }

    NYA_String* sql = nya_string_sprintf(
        scratch,
        "INSERT INTO %s (kind, payload, state, attempts, max_attempts, run_at, deadline, lease_expiry, worker, unique_key, created, updated, last_error) "
        "VALUES (?, ?, %d, 0, ?, ?, ?, 0, NULL, ?, ?, ?, NULL) "
        "ON CONFLICT (unique_key) WHERE unique_key IS NOT NULL AND state IN (%d, %d) %.*s",
        queue->table,
        NYA_JOB_STATE_PENDING,
        NYA_JOB_STATE_PENDING,
        NYA_JOB_STATE_CLAIMED,
        (int)conflict_sql->length,
        conflict_sql->items
    );

    NYA_SqlValue values[] = {
        nya_sql_text(kind),  nya_sql_blob(data, payload_size), nya_sql_s64((s64)max_attempts), nya_sql_s64(run_at),
        nya_sql_s64(deadline), nya_sql_text(options.unique_key), nya_sql_s64(now),               nya_sql_s64(now),
    };
    NYA_TRY(nya_sql_exec_bound(queue->database, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values)));

    // The surviving job's id, whether this call inserted it or an earlier one did: a no-op conflict wrote no row, so last_insert_id would be stale.
    if (out_id != nullptr) {
        NYA_String* find = nya_string_sprintf(
            scratch, "SELECT id FROM %s WHERE unique_key = ? AND state IN (%d, %d) ORDER BY id LIMIT 1", queue->table, NYA_JOB_STATE_PENDING,
            NYA_JOB_STATE_CLAIMED
        );
        NYA_SqlValue  key[]  = { nya_sql_text(options.unique_key) };
        NYA_SqlResult result = { 0 };
        NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, find), key, 1, &result));
        if (result.rows->length > 0) *out_id = nya_object_get(result.rows->items[0], "id")->as_s64;
    }

    return NYA_OK;
}

NYA_Error nya_job_claim(NYA_JobQueue* queue, NYA_ConstCString worker_id, NYA_Arena* out_arena, OUT NYA_QueuedJob* out_job, OUT b8* out_claimed) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "claim on a closed queue");
    nya_assert(worker_id != nullptr);
    nya_assert(out_arena != nullptr);
    nya_assert(out_job != nullptr);
    nya_assert(out_claimed != nullptr);

    *out_job     = (NYA_QueuedJob){ 0 };
    *out_claimed = false;

    s64 now = _nya_jobs_now();

    // A past-deadline job is moved out of the way before we look for one to run, so it never blocks the head of the queue and a claim never hands one back.
    NYA_TRY(_nya_jobs_reap(queue, now, nullptr));

    s64 lease_expiry = now + queue->lease_ns;

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_claim");
    defer      nya_arena_destroy(scratch);

    // The queue's whole race-safety: the row is chosen and flipped to claimed in one statement under SQLite's write lock, so the race loser re-evaluates the subquery and picks another job. Pending or lease-expired jobs are fair game; past-deadline ones were reaped above.
    NYA_String* sql = nya_string_sprintf(
        scratch,
        "UPDATE %s SET state = %d, worker = ?, attempts = attempts + 1, lease_expiry = ?, updated = ? "
        "WHERE id = ("
        "SELECT id FROM %s WHERE run_at <= ? AND (deadline = 0 OR deadline > ?) "
        "AND (state = %d OR (state = %d AND lease_expiry <= ?)) "
        "ORDER BY run_at ASC, id ASC LIMIT 1"
        ") "
        "RETURNING id, kind, payload, attempts, max_attempts, run_at, deadline, created, lease_expiry",
        queue->table,
        NYA_JOB_STATE_CLAIMED,
        queue->table,
        NYA_JOB_STATE_PENDING,
        NYA_JOB_STATE_CLAIMED
    );

    NYA_SqlValue values[] = {
        nya_sql_text(worker_id), nya_sql_s64(lease_expiry), nya_sql_s64(now),  // the SET
        nya_sql_s64(now),        nya_sql_s64(now),          nya_sql_s64(now),  // run_at <=, deadline >, lease_expiry <=
    };
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values), &result));

    if (result.rows->length == 0) return NYA_OK;  // nothing due; not an error

    _nya_jobs_row_read(result.rows->items[0], out_arena, out_job);
    *out_claimed = true;
    return NYA_OK;
}

NYA_Error nya_job_complete(NYA_JobQueue* queue, s64 job_id) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "complete on a closed queue");

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_complete");
    defer      nya_arena_destroy(scratch);

    // Only a claimed job completes: a completion for one not in flight affects no row and is reported as not found, so a real completion is told from a no-op.
    NYA_String* sql = nya_string_sprintf(
        scratch, "UPDATE %s SET state = %d, worker = NULL, lease_expiry = 0, updated = ? WHERE id = ? AND state = %d", queue->table,
        NYA_JOB_STATE_DONE, NYA_JOB_STATE_CLAIMED
    );

    NYA_SqlValue  values[] = { nya_sql_s64(_nya_jobs_now()), nya_sql_s64(job_id) };
    NYA_SqlResult result   = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values), &result));

    if (result.rows_affected == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no claimed job " FMTs64, job_id);
    return NYA_OK;
}

NYA_Error nya_job_fail(NYA_JobQueue* queue, s64 job_id, b8 retryable) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "fail on a closed queue");

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_fail");
    defer      nya_arena_destroy(scratch);

    // Attempt count and ceiling decide retry versus end of the road; read for the one claimed job, since a job not in flight is not one this call can fail.
    NYA_String* read_sql = nya_string_sprintf(scratch, "SELECT attempts, max_attempts FROM %s WHERE id = ? AND state = %d", queue->table, NYA_JOB_STATE_CLAIMED);
    NYA_SqlValue  key[]  = { nya_sql_s64(job_id) };
    NYA_SqlResult read   = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, read_sql), key, 1, &read));

    if (read.rows->length == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no claimed job " FMTs64, job_id);

    u32 attempts     = (u32)nya_object_get(read.rows->items[0], "attempts")->as_s64;
    u32 max_attempts = (u32)nya_object_get(read.rows->items[0], "max_attempts")->as_s64;
    s64 now          = _nya_jobs_now();

    // Retry only when asked and attempts remain, else dead-letter; attempts already counts this run, so `attempts < max_attempts` means one try is left after this.
    b8 retry = retryable && attempts < max_attempts;

    if (retry) {
        s64         delay      = _nya_jobs_backoff_ns(queue, attempts, job_id, now);
        s64         next_run   = now + delay;
        NYA_String* sql        = nya_string_sprintf(
            scratch, "UPDATE %s SET state = %d, worker = NULL, lease_expiry = 0, run_at = ?, updated = ?, last_error = ? WHERE id = ?", queue->table,
            NYA_JOB_STATE_PENDING
        );
        NYA_SqlValue  values[] = { nya_sql_s64(next_run), nya_sql_s64(now), nya_sql_text("retryable failure"), nya_sql_s64(job_id) };
        NYA_SqlResult result   = { 0 };
        NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values), &result));
        return NYA_OK;
    }

    NYA_ConstCString reason = retryable ? "max attempts reached" : "unrecoverable failure";
    NYA_String*      sql    = nya_string_sprintf(
        scratch, "UPDATE %s SET state = %d, worker = NULL, lease_expiry = 0, updated = ?, last_error = ? WHERE id = ?", queue->table,
        NYA_JOB_STATE_DEAD
    );
    NYA_SqlValue  values[] = { nya_sql_s64(now), nya_sql_text(reason), nya_sql_s64(job_id) };
    NYA_SqlResult result   = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values), &result));
    return NYA_OK;
}

NYA_Error nya_job_get(NYA_JobQueue* queue, s64 job_id, NYA_Arena* out_arena, OUT NYA_QueuedJob* out_job) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "get on a closed queue");
    nya_assert(out_arena != nullptr);
    nya_assert(out_job != nullptr);

    *out_job = (NYA_QueuedJob){ 0 };

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_get");
    defer      nya_arena_destroy(scratch);

    NYA_String* sql = nya_string_sprintf(
        scratch, "SELECT id, kind, payload, attempts, max_attempts, run_at, deadline, created, lease_expiry FROM %s WHERE id = ?", queue->table
    );

    NYA_SqlValue  key[]  = { nya_sql_s64(job_id) };
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), key, 1, &result));

    if (result.rows->length == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no job " FMTs64, job_id);

    _nya_jobs_row_read(result.rows->items[0], out_arena, out_job);
    return NYA_OK;
}

NYA_Error nya_jobs_count(NYA_JobQueue* queue, NYA_JobState state, OUT u64* out_count) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "count on a closed queue");
    nya_assert(out_count != nullptr);

    *out_count = 0;

    if (state < 0 || state >= NYA_JOB_STATE_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unknown job state %d", (int)state);

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_count");
    defer      nya_arena_destroy(scratch);

    NYA_String*   sql    = nya_string_sprintf(scratch, "SELECT COUNT(*) AS n FROM %s WHERE state = ?", queue->table);
    NYA_SqlValue  arg[]  = { nya_sql_s64((s64)state) };
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), arg, 1, &result));

    if (result.rows->length > 0) *out_count = (u64)nya_object_get(result.rows->items[0], "n")->as_s64;
    return NYA_OK;
}

NYA_Error nya_jobs_stats(NYA_JobQueue* queue, OUT NYA_JobStats* out_stats) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "stats on a closed queue");
    nya_assert(out_stats != nullptr);

    *out_stats = (NYA_JobStats){ 0 };

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_stats");
    defer      nya_arena_destroy(scratch);

    NYA_String*   sql    = nya_string_sprintf(scratch, "SELECT state, COUNT(*) AS n FROM %s GROUP BY state", queue->table);
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), nullptr, 0, &result));

    nya_array_foreach (result.rows, row) {
        NYA_JobState state = (NYA_JobState)nya_object_get(*row, "state")->as_s64;
        u64          n     = (u64)nya_object_get(*row, "n")->as_s64;

        switch (state) {
            case NYA_JOB_STATE_PENDING: out_stats->pending = n; break;
            case NYA_JOB_STATE_CLAIMED: out_stats->claimed = n; break;
            case NYA_JOB_STATE_DONE:    out_stats->done = n; break;
            case NYA_JOB_STATE_DEAD:    out_stats->dead = n; break;
            case NYA_JOB_STATE_EXPIRED: out_stats->expired = n; break;
            case NYA_JOB_STATE_COUNT:
            default:                    break;  // a state this build does not know: counted only in the total
        }
        out_stats->total += n;
    }

    return NYA_OK;
}

NYA_Error nya_jobs_reap_expired(NYA_JobQueue* queue, OUT u64* out_reaped) {
    nya_assert(queue != nullptr);
    nya_assert(queue->database != nullptr, "reap on a closed queue");

    return _nya_jobs_reap(queue, _nya_jobs_now(), out_reaped);
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

b8 _nya_jobs_table_valid(NYA_ConstCString name) {
    if (name == nullptr) return false;

    u64 length = strlen(name);
    if (length == 0 || length >= NYA_JOB_TABLE_MAX) return false;

    // [A-Za-z_] to start, [A-Za-z0-9_] after: the identifier grammar db_orm.h and db_blob.h hold a table name to, since it is never a bound parameter.
    for (u64 i = 0; i < length; i++) {
        char c      = name[i];
        b8   letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        b8   digit  = c >= '0' && c <= '9';
        if (i == 0 ? !letter : !(letter || digit)) return false;
    }

    return true;
}

s64 _nya_jobs_now(void) {
    // Through nya_instant_now, so a test's simulated clock measures deadlines and backoff and a run replays the same per seed. See base_clock_instant.h.
    return nya_instant_now().ns;
}

u64 _nya_jobs_hash(u64 x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

s64 _nya_jobs_backoff_ns(const NYA_JobQueue* queue, u32 attempts, s64 job_id, s64 now) {
    s64 cap = queue->backoff_cap_ns;
    s64 delay = queue->backoff_base_ns;

    // base * 2^(attempt-1): one base, then two, four, doubling until the cap, where it stops, so it can never overflow s64.
    u32 doublings = attempts > 0 ? attempts - 1 : 0;
    for (u32 i = 0; i < doublings; i++) {
        if (delay >= cap) break;
        delay <<= 1;
        if (delay < 0) { delay = cap; break; }  // overflow guard, though the cap check above precludes it
    }
    if (delay > cap) delay = cap;

    // Jitter (off by default) shaves up to `jitter` off the delay so a herd does not retry in lockstep; the spread depends only on job, attempt and clock, so a replay stays deterministic but loses strict monotonicity.
    if (queue->jitter > 0.0 && delay > 0) {
        u64 bits = _nya_jobs_hash((u64)job_id ^ (u64)now ^ ((u64)attempts << 32));
        f64 unit = (f64)(bits >> 11) * (1.0 / 9007199254740992.0);  // [0, 1)
        s64 shave = (s64)((f64)delay * queue->jitter * unit);
        delay -= shave;
        if (delay < 0) delay = 0;
    }

    return delay;
}

void _nya_jobs_row_read(NYA_Object* row, NYA_Arena* out_arena, OUT NYA_QueuedJob* out_job) {
    nya_assert(row != nullptr);
    nya_assert(out_arena != nullptr);
    nya_assert(out_job != nullptr);

    out_job->id           = nya_object_get(row, "id")->as_s64;
    out_job->attempts     = (u32)nya_object_get(row, "attempts")->as_s64;
    out_job->max_attempts = (u32)nya_object_get(row, "max_attempts")->as_s64;
    out_job->run_at       = (NYA_Instant){ .ns = nya_object_get(row, "run_at")->as_s64 };
    out_job->deadline     = (NYA_Instant){ .ns = nya_object_get(row, "deadline")->as_s64 };
    out_job->created      = (NYA_Instant){ .ns = nya_object_get(row, "created")->as_s64 };
    out_job->lease_expiry = (NYA_Instant){ .ns = nya_object_get(row, "lease_expiry")->as_s64 };

    NYA_ConstCString kind = nya_object_get(row, "kind")->as_string;
    out_job->kind         = nya_string_to_cstring(out_arena, nya_string_sprintf(out_arena, "%s", kind != nullptr ? kind : ""));

    // The blob comes back base64 (see db_sql.c's row builder); decode into the caller's arena, a zero-length payload keeping a non-null pointer so it reads as empty, not missing.
    NYA_Value*  payload = nya_object_get(row, "payload");
    NYA_String* decoded = nya_string_create(out_arena);
    if (payload != nullptr && payload->type == NYA_TYPE_STRING) {
        nya_base64_decode(decoded, (const u8*)payload->as_string, strlen(payload->as_string));
    }

    u8* bytes = nya_arena_alloc(out_arena, decoded->length > 0 ? decoded->length : 1);
    memcpy(bytes, decoded->items, decoded->length);

    out_job->payload      = bytes;
    out_job->payload_size = decoded->length;
}

NYA_Error _nya_jobs_reap(NYA_JobQueue* queue, s64 now, OUT u64* out_reaped) {
    if (out_reaped != nullptr) *out_reaped = 0;

    NYA_Arena* scratch = nya_arena_create(.name = "jobs_reap");
    defer      nya_arena_destroy(scratch);

    // A job with a passed deadline that has not finished is moved to expired (never run); both pending and claimed qualify, so an in-flight claim past its deadline is abandoned rather than completed late.
    NYA_String* sql = nya_string_sprintf(
        scratch, "UPDATE %s SET state = %d, worker = NULL, lease_expiry = 0, updated = ? WHERE deadline != 0 AND deadline <= ? AND state IN (%d, %d)",
        queue->table, NYA_JOB_STATE_EXPIRED, NYA_JOB_STATE_PENDING, NYA_JOB_STATE_CLAIMED
    );

    NYA_SqlValue  values[] = { nya_sql_s64(now), nya_sql_s64(now) };
    NYA_SqlResult result   = { 0 };
    NYA_TRY(nya_sql_query(queue->database, scratch, nya_string_to_cstring(scratch, sql), values, nya_carray_length(values), &result));

    if (out_reaped != nullptr) *out_reaped = result.rows_affected;
    return NYA_OK;
}
