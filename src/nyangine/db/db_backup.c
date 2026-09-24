#include "nyangine/nyangine.h"

#include <sqlite3.h>

// Compiled as part of db.c after db_sql.c: reaches into struct NYA_Database and _nya_sql_kind_from_sqlite, both in scope as one translation unit.

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/**
 * Closes a half-made destination connection and deletes its file, so a failed backup leaves nothing
 * behind for the next run to trip over. Safe on a path that was never created.
 * */
NYA_INTERNAL void _nya_sql_backup_abandon(sqlite3* dest, NYA_ConstCString dest_path);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_sql_backup_with_options(NYA_Database* source, NYA_SqlBackupOptions options) {
    nya_assert(source != nullptr);
    nya_assert(source->handle != nullptr, "database is closed");

    NYA_ConstCString dest_path = options.dest_path;
    if (dest_path == nullptr || dest_path[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "destination path is empty");

    // A key is validated and refused on a cipher-less build like nya_sql_open, before any file is made: a copy asked for encrypted must never land in the clear.
    if (options.key != nullptr || options.key_size != 0) {
        if (options.key == nullptr || options.key_size != NYA_SQL_KEY_SIZE) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a database key is %d bytes, not " FMTu32, NYA_SQL_KEY_SIZE, options.key_size);
        }
        if (!nya_sql_encryption_available()) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "a backup key was given and this build has no cipher to use it with; see db.h");
        }
    }

    // A copy of an encrypted source with no key would be written in the clear, a quiet way to lose the encryption.
    if (source->encrypted && options.key == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the source is encrypted; pass the key the copy is to be written under");
    }

    // Never over an existing file: half a copy written over last night's good one is worse than none.
    if (nya_filesystem_exists(dest_path)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' already exists; a backup will not overwrite it", dest_path);
    }

    sqlite3* dest = nullptr;
    int      code = sqlite3_open_v2(dest_path, &dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK) {
        NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(code), "could not open backup '%s': %s", dest_path, sqlite3_errmsg(dest));
        _nya_sql_backup_abandon(dest, dest_path);
        return error;
    }

#ifdef NYA_DB_SQLCIPHER
    // Key the destination before the first page is copied, or the copy is written in the clear. Only reachable on a build with a cipher.
    if (options.key != nullptr) {
        code = sqlite3_key(dest, options.key, (int)options.key_size);
        if (code != SQLITE_OK) {
            NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(code), "could not key backup '%s': %s", dest_path, sqlite3_errmsg(dest));
            _nya_sql_backup_abandon(dest, dest_path);
            return error;
        }
    }
#endif

    // "main" on both sides: the primary schema of each connection. backup_init reports failure on the destination handle, not a return code.
    sqlite3_backup* backup = sqlite3_backup_init(dest, "main", source->handle, "main");
    if (backup == nullptr) {
        NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(sqlite3_errcode(dest)), "could not start backup to '%s': %s", dest_path, sqlite3_errmsg(dest));
        _nya_sql_backup_abandon(dest, dest_path);
        return error;
    }

    const int pages_per_step = options.pages_per_step > 0 ? options.pages_per_step : 256;

    // A step blocked by another writer returns BUSY/LOCKED: sleep and retry, bounded so a wedged writer can't hang the backup; writes between steps are recopied for a consistent snapshot.
    const int max_busy_retries = 100;
    int       busy_retries     = 0;

    while (true) {
        code = sqlite3_backup_step(backup, pages_per_step);

        if (code == SQLITE_OK) {
            busy_retries = 0;
            continue;
        }
        if (code == SQLITE_DONE) break;

        if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
            if (busy_retries++ >= max_busy_retries) break;
            (void)sqlite3_sleep(25);
            continue;
        }

        break;  // a real error; finish frees the object and we report it below.
    }

    // finish frees the backup object on every path and returns the last error, or SQLITE_OK when every page made it across.
    int finish_code = sqlite3_backup_finish(backup);
    int result_code = code == SQLITE_DONE ? finish_code : code;

    if (result_code != SQLITE_OK) {
        NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(result_code), "backup to '%s' failed: %s", dest_path, sqlite3_errstr(result_code));
        _nya_sql_backup_abandon(dest, dest_path);
        return error;
    }

    // close_v2, matching nya_sql_close: tolerates pending work rather than refusing, though a finished backup leaves none.
    (void)sqlite3_close_v2(dest);
    return NYA_OK;
}

NYA_Error nya_sql_vacuum_into(NYA_Database* database, NYA_ConstCString dest_path) {
    nya_assert(database != nullptr);
    nya_assert(database->handle != nullptr, "database is closed");

    if (dest_path == nullptr || dest_path[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "destination path is empty");

    // Same no-clobber rule as nya_sql_backup; failing here keeps the error readable rather than SQLite's phrasing.
    if (nya_filesystem_exists(dest_path)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' already exists; a backup will not overwrite it", dest_path);
    }

    // Bound, not interpolated: the path is data, so a quote in it cannot become syntax.
    NYA_SqlValue arg[] = { nya_sql_text(dest_path) };
    return nya_sql_exec_bound(database, "VACUUM INTO ?", arg, 1);
}

NYA_Error nya_sql_checkpoint(NYA_Database* database, NYA_SqlCheckpoint mode) {
    nya_assert(database != nullptr);
    nya_assert(database->handle != nullptr, "database is closed");

    // A fixed string per mode: the mode is a closed set, so there is nothing to bind and nothing to inject.
    NYA_ConstCString pragma = nullptr;
    switch (mode) {
        case NYA_SQL_CHECKPOINT_PASSIVE:  pragma = "PRAGMA wal_checkpoint(PASSIVE);"; break;
        case NYA_SQL_CHECKPOINT_FULL:     pragma = "PRAGMA wal_checkpoint(FULL);"; break;
        case NYA_SQL_CHECKPOINT_RESTART:  pragma = "PRAGMA wal_checkpoint(RESTART);"; break;
        case NYA_SQL_CHECKPOINT_TRUNCATE: pragma = "PRAGMA wal_checkpoint(TRUNCATE);"; break;
        default:                          return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unknown checkpoint mode %d", (int)mode);
    }

    // The pragma returns one row of counters that exec discards; a reader-blocked checkpoint reports busy in that row, not an error, so exec sees success either way.
    return nya_sql_exec(database, pragma);
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

void _nya_sql_backup_abandon(sqlite3* dest, NYA_ConstCString dest_path) {
    if (dest != nullptr) (void)sqlite3_close_v2(dest);
    if (nya_filesystem_exists(dest_path)) (void)nya_filesystem_delete(dest_path);
}
