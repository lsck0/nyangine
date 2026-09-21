/**
 * @file orm.h
 *
 * A reflected struct, stored as a row. The schema comes from the `@reflect` table, the primary key
 * from the `@key` annotation on one of its fields, and every value crosses as a bound parameter.
 *
 * It is a small module because the two halves already met: nya_sql_query hands back each row as an
 * NYA_Object, and NYA_Object is exactly what nya_reflect_from_object reads. Loading a row into a
 * struct is therefore that one call and no conversion in between. Only the other direction is work,
 * since binding a parameter is not a document, and that is what this module is.
 *
 * Overview:
 *   nya_orm_open / nya_orm_close            bind a described type to a table, or refuse the type
 *   nya_orm_schema_create / _destroy        CREATE TABLE IF NOT EXISTS, DROP TABLE IF EXISTS
 *   nya_orm_schema_check                    what the table and the struct disagree about
 *   nya_orm_insert / nya_orm_delete         one row in, one row out, by key
 *   nya_orm_update                          every column of one row, by key
 *   nya_orm_find                            one row by key, into a struct
 *   nya_orm_select                          many rows, into an array of structs
 *   nya_orm_column_type                     which sqlite type a described field maps to
 *   nya_orm_at                              the i-th struct of what nya_orm_select returned
 *
 * ```c
 * // @reflect
 * struct GameRun {
 *     u32  id;            // @key
 *     u32  generations;
 *     f64  fitness;
 *     char ended[32];
 * };
 *
 * NYA_OrmTable* runs = nullptr;
 * NYA_TRY(nya_orm_open(arena, database, nya_reflect_of(GameRun), "runs", &runs));
 * defer nya_orm_close(runs);
 *
 * NYA_TRY(nya_orm_schema_create(runs));
 *
 * // id is zero, so sqlite assigns the rowid and nya_orm_insert writes it back into the struct.
 * GameRun run = { .generations = 12, .fitness = 0.75 };
 * NYA_TRY(nya_orm_insert(runs, &run));
 *
 * void* rows  = nullptr;
 * u32   count = 0;
 * NYA_TRY(nya_orm_select(runs, arena, "WHERE fitness > ? ORDER BY fitness DESC", (NYA_SqlValue[]){ nya_sql_f64(0.5) }, 1, &rows, &count));
 *
 * for (u32 i = 0; i < count; i++) {
 *     GameRun* loaded = nya_orm_at(runs, rows, i);
 * }
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * THE SCHEMA
 * ─────────────────────────────────────────────────────────
 *
 * One column per described field, named after the field, in the type's own order. What maps:
 *
 * | Described field                  | Column    | Notes                                              |
 * | :------------------------------- | :-------- | :------------------------------------------------- |
 * | `b8`..`b64`, `u8`..`u64`, `s8`..`s64`, `char` | `INTEGER` | a boolean stores 0 or 1              |
 * | `f32`, `f64`                     | `REAL`    |                                                     |
 * | `char*`                          | `TEXT`    | see the lifetime note below                         |
 * | `char[N]`                        | `TEXT`    | truncated on the way in, as reflection already does |
 * | an enum                          | `TEXT`    | the variant's name, so renumbering cannot reinterpret old rows |
 * | a `@bitflags` enum               | `INTEGER` | a set of names is a list, and a list is not a column |
 *
 * Everything else is refused by nya_orm_open, naming the field: a nested struct or union, a vector,
 * an array of anything but `char`, a pointer to anything but `char`. `@skip` the field and store it
 * by hand, or flatten it into the struct. A field reflection itself cannot describe never gets here.
 *
 * Rejected, at the point somebody would add it: serializing a nested struct into a TEXT column. It
 * would make every type mappable, and it buys a column nothing can query, order or index — a file
 * with extra steps — while tying the rows to a serde format version. A struct worth a column per
 * field is worth writing as one.
 *
 * The key is the `@key` field, exactly one per type, and it must be an integer or text. An integer
 * key is declared `INTEGER PRIMARY KEY`, which is sqlite's rowid alias, so an insert of a struct
 * whose key is zero lets sqlite assign it and nya_orm_insert writes the assigned value back into the
 * struct. A text key is never assigned: an empty one is refused, because a row has to be findable.
 *
 * Nothing else is declared. No NOT NULL, no DEFAULT, no UNIQUE, no index: this module names every
 * column in every statement it runs and so relies on no constraint, and declaring constraints it does
 * not need would make an existing table created without them look like a different schema. Add them
 * with nya_sql_exec if the data wants them; nya_orm_schema_check ignores them.
 *
 * ─────────────────────────────────────────────────────────
 * MIGRATION IS DETECTED, NEVER PERFORMED
 * ─────────────────────────────────────────────────────────
 *
 * A schema that has drifted from the struct is the thing that actually bites, so it is said out loud
 * rather than worked around. This module never runs ALTER TABLE, never rebuilds a table and never
 * drops data to make a struct fit.
 *
 * nya_orm_schema_create creates the table when it is absent and then checks the one that is there. It
 * fails, naming the columns, when the table is missing a column the struct describes, when a column's
 * declared type is not the one the field maps to, or when the key field is not the table's primary
 * key. Each of those loses data silently if it is worked against: a missing column makes every insert
 * fail, and a column whose affinity disagrees converts the value on the way in and drops it on the way
 * back out.
 *
 * A column no field describes is a warning and nothing more. Every statement here names its columns,
 * so a column this build has never heard of is neither read nor written, which is what lets an older
 * build keep working against a table a newer one has grown.
 *
 * What to do about a failure is the caller's, and there are only three honest answers: write the
 * ALTER TABLE yourself with nya_sql_exec, copy the rows into a new table and rename it, or delete the
 * file and start again. A generated migration would have to guess which rename was a rename and which
 * was a drop, and a wrong guess is somebody's save file.
 *
 * ─────────────────────────────────────────────────────────
 * RELATIONS ARE NOT IN SCOPE
 * ─────────────────────────────────────────────────────────
 *
 * No joins, no foreign keys, no lazy loading, no identity map, no dirty tracking, no query builder.
 * Written here because this is the file somebody would add them to.
 *
 * The reason is that each one stops being a mapping and starts being a framework. A relation needs a
 * type to describe another type's rows, which reflection cannot say; loading one needs a cache to
 * decide when to load it, which is state nobody asked for; and a query builder needs an expression
 * type that is a worse SQL than SQL. What this module owes is the boring half nobody wants to hand
 * write, which is one column per field and one bound parameter per column.
 *
 * Two tables that reference each other are two nya_orm_open calls and a `WHERE other_id = ?` passed
 * to nya_orm_select, which is a line of SQL in the caller and no machinery here.
 *
 * ─────────────────────────────────────────────────────────
 * WHAT ELSE TO KNOW BEFORE CALLING IT
 * ─────────────────────────────────────────────────────────
 *
 * - **A value never becomes SQL text.** Every column crosses as an NYA_SqlValue bound to a `?`, which
 *   is a security property and not a preference. The only strings this module builds are from field
 *   names, and the only one a caller supplies is `clauses` on nya_orm_select, which is SQL and must be
 *   written in the source. Never build it out of data.
 * - **The table name is parsed, not trusted.** Letters, digits and underscore, starting with a letter
 *   or underscore, because an identifier cannot be bound and so cannot be checked by sqlite.
 * - **Not thread safe**, exactly as the connection underneath is not. One NYA_OrmTable belongs to one
 *   thread, and two of them over one NYA_Database belong to the same one.
 * - **The statements are built once**, at nya_orm_open, into the arena it is given. Insert, update,
 *   delete and find allocate nothing at all; only the two that read rows take an arena, because the
 *   rows and the structs come out of it.
 * - **A `char*` column loads as a pointer into the arena** nya_orm_select or nya_orm_find was given,
 *   not as a copy the struct owns. It dies with that arena. A `char[N]` field is the struct's own
 *   storage and has no such rule, which is the reason to prefer it.
 * - **An enum value with no variant name is refused** on the way in, naming the field and the value,
 *   because storing the number in a column declared TEXT would not survive the trip back. Give the
 *   enum a variant for the value, including zero.
 * - **A NULL column leaves its field alone.** nya_orm_select zeroes each struct first, so in practice
 *   a NULL loads as zero; that is nya_reflect_from_object's rule, not a choice made here.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_types.h"
#include "nyangine/plugins/sqlite/sql.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Columns one table may have, which is fields one described type may have.
 *
 * Sixty-four because a row is a record and not a spreadsheet: the widest described type in this tree
 * is under twenty fields, and a struct past sixty-four wants splitting long before it wants storing.
 * A type with more is refused by nya_orm_open rather than truncated. Registered as the `orm_columns`
 * ceiling, whose live count is the widest table opened so far.
 * */
#ifndef NYA_ORM_COLUMN_MAX
#define NYA_ORM_COLUMN_MAX 64
#endif

/** Longest table name, terminator included. Long enough for any name a schema should have. */
#define NYA_ORM_NAME_MAX 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_OrmColumnType NYA_OrmColumnType;
typedef struct NYA_OrmTable    NYA_OrmTable;

/** The sqlite storage classes a described field can map to. */
enum NYA_OrmColumnType {
    NYA_ORM_COLUMN_INTEGER,
    NYA_ORM_COLUMN_REAL,
    NYA_ORM_COLUMN_TEXT,
    NYA_ORM_COLUMN_COUNT,
};

/** As the column is declared, which is also what PRAGMA table_info reports back. */
__attr_allow_unused static NYA_ConstCString NYA_ORM_COLUMN_TYPE_NAME_MAP[NYA_ORM_COLUMN_COUNT] = {
    [NYA_ORM_COLUMN_INTEGER] = "INTEGER",
    [NYA_ORM_COLUMN_REAL]    = "REAL",
    [NYA_ORM_COLUMN_TEXT]    = "TEXT",
};

/**
 * One difference between the table and the struct. `column` names it, `found` says what the table has
 * and `expected` what the struct wanted. All three live only for the duration of the call.
 *
 * The same shape as NYA_ReflectReportFn on purpose: a document that does not match its type and a
 * table that does not match its type are the same finding, and a reporter written for one prints the
 * other.
 * */
typedef void (*NYA_OrmReportFn)(NYA_ConstCString column, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

/**
 * One described type bound to one table. Plain data, built entirely by nya_orm_open: every statement
 * below is derived from `type` and `name`, so editing a field of this desynchronizes the rest.
 * */
struct NYA_OrmTable {
    NYA_Database* database;

    const NYA_TypeReflection* type;

    /** The table, parsed at open and copied here, so it outlives whatever the caller passed. */
    char name[NYA_ORM_NAME_MAX];

    /** The `@key` field. Never null: a type without exactly one usable key is refused at open. */
    const NYA_ReflectField* key;

    /** Whether the key is `INTEGER PRIMARY KEY`, and so assigned by sqlite when it is zero. */
    b8 key_is_integer;

    /** Every field that became a column, in the type's order. `columns[key_index]` is `key`. */
    const NYA_ReflectField* columns[NYA_ORM_COLUMN_MAX];
    u32                     column_count;
    u32                     key_index;

    /*
     * The statements, built once at open. Every value in them is a `?`.
     */
    NYA_ConstCString sql_create;
    NYA_ConstCString sql_insert;

    /** The insert with the key column left out, so sqlite assigns the rowid. Null for a text key. */
    NYA_ConstCString sql_insert_assigned;

    NYA_ConstCString sql_update;
    NYA_ConstCString sql_delete;

    /** `SELECT a, b, c FROM name`, with whatever a caller appends to it. */
    NYA_ConstCString sql_select;

    /** The same with `WHERE key = ?`. */
    NYA_ConstCString sql_find;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The `index`-th struct of what nya_orm_select returned. They are contiguous and `type->size` apart. */
#define nya_orm_at(table, instances, index) ((void*)((u8*)(instances) + ((u64)(index) * (table)->type->size)))

/**
 * Which column type describes `type`, or false when nothing does. The whole of the mapping, in one
 * place, so the schema, the binding and the drift check cannot disagree about it.
 * */
NYA_API b8 nya_orm_column_type(const NYA_TypeReflection* type, OUT NYA_OrmColumnType* out_column);

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

/**
 * Binds `type` to the table called `table_name` on `database`, building every statement into `arena`.
 * Touches the database not at all: this is the definition, and nya_orm_schema_create is what runs.
 *
 * Every reason the type cannot be a table is an error here rather than a surprise at the first
 * insert, naming the field: no `@key` or a key that is not an integer or text, a field whose type has
 * no column, more than NYA_ORM_COLUMN_MAX fields, no fields at all, or a table name that is not an
 * identifier.
 * */
NYA_API NYA_Error nya_orm_open(
    NYA_Arena* arena, NYA_Database* database, const NYA_TypeReflection* type, NYA_ConstCString table_name, OUT NYA_OrmTable** out_table
) __attr_no_discard;

/**
 * Releases the binding. Frees nothing, because the arena passed to nya_orm_open owns all of it; it
 * clears the table so a call after it is caught rather than run against a closed connection. Safe on
 * null and idempotent, so an unwind path does not have to check.
 * */
NYA_API void nya_orm_close(NYA_OrmTable* table);

/*
 * ─────────────────────────────────────────────────────────
 * THE SCHEMA
 * ─────────────────────────────────────────────────────────
 */

/**
 * `CREATE TABLE IF NOT EXISTS`, then nya_orm_schema_check over whatever is now there.
 *
 * Fails when the table that exists has drifted from the struct in a way that loses data, having
 * logged every difference as a warning first. See the migration note in this file's block: nothing
 * here alters a table, and a drifted one is refused rather than written to.
 * */
NYA_API NYA_Error nya_orm_schema_create(NYA_OrmTable* table) __attr_no_discard;

/** `DROP TABLE IF EXISTS`. The partner of nya_orm_schema_create, and as final as it sounds. */
NYA_API NYA_Error nya_orm_schema_destroy(NYA_OrmTable* table) __attr_no_discard;

/**
 * Reads the table's real columns and reports every difference from the struct, returning how many it
 * found, so a caller that only wants to know whether the schema is clean need not install a reporter.
 *
 * Reported and counted: the table missing entirely, a column the struct describes that is not there,
 * a column whose declared type is not the field's, a key field that is not the table's primary key,
 * and a column no field describes. Not compared: NOT NULL, defaults, uniqueness, collation, indexes
 * and column order, none of which this module relies on. Only the first four make
 * nya_orm_schema_create fail; see the migration note.
 *
 * Allocates from a scratch arena of its own and releases it before returning.
 * */
NYA_API u32 nya_orm_schema_check(NYA_OrmTable* table, NYA_OrmReportFn report, void* user_data);

/*
 * ─────────────────────────────────────────────────────────
 * ROWS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Inserts `instance` as one row, every field bound to a parameter.
 *
 * Takes it mutably because of the one case where the database decides a value: an integer key that is
 * zero is left out of the statement, sqlite assigns the rowid, and it is written back into the
 * struct's key field so the caller holds the row's identity afterwards. A non-zero key is stored as
 * it is, and a text key is always stored as it is and may not be empty.
 * */
NYA_API NYA_Error nya_orm_insert(NYA_OrmTable* table, void* instance) __attr_no_discard;

/**
 * Writes every non-key column of the row whose key `instance` holds. Not an upsert: a key that is not
 * in the table matches nothing and changes nothing, which is reported as NYA_ERROR_NOT_FOUND rather
 * than left to look like success.
 * */
NYA_API NYA_Error nya_orm_update(NYA_OrmTable* table, const void* instance) __attr_no_discard;

/**
 * Deletes the row with `key`. A key that matches nothing is NYA_ERROR_NOT_FOUND, for the same reason.
 * Build the value with nya_sql_s64 or nya_sql_text, matching the key's column type.
 * */
NYA_API NYA_Error nya_orm_delete(NYA_OrmTable* table, NYA_SqlValue key) __attr_no_discard;

/**
 * Reads the row with `key` over `out_instance`, which is zeroed first and must be `type->size` bytes.
 * NYA_ERROR_NOT_FOUND when there is no such row, so absence is in the return rather than in the data.
 *
 * `arena` holds the row the values are read from, which matters only for a `char*` field, whose
 * pointer aliases it.
 * */
NYA_API NYA_Error nya_orm_find(NYA_OrmTable* table, NYA_Arena* arena, NYA_SqlValue key, OUT void* out_instance) __attr_no_discard;

/**
 * Reads every matching row into a fresh array of structs allocated from `arena`, each zeroed and then
 * filled by nya_reflect_from_object. Reach one with nya_orm_at. No rows is success with a count of
 * zero, not an error.
 *
 * `clauses` is everything after the table name — a `WHERE`, an `ORDER BY`, a `LIMIT`, or null for
 * every row — and it is SQL, so it belongs in the source as a literal. Values go in `values`, one per
 * `?` in it, and the count is checked against the statement before anything runs.
 * */
NYA_API NYA_Error nya_orm_select(
    NYA_OrmTable*       table,
    NYA_Arena*          arena,
    NYA_ConstCString    clauses,
    const NYA_SqlValue* values,
    u32                 value_count,
    OUT void**          out_instances,
    OUT u32*            out_count
) __attr_no_discard;
