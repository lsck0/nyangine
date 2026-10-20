#include "nyangine-core/nyangine.h"

#include <sqlite3.h>

// sqlite-vec.h is upstream's, generated from a template at build time; declares sqlite3_vec_init. See vendor_sqlean.h / vendor_sqlvec.h.
#include "sqlite-vec.h"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** Bytes of SQLite's file header, which every database file starts with. */
#define _NYA_SQL_HEADER_SIZE 100

/** One cached statement. Its SQL text is the statement's own (sqlite3_sql), so the cache copies nothing. */
typedef struct {
    sqlite3_stmt* statement;
    u64           sql_hash;

    /** The connection's use counter when this was last handed out; the smallest is the least recently used. */
    u64 used_at;

    /** Handed out and not yet given back; a second use of the same SQL meanwhile prepares its own. */
    b8 in_use;
} _NYA_SqlCachedStatement;

/** A statement acquired for one call: from the cache, or prepared for this call alone and finalized after it. */
typedef struct {
    sqlite3_stmt* statement;
    b8            cached;
} _NYA_SqlStatement;

struct NYA_Database {
    sqlite3*    handle;
    NYA_Arena*  arena;
    const char* path;

    // Whether opened under a key; the key itself is never kept. db_backup.c uses this to refuse a plaintext copy of a database asked to stay encrypted.
    b8 encrypted;

    _NYA_SqlCachedStatement    statements[NYA_SQL_STATEMENT_CACHE_MAX];
    u64                        statement_uses;
    NYA_SqlStatementCacheStats statement_stats;
};

/** The most statements any one connection has cached, which is what the ceiling shows: the fullest connection is the one to size for. */
NYA_INTERNAL u32         _nya_sql_statement_cache_peak       = 0;
NYA_INTERNAL atomic_flag _nya_sql_statement_cache_registered = ATOMIC_FLAG_INIT;

/**
 * The sqlean bundle's entry point, defined in db_extensions.c.
 * */
extern int nya_sqlean_init(sqlite3* db, char** error_message, const sqlite3_api_routines* api);

/**
 * Registers the bundled extensions on `handle`, after the busy timeout is set rather than through
 * sqlite3_auto_extension inside the open: sqlean's `define` creates its table at init, and a lock held
 * by another connection then failed the open at once with "automatic extension loading failed".
 * */
NYA_INTERNAL NYA_Error _nya_sql_register_extensions(sqlite3* handle);

/** Sets every pragma `options` asks for, busy timeout first. */
NYA_INTERNAL NYA_Error _nya_sql_configure(sqlite3* handle, NYA_SqlOptions options);

/** A cached statement for `sql`, or one prepared for this call alone when the cache cannot hold it. Rejects trailing SQL. */
NYA_INTERNAL NYA_Error _nya_sql_statement_acquire(NYA_Database* database, NYA_ConstCString sql, OUT _NYA_SqlStatement* out_statement);

/** Gives `statement` back: a cached one is reset and unbound for the next call, an uncached one finalized. */
NYA_INTERNAL void _nya_sql_statement_release(NYA_Database* database, _NYA_SqlStatement statement);

/** Finalizes every cached statement, as a close needs. */
NYA_INTERNAL void _nya_sql_statement_cache_clear(NYA_Database* database);

/** Raises the ceiling's peak to `count` if it is the highest yet; any connection's thread may call it. */
NYA_INTERNAL void _nya_sql_statement_cache_peak_raise(u32 count);

/** Maps a SQLite result code onto the closest NYA_ErrorKind. */
NYA_INTERNAL NYA_ErrorKind _nya_sql_kind_from_sqlite(int code);

/** Binds `values` to a prepared statement's placeholders, which are 1 indexed in SQLite. */
NYA_INTERNAL NYA_Error _nya_sql_bind(sqlite3_stmt* statement, const NYA_SqlValue* values, u32 value_count);

/** Reads the current row of `statement` into a fresh object keyed by column name. */
NYA_INTERNAL NYA_Object* _nya_sql_row_to_object(sqlite3_stmt* statement, NYA_Arena* arena);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_ConstCString nya_sql_version(void) {
    return sqlite3_libversion();
}

NYA_ConstCString nya_sql_vec_version(void) {
    return SQLITE_VEC_VERSION;
}

b8 nya_sql_encryption_available(void) {
    // The seam, not the cipher: no cipher is vendored yet, so there is nothing to say yes to. See db.h.
#ifdef NYA_DB_SQLCIPHER
    return true;
#else
    return false;
#endif
}

NYA_Error nya_sql_open_with_options(NYA_Arena* arena, NYA_SqlOptions options, OUT NYA_Database** out_database) {
    nya_assert(arena != nullptr);
    nya_assert(out_database != nullptr);

    NYA_ConstCString path = options.path;

    if (path == nullptr || path[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "database path is empty");

    if (options.key != nullptr || options.key_size != 0) {
        if (options.key == nullptr || options.key_size != NYA_SQL_KEY_SIZE) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a database key is %d bytes, not " FMTu32, NYA_SQL_KEY_SIZE, options.key_size);
        }

        // Before the open, not after: a file created then refused would be an encrypted-by-request database sitting on disk in the clear.
        if (!nya_sql_encryption_available()) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' was given a key and this build has no cipher to use it with; see db.h", path);
        }
    }

    if (options.journal >= NYA_SQL_JOURNAL_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unknown journal %d", (s32)options.journal);
    if (options.synchronous >= NYA_SQL_SYNCHRONOUS_COUNT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unknown synchronous level %d", (s32)options.synchronous);
    }

    sqlite3* handle = nullptr;

    // NOMUTEX: this module is synchronous, so SQLite's internal locking would buy nothing; a cross-thread connection is outside this API.
    int code = sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, options.vfs);

    if (code != SQLITE_OK) {
        // open_v2 hands back a handle even on failure so the message can be read off it; closing it is still needed.
        NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(code), "could not open '%s': %s", path, sqlite3_errmsg(handle));
        sqlite3_close(handle);
        return error;
    }

#ifdef NYA_DB_SQLCIPHER
    // Before any pragma: journal_mode is the first read of the file, and an unkeyed read of an encrypted one is NOTADB.
    if (options.key != nullptr && sqlite3_key(handle, options.key, (int)options.key_size) != SQLITE_OK) {
        NYA_Error error = nya_error(NYA_ERROR_INVALID_ARGUMENT, "could not key '%s': %s", path, sqlite3_errmsg(handle));
        sqlite3_close(handle);
        return error;
    }
#endif

    NYA_Error configured = _nya_sql_configure(handle, options);
    if (configured.ok) configured = _nya_sql_register_extensions(handle);
    if (!configured.ok) {
        sqlite3_close(handle);
        return configured;
    }

    NYA_Database* database = nya_arena_alloc(arena, sizeof(NYA_Database));
    // A key that reached here was honoured, so `key != null` means the file is genuinely encrypted.
    *database = (NYA_Database){ .handle = handle, .arena = arena, .path = path, .encrypted = options.key != nullptr };

    if (!atomic_flag_test_and_set(&_nya_sql_statement_cache_registered)) {
        nya_ceiling_register("sql_statement_cache", NYA_SQL_STATEMENT_CACHE_MAX, &_nya_sql_statement_cache_peak);
    }

    *out_database = database;
    return NYA_OK;
}

void nya_sql_close(NYA_Database* database) {
    if (database == nullptr) return;
    if (database->handle == nullptr) return;

    _nya_sql_statement_cache_clear(database);

    // Logged, not returned: a close is no place to fail, and stale statistics only cost the next open a slower plan.
    NYA_Error optimized = nya_sql_optimize(database);
    if (!optimized.ok) nya_log_warn("could not optimize '%s' on close: %s", database->path, optimized.message);

    // close_v2, not close, which refuses outright while any statement is still open.
    int code = sqlite3_close_v2(database->handle);
    if (code != SQLITE_OK) nya_log_error("could not close '%s': %s", database->path, sqlite3_errmsg(database->handle));

    database->handle = nullptr;
}

NYA_Error nya_sql_optimize(NYA_Database* database) {
    return nya_sql_exec(database, "PRAGMA optimize;");
}

NYA_SqlStatementCacheStats nya_sql_statement_cache_get_stats(const NYA_Database* database) {
    nya_assert(database != nullptr);
    nya_assert(database->statement_stats.count <= NYA_SQL_STATEMENT_CACHE_MAX);

    return database->statement_stats;
}

NYA_Error nya_sql_exec(NYA_Database* database, NYA_ConstCString sql) {
    nya_assert(database != nullptr);
    nya_assert(database->handle != nullptr, "database is closed");

    if (sql == nullptr || sql[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "sql is empty");

    char* message = nullptr;
    int   code    = sqlite3_exec(database->handle, sql, nullptr, nullptr, &message);

    if (code != SQLITE_OK) {
        NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(code), "%s", message != nullptr ? message : sqlite3_errmsg(database->handle));
        sqlite3_free(message);
        return error;
    }

    return NYA_OK;
}

NYA_Error nya_sql_exec_bound(NYA_Database* database, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count) {
    NYA_SqlResult discarded = { 0 };

    // Runs the query path with a throwaway arena so binding and stepping have one implementation.
    NYA_Arena arena = nya_arena_create_on_stack(.name = "sql_exec_bound");
    defer     nya_arena_destroy_on_stack(&arena);

    return nya_sql_query(database, &arena, sql, values, value_count, &discarded);
}

NYA_Error nya_sql_query(
    NYA_Database* database, NYA_Arena* arena, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count, OUT NYA_SqlResult* out_result
) {
    nya_assert(database != nullptr);
    nya_assert(arena != nullptr);
    nya_assert(out_result != nullptr);
    nya_assert(database->handle != nullptr, "database is closed");

    if (sql == nullptr || sql[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "sql is empty");

    *out_result = (NYA_SqlResult){ .rows = nya_array_create(arena, NYA_SqlRow) };

    _NYA_SqlStatement acquired = { 0 };
    NYA_TRY(_nya_sql_statement_acquire(database, sql, &acquired));

    // Released on every path, including errors below; a statement left mid-step holds its read lock, and the next close refuses.
    defer _nya_sql_statement_release(database, acquired);

    sqlite3_stmt* statement = acquired.statement;

    NYA_TRY(_nya_sql_bind(statement, values, value_count));

    while (true) {
        int code = sqlite3_step(statement);

        if (code == SQLITE_ROW) {
            nya_array_push_back(out_result->rows, _nya_sql_row_to_object(statement, arena));
            continue;
        }

        if (code == SQLITE_DONE) break;

        return nya_error(_nya_sql_kind_from_sqlite(code), "%s", sqlite3_errmsg(database->handle));
    }

    out_result->rows_affected  = (u64)sqlite3_changes(database->handle);
    out_result->last_insert_id = (s64)sqlite3_last_insert_rowid(database->handle);

    return NYA_OK;
}

NYA_Error nya_sql_ping(NYA_Database* database) {
    NYA_Arena arena = nya_arena_create_on_stack(.name = "sql_ping");
    defer     nya_arena_destroy_on_stack(&arena);

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(database, &arena, "PRAGMA schema_version", nullptr, 0, &result));

    // Under WAL a read transaction checks the shared-memory index rather than the file, so a cached page answers through a dead disk. The header, read through the VFS, cannot.
    sqlite3_file* file = nullptr;
    if (sqlite3_file_control(database->handle, "main", SQLITE_FCNTL_FILE_POINTER, (void*)&file) != SQLITE_OK) return NYA_OK;
    // :memory: has no file behind it.
    if (file == nullptr || file->pMethods == nullptr) return NYA_OK;

    u8  header[_NYA_SQL_HEADER_SIZE];
    int code = file->pMethods->xRead(file, header, (int)sizeof(header), 0);
    // A file with no page written yet reads short, which is a working disk.
    if (code != SQLITE_OK && code != SQLITE_IOERR_SHORT_READ) return nya_error(_nya_sql_kind_from_sqlite(code), "could not read the header of '%s'", database->path);

    return NYA_OK;
}

NYA_Error nya_sql_transaction_begin(NYA_Database* database) {
    return nya_sql_exec(database, "BEGIN IMMEDIATE;");
}

NYA_Error nya_sql_transaction_commit(NYA_Database* database) {
    return nya_sql_exec(database, "COMMIT;");
}

NYA_Error nya_sql_transaction_rollback(NYA_Database* database) {
    return nya_sql_exec(database, "ROLLBACK;");
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_sql_register_extensions(sqlite3* handle) {
    static const struct {
        NYA_ConstCString name;
        int (*entry_point)(sqlite3*, char**, const sqlite3_api_routines*);
    } extensions[] = {
        { .name = "sqlean", .entry_point = &nya_sqlean_init },
        { .name = "sqlite-vec", .entry_point = &sqlite3_vec_init },
    };

    for (u64 i = 0; i < nya_carray_length(extensions); i++) {
        // No api table: both are compiled with SQLITE_CORE, so their sqlite3_* calls are direct and the argument is never read.
        char* message = nullptr;
        int   code    = extensions[i].entry_point(handle, &message, nullptr);
        if (code == SQLITE_OK) continue;

        NYA_Error error = nya_error(
            _nya_sql_kind_from_sqlite(code), "could not register the %s extensions: %s", extensions[i].name, message != nullptr ? message : sqlite3_errmsg(handle)
        );
        sqlite3_free(message);
        return error;
    }

    return NYA_OK;
}

NYA_Error _nya_sql_configure(sqlite3* handle, NYA_SqlOptions options) {
    s64 busy_timeout_ms = options.busy_timeout_ms == 0 ? NYA_SQL_BUSY_TIMEOUT_MS_DEFAULT : nya_clamp(options.busy_timeout_ms, (s64)0, (s64)S32_MAX);
    u32 cache_size_kib  = options.cache_size_kib == 0 ? NYA_SQL_CACHE_SIZE_KIB_DEFAULT : options.cache_size_kib;
    u32 checkpoint      = options.wal_autocheckpoint_pages == 0 ? NYA_SQL_WAL_AUTOCHECKPOINT_PAGES_DEFAULT : options.wal_autocheckpoint_pages;
    u64 journal_limit   = options.journal_size_limit_bytes == 0 ? NYA_SQL_JOURNAL_SIZE_LIMIT_BYTES_DEFAULT : options.journal_size_limit_bytes;
    b8  wal             = options.journal == NYA_SQL_JOURNAL_WAL;

    NYA_ConstCString synchronous = nullptr;
    switch (options.synchronous) {
        case NYA_SQL_SYNCHRONOUS_DEFAULT: synchronous = wal ? "NORMAL" : "FULL"; break;
        case NYA_SQL_SYNCHRONOUS_NORMAL:  synchronous = "NORMAL"; break;
        case NYA_SQL_SYNCHRONOUS_FULL:    synchronous = "FULL"; break;
        case NYA_SQL_SYNCHRONOUS_COUNT:
        default:                          nya_unreachable();
    }

    // First: journal_mode is the first statement to take a lock, and it has to wait out a held one rather than fail the open.
    int code = sqlite3_busy_timeout(handle, (int)busy_timeout_ms);
    if (code != SQLITE_OK) return nya_error(_nya_sql_kind_from_sqlite(code), "could not set the busy timeout: %s", sqlite3_errmsg(handle));

    // Every value is this file's own number or name, so interpolating is safe. A negative cache_size means KiB rather than pages.
    char pragmas[512];
    s32  length = snprintf(
        pragmas, sizeof(pragmas),
        "PRAGMA journal_mode = %s; PRAGMA synchronous = %s; PRAGMA cache_size = -" FMTu32 "; PRAGMA mmap_size = " FMTu64 "; "
        "PRAGMA temp_store = MEMORY; PRAGMA foreign_keys = ON; PRAGMA wal_autocheckpoint = " FMTu32 "; PRAGMA journal_size_limit = " FMTu64 ";",
        wal ? "WAL" : "DELETE", synchronous, cache_size_kib, options.mmap_size_bytes, checkpoint, journal_limit
    );
    nya_assert(length > 0 && (u64)length < sizeof(pragmas), "the pragma batch outgrew its buffer");

    char* message = nullptr;
    code          = sqlite3_exec(handle, pragmas, nullptr, nullptr, &message);
    if (code != SQLITE_OK) {
        NYA_Error error =
            nya_error(_nya_sql_kind_from_sqlite(code), "could not configure the connection: %s", message != nullptr ? message : sqlite3_errmsg(handle));
        sqlite3_free(message);
        return error;
    }

    // What SQLite's documentation gives a long lived connection at open: the analysis is capped at a few hundred rows an index, so it is quick on any table.
    code = sqlite3_exec(handle, "PRAGMA optimize = 0x10002;", nullptr, nullptr, nullptr);
    if (code != SQLITE_OK) nya_log_warn("could not optimize at open: %s", sqlite3_errmsg(handle));

    return NYA_OK;
}

void _nya_sql_statement_cache_peak_raise(u32 count) {
    nya_assert(count <= NYA_SQL_STATEMENT_CACHE_MAX);

    u32 peak = __atomic_load_n(&_nya_sql_statement_cache_peak, __ATOMIC_RELAXED);
    while (peak < count && !__atomic_compare_exchange_n(&_nya_sql_statement_cache_peak, &peak, count, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

NYA_Error _nya_sql_statement_acquire(NYA_Database* database, NYA_ConstCString sql, OUT _NYA_SqlStatement* out_statement) {
    NYA_SqlStatementCacheStats* stats = &database->statement_stats;
    nya_assert(stats->count <= NYA_SQL_STATEMENT_CACHE_MAX);

    u64 sql_size = strlen(sql);
    u64 sql_hash = nya_hash_fnv1a(sql, sql_size);
    database->statement_uses++;

    // A linear scan: 64 hashes in a row are cheaper to compare than a map is to probe and keep.
    for (u32 i = 0; i < stats->count; i++) {
        _NYA_SqlCachedStatement* entry = &database->statements[i];
        if (entry->sql_hash != sql_hash || entry->in_use || strcmp(sqlite3_sql(entry->statement), sql) != 0) continue;

        entry->in_use  = true;
        entry->used_at = database->statement_uses;
        stats->hits++;
        *out_statement = (_NYA_SqlStatement){ .statement = entry->statement, .cached = true };
        return NYA_OK;
    }

    stats->misses++;

    sqlite3_stmt*    statement = nullptr;
    NYA_ConstCString tail      = nullptr;

    // PERSISTENT: the statement is kept, so SQLite leaves its memory out of the lookaside pool meant for short lived ones.
    int code = sqlite3_prepare_v3(database->handle, sql, (int)sql_size + 1, SQLITE_PREPARE_PERSISTENT, &statement, &tail);
    if (code != SQLITE_OK) return nya_error(_nya_sql_kind_from_sqlite(code), "could not prepare statement: %s", sqlite3_errmsg(database->handle));

    // prepare silently ignores anything after the first statement, so "INSERT ...; DROP TABLE ..." would look like it ran in full.
    if (tail != nullptr && tail[0] != '\0') {
        sqlite3_finalize(statement);
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "expected a single statement, got trailing sql: '%s'", tail);
    }

    // Only whitespace or a comment prepares to no statement at all; stepping a null one is SQLITE_MISUSE.
    if (statement == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "sql holds no statement");

    // A text SQLite keeps differently from ours could never be found again; it runs once and is finalized.
    if (strcmp(sqlite3_sql(statement), sql) != 0) {
        *out_statement = (_NYA_SqlStatement){ .statement = statement, .cached = false };
        return NYA_OK;
    }

    u32 slot = stats->count;
    if (slot == NYA_SQL_STATEMENT_CACHE_MAX) {
        u64 oldest = U64_MAX;
        for (u32 i = 0; i < NYA_SQL_STATEMENT_CACHE_MAX; i++) {
            if (database->statements[i].in_use || database->statements[i].used_at >= oldest) continue;
            oldest = database->statements[i].used_at;
            slot   = i;
        }

        // Every entry handed out is a recursion 64 statements deep; that one call goes uncached.
        if (slot == NYA_SQL_STATEMENT_CACHE_MAX) {
            *out_statement = (_NYA_SqlStatement){ .statement = statement, .cached = false };
            return NYA_OK;
        }

        sqlite3_finalize(database->statements[slot].statement);
        stats->evictions++;
    } else {
        stats->count++;
        _nya_sql_statement_cache_peak_raise(stats->count);
    }

    database->statements[slot] = (_NYA_SqlCachedStatement){ .statement = statement, .sql_hash = sql_hash, .used_at = database->statement_uses, .in_use = true };
    nya_assert(stats->count <= NYA_SQL_STATEMENT_CACHE_MAX);

    *out_statement = (_NYA_SqlStatement){ .statement = statement, .cached = true };
    return NYA_OK;
}

void _nya_sql_statement_release(NYA_Database* database, _NYA_SqlStatement statement) {
    nya_assert(statement.statement != nullptr);

    if (!statement.cached) {
        sqlite3_finalize(statement.statement);
        return;
    }

    for (u32 i = 0; i < database->statement_stats.count; i++) {
        _NYA_SqlCachedStatement* entry = &database->statements[i];
        if (entry->statement != statement.statement) continue;

        nya_assert(entry->in_use, "a cached statement was given back twice");

        // reset's code repeats the step's error, which the caller already has; clear_bindings frees the copies of bound text and blobs now rather than at the next use.
        (void)sqlite3_reset(entry->statement);
        (void)sqlite3_clear_bindings(entry->statement);
        entry->in_use = false;
        return;
    }

    nya_unreachable();
}

void _nya_sql_statement_cache_clear(NYA_Database* database) {
    for (u32 i = 0; i < database->statement_stats.count; i++) {
        nya_assert(!database->statements[i].in_use, "a connection closed while one of its statements was running");
        sqlite3_finalize(database->statements[i].statement);
    }

    nya_memset(database->statements, 0, sizeof(database->statements));
    database->statement_stats.count = 0;
}

NYA_Error _nya_sql_bind(sqlite3_stmt* statement, const NYA_SqlValue* values, u32 value_count) {
    if (value_count > 0 && values == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "value_count is %u but values is null", value_count);
    }

    // Checked before the nothing-to-do case: an unbound placeholder is NULL to SQLite, and binding past the count is a SQLITE_RANGE that misreads as a database fault.
    int expected = sqlite3_bind_parameter_count(statement);
    if ((u32)expected != value_count) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "statement takes %d parameters but %u were given", expected, value_count);
    }

    if (value_count == 0) return NYA_OK;

    for (u32 i = 0; i < value_count; i++) {
        // SQLite numbers placeholders from one.
        int index = (int)i + 1;
        int code  = SQLITE_OK;

        switch (values[i].kind) {
            case NYA_SQL_VALUE_NULL: code = sqlite3_bind_null(statement, index); break;
            case NYA_SQL_VALUE_S64:  code = sqlite3_bind_int64(statement, index, values[i].as_s64); break;
            case NYA_SQL_VALUE_F64:  code = sqlite3_bind_double(statement, index, values[i].as_f64); break;

            // SQLITE_TRANSIENT: SQLite copies the bytes, so callers need not keep strings alive until the statement steps.
            case NYA_SQL_VALUE_TEXT:
                code = sqlite3_bind_text(statement, index, values[i].as_text, -1, SQLITE_TRANSIENT);
                break;

            case NYA_SQL_VALUE_BLOB:
                code = sqlite3_bind_blob64(statement, index, values[i].as_blob.data, values[i].as_blob.size, SQLITE_TRANSIENT);
                break;

            case NYA_SQL_VALUE_COUNT:
            default: return nya_error(NYA_ERROR_INVALID_ARGUMENT, "parameter %u has an unknown kind %d", i, (int)values[i].kind);
        }

        if (code != SQLITE_OK) return nya_error(_nya_sql_kind_from_sqlite(code), "could not bind parameter %u", i);
    }

    return NYA_OK;
}

NYA_Object* _nya_sql_row_to_object(sqlite3_stmt* statement, NYA_Arena* arena) {
    NYA_Object* row    = nya_object_create(arena);
    int         column_count = sqlite3_column_count(statement);

    for (int i = 0; i < column_count; i++) {
        NYA_ConstCString name = sqlite3_column_name(statement, i);
        if (name == nullptr) continue;

        // The key is copied into the arena; column_name storage belongs to the statement and is freed at finalize.
        NYA_String* key_string = nya_string_sprintf(arena, "%s", name);
        NYA_CString key        = nya_string_to_cstring(arena, key_string);

        switch (sqlite3_column_type(statement, i)) {
            case SQLITE_INTEGER: nya_object_add(row, key, (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = sqlite3_column_int64(statement, i) }); break;
            case SQLITE_FLOAT:   nya_object_add(row, key, (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = sqlite3_column_double(statement, i) }); break;

            case SQLITE_TEXT: {
                NYA_ConstCString text = (NYA_ConstCString)sqlite3_column_text(statement, i);
                NYA_String*      copy = nya_string_sprintf(arena, "%s", text != nullptr ? text : "");
                nya_object_add(row, key, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, copy) });
            } break;

            // Blobs come back base64 encoded rather than as bytes.
            case SQLITE_BLOB: {
                // column_blob returns null for a zero-length blob (a real value), so an empty non-null pointer stands in: an empty blob encodes to "", not a crash.
                const u8* data = sqlite3_column_blob(statement, i);
                int       size = sqlite3_column_bytes(statement, i);

                NYA_String* encoded = nya_string_create(arena);
                nya_base64_encode(encoded, data != nullptr ? data : (const u8*)"", (u64)(size > 0 ? size : 0));
                nya_object_add(row, key, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, encoded) });
            } break;

            // Present with a null value rather than absent, so a caller can tell "no such column" from "this column is null".
            case SQLITE_NULL:
            default:          nya_object_add(row, key, (NYA_Value){ .type = NYA_TYPE_NULL }); break;
        }
    }

    return row;
}

NYA_ErrorKind _nya_sql_kind_from_sqlite(int code) {
    switch (code & 0xFF) {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:     return NYA_ERROR_NONE;

        case SQLITE_PERM:
        case SQLITE_AUTH:
        case SQLITE_READONLY: return NYA_ERROR_PERMISSION_DENIED;

        case SQLITE_CANTOPEN:
        case SQLITE_NOTFOUND: return NYA_ERROR_NOT_FOUND;

        case SQLITE_NOMEM:    return NYA_ERROR_OUT_OF_MEMORY;

        case SQLITE_BUSY:
        case SQLITE_LOCKED:   return NYA_ERROR_TIMEOUT;

        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:   return NYA_ERROR_CORRUPT;

        case SQLITE_IOERR:
        case SQLITE_FULL:     return NYA_ERROR_IO;

        // Syntax error, missing table, failed constraint: all mean the statement or its parameters were wrong, the caller's mistake.
        case SQLITE_ERROR:
        case SQLITE_MISUSE:
        case SQLITE_RANGE:
        case SQLITE_CONSTRAINT:
        case SQLITE_MISMATCH:  return NYA_ERROR_INVALID_ARGUMENT;

        default:               return NYA_ERROR_NOT_OK;
    }
}
