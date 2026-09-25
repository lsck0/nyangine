/**
 * The db module's backup: a hot snapshot of a live database, a defragmenting copy, and a WAL
 * checkpoint. Every test opens a real temp-file database, since a backup is about a file on disk
 * and ":memory:" has none.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Removes a database file and the WAL/SHM sidecars a WAL-mode connection leaves beside it. */
static void remove_database(NYA_ConstCString path) {
  (void)remove(path);
  char sidecar[512];
  (void)snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
  (void)remove(sidecar);
  (void)snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
  (void)remove(sidecar);
}

/** Counts the rows in `table` on a fresh connection to `path`, proving the file stands on its own. */
static u64 count_rows(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString table) {
  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, path, &db));
  defer nya_sql_close(db);

  NYA_String*   sql    = nya_string_sprintf(arena, "SELECT COUNT(*) AS n FROM %s", table);
  NYA_SqlResult result = { 0 };
  NYA_EXPECT(nya_sql_query(db, arena, nya_string_to_cstring(arena, sql), nullptr, 0, &result));

  nya_assert(result.rows->length == 1);
  return (u64)nya_object_get(result.rows->items[0], "n")->as_s64;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_backup");
  defer      nya_arena_destroy(arena);

  nya_log_info("SQLite %s", nya_sql_version());

  // TEST: a hot backup of a live, open, still-writable database is a consistent snapshot
  {
    NYA_ConstCString source_path = "./_test_backup_src.db";
    NYA_ConstCString dest_path   = "./_test_backup_dst.db";
    remove_database(source_path);
    remove_database(dest_path);
    defer remove_database(source_path);
    defer remove_database(dest_path);

    NYA_Database* source = nullptr;
    NYA_EXPECT(nya_sql_open(arena, source_path, &source));
    defer nya_sql_close(source);

    NYA_EXPECT(nya_sql_exec(source, "CREATE TABLE runs (id INTEGER PRIMARY KEY, score INTEGER)"));
    for (s64 i = 1; i <= 100; i++) {
      NYA_SqlValue row[] = { nya_sql_s64(i * 10) };
      NYA_EXPECT(nya_sql_exec_bound(source, "INSERT INTO runs (score) VALUES (?)", row, 1));
    }

    // The source connection is open and has just written; the backup runs against it as it stands, never closing or copying the file out from under it.
    NYA_EXPECT(nya_sql_backup(source, dest_path));
    nya_assert(nya_filesystem_exists(dest_path), "the backup file was written");

    // And the source is fully writable immediately after: the backup did not leave it locked. These rows land after the snapshot, so they must not appear in it — that is what "point in time" means. (One thread, so the write is after rather than during; the API's guarantee that a write mid-copy is recopied is what makes the same true under a live server, which Phase 3 threads.)
    for (s64 i = 101; i <= 200; i++) {
      NYA_SqlValue row[] = { nya_sql_s64(i * 10) };
      NYA_EXPECT(nya_sql_exec_bound(source, "INSERT INTO runs (score) VALUES (?)", row, 1));
    }

    nya_assert(count_rows(arena, dest_path, "runs") == 100, "the backup holds exactly the rows committed when it was taken");
    nya_assert(count_rows(arena, source_path, "runs") == 200, "and the source kept growing past it");

    // The copied rows are the real values, not just the right count.
    NYA_Database* copy = nullptr;
    NYA_EXPECT(nya_sql_open(arena, dest_path, &copy));
    defer nya_sql_close(copy);

    NYA_SqlResult result = { 0 };
    NYA_EXPECT(nya_sql_query(copy, arena, "SELECT score FROM runs WHERE id = ?", (NYA_SqlValue[]){ nya_sql_s64(42) }, 1, &result));
    nya_assert(result.rows->length == 1 && nya_object_get(result.rows->items[0], "score")->as_s64 == 420, "a copied row holds its value");
  }

  // TEST: a smaller page step still copies the whole database
  {
    NYA_ConstCString source_path = "./_test_backup_step_src.db";
    NYA_ConstCString dest_path   = "./_test_backup_step_dst.db";
    remove_database(source_path);
    remove_database(dest_path);
    defer remove_database(source_path);
    defer remove_database(dest_path);

    NYA_Database* source = nullptr;
    NYA_EXPECT(nya_sql_open(arena, source_path, &source));
    defer nya_sql_close(source);

    NYA_EXPECT(nya_sql_exec(source, "CREATE TABLE t (id INTEGER PRIMARY KEY, blob BLOB)"));

    // Enough rows to span more than one page, so a one-page step has to loop.
    u8 payload[256];
    for (u32 i = 0; i < sizeof(payload); i++) payload[i] = (u8)i;
    for (s64 i = 0; i < 200; i++) {
      NYA_SqlValue row[] = { nya_sql_blob(payload, sizeof(payload)) };
      NYA_EXPECT(nya_sql_exec_bound(source, "INSERT INTO t (blob) VALUES (?)", row, 1));
    }

    NYA_EXPECT(nya_sql_backup(source, dest_path, .pages_per_step = 1));
    nya_assert(count_rows(arena, dest_path, "t") == 200, "one page per step copies every row all the same");
  }

  // TEST: a backup refuses an existing destination and an empty path
  {
    NYA_ConstCString source_path = "./_test_backup_refuse_src.db";
    NYA_ConstCString dest_path   = "./_test_backup_refuse_dst.db";
    remove_database(source_path);
    remove_database(dest_path);
    defer remove_database(source_path);
    defer remove_database(dest_path);

    NYA_Database* source = nullptr;
    NYA_EXPECT(nya_sql_open(arena, source_path, &source));
    defer nya_sql_close(source);
    NYA_EXPECT(nya_sql_exec(source, "CREATE TABLE t (v INTEGER)"));

    NYA_Error empty = nya_sql_backup(source, "");
    nya_assert(empty.kind == NYA_ERROR_INVALID_ARGUMENT, "an empty destination is a caller mistake");

    NYA_EXPECT(nya_sql_backup(source, dest_path));

    // The second time the file is there, and a backup will not write over it.
    NYA_Error clobber = nya_sql_backup(source, dest_path);
    nya_assert(clobber.kind == NYA_ERROR_INVALID_ARGUMENT, "a backup refuses to overwrite an existing file");
  }

  // TEST: a WAL-mode database backs up its committed frames, and the checkpoint runs
  {
    NYA_ConstCString source_path = "./_test_backup_wal_src.db";
    NYA_ConstCString dest_path   = "./_test_backup_wal_dst.db";
    remove_database(source_path);
    remove_database(dest_path);
    defer remove_database(source_path);
    defer remove_database(dest_path);

    NYA_Database* source = nullptr;
    NYA_EXPECT(nya_sql_open(arena, source_path, &source));
    defer nya_sql_close(source);

    // WAL mode: committed writes go to the -wal sidecar until a checkpoint folds them in.
    NYA_EXPECT(nya_sql_exec(source, "PRAGMA journal_mode = WAL"));
    NYA_EXPECT(nya_sql_exec(source, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER)"));
    for (s64 i = 1; i <= 50; i++) {
      NYA_SqlValue row[] = { nya_sql_s64(i) };
      NYA_EXPECT(nya_sql_exec_bound(source, "INSERT INTO t (v) VALUES (?)", row, 1));
    }

    // Backed up with no manual checkpoint first: the online backup reads through the WAL, so the frames sitting in it are captured all the same.
    NYA_EXPECT(nya_sql_backup(source, dest_path));
    nya_assert(count_rows(arena, dest_path, "t") == 50, "a WAL-mode backup captures frames still in the log");

    // The checkpoint runs and the data survives it. TRUNCATE folds every committed frame into the database file and empties the WAL.
    NYA_EXPECT(nya_sql_checkpoint(source, NYA_SQL_CHECKPOINT_TRUNCATE));
    NYA_SqlResult after = { 0 };
    NYA_EXPECT(nya_sql_query(source, arena, "SELECT COUNT(*) AS n FROM t", nullptr, 0, &after));
    nya_assert(nya_object_get(after.rows->items[0], "n")->as_s64 == 50, "the checkpoint kept every row");

    // The gentler modes run too.
    NYA_EXPECT(nya_sql_checkpoint(source, NYA_SQL_CHECKPOINT_PASSIVE));

    // An out-of-range mode is a caller mistake, caught rather than passed to SQLite.
    NYA_Error bogus = nya_sql_checkpoint(source, (NYA_SqlCheckpoint)999);
    nya_assert(bogus.kind == NYA_ERROR_INVALID_ARGUMENT, "an unknown checkpoint mode is refused");
  }

  // TEST: VACUUM INTO copies a live database as the single-statement alternative
  {
    NYA_ConstCString source_path = "./_test_backup_vacuum_src.db";
    NYA_ConstCString dest_path   = "./_test_backup_vacuum_dst.db";
    remove_database(source_path);
    remove_database(dest_path);
    defer remove_database(source_path);
    defer remove_database(dest_path);

    NYA_Database* source = nullptr;
    NYA_EXPECT(nya_sql_open(arena, source_path, &source));
    defer nya_sql_close(source);

    NYA_EXPECT(nya_sql_exec(source, "CREATE TABLE t (name TEXT, score INTEGER)"));
    NYA_SqlValue row[] = { nya_sql_text("nyangine"), nya_sql_s64(7) };
    NYA_EXPECT(nya_sql_exec_bound(source, "INSERT INTO t VALUES (?, ?)", row, 1 + 1));

    NYA_EXPECT(nya_sql_vacuum_into(source, dest_path));
    nya_assert(nya_filesystem_exists(dest_path), "VACUUM INTO wrote the copy");
    nya_assert(count_rows(arena, dest_path, "t") == 1, "and the row is in it");

    // Same no-clobber rule as nya_sql_backup.
    NYA_Error clobber = nya_sql_vacuum_into(source, dest_path);
    nya_assert(clobber.kind == NYA_ERROR_INVALID_ARGUMENT, "VACUUM INTO refuses an existing file too");
  }

  // TEST: the encrypted-source path is handled, not silently defeated
  {
    // This build vendors plain SQLite, so nya_sql_encryption_available() is false and no source can be opened under a key (nya_sql_open refuses it). The keyed backup round-trip is therefore not exercisable here; what is asserted is the seam that keeps it honest when SQLCipher lands: - a backup key this build cannot honour is refused, not taken and ignored; - and no destination file is left behind by the refusal. See db.h's encryption note and nya_sql_backup_with_options.
    NYA_ConstCString source_path = "./_test_backup_key_src.db";
    NYA_ConstCString dest_path   = "./_test_backup_key_dst.db";
    remove_database(source_path);
    remove_database(dest_path);
    defer remove_database(source_path);
    defer remove_database(dest_path);

    NYA_Database* source = nullptr;
    NYA_EXPECT(nya_sql_open(arena, source_path, &source));
    defer nya_sql_close(source);
    NYA_EXPECT(nya_sql_exec(source, "CREATE TABLE t (v INTEGER)"));

    u8 key[NYA_SQL_KEY_SIZE] = { 0 };
    nya_assert(nya_os_random_bytes(key, sizeof(key)));

    NYA_Error keyed = nya_sql_backup(source, dest_path, .key = key, .key_size = sizeof(key));
    if (nya_sql_encryption_available()) {
      NYA_EXPECT(keyed);
    } else {
      nya_assert(!keyed.ok && keyed.kind == NYA_ERROR_NOT_SUPPORTED, "a key this build cannot use is refused, not ignored");
      nya_assert(!nya_filesystem_exists(dest_path), "and the refusal left no file behind");
    }

    // A wrong-sized key is a caller mistake, caught before anything is opened.
    NYA_Error short_key = nya_sql_backup(source, dest_path, .key = key, .key_size = 16);
    nya_assert(short_key.kind == NYA_ERROR_INVALID_ARGUMENT, "a 16 byte key is not a key");
    nya_assert(!nya_filesystem_exists(dest_path), "and it created nothing");
  }

  printf("PASSED: test_db_backup\n");
  return 0;
}
