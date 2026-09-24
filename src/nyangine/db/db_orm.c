#include "nyangine/nyangine.h"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** The widest table opened so far, which is what the `orm_columns` ceiling shows. */
NYA_INTERNAL u32 _nya_orm_columns_live = 0;

/**
 * Parses a table name into `out`. An identifier cannot be a bound parameter, so it is the one piece
 * of a statement that comes from the caller as text, and it is parsed rather than trusted.
 * */
NYA_INTERNAL NYA_Error _nya_orm_parse_name(NYA_ConstCString name, OUT char* out, u64 capacity);

/** Builds every statement the table will ever run, once, into `arena`. */
NYA_INTERNAL NYA_Error _nya_orm_build_statements(NYA_OrmTable* table, NYA_Arena* arena);

/** One field of `instance` as a bound parameter. The whole of the struct-to-row direction. */
NYA_INTERNAL NYA_Error _nya_orm_bind(const NYA_OrmTable* table, const NYA_ReflectField* field, const void* instance, OUT NYA_SqlValue* out_value);

/** Whether `key` is the shape this table's key column takes, so a mismatch is said rather than matching nothing. */
NYA_INTERNAL NYA_Error _nya_orm_check_key(const NYA_OrmTable* table, NYA_SqlValue key);

/** One row of `PRAGMA table_info`, as this module reads it. */
typedef struct {
    NYA_ConstCString name;
    NYA_ConstCString type;
    b8               is_primary_key;
} _NYA_OrmColumnInfo;

/** Reads the table's real columns into `out_columns`, and answers how many there are. */
NYA_INTERNAL NYA_Error
_nya_orm_table_info(NYA_OrmTable* table, NYA_Arena* arena, OUT _NYA_OrmColumnInfo* out_columns, u32 capacity, OUT u32* out_count);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

b8 nya_orm_column_type(const NYA_TypeReflection* type, OUT NYA_OrmColumnType* out_column) {
    if (type == nullptr || out_column == nullptr) return false;

    if (type->kind == NYA_REFLECT_ENUM) {
        // Flags stay integer (a list of names is not a column); a plain enum keeps its name. See db_orm.h.
        *out_column = type->is_bitflags ? NYA_ORM_COLUMN_INTEGER : NYA_ORM_COLUMN_TEXT;
        return true;
    }

    if (nya_reflect_is_char_array(type)) {
        *out_column = NYA_ORM_COLUMN_TEXT;
        return true;
    }

    if (type->kind != NYA_REFLECT_PRIMITIVE) return false;

    switch (type->primitive) {
        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_U8:
        case NYA_TYPE_U16:
        case NYA_TYPE_U32:
        case NYA_TYPE_U64:
        case NYA_TYPE_S8:
        case NYA_TYPE_S16:
        case NYA_TYPE_S32:
        case NYA_TYPE_S64:
        case NYA_TYPE_CHAR:   *out_column = NYA_ORM_COLUMN_INTEGER; return true;

        case NYA_TYPE_F32:
        case NYA_TYPE_F64:    *out_column = NYA_ORM_COLUMN_REAL; return true;

        case NYA_TYPE_STRING: *out_column = NYA_ORM_COLUMN_TEXT; return true;

        // Everything else on purpose: 128-bit widths, f16, wide chars and pointer primitives have no storage class here and are refused at open.
        default:              return false;
    }
}

// ───────────────────────────────────── LIFETIME ─────────────────────────────────────

NYA_Error nya_orm_open(
    NYA_Arena* arena, NYA_Database* database, const NYA_TypeReflection* type, NYA_ConstCString table_name, OUT NYA_OrmTable** out_table
) {
    nya_assert(arena != nullptr);
    nya_assert(out_table != nullptr);

    if (database == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no database");
    if (type == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no type");

    if (type->kind != NYA_REFLECT_STRUCT) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' is not a struct, and only a struct is a row", type->name);
    }

    NYA_OrmTable* table = nya_arena_alloc(arena, sizeof(NYA_OrmTable));
    *table              = (NYA_OrmTable){ .database = database, .type = type };

    NYA_TRY(_nya_orm_parse_name(table_name, table->name, sizeof(table->name)));

    if (type->field_count > NYA_ORM_COLUMN_MAX) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' has " FMTu32 " fields, more than the %d columns a table may have", type->name,
                         type->field_count, NYA_ORM_COLUMN_MAX);
    }

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField* field = &type->fields[i];

        // The generator's placeholder for a struct whose every field was skipped; see reflection.c.
        if (field->type == nullptr || field->name == nullptr) continue;

        NYA_OrmColumnType column = NYA_ORM_COLUMN_COUNT;

        // Refused here rather than at the first insert: a wrong type is a fact about the definition, not any particular row.
        if (!nya_orm_column_type(field->type, &column)) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s.%s' is a '%s', which has no column type; @skip it or flatten it into the struct",
                             type->name, field->name, field->type->name);
        }

        if (field->is_key) {
            if (table->key != nullptr) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has a second @key on '%s'; a row has one identity", type->name, field->name);
            }

            // A float is not an identity: two that compare equal need not be the same bits, and sqlite's rowid alias is an integer.
            if (column == NYA_ORM_COLUMN_REAL) {
                return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s.%s' is a @key of real type; a key has to be an integer or text", type->name,
                                 field->name);
            }

            table->key            = field;
            table->key_index      = table->column_count;
            table->key_is_integer = column == NYA_ORM_COLUMN_INTEGER;
        }

        table->columns[table->column_count] = field;
        table->column_count++;
    }

    if (table->column_count == 0) return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' has no field that could be a column", type->name);

    if (table->key == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has no @key field, so no row of it could be found again", type->name);
    }

    NYA_TRY(_nya_orm_build_statements(table, arena));

    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        // Columns are per table, so the live count is the widest table opened so far: how close any one type is to the ceiling.
        nya_ceiling_register("orm_columns", NYA_ORM_COLUMN_MAX, &_nya_orm_columns_live);
        ceiling_registered = true;
    }

    if (table->column_count > _nya_orm_columns_live) _nya_orm_columns_live = table->column_count;

    *out_table = table;
    return NYA_OK;
}

void nya_orm_close(NYA_OrmTable* table) {
    if (table == nullptr) return;

    // Zeroed rather than freed (the open arena owns every byte): the per-call assert then fires on a closed table instead of running against a since-closed connection.
    *table = (NYA_OrmTable){ 0 };
}

// ───────────────────────────────────── THE SCHEMA ─────────────────────────────────────

/** Logs one schema difference. The reporter nya_orm_schema_create installs. */
NYA_INTERNAL void _nya_orm_report_to_log(NYA_ConstCString column, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    nya_log_warn("%s: column '%s' is %s, expected %s.", (NYA_ConstCString)user_data, column, found, expected);
}

u32 nya_orm_schema_check(NYA_OrmTable* table, NYA_OrmReportFn report, void* user_data) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_schema_check");
    defer     nya_arena_destroy_on_stack(&scratch);

    // Sized to the column max even though the table may carry unknown columns: only the first NYA_ORM_COLUMN_MAX are inspected.
    _NYA_OrmColumnInfo actual[NYA_ORM_COLUMN_MAX] = { 0 };
    u32                actual_count               = 0;

    NYA_Error read = _nya_orm_table_info(table, &scratch, actual, nya_carray_length(actual), &actual_count);
    if (!read.ok) {
        if (report != nullptr) report(table->name, (NYA_ConstCString)read.message, "a readable table", user_data);
        return 1;
    }

    if (actual_count == 0) {
        if (report != nullptr) report(table->name, "not a table in this database", "the table to exist", user_data);
        return 1;
    }

    u32 problems = 0;

    for (u32 i = 0; i < table->column_count; i++) {
        const NYA_ReflectField* field  = table->columns[i];
        NYA_OrmColumnType       column = NYA_ORM_COLUMN_COUNT;

        // Cannot fail (checked at open); read again rather than cached so schema, binding and this check ask the same function.
        (void)nya_orm_column_type(field->type, &column);

        const _NYA_OrmColumnInfo* found = nullptr;

        for (u32 c = 0; c < actual_count; c++) {
            if (nya_string_equals(actual[c].name, field->name)) found = &actual[c];
        }

        if (found == nullptr) {
            if (report != nullptr) report(field->name, "not a column of the table", NYA_ORM_COLUMN_TYPE_NAME_MAP[column], user_data);
            problems++;
            continue;
        }

        // Case insensitively, since a hand-written table may say `integer`. Only the declared type is compared, since its affinity is what would lose data.
        NYA_String* declared = nya_string_from(&scratch, found->type);
        nya_string_to_upper(declared);

        if (!nya_string_equals(declared, NYA_ORM_COLUMN_TYPE_NAME_MAP[column])) {
            if (report != nullptr) report(field->name, found->type, NYA_ORM_COLUMN_TYPE_NAME_MAP[column], user_data);
            problems++;
        }

        if (field == table->key && !found->is_primary_key) {
            if (report != nullptr) report(field->name, "not the table's primary key", "the primary key", user_data);
            problems++;
        }
    }

    // The other direction: a warning, not a failure. Every statement names its columns, so a column no field describes is never read or written. See db_orm.h.
    for (u32 c = 0; c < actual_count; c++) {
        b8 described = false;

        for (u32 i = 0; i < table->column_count; i++) {
            if (nya_string_equals(actual[c].name, table->columns[i]->name)) described = true;
        }

        if (described) continue;

        if (report != nullptr) report(actual[c].name, "a column no field of the struct describes", "nothing, and it is left alone", user_data);
        problems++;
    }

    return problems;
}

NYA_Error nya_orm_schema_create(NYA_OrmTable* table) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    NYA_TRY(nya_sql_exec(table->database, table->sql_create));

    if (nya_orm_schema_check(table, _nya_orm_report_to_log, (void*)table->type->name) == 0) return NYA_OK;

    // Counted a second time: the return is decided only by the data-losing subset (missing column, disagreeing type, wrong key), not an extra column.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_schema_create");
    defer     nya_arena_destroy_on_stack(&scratch);

    _NYA_OrmColumnInfo actual[NYA_ORM_COLUMN_MAX] = { 0 };
    u32                actual_count               = 0;

    NYA_TRY(_nya_orm_table_info(table, &scratch, actual, nya_carray_length(actual), &actual_count));

    for (u32 i = 0; i < table->column_count; i++) {
        const NYA_ReflectField* field  = table->columns[i];
        NYA_OrmColumnType       column = NYA_ORM_COLUMN_COUNT;

        (void)nya_orm_column_type(field->type, &column);

        const _NYA_OrmColumnInfo* found = nullptr;

        for (u32 c = 0; c < actual_count; c++) {
            if (nya_string_equals(actual[c].name, field->name)) found = &actual[c];
        }

        if (found == nullptr) {
            return nya_error(NYA_ERROR_CORRUPT, "'%s' has no column '%s'; see the migration note in db_orm.h", table->name, field->name);
        }

        NYA_String* declared = nya_string_from(&scratch, found->type);
        nya_string_to_upper(declared);

        if (!nya_string_equals(declared, NYA_ORM_COLUMN_TYPE_NAME_MAP[column])) {
            return nya_error(NYA_ERROR_CORRUPT, "'%s.%s' is declared %s, not %s; see the migration note in db_orm.h", table->name, field->name,
                             found->type, NYA_ORM_COLUMN_TYPE_NAME_MAP[column]);
        }

        if (field == table->key && !found->is_primary_key) {
            return nya_error(NYA_ERROR_CORRUPT, "'%s.%s' is not the table's primary key; see the migration note in db_orm.h", table->name,
                             field->name);
        }
    }

    return NYA_OK;
}

NYA_Error nya_orm_schema_destroy(NYA_OrmTable* table) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_schema_destroy");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_String* sql = nya_string_sprintf(&scratch, "DROP TABLE IF EXISTS %s", table->name);

    return nya_sql_exec(table->database, nya_string_to_cstring(&scratch, sql));
}

// ───────────────────────────────────── ROWS ─────────────────────────────────────

NYA_Error nya_orm_insert(NYA_OrmTable* table, void* instance) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    if (instance == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no instance to insert");

    NYA_SqlValue key = { 0 };
    NYA_TRY(_nya_orm_bind(table, table->key, instance, &key));

    // The one value the database decides: a zero integer key means "give me one", which sqlite's rowid alias does for a column left out.
    b8 assigned = table->key_is_integer && key.as_s64 == 0;

    if (!table->key_is_integer && (key.kind != NYA_SQL_VALUE_TEXT || key.as_text[0] == '\0')) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' is empty; a text key has to identify the row", table->type->name,
                         table->key->name);
    }

    NYA_SqlValue values[NYA_ORM_COLUMN_MAX] = { 0 };
    u32          value_count                = 0;

    for (u32 i = 0; i < table->column_count; i++) {
        if (assigned && i == table->key_index) continue;

        NYA_TRY(_nya_orm_bind(table, table->columns[i], instance, &values[value_count]));
        value_count++;
    }

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_insert");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(table->database, &scratch, assigned ? table->sql_insert_assigned : table->sql_insert, values, value_count, &result));

    // Written back, so the caller holds the row's identity rather than a zero that names nothing.
    if (assigned) {
        void* address = nya_reflect_field_pointer(instance, table->key);

        (void)nya_reflect_write(table->key->type, address, (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = result.last_insert_id });
    }

    return NYA_OK;
}

NYA_Error nya_orm_update(NYA_OrmTable* table, const void* instance) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    if (instance == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no instance to update");

    if (table->sql_update == nullptr) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' is nothing but its key, so there is no column to update", table->type->name);
    }

    NYA_SqlValue values[NYA_ORM_COLUMN_MAX] = { 0 };
    u32          value_count                = 0;

    for (u32 i = 0; i < table->column_count; i++) {
        if (i == table->key_index) continue;

        NYA_TRY(_nya_orm_bind(table, table->columns[i], instance, &values[value_count]));
        value_count++;
    }

    // Last, because the key is the statement's trailing `WHERE key = ?`.
    NYA_TRY(_nya_orm_bind(table, table->key, instance, &values[value_count]));
    value_count++;

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_update");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(table->database, &scratch, table->sql_update, values, value_count, &result));

    // An update that matched nothing is not an update, so it is said rather than returned as success.
    if (result.rows_affected == 0) {
        return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no row with that %s", table->name, table->key->name);
    }

    return NYA_OK;
}

NYA_Error nya_orm_delete(NYA_OrmTable* table, NYA_SqlValue key) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    NYA_TRY(_nya_orm_check_key(table, key));

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_delete");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(table->database, &scratch, table->sql_delete, &key, 1, &result));

    if (result.rows_affected == 0) {
        return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no row with that %s", table->name, table->key->name);
    }

    return NYA_OK;
}

NYA_Error nya_orm_find(NYA_OrmTable* table, NYA_Arena* arena, NYA_SqlValue key, OUT void* out_instance) {
    nya_assert(table != nullptr);
    nya_assert(arena != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    if (out_instance == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nowhere to put the row");

    NYA_TRY(_nya_orm_check_key(table, key));

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(table->database, arena, table->sql_find, &key, 1, &result));

    if (result.rows->length == 0) return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no row with that %s", table->name, table->key->name);

    // Zeroed first: nya_reflect_from_object leaves an unmentioned field alone, and a NULL column is one it does not write.
    nya_memset(out_instance, 0, table->type->size);

    return nya_reflect_from_object(table->type, out_instance, result.rows->items[0]);
}

NYA_Error nya_orm_select(
    NYA_OrmTable*       table,
    NYA_Arena*          arena,
    NYA_ConstCString    clauses,
    const NYA_SqlValue* values,
    u32                 value_count,
    OUT void**          out_instances,
    OUT u32*            out_count
) {
    nya_assert(table != nullptr);
    nya_assert(arena != nullptr);
    nya_assert(out_instances != nullptr);
    nya_assert(out_count != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    *out_instances = nullptr;
    *out_count     = 0;

    NYA_ConstCString sql = table->sql_select;

    // The only string a caller contributes, and it is SQL not data; values bind to its `?`, count-checked by nya_sql_query. See db_orm.h.
    if (clauses != nullptr && clauses[0] != '\0') {
        NYA_String* whole = nya_string_sprintf(arena, "%s %s", table->sql_select, clauses);
        sql               = nya_string_to_cstring(arena, whole);
    }

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(table->database, arena, sql, values, value_count, &result));

    if (result.rows->length == 0) return NYA_OK;

    u32   count     = (u32)result.rows->length;
    void* instances = nya_arena_alloc(arena, table->type->size * count);

    nya_memset(instances, 0, table->type->size * count);

    for (u32 i = 0; i < count; i++) {
        // The whole row-to-struct direction: a row is already the document a described type reads from.
        NYA_TRY(nya_reflect_from_object(table->type, nya_orm_at(table, instances, i), result.rows->items[i]));
    }

    *out_instances = instances;
    *out_count     = count;

    return NYA_OK;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_orm_parse_name(NYA_ConstCString name, OUT char* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 1);

    if (name == nullptr || name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the table name is empty");

    u64 length = strlen(name);

    if (length >= capacity) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the table name '%s' is longer than " FMTu64 " bytes", name, capacity - 1);
    }

    for (u64 i = 0; i < length; i++) {
        char character = name[i];

        b8 letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
        b8 digit  = character >= '0' && character <= '9';

        // A leading digit is refused: whatever goes into a statement unquoted must be an identifier, and "1; DROP TABLE" starts with a digit.
        if (letter || (digit && i > 0)) continue;

        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the table name '%s' is not an identifier: '%c' at " FMTu64, name, character, i);
    }

    nya_memcpy(out, name, length);
    out[length] = '\0';

    return NYA_OK;
}

NYA_Error _nya_orm_build_statements(NYA_OrmTable* table, NYA_Arena* arena) {
    nya_assert(table != nullptr);
    nya_assert(table->key != nullptr);
    nya_assert(table->column_count > 0);

    NYA_String* definitions = nya_string_create(arena);
    NYA_String* names       = nya_string_create(arena);
    NYA_String* marks       = nya_string_create(arena);
    NYA_String* other_names = nya_string_create(arena);
    NYA_String* other_marks = nya_string_create(arena);
    NYA_String* assignments = nya_string_create(arena);

    u32 others = 0;

    for (u32 i = 0; i < table->column_count; i++) {
        const NYA_ReflectField* field  = table->columns[i];
        NYA_OrmColumnType       column = NYA_ORM_COLUMN_COUNT;

        (void)nya_orm_column_type(field->type, &column);

        NYA_ConstCString separator = i > 0 ? ", " : "";

        // Nothing but the type and, for the key, PRIMARY KEY; see db_orm.h for why no unneeded constraint is declared.
        nya_string_extend_sprintf(definitions, "%s%s %s%s", separator, field->name, NYA_ORM_COLUMN_TYPE_NAME_MAP[column],
                                  field == table->key ? " PRIMARY KEY" : "");

        nya_string_extend_sprintf(names, "%s%s", separator, field->name);
        nya_string_extend_sprintf(marks, "%s?", separator);

        if (i == table->key_index) continue;

        NYA_ConstCString other_separator = others > 0 ? ", " : "";

        nya_string_extend_sprintf(other_names, "%s%s", other_separator, field->name);
        nya_string_extend_sprintf(other_marks, "%s?", other_separator);
        nya_string_extend_sprintf(assignments, "%s%s = ?", other_separator, field->name);

        others++;
    }

    NYA_ConstCString name         = table->name;
    NYA_ConstCString column_names = nya_string_to_cstring(arena, names);

    table->sql_create = nya_string_to_cstring(
        arena, nya_string_sprintf(arena, "CREATE TABLE IF NOT EXISTS %s (%s)", name, nya_string_to_cstring(arena, definitions))
    );

    table->sql_insert = nya_string_to_cstring(
        arena, nya_string_sprintf(arena, "INSERT INTO %s (%s) VALUES (%s)", name, column_names, nya_string_to_cstring(arena, marks))
    );

    // A type that is nothing but its key still inserts: sqlite spells an all-defaults row this way, since `INSERT INTO t () VALUES ()` is invalid.
    if (table->key_is_integer) {
        table->sql_insert_assigned =
            others == 0 ? nya_string_to_cstring(arena, nya_string_sprintf(arena, "INSERT INTO %s DEFAULT VALUES", name))
                        : nya_string_to_cstring(arena, nya_string_sprintf(arena, "INSERT INTO %s (%s) VALUES (%s)", name,
                                                                          nya_string_to_cstring(arena, other_names),
                                                                          nya_string_to_cstring(arena, other_marks)));
    }

    // Left null when there is nothing but the key, so nya_orm_update says so rather than running `UPDATE t SET WHERE id = ?`.
    if (others > 0) {
        table->sql_update = nya_string_to_cstring(
            arena, nya_string_sprintf(arena, "UPDATE %s SET %s WHERE %s = ?", name, nya_string_to_cstring(arena, assignments), table->key->name)
        );
    }

    table->sql_delete = nya_string_to_cstring(arena, nya_string_sprintf(arena, "DELETE FROM %s WHERE %s = ?", name, table->key->name));
    table->sql_select = nya_string_to_cstring(arena, nya_string_sprintf(arena, "SELECT %s FROM %s", column_names, name));
    table->sql_find = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s WHERE %s = ?", table->sql_select, table->key->name));

    return NYA_OK;
}

NYA_Error _nya_orm_bind(const NYA_OrmTable* table, const NYA_ReflectField* field, const void* instance, OUT NYA_SqlValue* out_value) {
    nya_assert(field != nullptr);
    nya_assert(instance != nullptr);
    nya_assert(out_value != nullptr);

    const NYA_TypeReflection* field_type = field->type;
    const void*               address    = (const u8*)instance + field->offset;

    NYA_OrmColumnType column = NYA_ORM_COLUMN_COUNT;

    nya_assert(nya_orm_column_type(field_type, &column), "'%s.%s' has no column type, which nya_orm_open refuses", table->type->name,
               field->name);

    // An enum is stored as its variant's name, so renumbering the enum cannot reinterpret old rows.
    if (field_type->kind == NYA_REFLECT_ENUM && !field_type->is_bitflags) {
        s64 number = 0;

        if (!nya_reflect_value_to_s64(nya_reflect_read(field_type, address), &number)) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' does not read as a number", table->type->name, field->name);
        }

        NYA_ConstCString variant = nya_reflect_variant_name(field_type, number);

        // Refused rather than stored as the number: the TEXT column would return text naming no variant and be dropped, reading as zero later.
        if (variant == nullptr) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' holds " FMTs64 ", which is not a variant of '%s'", table->type->name,
                             field->name, number, field_type->name);
        }

        *out_value = nya_sql_text(variant);
        return NYA_OK;
    }

    if (nya_reflect_is_char_array(field_type)) {
        const char* text   = (const char*)address;
        u32         length = 0;

        while (length < field_type->element_count && text[length] != '\0') length++;

        // sqlite measures a bound string with strlen, so an unterminated char[N] would be read past its end; refused by name instead.
        if (length == field_type->element_count) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' is not terminated within its " FMTu32 " bytes", table->type->name,
                             field->name, field_type->element_count);
        }

        *out_value = nya_sql_text(text);
        return NYA_OK;
    }

    NYA_Value value = nya_reflect_read(field_type, address);

    switch (column) {
        case NYA_ORM_COLUMN_INTEGER: {
            s64 number = 0;

            if (!nya_reflect_value_to_s64(value, &number)) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' does not read as an integer", table->type->name, field->name);
            }

            *out_value = nya_sql_s64(number);
            return NYA_OK;
        }

        case NYA_ORM_COLUMN_REAL: {
            f64 number = 0.0;

            if (!nya_reflect_value_to_f64(value, &number)) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' does not read as a real", table->type->name, field->name);
            }

            *out_value = nya_sql_f64(number);
            return NYA_OK;
        }

        // The only text left is a `char*`; a null one is SQL NULL rather than an empty string, so the two stay different.
        case NYA_ORM_COLUMN_TEXT: {
            if (value.type != NYA_TYPE_STRING) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' does not read as text", table->type->name, field->name);
            }

            *out_value = value.as_string != nullptr ? nya_sql_text(value.as_string) : nya_sql_null();
            return NYA_OK;
        }

        case NYA_ORM_COLUMN_COUNT:
        default: nya_unreachable();
    }
}

NYA_Error _nya_orm_check_key(const NYA_OrmTable* table, NYA_SqlValue key) {
    NYA_SqlValueKind wanted = table->key_is_integer ? NYA_SQL_VALUE_S64 : NYA_SQL_VALUE_TEXT;

    // A key of the wrong shape matches no row, which looks exactly like absence, so it is said rather than left to read that way.
    if (key.kind != wanted) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s.%s' is %s, so the key has to be built with nya_sql_%s", table->type->name,
                         table->key->name, table->key_is_integer ? "an integer" : "text", table->key_is_integer ? "s64" : "text");
    }

    return NYA_OK;
}

NYA_Error _nya_orm_table_info(NYA_OrmTable* table, NYA_Arena* arena, OUT _NYA_OrmColumnInfo* out_columns, u32 capacity, OUT u32* out_count) {
    nya_assert(out_columns != nullptr);
    nya_assert(out_count != nullptr);

    *out_count = 0;

    // The table name cannot be bound, which is why nya_orm_open parses it into an identifier; nothing else here comes from outside this module.
    NYA_String* sql = nya_string_sprintf(arena, "PRAGMA table_info(%s)", table->name);

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(table->database, arena, nya_string_to_cstring(arena, sql), nullptr, 0, &result));

    nya_array_foreach (result.rows, row) {
        if (*out_count >= capacity) break;

        NYA_Value* name = nya_object_get(*row, "name");
        NYA_Value* type = nya_object_get(*row, "type");
        NYA_Value* key  = nya_object_get(*row, "pk");

        if (name == nullptr || name->type != NYA_TYPE_STRING) continue;

        out_columns[*out_count] = (_NYA_OrmColumnInfo){
            .name           = name->as_string,
            .type           = type != nullptr && type->type == NYA_TYPE_STRING ? type->as_string : "",
            .is_primary_key = key != nullptr && key->type == NYA_TYPE_S64 && key->as_s64 != 0,
        };

        (*out_count)++;
    }

    return NYA_OK;
}
