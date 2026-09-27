/**
 * db_fault.h's lying VFS, and the bugs the simulation found with it, each held here by one case: a
 * readiness check that could not see a dead disk, an account lookup that read a fault as a deleted
 * user, and a session cookie read as nobody, a 401 that signs a real user out, when the disk failed.
 *
 * A real file, since a fault below the pager needs one: `:memory:` never calls the VFS.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include <string.h>

#define DB_PATH      "./_test_db_fault.db"
#define JOURNAL_PATH "./_test_db_fault.db-journal"

enum {
    /** Rows the power cut's transaction writes: enough that half of them surviving would show. */
    CUT_ROWS = 6,

    /** Calls the power cut lets through, walked from none to past the whole commit. */
    CUT_AFTER_MAX = 24,
};

#define PASSWORD "a correct horse battery staple"

static const u8 SEAL_SECRET[] = "test seal secret, sixteen or more";

/** Where the sleep hook adds up what it was asked to wait, so nothing here really waits. */
static s64 SLEPT_NS = 0;

static void sleep_counted(void* context, NYA_Duration duration) {
    nya_unused(context);
    SLEPT_NS += duration.ns;
}

static void remove_files(void) {
    (void)remove(DB_PATH);
    (void)remove(JOURNAL_PATH);
}

static u64 count_rows(NYA_Database* db, NYA_Arena* arena) {
    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT COUNT(*) AS n FROM t", nullptr, 0, &result));
    return (u64)nya_object_get(result.rows->items[0], "n")->as_s64;
}

static b8 integrity_ok(NYA_Database* db, NYA_Arena* arena) {
    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "PRAGMA integrity_check", nullptr, 0, &result));
    return nya_string_equals(nya_object_get(result.rows->items[0], "integrity_check")->as_string, "ok");
}

static NYA_Database* open_fresh(NYA_Arena* arena) {
    remove_files();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, DB_PATH, &db, .vfs = NYA_DB_FAULT_VFS));
    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)"));
    NYA_EXPECT(nya_sql_exec(db, "PRAGMA busy_timeout = 5000"));
    return db;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_db_fault");
    defer      nya_arena_destroy(arena);

    nya_db_fault_register(sleep_counted, nullptr);
    defer nya_db_fault_unregister();
    defer remove_files();

    // TEST: a counted BUSY is waited out by the busy handler, on the hook's clock and not the wall's
    {
        NYA_Database* db = open_fresh(arena);
        defer nya_sql_close(db);

        u64 hits = nya_db_fault_hits();
        SLEPT_NS = 0;

        nya_db_fault_arm(.kind = NYA_DB_FAULT_BUSY, .count = 3);
        NYA_EXPECT(nya_sql_exec(db, "INSERT INTO t (v) VALUES (1)"), "three busy answers are inside a five second busy_timeout");

        nya_assert_eq(nya_db_fault_hits() - hits, 3ULL);
        nya_assert(SLEPT_NS > 0, "the busy handler backed off through the hook");
        nya_assert(!nya_db_fault_armed(NYA_DB_FAULT_BUSY), "a counted fault disarms itself on its last call");

        // until healed, the handler gives up and the caller sees a timeout it can map to 503.
        nya_db_fault_arm(.kind = NYA_DB_FAULT_BUSY);
        NYA_Error busy = nya_sql_exec(db, "INSERT INTO t (v) VALUES (2)");
        nya_assert(busy.kind == NYA_ERROR_TIMEOUT);
        nya_db_fault_disarm(NYA_DB_FAULT_BUSY);

        nya_assert_eq(count_rows(db, arena), 1ULL);
    }

    // TEST: a write, a sync and a full disk each fail their statement whole, and the next one succeeds
    {
        NYA_Database* db = open_fresh(arena);
        defer nya_sql_close(db);

        NYA_DbFaultKind kinds[] = { NYA_DB_FAULT_IOERR_WRITE, NYA_DB_FAULT_IOERR_FSYNC, NYA_DB_FAULT_FULL };

        for (u32 i = 0; i < nya_carray_length(kinds); i++) {
            nya_db_fault_arm(.kind = kinds[i], .count = 1);
            nya_assert(nya_sql_exec(db, "INSERT INTO t (v) VALUES (1)").kind == NYA_ERROR_IO);
            nya_assert(!nya_db_fault_armed(kinds[i]));

            NYA_EXPECT(nya_sql_exec(db, "INSERT INTO t (v) VALUES (2)"), "the same connection recovers once the disk does");
        }

        nya_assert_eq(count_rows(db, arena), (u64)nya_carray_length(kinds));
        nya_assert(integrity_ok(db, arena));
    }

    // TEST: a readiness check has to reach the file. SELECT 1 never does, so it stayed green through a dead disk
    {
        NYA_Database* db = open_fresh(arena);
        defer nya_sql_close(db);

        NYA_EXPECT(nya_sql_ping(db));

        nya_db_fault_arm(.kind = NYA_DB_FAULT_IOERR_READ);

        NYA_SqlResult result = { 0 };
        NYA_EXPECT(nya_sql_query(db, arena, "SELECT 1", nullptr, 0, &result), "SELECT 1 touches no page, which is the bug a ping exists for");
        nya_assert(nya_sql_ping(db).kind == NYA_ERROR_IO);

        nya_db_fault_disarm_all();
        NYA_EXPECT(nya_sql_ping(db));
    }

    // TEST: a stall waits on the hook, then the call goes through
    {
        NYA_Database* db = open_fresh(arena);
        defer nya_sql_close(db);

        SLEPT_NS = 0;
        nya_db_fault_arm(.kind = NYA_DB_FAULT_STALL, .count = 1, .stall = nya_duration_from_s(90));
        NYA_EXPECT(nya_sql_exec(db, "INSERT INTO t (v) VALUES (1)"));
        nya_assert_eq(SLEPT_NS, 90 * NYA_NS_PER_SECOND);
    }

    // TEST: a power cut anywhere in a commit leaves a whole file with the transaction all there or not at all
    for (u32 after = 0; after <= CUT_AFTER_MAX; after++) {
        NYA_Database* db = open_fresh(arena);
        NYA_EXPECT(nya_sql_exec(db, "INSERT INTO t (v) VALUES (0)"));

        u64 hits = nya_db_fault_hits();
        nya_db_fault_arm(.kind = NYA_DB_FAULT_POWER_LOSS, .after = after);

        NYA_EXPECT(nya_sql_transaction_begin(db));
        for (u32 i = 0; i < CUT_ROWS; i++) NYA_EXPECT(nya_sql_exec(db, "INSERT INTO t (v) VALUES (1)"));
        NYA_EXPECT(nya_sql_transaction_commit(db), "a lost write reports success, which is the point of the fault");

        b8 cut = nya_db_fault_hits() != hits;

        // the process dies with the power out: the close's own cleanup is lost too.
        nya_sql_close(db);
        nya_db_fault_disarm_all();

        NYA_EXPECT(nya_sql_open(arena, DB_PATH, &db, .vfs = NYA_DB_FAULT_VFS));

        u64 rows = count_rows(db, arena);
        nya_assert(integrity_ok(db, arena), "the file is corrupt after a cut %u calls into the commit", after);
        nya_assert(rows == 1 || rows == 1 + CUT_ROWS, "a cut %u calls in left %llu rows of one transaction", after, (unsigned long long)rows);
        if (!cut) nya_assert_eq(rows, 1ULL + CUT_ROWS);

        nya_sql_close(db);
    }

    // TEST: a disk fault behind a session cookie is 503, not the 401 that signs the user out; a missing row is still nobody
    {
        NYA_Database* db = open_fresh(arena);
        defer nya_sql_close(db);

        NYA_EXPECT(nya_accounts_open(arena, db));
        defer nya_accounts_close();

        const NYA_HttpRouter* router = nya_http_accounts_open((NYA_HttpAccountsConfig){
            .arena                  = arena,
            .database               = db,
            .totp_issuer            = "test",
            .login_seal_secret      = SEAL_SECRET,
            .login_seal_secret_size = sizeof(SEAL_SECRET) - 1,
            .session_same_site      = NYA_HTTP_SAME_SITE_STRICT,
        });
        nya_assert(router != nullptr);
        defer nya_http_accounts_close();

        NYA_AccountUser    user    = { 0 };
        NYA_AccountSession session = { 0 };
        NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &user));
        NYA_EXPECT(nya_account_session_issue(arena, user.id, "192.0.2.1", "test", &session));

        NYA_HttpRequest request = { .method = NYA_HTTP_METHOD_GET, .header_count = 1 };
        (void)snprintf(request.headers[0].name, sizeof(request.headers[0].name), "cookie");
        (void)snprintf(request.headers[0].value, sizeof(request.headers[0].value), "%s=%s", NYA_HTTP_SESSION_COOKIE, session.token);

        NYA_HttpExchange exchange = { .request = &request, .arena = arena };
        NYA_AccountUser  caller   = { 0 };

        nya_assert(nya_http_accounts_caller(&exchange, &caller) == NYA_HTTP_STATUS_OK);
        nya_assert_eq(caller.id, user.id);

        nya_db_fault_arm(.kind = NYA_DB_FAULT_IOERR_READ);
        nya_assert(nya_account_find_by_id(arena, user.id, &caller).kind == NYA_ERROR_IO);
        nya_assert(nya_http_accounts_caller(&exchange, &caller) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE);
        nya_db_fault_disarm_all();

        nya_assert(nya_account_find_by_id(arena, user.id + 1, &caller).kind == NYA_ERROR_NOT_FOUND);
        nya_assert(nya_http_accounts_caller(&exchange, &caller) == NYA_HTTP_STATUS_OK);
    }

    printf("PASSED: test_db_fault\n");
    return EXIT_SUCCESS;
}
