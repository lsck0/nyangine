/**
 * @file sql.h
 *
 * SQLite, in terms of NYA_Object.
 *
 * A row is an NYA_Object keyed by column name, so a query result is the same type a JSON response
 * body is, and either can go through serde to disk without a conversion step in between.
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
 * **Synchronous.** Every call blocks until SQLite is done. On a local file that is microseconds for
 * a keyed lookup and rather more for anything that scans, so treat a query the way you would treat
 * a file read: fine at a load boundary, not in the middle of a frame you care about.
 *
 * A plugin: nothing here is compiled unless -DNYA_PLUGIN_SQLITE is set. See plugins.h for why these
 * are not part of base.
 *
 * ## Extensions
 *
 * Every connection nya_sql_open returns has two extension bundles registered on it, statically
 * linked rather than loaded from disk. Nothing has to be switched on, and there is no API for them
 * beyond SQL itself — they are functions and virtual tables, so they are used by writing a query.
 *
 * **sqlite-vec** adds vector search: a `vec0` virtual table holding embeddings, and `vec_*` scalar
 * functions over float, int8 and binary vectors. A k nearest neighbour lookup is a `MATCH` with a
 * `k`, and comes back as rows like anything else:
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
 *
 * It is brute force, not an approximate index: every query scans every vector in the table. That is
 * fast enough for the tens of thousands a game is likely to have and is not what you would build a
 * web scale search on.
 *
 * **sqlean** adds the string, math, statistics, time, fuzzy matching, unicode, uuid and CSV
 * functions that SQLite leaves out. `median()`, `text_split()`, `levenshtein()`, `uuid4()` and
 * `generate_series()` all come from there. Three of its fourteen extensions are deliberately left
 * out, one of them because it would let a query write files; the reasoning is in
 * plugins/sqlite/sqlean_extensions.c.
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
 *
 * Deliberately its own small type rather than NYA_Value. A bind has to distinguish text from blob,
 * which SQLite stores and compares differently and NYA_Value does not model, and it has to be able
 * to say "null" as a value rather than as an absence.
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
 *
 * The handle is allocated from `arena` and must be closed with nya_sql_close before that arena goes
 * away — closing releases the SQLite connection, which the arena knows nothing about.
 * */
NYA_API NYA_Error nya_sql_open(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_Database** out_database) __attr_no_discard;

/** Closes the connection. Safe on null, so an unwind path does not need to check. */
NYA_API void nya_sql_close(NYA_Database* database);

/**
 * Runs a statement that returns no rows.
 *
 * `sql` may contain several statements separated by semicolons, which is what makes this the right
 * call for schema setup. It takes no parameters for exactly that reason — SQLite binds parameters
 * per statement, and quietly applying them to only the first would be worse than not offering them.
 * Use nya_sql_exec_bound for a single parameterised statement.
 * */
NYA_API NYA_Error nya_sql_exec(NYA_Database* database, NYA_ConstCString sql) __attr_no_discard;

/** One statement, with parameters bound to its `?` placeholders. Returns no rows. */
NYA_API NYA_Error nya_sql_exec_bound(NYA_Database* database, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count) __attr_no_discard;

/**
 * Runs one statement and collects every row into `out_result`.
 *
 * Parameters are **bound**, never interpolated into the string. That is not a style preference:
 * building SQL by concatenating a player's name or a value from a server is how a save file or a
 * leaderboard reply gets to run arbitrary SQL against the local database. There is deliberately no
 * function here that takes a format string.
 *
 * Everything in `out_result` comes from `arena`. A column with a NULL value is present in the row
 * object with a null NYA_Value, rather than being absent, so a caller can tell "no such column"
 * from "column is null".
 * */
NYA_API NYA_Error nya_sql_query(
    NYA_Database* database, NYA_Arena* arena, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count, OUT NYA_SqlResult* out_result
) __attr_no_discard;

/*
 * Transactions.
 *
 * Worth using for more than atomicity: SQLite commits every unwrapped statement on its own, so a
 * thousand inserts outside a transaction are a thousand fsyncs and take roughly a thousand times
 * longer than the same inserts inside one.
 */
NYA_API NYA_Error nya_sql_transaction_begin(NYA_Database* database) __attr_no_discard;
NYA_API NYA_Error nya_sql_transaction_commit(NYA_Database* database) __attr_no_discard;
NYA_API NYA_Error nya_sql_transaction_rollback(NYA_Database* database) __attr_no_discard;

/** The library version SQLite reports, for a log line or a bug report. */
NYA_API NYA_ConstCString nya_sql_version(void) __attr_no_discard;

/** The sqlite-vec version linked in, in upstream's `vX.Y.Z` form. Same purpose as nya_sql_version. */
NYA_API NYA_ConstCString nya_sql_vec_version(void) __attr_no_discard;
