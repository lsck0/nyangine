#include "nyangine-core/nyangine.h"

/*
 * The database half of the engine action set, registered by nya_simulation_actions_add when the build
 * has the db module and a save root. What it does and what it asserts is in testing_actions.h.
 */

#if defined(NYA_TESTING) && defined(NYA_MODULE_DB)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The file, under the save root, and the rollback journal SQLite keeps beside it. */
#define _NYA_SIMULATION_DB_FILE    "simulation.db"
#define _NYA_SIMULATION_DB_JOURNAL "simulation.db-journal"

#define _NYA_SIMULATION_DB_USER     "simulated"
#define _NYA_SIMULATION_DB_PASSWORD "a simulated password, long enough"

/** Who the simulated requests come from: TEST-NET-1, RFC 5737, which routes nowhere. */
#define _NYA_SIMULATION_DB_ADDRESS "192.0.2.1"

enum {
    /** Jobs one run enqueues. A drain completes at most this many, so it is also the drain's loop bound. */
    _NYA_SIMULATION_DB_JOB_MAX = 256,

    /** Rows a crash transaction writes. Enough that a torn commit would show as a count between the two ends. */
    _NYA_SIMULATION_DB_CRASH_ROWS_MAX = 8,

    /** Calls a power cut lets through first. Past a commit's journal and page writes, so the cut lands anywhere in it. */
    _NYA_SIMULATION_DB_CRASH_AFTER_MAX = 16,

    /** Calls an armed fault answers before it disarms itself; a quarter of the arms answer until healed instead. */
    _NYA_SIMULATION_DB_FAULT_COUNT_MAX = 8,

    /** Longest stall, in simulated milliseconds: past the lease, so a stalled claim can lose its job. */
    _NYA_SIMULATION_DB_STALL_MS_MAX = 40'000,

    /** The queue's lease and backoff ceiling, short so a drain only has to move the clock by minutes. */
    _NYA_SIMULATION_DB_LEASE_S       = 30,
    _NYA_SIMULATION_DB_BACKOFF_CAP_S = 60,

    /** Retries a job gets. Far past what the faults can burn, so a dead job is a finding and not bad luck. */
    _NYA_SIMULATION_DB_JOB_ATTEMPTS = 64,
};

/** The seal secret the accounts routes demand. The ASCII of what it is, since nothing is ever sealed with it that matters. */
NYA_INTERNAL const u8 _NYA_SIMULATION_DB_SECRET[] = "simulation seal secret, not real";

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    b8 initialized;

    /** Where the file is, for the lifetime of the set. */
    NYA_Arena*  arena;
    NYA_CString path;

    /** The connection, the queue and the accounts tables: everything a restart throws away. */
    NYA_Arena*            connection;
    NYA_Database*         database;
    NYA_JobQueue*         queue;
    const NYA_HttpRouter* accounts;

    /** Per action, reset at the top of each. */
    NYA_Arena* scratch;

    /** The session the HTTP action presents, issued once while the disk was healthy. */
    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES];

    /** Per kind, whether it answers every call until healed: a BUSY or a read error like that means /readyz must say down. */
    b8 until_healed[NYA_DB_FAULT_KIND_COUNT];

    u64 enqueue_attempted;
    u64 enqueue_acknowledged;

    u8               body[NYA_HTTP_MAX_RESPONSE_BYTES];
    NYA_HttpResponse response;
} _NYA_SimulationDb;

NYA_INTERNAL _NYA_SimulationDb _NYA_SIMULATION_DB = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The busy handler's backoff and a stall, moved onto the simulated clock rather than waited out. */
NYA_INTERNAL void _nya_simulation_db_sleep(void* context, NYA_Duration duration) {
    nya_simulation_advance(context, (u64)duration.ns);
}

/** What /readyz asks: a read that reaches the file. */
NYA_INTERNAL b8 _nya_simulation_db_ready(void* user) {
    nya_unused(user);

    return _NYA_SIMULATION_DB.database != nullptr && nya_sql_ping(_NYA_SIMULATION_DB.database).ok;
}

/** Whether the disk tells the truth: nothing armed but a stall, which delays and never fails. */
NYA_INTERNAL b8 _nya_simulation_db_healthy(void) {
    for (u32 kind = 0; kind < NYA_DB_FAULT_KIND_COUNT; kind++) {
        if (kind != NYA_DB_FAULT_STALL && nya_db_fault_armed((NYA_DbFaultKind)kind)) return false;
    }

    return true;
}

/** Rows in the table the writes go to, or false when the read failed. */
NYA_INTERNAL b8 _nya_simulation_db_rows(OUT u64* out_rows) {
    NYA_SqlResult result = { 0 };
    if (!nya_sql_query(_NYA_SIMULATION_DB.database, _NYA_SIMULATION_DB.scratch, "SELECT COUNT(*) AS n FROM simulated_rows", nullptr, 0, &result).ok) return false;

    *out_rows = (u64)nya_object_get(result.rows->items[0], "n")->as_s64;
    return true;
}

/** One row, stamped with the step that wrote it. */
NYA_INTERNAL NYA_Error _nya_simulation_db_row_write(NYA_SimulationRun* run) {
    NYA_SqlValue step[] = { nya_sql_s64((s64)run->step) };
    return nya_sql_exec_bound(_NYA_SIMULATION_DB.database, "INSERT INTO simulated_rows (step) VALUES (?)", step, 1);
}

/** Opens the file, the queue and the accounts routes over it. The first open also makes the account and its session. */
NYA_INTERNAL NYA_Error _nya_simulation_db_open(void) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;

    NYA_TRY(nya_sql_open(db->connection, db->path, &db->database, .vfs = NYA_DB_FAULT_VFS));
    NYA_TRY(nya_sql_exec(db->database, "CREATE TABLE IF NOT EXISTS simulated_rows (id INTEGER PRIMARY KEY, step INTEGER NOT NULL)"));

    NYA_TRY(nya_jobs_open(
        db->connection,
        db->database,
        &db->queue,
        .default_max_attempts = _NYA_SIMULATION_DB_JOB_ATTEMPTS,
        .lease                = nya_duration_from_s(_NYA_SIMULATION_DB_LEASE_S),
        .backoff_cap          = nya_duration_from_s(_NYA_SIMULATION_DB_BACKOFF_CAP_S)
    ));

    NYA_TRY(nya_accounts_open(db->connection, db->database));

    db->accounts = nya_http_accounts_open((NYA_HttpAccountsConfig){
        .arena                  = db->connection,
        .database               = db->database,
        .totp_issuer            = "simulation",
        .login_seal_secret      = _NYA_SIMULATION_DB_SECRET,
        .login_seal_secret_size = sizeof(_NYA_SIMULATION_DB_SECRET) - 1,
        .session_same_site      = NYA_HTTP_SAME_SITE_STRICT,
    });
    if (db->accounts == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the accounts routes did not mount");

    if (db->token[0] != '\0') return NYA_OK;

    // argon2id is slow on purpose, so the one account is made once per run and survives every restart in the file.
    NYA_AccountUser    user    = { 0 };
    NYA_AccountSession session = { 0 };
    NYA_TRY(nya_account_create(db->connection, _NYA_SIMULATION_DB_USER, _NYA_SIMULATION_DB_PASSWORD, &user));
    NYA_TRY(nya_account_session_issue(db->connection, user.id, _NYA_SIMULATION_DB_ADDRESS, "simulation", &session));

    (void)snprintf(db->token, sizeof(db->token), "%s", session.token);
    return NYA_OK;
}

/** Lets go of everything the open made, the way a process dying lets go of it: whatever the disk is doing. */
NYA_INTERNAL void _nya_simulation_db_close(void) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;

    nya_http_accounts_close();
    nya_accounts_close();
    nya_jobs_close(db->queue);
    nya_sql_close(db->database);

    db->database = nullptr;
    db->queue    = nullptr;
    db->accounts = nullptr;

    nya_arena_free_all(db->connection);
}

/** One GET through the accounts and health routers, in process, with the session cookie. Returns the status. */
NYA_INTERNAL NYA_HttpStatus _nya_simulation_db_get(NYA_ConstCString target) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;

    NYA_HttpRequest request = { .method = NYA_HTTP_METHOD_GET, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(target, strlen(target), &request.target, &failure), "while building a simulated request");
    (void)snprintf(request.path, sizeof(request.path), "%.*s", (int)request.target.path.length, request.target.text + request.target.path.offset);

    // lowercase, as the parser stores every header name.
    NYA_HttpHeader* cookie = &request.headers[request.header_count++];
    (void)snprintf(cookie->name, sizeof(cookie->name), "cookie");
    (void)snprintf(cookie->value, sizeof(cookie->value), "%s=%s", NYA_HTTP_SESSION_COOKIE, db->token);

    const NYA_HttpRouter* routers[] = { db->accounts, nya_http_health_router() };

    NYA_HttpExchange exchange = {
        .request  = &request,
        .response = &db->response,
        .arena    = db->scratch,
        .now_s    = (u64)(nya_instant_now().ns / NYA_NS_PER_SECOND),
        .address  = _NYA_SIMULATION_DB_ADDRESS,
    };

    nya_http_response_reset(&db->response);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_simulation_db_do_write(NYA_SimulationRun* run) {
    b8 healthy = _nya_simulation_db_healthy();
    nya_arena_free_all(_NYA_SIMULATION_DB.scratch);

    NYA_Error written = _nya_simulation_db_row_write(run);
    if (!written.ok && healthy) nya_simulation_fail(run, "a write failed on a healthy disk: %s", written.message);
}

NYA_INTERNAL void _nya_simulation_db_do_read(NYA_SimulationRun* run) {
    b8 healthy = _nya_simulation_db_healthy();
    nya_arena_free_all(_NYA_SIMULATION_DB.scratch);

    u64 rows = 0;
    if (!_nya_simulation_db_rows(&rows) && healthy) nya_simulation_fail(run, "a read failed on a healthy disk");
}

NYA_INTERNAL void _nya_simulation_db_do_job_enqueue(NYA_SimulationRun* run) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;
    b8                 healthy = _nya_simulation_db_healthy();

    if (db->enqueue_attempted >= _NYA_SIMULATION_DB_JOB_MAX) return;

    db->enqueue_attempted++;

    s64       id       = 0;
    NYA_Error enqueued = nya_job_enqueue(db->queue, "simulated", nullptr, 0, &id);

    if (enqueued.ok) {
        db->enqueue_acknowledged++;
    } else if (healthy) {
        nya_simulation_fail(run, "an enqueue failed on a healthy disk: %s", enqueued.message);
    }
}

NYA_INTERNAL void _nya_simulation_db_do_job_work(NYA_SimulationRun* run) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;
    b8                 healthy = _nya_simulation_db_healthy();
    nya_arena_free_all(db->scratch);

    NYA_QueuedJob job     = { 0 };
    b8            claimed = false;
    NYA_Error     claim   = nya_job_claim(db->queue, "simulation", db->scratch, &job, &claimed);

    if (!claim.ok && healthy) nya_simulation_fail(run, "a claim failed on a healthy disk: %s", claim.message);
    if (!claim.ok || !claimed) return;

    // the job's work touches the same disk, so a fault fails it and the queue has to bring it back.
    NYA_Error work    = _nya_simulation_db_row_write(run);
    NYA_Error settled = work.ok ? nya_job_complete(db->queue, job.id) : nya_job_fail(db->queue, job.id, true);

    // a stall can outlast the lease, and a completion for a job whose lease ran out is refused as not found.
    if (!settled.ok && settled.kind != NYA_ERROR_NOT_FOUND && healthy) {
        nya_simulation_fail(run, "a job could not be settled on a healthy disk: %s", settled.message);
    }
}

NYA_INTERNAL void _nya_simulation_db_do_jobs_drain(NYA_SimulationRun* run) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;
    if (!_nya_simulation_db_healthy()) return;

    nya_arena_free_all(db->scratch);

    // past every lease and every backoff, so whatever a fault left claimed or rescheduled is due now.
    nya_simulation_advance(run, (u64)(_NYA_SIMULATION_DB_LEASE_S + _NYA_SIMULATION_DB_BACKOFF_CAP_S + 1) * (u64)NYA_NS_PER_SECOND);

    for (u32 i = 0; i <= _NYA_SIMULATION_DB_JOB_MAX; i++) {
        NYA_QueuedJob job     = { 0 };
        b8            claimed = false;

        if (!nya_job_claim(db->queue, "simulation", db->scratch, &job, &claimed).ok) {
            nya_simulation_fail(run, "a drain could not claim");
            return;
        }
        if (!claimed) break;

        if (!_nya_simulation_db_row_write(run).ok || !nya_job_complete(db->queue, job.id).ok) {
            nya_simulation_fail(run, "a drain could not finish job %lld", (long long)job.id);
            return;
        }
    }

    NYA_JobStats stats = { 0 };
    if (!nya_jobs_stats(db->queue, &stats).ok) {
        nya_simulation_fail(run, "a drain could not read the queue");
        return;
    }

    if (stats.done != stats.total) {
        nya_simulation_fail(run, "after the faults cleared, %llu of %llu jobs never finished (%llu pending, %llu claimed, %llu dead)",
                            (unsigned long long)(stats.total - stats.done), (unsigned long long)stats.total, (unsigned long long)stats.pending,
                            (unsigned long long)stats.claimed, (unsigned long long)stats.dead);
    }

    // an acknowledged enqueue is on disk; one that failed may or may not be, since a failed commit can still have reached the file.
    if (stats.total < db->enqueue_acknowledged || stats.total > db->enqueue_attempted) {
        nya_simulation_fail(run, "the queue holds %llu jobs where %llu were acknowledged of %llu tried", (unsigned long long)stats.total,
                            (unsigned long long)db->enqueue_acknowledged, (unsigned long long)db->enqueue_attempted);
    }
}

NYA_INTERNAL void _nya_simulation_db_do_http_session(NYA_SimulationRun* run) {
    b8 healthy = _nya_simulation_db_healthy();
    nya_arena_free_all(_NYA_SIMULATION_DB.scratch);

    NYA_HttpStatus status = _nya_simulation_db_get(NYA_HTTP_ACCOUNTS_SESSION_PATH);

    // a lying disk is the server's trouble and says so as 503; a 401 would sign a real user out, a 500 blames the code.
    if (healthy ? status != NYA_HTTP_STATUS_OK : (status != NYA_HTTP_STATUS_OK && status != NYA_HTTP_STATUS_SERVICE_UNAVAILABLE)) {
        nya_simulation_fail(run, "GET %s answered %d with the disk %s", NYA_HTTP_ACCOUNTS_SESSION_PATH, (s32)status,
                            healthy ? "healthy" : "faulted");
    }
}

NYA_INTERNAL void _nya_simulation_db_do_http_ready(NYA_SimulationRun* run) {
    b8 healthy = _nya_simulation_db_healthy();
    nya_arena_free_all(_NYA_SIMULATION_DB.scratch);

    NYA_HttpStatus status = _nya_simulation_db_get(NYA_HTTP_READYZ_PATH);

    if (healthy && status != NYA_HTTP_STATUS_OK) nya_simulation_fail(run, "/readyz answered %d on a healthy disk", (s32)status);
    b8 down = _NYA_SIMULATION_DB.until_healed[NYA_DB_FAULT_BUSY] || _NYA_SIMULATION_DB.until_healed[NYA_DB_FAULT_IOERR_READ];
    if (down && status != NYA_HTTP_STATUS_SERVICE_UNAVAILABLE) {
        nya_simulation_fail(run, "/readyz answered %d while no read could reach the file", (s32)status);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FAULTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_simulation_db_fault_arm(NYA_SimulationRun* run) {
    // every kind but the power cut, which is its own fault below because it ends in a restart.
    NYA_DbFaultKind kind  = (NYA_DbFaultKind)nya_simulation_below(run, NYA_DB_FAULT_POWER_LOSS);
    u32             count = nya_simulation_chance(run, 25) ? 0 : 1 + (u32)nya_simulation_below(run, _NYA_SIMULATION_DB_FAULT_COUNT_MAX);
    u32             after = nya_simulation_chance(run, 50) ? 0 : (u32)nya_simulation_below(run, _NYA_SIMULATION_DB_FAULT_COUNT_MAX);

    nya_db_fault_arm(.kind = kind, .after = after, .count = count, .stall = nya_duration_from_ms(1 + (s64)nya_simulation_below(run, _NYA_SIMULATION_DB_STALL_MS_MAX)));

    // a re-arm replaces whatever the kind had, its verdict included.
    _NYA_SIMULATION_DB.until_healed[kind] = count == 0 && after == 0;

    // a disk no read can reach is asked about at once, so every such arm is checked and not only the few a later poll lands on.
    if (_NYA_SIMULATION_DB.until_healed[kind] && (kind == NYA_DB_FAULT_BUSY || kind == NYA_DB_FAULT_IOERR_READ)) _nya_simulation_db_do_http_ready(run);
}

NYA_INTERNAL void _nya_simulation_db_fault_heal(NYA_SimulationRun* run) {
    nya_unused(run);

    nya_db_fault_disarm_all();
    nya_memset(_NYA_SIMULATION_DB.until_healed, 0, sizeof(_NYA_SIMULATION_DB.until_healed));
}

NYA_INTERNAL void _nya_simulation_db_fault_power_loss(NYA_SimulationRun* run) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;
    nya_arena_free_all(db->scratch);

    _nya_simulation_db_fault_heal(run);

    u64 before = 0;
    if (!_nya_simulation_db_rows(&before)) {
        nya_simulation_fail(run, "a healed disk could not be read before a power cut");
        return;
    }

    // the cut lands anywhere in one transaction: before its journal, between its pages, after its commit.
    u64 hits = nya_db_fault_hits();
    u32 rows = 1 + (u32)nya_simulation_below(run, _NYA_SIMULATION_DB_CRASH_ROWS_MAX);
    nya_db_fault_arm(.kind = NYA_DB_FAULT_POWER_LOSS, .after = (u32)nya_simulation_below(run, _NYA_SIMULATION_DB_CRASH_AFTER_MAX));

    b8 committed = nya_sql_transaction_begin(db->database).ok;
    for (u32 i = 0; i < rows && committed; i++) committed = _nya_simulation_db_row_write(run).ok;
    committed = committed && nya_sql_transaction_commit(db->database).ok;

    b8 cut = nya_db_fault_hits() != hits;

    // the close happens with the power still out, so whatever it tries to tidy is lost like the rest.
    _nya_simulation_db_close();
    nya_db_fault_disarm_all();

    NYA_Error reopened = _nya_simulation_db_open();
    if (!reopened.ok) {
        nya_simulation_fail(run, "the database did not reopen after a power cut: %s", reopened.message);
        return;
    }

    nya_arena_free_all(db->scratch);

    NYA_SqlResult check = { 0 };
    if (!nya_sql_query(db->database, db->scratch, "PRAGMA integrity_check", nullptr, 0, &check).ok || check.rows->length != 1) {
        nya_simulation_fail(run, "integrity_check did not run after a power cut");
        return;
    }

    NYA_Value* verdict = nya_object_get(check.rows->items[0], "integrity_check");
    if (verdict == nullptr || verdict->type != NYA_TYPE_STRING || !nya_string_equals(verdict->as_string, "ok")) {
        nya_simulation_fail(run, "the file is corrupt after a power cut");
        return;
    }

    u64 after = 0;
    if (!_nya_simulation_db_rows(&after)) {
        nya_simulation_fail(run, "a recovered file could not be read");
        return;
    }

    // atomic either way; durable when the power never touched it.
    if (after != before && after != before + rows) nya_simulation_fail(run, "a power cut left %llu of %u rows of one transaction", (unsigned long long)(after - before), rows);
    if (committed && !cut && after != before + rows) nya_simulation_fail(run, "a commit the power cut never reached is gone");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INVARIANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_simulation_db_check_ready(NYA_SimulationRun* run) {
    // after any fault, once the disk heals, the same connection has to recover without being reopened.
    if (_nya_simulation_db_healthy() && !_nya_simulation_db_ready(nullptr)) nya_simulation_fail(run, "the database is unreachable on a healthy disk");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_simulation_db_add(NYA_SimulationRun* run) {
    nya_assert(!_NYA_SIMULATION_DB.initialized, "the database action set is already registered");

    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;

    *db = (_NYA_SimulationDb){
        .initialized = true,
        .arena       = nya_arena_create(.name = "simulation_db"),
        .connection  = nya_arena_create(.name = "simulation_db_connection"),
        .scratch     = nya_arena_create(.name = "simulation_db_scratch"),
    };

    NYA_String* path = nya_save_path(db->arena, _NYA_SIMULATION_DB_FILE);
    nya_assert(path != nullptr, "the database actions are only added with a save root");
    db->path = nya_string_to_cstring(db->arena, path);

    // a file left by a run that crashed would replay differently, so every run starts from nothing.
    (void)nya_save_delete(_NYA_SIMULATION_DB_FILE);
    (void)nya_save_delete(_NYA_SIMULATION_DB_JOURNAL);

    nya_db_fault_register(_nya_simulation_db_sleep, run);
    nya_http_response_create(&db->response, db->body, sizeof(db->body));
    NYA_EXPECT(nya_http_health_check_register("db", _nya_simulation_db_ready, nullptr));
    NYA_EXPECT(_nya_simulation_db_open(), "while opening the simulated database");

    nya_simulation_action_add(run, "db_write", 10, _nya_simulation_db_do_write);
    nya_simulation_action_add(run, "db_read", 6, _nya_simulation_db_do_read);
    nya_simulation_action_add(run, "job_enqueue", 8, _nya_simulation_db_do_job_enqueue);
    nya_simulation_action_add(run, "job_work", 10, _nya_simulation_db_do_job_work);
    nya_simulation_action_add(run, "jobs_drain", 3, _nya_simulation_db_do_jobs_drain);
    nya_simulation_action_add(run, "http_session", 6, _nya_simulation_db_do_http_session);
    nya_simulation_action_add(run, "http_ready", 4, _nya_simulation_db_do_http_ready);
    nya_simulation_fault_add(run, "fault_db_arm", 5, _nya_simulation_db_fault_arm);
    nya_simulation_fault_add(run, "fault_db_heal", 5, _nya_simulation_db_fault_heal);
    nya_simulation_fault_add(run, "fault_db_power_loss", 2, _nya_simulation_db_fault_power_loss);

    nya_simulation_check_add(run, "db_ready", _nya_simulation_db_check_ready);
}

NYA_INTERNAL void _nya_simulation_db_remove(void) {
    _NYA_SimulationDb* db = &_NYA_SIMULATION_DB;
    if (!db->initialized) return;

    nya_db_fault_disarm_all();
    _nya_simulation_db_close();

    nya_http_health_checks_clear();
    nya_http_response_destroy(&db->response);
    nya_db_fault_unregister();

    (void)nya_save_delete(_NYA_SIMULATION_DB_FILE);
    (void)nya_save_delete(_NYA_SIMULATION_DB_JOURNAL);

    nya_arena_destroy(db->scratch);
    nya_arena_destroy(db->connection);
    nya_arena_destroy(db->arena);

    *db = (_NYA_SimulationDb){ 0 };
}

#endif // NYA_TESTING && NYA_MODULE_DB
