#include "nyangine/nyangine.h"

#include <sqlite3.h>

// Both are compiled into their own archives and linked in, never loaded at runtime; see
// vendor_sqlean.h and vendor_sqlvec.h. sqlite-vec.h is upstream's, generated from a template at
// build time, and declares sqlite3_vec_init.
#include "sqlite-vec.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_Database {
    sqlite3*    handle;
    NYA_Arena*  arena;
    const char* path;
};

/**
 * The sqlean bundle's entry point, defined in db_extensions.c.
 * */
extern int nya_sqlean_init(sqlite3* db, char** error_message, const sqlite3_api_routines* api);

/** Arranges for every connection opened afterwards to have the bundled extensions registered. */
NYA_INTERNAL void _nya_sql_register_extensions(void);

/** Maps a SQLite result code onto the closest NYA_ErrorKind. */
NYA_INTERNAL NYA_ErrorKind _nya_sql_kind_from_sqlite(int code);

/** Binds `values` to a prepared statement's placeholders, which are 1 indexed in SQLite. */
NYA_INTERNAL NYA_Error _nya_sql_bind(sqlite3_stmt* statement, const NYA_SqlValue* values, u32 value_count);

/** Reads the current row of `statement` into a fresh object keyed by column name. */
NYA_INTERNAL NYA_Object* _nya_sql_row_to_object(sqlite3_stmt* statement, NYA_Arena* arena);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_sql_version(void) {
    return sqlite3_libversion();
}

NYA_ConstCString nya_sql_vec_version(void) {
    return SQLITE_VEC_VERSION;
}

b8 nya_sql_encryption_available(void) {
    /*
     * The seam and not the cipher. SQLCipher defines this one and takes the key through sqlite3_key
     * immediately after the open, followed by a read of sqlite_schema, because that is where a wrong
     * key is found: keying itself succeeds against any bytes. Until it is vendored there is nothing
     * to say yes to. See db.h.
     */
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

        // Before the open and not after it: a file created here and then refused would be a database
        // somebody asked to have encrypted, sitting on the disk in the clear.
        if (!nya_sql_encryption_available()) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' was given a key and this build has no cipher to use it with; see db.h", path);
        }
    }

    // Before the open below, not after: an auto extension only applies to connections created once
    // it is registered, so a connection opened first would silently lack every function.
    _nya_sql_register_extensions();

    sqlite3* handle = nullptr;

    // NOMUTEX: this module is synchronous and documents itself as such, so paying for SQLite's
    // internal locking on every call would buy nothing. A connection shared across threads is
    // outside what this API offers.
    int code = sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);

    if (code != SQLITE_OK) {
        // sqlite3_open_v2 hands back a handle even on failure, purely so the message can be read
        // off it, and closing it is the caller's job either way.
        NYA_Error error = nya_error(_nya_sql_kind_from_sqlite(code), "could not open '%s': %s", path, sqlite3_errmsg(handle));
        sqlite3_close(handle);
        return error;
    }

    NYA_Database* database = nya_arena_alloc(arena, sizeof(NYA_Database));
    *database              = (NYA_Database){ .handle = handle, .arena = arena, .path = path };

    // Foreign keys are off by default in SQLite, for compatibility with databases written before it
    // had them. Nothing in this engine predates that, and a schema that declares a reference should
    // have it enforced.
    (void)sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);

    *out_database = database;
    return NYA_OK;
}

void nya_sql_close(NYA_Database* database) {
    if (database == nullptr) return;
    if (database->handle == nullptr) return;

    /*
     * close_v2 rather than close, which refuses outright while any statement is still open.
     */
    int code = sqlite3_close_v2(database->handle);
    if (code != SQLITE_OK) nya_log_error("could not close '%s': %s", database->path, sqlite3_errmsg(database->handle));

    database->handle = nullptr;
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

    // Runs through the query path with a throwaway arena, so binding and stepping have exactly one
    // implementation. A statement that returns no rows simply produces no objects.
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

    sqlite3_stmt*    statement = nullptr;
    NYA_ConstCString tail      = nullptr;

    int code = sqlite3_prepare_v2(database->handle, sql, -1, &statement, &tail);
    if (code != SQLITE_OK) {
        return nya_error(_nya_sql_kind_from_sqlite(code), "could not prepare statement: %s", sqlite3_errmsg(database->handle));
    }

    // Finalized however this returns, including through the error paths below. Without it a failed
    // step leaves the statement open and the next nya_sql_close refuses.
    defer sqlite3_finalize(statement);

    // Anything after the first statement is silently ignored by prepare_v2, which would make
    // "INSERT ...; DROP TABLE ..." look like it ran in full. Said out loud instead.
    if (tail != nullptr && tail[0] != '\0') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "expected a single statement, got trailing sql: '%s'", tail);
    }

    NYA_TRY(_nya_sql_bind(statement, values, value_count));

    while (true) {
        code = sqlite3_step(statement);

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

NYA_Error nya_sql_transaction_begin(NYA_Database* database) {
    return nya_sql_exec(database, "BEGIN TRANSACTION;");
}

NYA_Error nya_sql_transaction_commit(NYA_Database* database) {
    return nya_sql_exec(database, "COMMIT;");
}

NYA_Error nya_sql_transaction_rollback(NYA_Database* database) {
    return nya_sql_exec(database, "ROLLBACK;");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_sql_register_extensions(void) {
    /*
     * sqlite3_auto_extension takes `void (*)(void)` and SQLite calls back through the real signature. The
     * cast is unavoidable; upstream's documentation does the same.
     */
    static const struct {
        NYA_ConstCString name;
        int (*entry_point)(sqlite3*, char**, const sqlite3_api_routines*);
    } extensions[] = {
        { .name = "sqlean", .entry_point = &nya_sqlean_init },
        { .name = "sqlite-vec", .entry_point = &sqlite3_vec_init },
    };

    for (u64 i = 0; i < nya_carray_length(extensions); i++) {
        void (*as_generic)(void) = nullptr;
        nya_memcpy(&as_generic, &extensions[i].entry_point, sizeof(as_generic));

        /*
         * Called on every open: sqlite3_auto_extension already ignores an entry point it has, and a flag here
         * would need to be thread safe.
         */
        int code = sqlite3_auto_extension(as_generic);
        if (code != SQLITE_OK) {
            // Not fatal, and deliberately not an error return. The database still opens and every
            // statement that does not reach for these functions still works; what breaks is the
            // subset of queries that use them, and those fail with SQLite's own "no such function".
            nya_log_warn("could not register the %s sqlite extensions (code %d)", extensions[i].name, code);
        }
    }
}

NYA_Error _nya_sql_bind(sqlite3_stmt* statement, const NYA_SqlValue* values, u32 value_count) {
    if (value_count > 0 && values == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "value_count is %u but values is null", value_count);
    }

    // Checked rather than trusted, and before the nothing-to-do case rather than after it: a
    // placeholder nobody binds is not an error to SQLite, it is NULL, so a statement given no values
    // at all would quietly match no rows. Binding past the count is a SQLITE_RANGE error that reads
    // as a database problem when the actual fault is that the call site and the string disagree.
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

            // SQLITE_TRANSIENT: SQLite copies the bytes. The alternative would make every caller
            // responsible for keeping its strings alive until the statement is stepped, which is a
            // lifetime rule nobody would remember and the copy is not the expensive part of a query.
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

        // The key is copied into the arena. sqlite3_column_name's storage belongs to the statement
        // and is freed by sqlite3_finalize, which happens before the caller ever reads the row.
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

            /*
             * Blobs come back base64 encoded rather than as bytes.
             */
            case SQLITE_BLOB: {
                const u8* data = sqlite3_column_blob(statement, i);
                int       size = sqlite3_column_bytes(statement, i);

                NYA_String* encoded = nya_string_create(arena);
                nya_base64_encode(encoded, data, (u64)(size > 0 ? size : 0));
                nya_object_add(row, key, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, encoded) });
            } break;

            // Present with a null value rather than absent, so a caller can tell "no such column"
            // from "this column is null".
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

        // A syntax error, a missing table, a failed constraint: all of them mean the statement or
        // its parameters were wrong, which is the caller's mistake rather than the database's.
        case SQLITE_ERROR:
        case SQLITE_MISUSE:
        case SQLITE_RANGE:
        case SQLITE_CONSTRAINT:
        case SQLITE_MISMATCH:  return NYA_ERROR_INVALID_ARGUMENT;

        default:               return NYA_ERROR_NOT_OK;
    }
}
