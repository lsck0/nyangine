/**
 * The SQLite plugin: rows as NYA_Object, bound parameters, transactions.
 *
 * Everything runs against ":memory:", so the suite touches no disk and two tests cannot see each
 * other's tables. The one exception is the file backed test at the bottom, which exists because
 * "opens a path and creates it" is the behaviour a save file depends on and an in memory database
 * cannot exercise it.
 *
 * The plugin flags come from FLAGS_PLUGINS on the test build rule, the same ones the project
 * compiles with. Without them this file would compile to nothing and report a pass.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Opens a fresh in memory database. Each test gets its own, so none can pollute another. */
static NYA_Database* open_memory(NYA_Arena* arena) {
  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
  return db;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_sql");
  defer      nya_arena_destroy(arena);

  nya_log_info("SQLite %s", nya_sql_version());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: opening rejects nothing sensible, and a closed handle is safe to close
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a row comes back as an object keyed by column name
  // ─────────────────────────────────────────────────────────────────────────────
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

    // The whole point of the plugin: a row is an ordinary object, so every accessor that works on a
    // parsed JSON body works here too.
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

    // Present with a null value rather than absent, which is what lets a caller tell "no such
    // column" from "this column is null".
    NYA_Value* missing = nya_object_get(row, "missing");
    nya_assert(missing != nullptr, "a NULL column is still a key");
    nya_assert(missing->type == NYA_TYPE_NULL);

    nya_assert(nya_object_get(row, "no_such_column") == nullptr, "a column that does not exist is absent");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: parameters are bound, so quotes in data cannot become syntax
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE players (name TEXT)"));

    // The classic. Interpolated into the string this closes the literal and drops the table; bound,
    // it is just an unusual name.
    NYA_ConstCString hostile  = "Robert'); DROP TABLE players;--";
    NYA_SqlValue     insert[] = { nya_sql_text(hostile) };
    NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO players (name) VALUES (?)", insert, 1));

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT name FROM players", nullptr, 0, &result));

    nya_assert(result.rows->length == 1, "the table still exists and holds the row");
    nya_assert(nya_string_equals(nya_object_get(result.rows->items[0], "name")->as_string, hostile), "stored verbatim, as data");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a parameter count mismatch is caught before SQLite sees it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (a INTEGER, b INTEGER)"));

    // Checked by the plugin rather than surfacing as SQLITE_RANGE, which reads like a database
    // problem when the actual fault is that the call site and the string disagree.
    NYA_SqlValue too_few[] = { nya_sql_s64(1) };
    NYA_Error    result    = nya_sql_exec_bound(db, "INSERT INTO t (a, b) VALUES (?, ?)", too_few, 1);
    nya_assert(result.kind == NYA_ERROR_INVALID_ARGUMENT, "one value for two placeholders");

    NYA_SqlResult query  = { 0 };
    NYA_Error     nulled = nya_sql_query(db, arena, "SELECT * FROM t WHERE a = ?", nullptr, 1, &query);
    nya_assert(nulled.kind == NYA_ERROR_INVALID_ARGUMENT, "a count with no values is a mistake, not a crash");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: query refuses more than one statement
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE t (a INTEGER)"));

    // sqlite3_prepare_v2 silently ignores everything after the first statement, which would make
    // this look like it fully ran. Refused instead.
    NYA_SqlResult result = { 0 };
    NYA_Error     error  = nya_sql_query(db, arena, "INSERT INTO t VALUES (1); DROP TABLE t;", nullptr, 0, &result);
    nya_assert(error.kind == NYA_ERROR_INVALID_ARGUMENT, "trailing sql is refused rather than dropped");

    // And the table survived, because nothing was executed.
    NYA_SqlResult check = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT * FROM t", nullptr, 0, &check));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: exec takes multiple statements, which is what schema setup needs
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: rows_affected and last_insert_id
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a rolled back transaction leaves nothing behind
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: broken sql is an error rather than a crash, and leaves the handle usable
  // ─────────────────────────────────────────────────────────────────────────────
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

    // The connection is still usable, which is what proves the failed statement was finalized
    // rather than left open — nya_sql_close would otherwise refuse at the end of this block.
    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE fine (x INTEGER)"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a constraint violation is reported, not silently swallowed
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: blobs survive the round trip, base64 encoded into the row
  // ─────────────────────────────────────────────────────────────────────────────
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

    // Base64 rather than raw bytes, because NYA_Value has no byte array and a row has to stay
    // something serde can write out unchanged.
    NYA_Value* data = nya_object_get(result.rows->items[0], "data");
    nya_assert(data != nullptr && data->type == NYA_TYPE_STRING, "a blob arrives as text");

    NYA_String* decoded = nya_string_create(arena);
    nya_base64_decode(decoded, (const u8*)data->as_string, strlen(data->as_string));
    nya_assert(decoded->length == sizeof(bytes), "decoded to " FMTu64 " bytes", decoded->length);
    nya_assert(memcmp(decoded->items, bytes, sizeof(bytes)) == 0, "and the bytes survived, zero byte included");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a file backed database is created and persists across connections
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The one thing ":memory:" cannot show: that reopening a path finds what the last connection
    // wrote, which is the whole premise of a save file.
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a row serializes as JSON, which is the reason it is an NYA_Object
  // ─────────────────────────────────────────────────────────────────────────────
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

  printf("PASSED: test_sql\n");
  return 0;
}
