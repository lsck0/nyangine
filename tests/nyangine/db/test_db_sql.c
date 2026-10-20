/**
 * The db module's connection: rows as NYA_Object, bound parameters, transactions.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Opens a fresh in memory database. Each test gets its own, so none can pollute another. */
static NYA_Database* open_memory(NYA_Arena* arena) {
  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
  return db;
}

/** A file of its own, for what `:memory:` cannot show: the journal, and a second connection's lock. */
#define TUNED_PATH "./_test_db_sql_tuned.db"

static void remove_tuned(void) {
  (void)remove(TUNED_PATH);
  (void)remove(TUNED_PATH "-wal");
  (void)remove(TUNED_PATH "-shm");
  (void)remove(TUNED_PATH "-journal");
}

/** A pragma's one value. */
static NYA_Value* pragma_value(NYA_Database* db, NYA_Arena* arena, NYA_ConstCString name) {
  char sql[64];
  (void)snprintf(sql, sizeof(sql), "PRAGMA %s", name);

  NYA_SqlResult result = { 0 };
  NYA_EXPECT(nya_sql_query(db, arena, sql, nullptr, 0, &result));
  nya_assert_eq(result.rows->length, 1ULL);

  // busy_timeout is the one pragma whose column is not named after it.
  NYA_ConstCString column = strcmp(name, "busy_timeout") == 0 ? "timeout" : name;
  NYA_Value*       value  = nya_object_get(result.rows->items[0], (NYA_CString)column);
  nya_assert(value != nullptr, "PRAGMA %s came back without its column", name);
  return value;
}

static s64 pragma_s64(NYA_Database* db, NYA_Arena* arena, NYA_ConstCString name) {
  return pragma_value(db, arena, name)->as_s64;
}

static NYA_ConstCString pragma_text(NYA_Database* db, NYA_Arena* arena, NYA_ConstCString name) {
  return pragma_value(db, arena, name)->as_string;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_sql");
  defer      nya_arena_destroy(arena);

  nya_log_info("SQLite %s", nya_sql_version());

  // TEST: opening rejects nothing sensible, and a closed handle is safe to close
  {
    NYA_Database* db     = nullptr;
    NYA_Error     result = nya_sql_open(arena, "", &db);
    nya_assert(result.kind == NYA_ERROR_INVALID_ARGUMENT, "an empty path is a caller mistake, not a panic");

    // Safe on null so an unwind path does not have to check.
    nya_sql_close(nullptr);

    db = open_memory(arena);
    nya_sql_close(db);
    nya_sql_close(db);  // idempotent: the second close sees a null handle and returns
  }

  // TEST: a row comes back as an object keyed by column name
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE runs (id INTEGER PRIMARY KEY, name TEXT, score INTEGER, ratio REAL, blob BLOB, missing TEXT)"));

    NYA_SqlValue insert[] = { nya_sql_text("first"), nya_sql_s64(4200), nya_sql_f64(0.5) };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO runs (name, score, ratio) VALUES (?, ?, ?)", insert, 3));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT * FROM runs", nullptr, 0, &result));

    nya_assert(result.rows->length == 1, "one insert, one row");

    NYA_Object* row = result.rows->items[0];
    nya_assert(row != nullptr);

    // The whole point of the module: a row is an ordinary object, so every accessor that works on a parsed JSON body works here too.
    NYA_Value* name = nya_object_get(row, "name");
    nya_assert(name != nullptr, "columns are keyed by their name");
    nya_assert(name->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(name->as_string, "first"));

    NYA_Value* score = nya_object_get(row, "score");
    nya_assert(score != nullptr && score->type == NYA_TYPE_S64);
    nya_assert(score->as_s64 == 4200, "got " FMTs64, score->as_s64);

    NYA_Value* ratio = nya_object_get(row, "ratio");
    nya_assert(ratio != nullptr && ratio->type == NYA_TYPE_F64);
    nya_assert(ratio->as_f64 == 0.5);

    // Present with a null value rather than absent, which is what lets a caller tell "no such column" from "this column is null".
    NYA_Value* missing = nya_object_get(row, "missing");
    nya_assert(missing != nullptr, "a NULL column is still a key");
    nya_assert(missing->type == NYA_TYPE_NULL);

    nya_assert(nya_object_get(row, "no_such_column") == nullptr, "a column that does not exist is absent");
  }

  // TEST: parameters are bound, so quotes in data cannot become syntax
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE players (name TEXT)"));

    // The classic. Interpolated into the string this closes the literal and drops the table; bound, it is just an unusual name.
    NYA_ConstCString hostile  = "Robert'); DROP TABLE players;--";
    NYA_SqlValue     insert[] = { nya_sql_text(hostile) };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO players (name) VALUES (?)", insert, 1));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT name FROM players", nullptr, 0, &result));

    nya_assert(result.rows->length == 1, "the table still exists and holds the row");
    nya_assert(nya_string_equals(nya_object_get(result.rows->items[0], "name")->as_string, hostile), "stored verbatim, as data");
  }

  // TEST: a parameter count mismatch is caught before SQLite sees it
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (a INTEGER, b INTEGER)"));

    // Checked by the module rather than surfacing as SQLITE_RANGE, which reads like a database problem when the actual fault is that the call site and the string disagree.
    NYA_SqlValue too_few[] = { nya_sql_s64(1) };
    NYA_Error    result    = nya_sql_exec_bound(db, "INSERT INTO t (a, b) VALUES (?, ?)", too_few, 1);
    nya_assert(result.kind == NYA_ERROR_INVALID_ARGUMENT, "one value for two placeholders");

    NYA_SqlResult query  = { 0 };
    NYA_Error     nulled = nya_sql_query(db, arena, "SELECT * FROM t WHERE a = ?", nullptr, 1, &query);
    nya_assert(nulled.kind == NYA_ERROR_INVALID_ARGUMENT, "a count with no values is a mistake, not a crash");

    // A placeholder nobody binds is NULL to SQLite rather than an error, so a statement handed no values at all would match no rows and look like an empty table.
    NYA_Error unbound = nya_sql_query(db, arena, "SELECT * FROM t WHERE a = ?", nullptr, 0, &query);
    nya_assert(unbound.kind == NYA_ERROR_INVALID_ARGUMENT, "no values for one placeholder is the same mistake");

    NYA_EXPECT(nya_sql_query(db, arena, "SELECT * FROM t", nullptr, 0, &query));
  }

  // TEST: query refuses more than one statement
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (a INTEGER)"));

    // sqlite3_prepare_v2 silently ignores everything after the first statement, which would make this look like it fully ran. Refused instead.
    NYA_SqlResult result = { 0 };
    NYA_Error     error  = nya_sql_query(db, arena, "INSERT INTO t VALUES (1); DROP TABLE t;", nullptr, 0, &result);
    nya_assert(error.kind == NYA_ERROR_INVALID_ARGUMENT, "trailing sql is refused rather than dropped");

    // And the table survived, because nothing was executed.
    NYA_SqlResult check = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT * FROM t", nullptr, 0, &check));
  }

  // TEST: exec takes multiple statements, which is what schema setup needs
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE a (x INTEGER); CREATE TABLE b (y INTEGER); INSERT INTO a VALUES (1);"));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT x FROM a", nullptr, 0, &result));
    nya_assert(result.rows->length == 1, "every statement ran");

    NYA_EXPECT(nya_sql_query(db, arena, "SELECT y FROM b", nullptr, 0, &result));
    nya_assert(result.rows->length == 0, "including the one that created an empty table");
  }

  // TEST: rows_affected and last_insert_id
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)"));

    NYA_SqlResult insert = { 0 };
    NYA_SqlValue  one[]  = { nya_sql_s64(10) };
    NYA_EXPECT(nya_sql_query(db, arena, "INSERT INTO t (v) VALUES (?)", one, 1, &insert));

    nya_assert(insert.rows_affected == 1, "one row inserted, got " FMTu64, insert.rows_affected);
    nya_assert(insert.last_insert_id == 1, "the first rowid is 1, got " FMTs64, insert.last_insert_id);
    nya_assert(insert.rows->length == 0, "an insert returns no rows");

    NYA_SqlValue two[] = { nya_sql_s64(20) };
    NYA_EXPECT(nya_sql_query(db, arena, "INSERT INTO t (v) VALUES (?)", two, 1, &insert));
    nya_assert(insert.last_insert_id == 2);

    NYA_SqlResult update = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "UPDATE t SET v = 0", nullptr, 0, &update));
    nya_assert(update.rows_affected == 2, "both rows updated, got " FMTu64, update.rows_affected);
  }

  // TEST: a rolled back transaction leaves nothing behind
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (v INTEGER)"));

    NYA_EXPECT(nya_sql_transaction_begin(db));
    NYA_SqlValue value[] = { nya_sql_s64(1) };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO t VALUES (?)", value, 1));
    NYA_EXPECT(nya_sql_transaction_rollback(db));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT v FROM t", nullptr, 0, &result));
    nya_assert(result.rows->length == 0, "the rollback undid the insert");

    NYA_EXPECT(nya_sql_transaction_begin(db));
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO t VALUES (?)", value, 1));
    NYA_EXPECT(nya_sql_transaction_commit(db));

    NYA_EXPECT(nya_sql_query(db, arena, "SELECT v FROM t", nullptr, 0, &result));
    nya_assert(result.rows->length == 1, "and the commit kept one");
  }

  // TEST: broken sql is an error rather than a crash, and leaves the handle usable
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_SqlResult result = { 0 };
    NYA_Error     error  = nya_sql_query(db, arena, "SELECT FROM WHERE", nullptr, 0, &result);
    nya_assert(error.kind == NYA_ERROR_INVALID_ARGUMENT, "a syntax error is the caller's mistake");

    NYA_Error missing_table = nya_sql_query(db, arena, "SELECT * FROM nope", nullptr, 0, &result);
    nya_assert(missing_table.kind == NYA_ERROR_INVALID_ARGUMENT);

    NYA_Error empty = nya_sql_exec(db, "");
    nya_assert(empty.kind == NYA_ERROR_INVALID_ARGUMENT);

    // the connection is still usable, proving the failed statement was finalized; nya_sql_close would refuse otherwise.
    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE fine (x INTEGER)"));
  }

  // TEST: a constraint violation is reported, not silently swallowed
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT NOT NULL UNIQUE)"));

    NYA_SqlValue value[] = { nya_sql_text("only") };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO t (v) VALUES (?)", value, 1));

    NYA_Error duplicate = nya_sql_exec_bound(db, "INSERT INTO t (v) VALUES (?)", value, 1);
    nya_assert(duplicate.kind == NYA_ERROR_INVALID_ARGUMENT, "a unique violation is an error");

    NYA_SqlValue null_value[] = { nya_sql_null() };
    NYA_Error    not_null     = nya_sql_exec_bound(db, "INSERT INTO t (v) VALUES (?)", null_value, 1);
    nya_assert(not_null.kind == NYA_ERROR_INVALID_ARGUMENT, "so is a NOT NULL violation");
  }

  // TEST: blobs survive the round trip, base64 encoded into the row
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (data BLOB)"));

    // Deliberately contains a zero byte, which is what separates a blob from text.
    const u8     bytes[]  = { 0x00, 0x01, 0xFE, 0xFF, 0x42 };
    NYA_SqlValue insert[] = { nya_sql_blob(bytes, sizeof(bytes)) };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO t (data) VALUES (?)", insert, 1));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT data FROM t", nullptr, 0, &result));
    nya_assert(result.rows->length == 1);

    // Base64 rather than raw bytes, because NYA_Value has no byte array and a row has to stay something serde can write out unchanged.
    NYA_Value* data = nya_object_get(result.rows->items[0], "data");
    nya_assert(data != nullptr && data->type == NYA_TYPE_STRING, "a blob arrives as text");

    NYA_String* decoded = nya_string_create(arena);
    nya_base64_decode(decoded, (const u8*)data->as_string, strlen(data->as_string));
    nya_assert(decoded->length == sizeof(bytes), "decoded to " FMTu64 " bytes", decoded->length);
    nya_assert(memcmp(decoded->items, bytes, sizeof(bytes)) == 0, "and the bytes survived, zero byte included");
  }

  // TEST: a key is refused by a build with no cipher, and no file is left behind
  {
    // The encryption seam. Written so it stays true on the day SQLCipher lands: what is asserted is that a key is either honoured or refused, never taken and then ignored.
    u8 key[NYA_SQL_KEY_SIZE] = { 0 };
    nya_assert(nya_os_random_bytes(key, sizeof(key)));

    NYA_ConstCString path = "./_test_sql_keyed.db";
    (void)remove(path);
    defer (void)remove(path);

    NYA_Database* db     = nullptr;
    NYA_Error     opened = nya_sql_open(arena, path, &db, .key = key, .key_size = sizeof(key));

    if (nya_sql_encryption_available()) {
      NYA_EXPECT(opened);
      nya_sql_close(db);
    } else {
      nya_assert(!opened.ok && opened.kind == NYA_ERROR_NOT_SUPPORTED, "a key this build cannot use is refused, not ignored");

      // The half that matters: refusing after creating the file would leave a database somebody asked to have encrypted sitting there in the clear.
      nya_assert(!nya_filesystem_exists(path), "a refused key created '%s' anyway", path);
    }

    // A key of the wrong size is the caller's mistake and is caught before anything is opened.
    NYA_Error short_key = nya_sql_open(arena, path, &db, .key = key, .key_size = 16);
    nya_assert(short_key.kind == NYA_ERROR_INVALID_ARGUMENT, "a 16 byte key is not a key");
    nya_assert(!nya_filesystem_exists(path), "and it created nothing either");
  }

  // TEST: a file backed database is created and persists across connections
  {
    // The one thing ":memory:" cannot show: that reopening a path finds what the last connection wrote, which is the whole premise of a save file.
    NYA_ConstCString path = "./_test_sql_persist.db";
    (void)remove(path);
    defer (void)remove(path);

    {
      NYA_Database* db = nullptr;
      NYA_EXPECT(nya_sql_open(arena, path, &db));
      defer nya_sql_close(db);

      NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE save (slot INTEGER, score INTEGER)"));
      NYA_SqlValue row[] = { nya_sql_s64(1), nya_sql_s64(9001) };
      NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO save VALUES (?, ?)", row, 2));
    }

    nya_assert(nya_filesystem_exists(path), "opening a path that did not exist created it");

    {
      NYA_Database* db = nullptr;
      NYA_EXPECT(nya_sql_open(arena, path, &db));
      defer nya_sql_close(db);

      NYA_SqlResult result = { 0 };
      NYA_EXPECT(nya_sql_query(db, arena, "SELECT score FROM save WHERE slot = ?", (NYA_SqlValue[]){ nya_sql_s64(1) }, 1, &result));

      nya_assert(result.rows->length == 1, "the row written by the previous connection is still there");
      nya_assert(nya_object_get(result.rows->items[0], "score")->as_s64 == 9001);
    }
  }

  // TEST: a row serializes as JSON, which is the reason it is an NYA_Object
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (name TEXT, score INTEGER)"));
    NYA_SqlValue row[] = { nya_sql_text("nyangine"), nya_sql_s64(7) };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO t VALUES (?, ?)", row, 2));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT name, score FROM t", nullptr, 0, &result));

    // No conversion step: what came out of the database goes straight through serde.
    NYA_String* json = nya_serde_json_serialize(arena, result.rows->items[0], NYA_SERDE_NONE);
    nya_assert(nya_string_contains(json, "\"name\""), "got %s", nya_string_to_cstring(arena, json));
    nya_assert(nya_string_contains(json, "nyangine"));
    nya_assert(nya_string_contains(json, "\"score\""));
  }

  // TEST: a file opens tuned: WAL, synchronous NORMAL, the busy timeout, the page cache, temp tables in memory, foreign keys on
  {
    (void)remove(TUNED_PATH);
    defer remove_tuned();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, TUNED_PATH, &db));
    defer nya_sql_close(db);

    nya_assert(nya_string_equals(pragma_text(db, arena, "journal_mode"), "wal"));
    // the numbers are SQLite's own encodings: NORMAL is 1, MEMORY is 2, and a negative cache size is KiB.
    nya_assert_eq(pragma_s64(db, arena, "synchronous"), 1LL);
    nya_assert_eq(pragma_s64(db, arena, "busy_timeout"), (s64)NYA_SQL_BUSY_TIMEOUT_MS_DEFAULT);
    nya_assert_eq(pragma_s64(db, arena, "cache_size"), -(s64)NYA_SQL_CACHE_SIZE_KIB_DEFAULT);
    nya_assert_eq(pragma_s64(db, arena, "temp_store"), 2LL);
    nya_assert_eq(pragma_s64(db, arena, "foreign_keys"), 1LL);
    nya_assert_eq(pragma_s64(db, arena, "mmap_size"), 0LL);
    nya_assert_eq(pragma_s64(db, arena, "wal_autocheckpoint"), (s64)NYA_SQL_WAL_AUTOCHECKPOINT_PAGES_DEFAULT);
    nya_assert_eq(pragma_s64(db, arena, "journal_size_limit"), (s64)NYA_SQL_JOURNAL_SIZE_LIMIT_BYTES_DEFAULT);
  }

  // TEST: every knob is honoured, and DELETE with the default level gets FULL, the level it cannot be corrupted at
  {
    (void)remove(TUNED_PATH);
    defer remove_tuned();

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(
        arena, TUNED_PATH, &db, .journal = NYA_SQL_JOURNAL_DELETE, .busy_timeout_ms = 250, .cache_size_kib = 4096, .mmap_size_bytes = 1 << 20,
        .wal_autocheckpoint_pages = 500, .journal_size_limit_bytes = 1 << 22
    ));
    defer nya_sql_close(db);

    nya_assert(nya_string_equals(pragma_text(db, arena, "journal_mode"), "delete"));
    nya_assert_eq(pragma_s64(db, arena, "synchronous"), 2LL);
    nya_assert_eq(pragma_s64(db, arena, "busy_timeout"), 250LL);
    nya_assert_eq(pragma_s64(db, arena, "cache_size"), -4096LL);
    nya_assert_eq(pragma_s64(db, arena, "mmap_size"), 1LL << 20);
    nya_assert_eq(pragma_s64(db, arena, "wal_autocheckpoint"), 500LL);
    nya_assert_eq(pragma_s64(db, arena, "journal_size_limit"), 1LL << 22);

    NYA_Database* durable = nullptr;
    NYA_EXPECT(nya_sql_open(arena, TUNED_PATH, &durable, .synchronous = NYA_SQL_SYNCHRONOUS_FULL));
    defer nya_sql_close(durable);
    nya_assert_eq(pragma_s64(durable, arena, "synchronous"), 2LL);

    nya_assert(nya_sql_open(arena, TUNED_PATH, &durable, .journal = NYA_SQL_JOURNAL_COUNT).kind == NYA_ERROR_INVALID_ARGUMENT);
    nya_assert(nya_sql_open(arena, TUNED_PATH, &durable, .synchronous = NYA_SQL_SYNCHRONOUS_COUNT).kind == NYA_ERROR_INVALID_ARGUMENT);
  }

  // TEST: the same SQL a second time reuses its prepared statement, through exec_bound, query and the ORM's calls alike
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER NOT NULL)"));
    NYA_SqlStatementCacheStats before = nya_sql_statement_cache_get_stats(db);

    for (s64 i = 0; i < 10; i++) NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO t (v) VALUES (?)", (NYA_SqlValue[]){ nya_sql_s64(i) }, 1));

    NYA_SqlStatementCacheStats after = nya_sql_statement_cache_get_stats(db);
    nya_assert_eq(after.misses - before.misses, 1ULL);
    nya_assert_eq(after.hits - before.hits, 9ULL);
    nya_assert_eq(after.count - before.count, 1U);

    // a bound value does not outlive its call: the reused statement starts unbound, so a missing value is refused, not the last one reused.
    nya_assert(nya_sql_exec_bound(db, "INSERT INTO t (v) VALUES (?)", nullptr, 0).kind == NYA_ERROR_INVALID_ARGUMENT);

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT COUNT(*) AS n, SUM(v) AS total FROM t", nullptr, 0, &result));
    nya_assert_eq(nya_object_get(result.rows->items[0], "n")->as_s64, 10LL);
    nya_assert_eq(nya_object_get(result.rows->items[0], "total")->as_s64, 45LL);
  }

  // TEST: a statement that failed its step is reset, so its next use runs clean
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER NOT NULL)"));

    NYA_ConstCString insert = "INSERT INTO t (id, v) VALUES (?, ?)";
    NYA_EXPECT(nya_sql_exec_bound(db, insert, (NYA_SqlValue[]){ nya_sql_s64(1), nya_sql_s64(1) }, 2));
    nya_assert(nya_sql_exec_bound(db, insert, (NYA_SqlValue[]){ nya_sql_s64(1), nya_sql_s64(2) }, 2).kind == NYA_ERROR_INVALID_ARGUMENT);
    NYA_EXPECT(nya_sql_exec_bound(db, insert, (NYA_SqlValue[]){ nya_sql_s64(2), nya_sql_s64(2) }, 2), "the constraint failure did not stick to the statement");

    // and holds no lock afterwards: a close with a statement mid-step would log a failure, a VACUUM refuse outright.
    NYA_EXPECT(nya_sql_exec(db, "VACUUM"));
  }

  // TEST: a cached SELECT * sees a column added after it was prepared
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (a INTEGER)"));
    NYA_EXPECT(nya_sql_exec(db, "INSERT INTO t VALUES (1)"));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT * FROM t", nullptr, 0, &result));
    nya_assert(nya_object_get(result.rows->items[0], "b") == nullptr);

    NYA_EXPECT(nya_sql_exec(db, "ALTER TABLE t ADD COLUMN b INTEGER NOT NULL DEFAULT 7"));
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT * FROM t", nullptr, 0, &result));
    nya_assert(nya_object_get(result.rows->items[0], "b") != nullptr, "SQLite re-prepared the cached statement against the new schema");
    nya_assert_eq(nya_object_get(result.rows->items[0], "b")->as_s64, 7LL);
  }

  // TEST: past its capacity the cache evicts the least recently used statement and keeps the hot one
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_SqlResult    result = { 0 };
    NYA_ConstCString hot    = "SELECT 0 AS hot";
    NYA_EXPECT(nya_sql_query(db, arena, hot, nullptr, 0, &result));

    char sql[64];
    for (u32 i = 1; i <= NYA_SQL_STATEMENT_CACHE_MAX + 8; i++) {
      (void)snprintf(sql, sizeof(sql), "SELECT %u AS cold", i);
      NYA_EXPECT(nya_sql_query(db, arena, sql, nullptr, 0, &result));
      nya_assert_eq(nya_object_get(result.rows->items[0], "cold")->as_s64, (s64)i);
      NYA_EXPECT(nya_sql_query(db, arena, hot, nullptr, 0, &result));
    }

    NYA_SqlStatementCacheStats stats = nya_sql_statement_cache_get_stats(db);
    nya_assert_eq(stats.count, (u32)NYA_SQL_STATEMENT_CACHE_MAX);
    nya_assert(stats.evictions >= 8, "8 more statements than the cache holds, " FMTu64 " evicted", stats.evictions);

    u64 hits = stats.hits;
    NYA_EXPECT(nya_sql_query(db, arena, hot, nullptr, 0, &result));
    // the statement used every other call was never the one evicted.
    nya_assert_eq(nya_sql_statement_cache_get_stats(db).hits, hits + 1);

    // the first cold one is long gone; asking again is a miss that prepares it anew.
    u64 misses = nya_sql_statement_cache_get_stats(db).misses;
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT 1 AS cold", nullptr, 0, &result));
    nya_assert_eq(nya_sql_statement_cache_get_stats(db).misses, misses + 1);
  }

  // TEST: a transaction takes the write lock at BEGIN, so a second connection's write waits rather than racing it to a BUSY_SNAPSHOT
  {
    (void)remove(TUNED_PATH);
    defer remove_tuned();

    NYA_Database* holder = nullptr;
    NYA_Database* other  = nullptr;
    NYA_EXPECT(nya_sql_open(arena, TUNED_PATH, &holder));
    defer nya_sql_close(holder);
    NYA_EXPECT(nya_sql_open(arena, TUNED_PATH, &other, .busy_timeout_ms = -1));
    defer nya_sql_close(other);

    NYA_EXPECT(nya_sql_exec(holder, "CREATE TABLE t (v INTEGER)"));
    NYA_EXPECT(nya_sql_transaction_begin(holder));
    nya_assert(nya_sql_exec(other, "INSERT INTO t VALUES (1)").kind == NYA_ERROR_TIMEOUT, "BEGIN alone already holds the write lock");
    NYA_EXPECT(nya_sql_transaction_rollback(holder));
    NYA_EXPECT(nya_sql_exec(other, "INSERT INTO t VALUES (1)"));
  }

  // TEST: optimize runs on a live connection, and a close after it is clean
  {
    NYA_Database* db = open_memory(arena);
    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (a INTEGER)"));
    NYA_EXPECT(nya_sql_exec(db, "CREATE INDEX t_a ON t (a)"));
    NYA_EXPECT(nya_sql_optimize(db));
    nya_sql_close(db);
  }

  printf("PASSED: test_db_sql\n");
  return 0;
}
