/**
 * @file db_sql.h
 *
 * Example:
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "db");
 * defer      nya_arena_destroy(arena);
 *
 * NYA_Database* db = nullptr;
 * NYA_EXPECT(nya_sql_open(arena, "./save.db", &db));
 * defer nya_sql_close(db);
 *
 * NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY, score INTEGER)"));
 *
 * // Bound, never interpolated. See nya_sql_query.
 * NYA_SqlValue args[] = { nya_sql_s64(4200) };
 * NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO runs (score) VALUES (?)", args, 1));
 *
 * NYA_SqlResult result = { 0 };
 * NYA_EXPECT(nya_sql_query(db, arena, "SELECT id, score FROM runs WHERE score > ?", args, 1, &result));
 *
 * nya_array_foreach (result.rows, row) {
 *     NYA_Value* score = nya_object_get(*row, "score");
 * }
 * ```
 *
 * ```c
 * NYA_EXPECT(nya_sql_exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS lines USING vec0(embedding float[384])"));
 *
 * // A vector is bound as a blob of little endian f32, which is what vec0 stores.
 * f32          embedding[384] = { 0 };
 * NYA_SqlValue insert[]       = { nya_sql_s64(line_id), nya_sql_blob((const u8*)embedding, sizeof(embedding)) };
 * NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO lines (rowid, embedding) VALUES (?, ?)", insert, 2));
 *
 * NYA_SqlValue  search[] = { nya_sql_blob((const u8*)query_embedding, sizeof(query_embedding)) };
 * NYA_SqlResult nearest  = { 0 };
 * NYA_EXPECT(nya_sql_query(db, arena, "SELECT rowid, distance FROM lines WHERE embedding MATCH ? AND k = 8", search, 1, &nearest));
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Database  NYA_Database;
typedef struct NYA_SqlResult NYA_SqlResult;
typedef struct NYA_SqlValue  NYA_SqlValue;
typedef enum NYA_SqlValueKind NYA_SqlValueKind;

/** One row. A named typedef because nya_derive_array needs a single token for its type. */
typedef NYA_Object* NYA_SqlRow;
nya_derive_array(NYA_SqlRow);

enum NYA_SqlValueKind {
    NYA_SQL_VALUE_NULL,
    NYA_SQL_VALUE_S64,
    NYA_SQL_VALUE_F64,
    NYA_SQL_VALUE_TEXT,
    NYA_SQL_VALUE_BLOB,
    NYA_SQL_VALUE_COUNT,
};

/**
 * One bound parameter.
 * */
struct NYA_SqlValue {
    NYA_SqlValueKind kind;

    union {
        s64              as_s64;
        f64              as_f64;
        NYA_ConstCString as_text;

        struct {
            const u8* data;
            u64       size;
        } as_blob;
    };
};

struct NYA_SqlResult {
    /** One NYA_Object per row, keyed by column name. Empty rather than null when nothing matched. */
    NYA_ArrayᐸNYA_SqlRowᐳ* rows;

    /** Rows changed by the statement, as sqlite3_changes reports it. Zero for a SELECT. */
    u64 rows_affected;

    /** Rowid of the last successful insert on this connection, or zero when there was none. */
    s64 last_insert_id;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Constructors for bound parameters, so a call site reads as data rather than as struct assembly.
 */
#define nya_sql_null()          ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_NULL })
#define nya_sql_s64(value)      ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_S64, .as_s64 = (value) })
#define nya_sql_f64(value)      ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_F64, .as_f64 = (value) })
#define nya_sql_text(value)     ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_TEXT, .as_text = (value) })
#define nya_sql_blob(ptr, len)  ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_BLOB, .as_blob = { .data = (ptr), .size = (len) } })

/**
 * Opens `path`, creating it if it is not there. Use ":memory:" for a database that never touches disk.
 * */
NYA_API NYA_Error nya_sql_open(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_Database** out_database) __attr_no_discard;

/** Closes the connection. Safe on null, so an unwind path does not need to check. */
NYA_API void nya_sql_close(NYA_Database* database);

/**
 * Runs a statement that returns no rows.
 * */
NYA_API NYA_Error nya_sql_exec(NYA_Database* database, NYA_ConstCString sql) __attr_no_discard;

/** One statement, with parameters bound to its `?` placeholders. Returns no rows. */
NYA_API NYA_Error nya_sql_exec_bound(NYA_Database* database, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count) __attr_no_discard;

/**
 * Runs one statement and collects every row into `out_result`.
 * */
NYA_API NYA_Error nya_sql_query(
    NYA_Database* database, NYA_Arena* arena, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count, OUT NYA_SqlResult* out_result
) __attr_no_discard;

/*
 * Transactions.
 */
NYA_API NYA_Error nya_sql_transaction_begin(NYA_Database* database) __attr_no_discard;
NYA_API NYA_Error nya_sql_transaction_commit(NYA_Database* database) __attr_no_discard;
NYA_API NYA_Error nya_sql_transaction_rollback(NYA_Database* database) __attr_no_discard;

/** The library version SQLite reports, for a log line or a bug report. */
NYA_API NYA_ConstCString nya_sql_version(void) __attr_no_discard;

/** The sqlite-vec version linked in, in upstream's `vX.Y.Z` form. Same purpose as nya_sql_version. */
NYA_API NYA_ConstCString nya_sql_vec_version(void) __attr_no_discard;
