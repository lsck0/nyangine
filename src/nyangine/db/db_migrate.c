#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One column, as both sides of the difference are read: the name it has, the storage class it is
 * declared with, and whether it is the primary key. A reflection and a `PRAGMA table_info` both
 * reduce to this, which is the whole reason the same diff can answer both questions.
 * */
typedef struct {
    char              name[NYA_ORM_NAME_MAX];
    NYA_OrmColumnType type;
    b8                is_key;
} _NYA_MigrationColumn;

/** One side of the difference. An empty one is a table that is not there. */
typedef struct {
    _NYA_MigrationColumn columns[NYA_ORM_COLUMN_MAX];
    u32                  count;
} _NYA_MigrationSchema;

/** Reads a described type into a schema, refusing every type nya_orm_open would refuse. */
NYA_INTERNAL NYA_Error _nya_migration_schema_from_type(const NYA_TypeReflection* type, OUT _NYA_MigrationSchema* out_schema);

/** Reads what the database actually has, through the same PRAGMA the ORM's drift check reads. */
NYA_INTERNAL NYA_Error _nya_migration_schema_from_table(NYA_OrmTable* table, NYA_Arena* arena, OUT _NYA_MigrationSchema* out_schema);

/** The column called `name`, or null. */
NYA_INTERNAL const _NYA_MigrationColumn* _nya_migration_column_find(const _NYA_MigrationSchema* schema, NYA_ConstCString name);

/** The whole derivation, over two schemas and nothing else. */
NYA_INTERNAL NYA_Error _nya_migration_diff(
    NYA_Arena* arena, const _NYA_MigrationSchema* from, const _NYA_MigrationSchema* to, OUT NYA_MigrationPlan* plan
);

/** `CREATE TABLE IF NOT EXISTS`, built from a schema rather than from a table's cached statement. */
NYA_INTERNAL NYA_ConstCString _nya_migration_create_sql(NYA_Arena* arena, NYA_ConstCString table, const _NYA_MigrationSchema* schema);

/** Copies `text` into `arena`, so a plan outlives whatever it was read from. */
NYA_INTERNAL NYA_ConstCString _nya_migration_copy(NYA_Arena* arena, NYA_ConstCString text);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_migration_refusal_reason(NYA_MigrationRefusalKind kind) {
    switch (kind) {
        case NYA_MIGRATION_REFUSAL_REMOVED_COLUMN:
            return "the table has it and the struct does not describe it, which is a drop or a rename and looks the same either way; it is left alone";

        case NYA_MIGRATION_REFUSAL_RETYPED_COLUMN:
            return "it is declared as another type, and converting it is a decision about what an unconvertible value becomes";

        case NYA_MIGRATION_REFUSAL_MOVED_KEY: return "the primary key would move, which is a rebuild of the table and a copy of its rows";

        case NYA_MIGRATION_REFUSAL_COUNT:
        default:                              return "an unknown difference";
    }
}

NYA_Error nya_migration_plan_from_table(NYA_OrmTable* table, NYA_Arena* arena, OUT NYA_MigrationPlan** out_plan) {
    nya_assert(arena != nullptr);
    nya_assert(out_plan != nullptr);

    if (table == nullptr || table->database == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the table is closed");

    _NYA_MigrationSchema wanted = { 0 };
    _NYA_MigrationSchema actual = { 0 };

    NYA_TRY(_nya_migration_schema_from_type(table->type, &wanted));
    NYA_TRY(_nya_migration_schema_from_table(table, arena, &actual));

    NYA_MigrationPlan* plan = nya_arena_alloc(arena, sizeof(NYA_MigrationPlan));
    nya_memset(plan, 0, sizeof(NYA_MigrationPlan));

    // The name was parsed by nya_orm_open, so this is a copy of an identifier and not a parse.
    (void)snprintf(plan->table, sizeof(plan->table), "%s", table->name);

    NYA_TRY(_nya_migration_diff(arena, &actual, &wanted, plan));

    *out_plan = plan;
    return NYA_OK;
}

NYA_Error nya_migration_plan_from_types(
    NYA_Arena*                arena,
    const NYA_TypeReflection* from,
    const NYA_TypeReflection* to,
    NYA_ConstCString          table_name,
    OUT NYA_MigrationPlan**   out_plan
) {
    nya_assert(arena != nullptr);
    nya_assert(out_plan != nullptr);

    if (to == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no type to migrate to");

    NYA_MigrationPlan* plan = nya_arena_alloc(arena, sizeof(NYA_MigrationPlan));
    nya_memset(plan, 0, sizeof(NYA_MigrationPlan));

    NYA_TRY(_nya_orm_parse_name(table_name, plan->table, sizeof(plan->table)));

    _NYA_MigrationSchema before = { 0 };
    _NYA_MigrationSchema after  = { 0 };

    // A null `from` is a table that does not exist yet, which is an empty schema and therefore a
    // create. The same case the live reader produces for a database with no such table.
    if (from != nullptr) NYA_TRY(_nya_migration_schema_from_type(from, &before));
    NYA_TRY(_nya_migration_schema_from_type(to, &after));

    NYA_TRY(_nya_migration_diff(arena, &before, &after, plan));

    *out_plan = plan;
    return NYA_OK;
}

NYA_Error nya_migration_apply(NYA_Database* database, const NYA_MigrationPlan* plan) {
    nya_assert(database != nullptr);

    if (plan == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no plan");

    // Nothing at all rather than the derivable half: a table left between two schemas is one nobody
    // wrote code for. The first blocking refusal is the message, since fixing it is one decision.
    if (plan->blocked) {
        for (u32 i = 0; i < plan->refusal_count; i++) {
            if (plan->refusals[i].kind == NYA_MIGRATION_REFUSAL_REMOVED_COLUMN) continue;

            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s.%s' cannot be migrated: %s (it is %s, the struct wants %s)", plan->table,
                             plan->refusals[i].column, nya_migration_refusal_reason(plan->refusals[i].kind), plan->refusals[i].found,
                             plan->refusals[i].expected);
        }

        nya_unreachable();
    }

    if (plan->step_count == 0) return NYA_OK;

    NYA_TRY(nya_sql_transaction_begin(database));

    for (u32 i = 0; i < plan->step_count; i++) {
        NYA_Error ran = nya_sql_exec(database, plan->steps[i].sql);
        if (ran.ok) continue;

        // The disk that fills halfway through. Rolled back so the schema is the one the program was
        // written against, and the rollback's own failure is not allowed to hide the real error.
        NYA_Error unwound = nya_sql_transaction_rollback(database);
        if (!unwound.ok) nya_log_error("could not roll back the migration of '%s': %s", plan->table, (NYA_ConstCString)unwound.message);

        return ran;
    }

    return nya_sql_transaction_commit(database);
}

NYA_Error nya_orm_schema_migrate(NYA_OrmTable* table) {
    nya_assert(table != nullptr);
    nya_assert(table->database != nullptr, "the table is closed");

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "orm_schema_migrate");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_MigrationPlan* plan = nullptr;
    NYA_TRY(nya_migration_plan_from_table(table, &scratch, &plan));

    // Said before anything runs, including the refusals that do not stop it: a column the schema
    // keeps and this build does not describe is the one difference somebody has to notice by reading.
    for (u32 i = 0; i < plan->refusal_count; i++) {
        nya_log_warn("%s.%s: %s", plan->table, plan->refusals[i].column, nya_migration_refusal_reason(plan->refusals[i].kind));
    }

    for (u32 i = 0; i < plan->step_count; i++) nya_log_info("%s: %s", plan->table, plan->steps[i].sql);

    return nya_migration_apply(table->database, plan);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString _nya_migration_copy(NYA_Arena* arena, NYA_ConstCString text) {
    return nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s", text != nullptr ? text : ""));
}

NYA_Error _nya_migration_schema_from_type(const NYA_TypeReflection* type, OUT _NYA_MigrationSchema* out_schema) {
    nya_assert(out_schema != nullptr);

    if (type == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no type");

    if (type->kind != NYA_REFLECT_STRUCT) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' is not a struct, and only a struct is a row", type->name);
    }

    *out_schema = (_NYA_MigrationSchema){ 0 };

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField* field = &type->fields[i];

        // The generator's placeholder for a struct whose every field was skipped; see reflection.c.
        if (field->type == nullptr || field->name == nullptr) continue;

        if (out_schema->count >= NYA_ORM_COLUMN_MAX) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' has more than the %d columns a table may have", type->name, NYA_ORM_COLUMN_MAX);
        }

        NYA_OrmColumnType column = NYA_ORM_COLUMN_COUNT;

        // The same mapping nya_orm_open refuses on, asked the same way, so a plan cannot be built for
        // a type the ORM would not open.
        if (!nya_orm_column_type(field->type, &column)) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s.%s' is a '%s', which has no column type", type->name, field->name,
                             field->type->name);
        }

        _NYA_MigrationColumn* entry = &out_schema->columns[out_schema->count];

        // A field name is a C identifier by construction, and is parsed anyway: it goes into a
        // statement unquoted, and this module trusts nothing it puts there.
        NYA_TRY(_nya_orm_parse_name(field->name, entry->name, sizeof(entry->name)));

        entry->type   = column;
        entry->is_key = field->is_key;

        out_schema->count++;
    }

    if (out_schema->count == 0) return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' has no field that could be a column", type->name);

    return NYA_OK;
}

NYA_Error _nya_migration_schema_from_table(NYA_OrmTable* table, NYA_Arena* arena, OUT _NYA_MigrationSchema* out_schema) {
    nya_assert(out_schema != nullptr);

    *out_schema = (_NYA_MigrationSchema){ 0 };

    _NYA_OrmColumnInfo actual[NYA_ORM_COLUMN_MAX] = { 0 };
    u32                actual_count               = 0;

    // The ORM's own reader, so a plan and nya_orm_schema_check cannot disagree about what is there.
    NYA_TRY(_nya_orm_table_info(table, arena, actual, nya_carray_length(actual), &actual_count));

    for (u32 i = 0; i < actual_count; i++) {
        _NYA_MigrationColumn* entry = &out_schema->columns[out_schema->count];

        // A table written by somebody else can hold a name that is not an identifier, and there is
        // nothing to derive from a column this module could never name in a statement. Skipped,
        // which leaves it exactly where it is, as an undescribed column already is.
        if (!_nya_orm_parse_name(actual[i].name, entry->name, sizeof(entry->name)).ok) {
            nya_log_warn("'%s' has a column named '%s', which is not an identifier; leaving it alone", table->name, actual[i].name);
            continue;
        }

        // Case insensitively, because a table written by hand may well say `integer`. Anything the
        // mapping does not know is left as COUNT, which differs from every real type and so reads as
        // a retype rather than as a match.
        NYA_String* declared = nya_string_from(arena, actual[i].type);
        nya_string_to_upper(declared);

        entry->type = NYA_ORM_COLUMN_COUNT;

        for (u32 known = 0; known < NYA_ORM_COLUMN_COUNT; known++) {
            if (nya_string_equals(declared, NYA_ORM_COLUMN_TYPE_NAME_MAP[known])) entry->type = (NYA_OrmColumnType)known;
        }

        entry->is_key = actual[i].is_primary_key;

        out_schema->count++;
    }

    return NYA_OK;
}

const _NYA_MigrationColumn* _nya_migration_column_find(const _NYA_MigrationSchema* schema, NYA_ConstCString name) {
    for (u32 i = 0; i < schema->count; i++) {
        if (nya_string_equals(schema->columns[i].name, name)) return &schema->columns[i];
    }

    return nullptr;
}

NYA_ConstCString _nya_migration_create_sql(NYA_Arena* arena, NYA_ConstCString table, const _NYA_MigrationSchema* schema) {
    NYA_String* definitions = nya_string_create(arena);

    for (u32 i = 0; i < schema->count; i++) {
        // The same shape nya_orm_open builds, for the same reason: nothing but the type and, for the
        // key, PRIMARY KEY. A constraint nothing relies on would make an existing table look drifted.
        nya_string_extend_sprintf(definitions, "%s%s %s%s", i > 0 ? ", " : "", schema->columns[i].name,
                                  NYA_ORM_COLUMN_TYPE_NAME_MAP[schema->columns[i].type], schema->columns[i].is_key ? " PRIMARY KEY" : "");
    }

    NYA_String* sql = nya_string_sprintf(arena, "CREATE TABLE IF NOT EXISTS %s (%s)", table, nya_string_to_cstring(arena, definitions));

    return nya_string_to_cstring(arena, sql);
}

NYA_Error _nya_migration_diff(NYA_Arena* arena, const _NYA_MigrationSchema* from, const _NYA_MigrationSchema* to, OUT NYA_MigrationPlan* plan) {
    nya_assert(plan != nullptr);

    // The table is not there at all, which is the one difference with no ambiguity in it whatsoever.
    if (from->count == 0) {
        plan->steps[0] = (NYA_MigrationStep){
            .kind    = NYA_MIGRATION_STEP_CREATE_TABLE,
            .subject = _nya_migration_copy(arena, plan->table),
            .sql     = _nya_migration_create_sql(arena, plan->table, to),
        };
        plan->step_count = 1;

        return NYA_OK;
    }

    for (u32 i = 0; i < to->count; i++) {
        const _NYA_MigrationColumn* wanted = &to->columns[i];
        const _NYA_MigrationColumn* found  = _nya_migration_column_find(from, wanted->name);

        if (found == nullptr) {
            // sqlite cannot add a primary key to a table that exists, and it should not be able to:
            // which row is which would be decided after the fact, for rows that are already there.
            if (wanted->is_key) {
                plan->refusals[plan->refusal_count] = (NYA_MigrationRefusal){
                    .kind     = NYA_MIGRATION_REFUSAL_MOVED_KEY,
                    .column   = _nya_migration_copy(arena, wanted->name),
                    .found    = "not a column of the table",
                    .expected = "the primary key",
                };
                plan->refusal_count++;
                plan->blocked = true;
                continue;
            }

            NYA_String* sql = nya_string_sprintf(arena, "ALTER TABLE %s ADD COLUMN %s %s", plan->table, wanted->name,
                                                 NYA_ORM_COLUMN_TYPE_NAME_MAP[wanted->type]);

            // No DEFAULT and no NOT NULL: every row already there gets NULL, and NULL loads as zero
            // because nya_orm_select zeroes a struct before it fills it. That is the data migration.
            plan->steps[plan->step_count] = (NYA_MigrationStep){
                .kind    = NYA_MIGRATION_STEP_ADD_COLUMN,
                .subject = _nya_migration_copy(arena, wanted->name),
                .sql     = nya_string_to_cstring(arena, sql),
            };
            plan->step_count++;
            continue;
        }

        if (found->type != wanted->type) {
            plan->refusals[plan->refusal_count] = (NYA_MigrationRefusal){
                .kind     = NYA_MIGRATION_REFUSAL_RETYPED_COLUMN,
                .column   = _nya_migration_copy(arena, wanted->name),
                .found    = found->type < NYA_ORM_COLUMN_COUNT ? NYA_ORM_COLUMN_TYPE_NAME_MAP[found->type] : "a type with no mapping",
                .expected = NYA_ORM_COLUMN_TYPE_NAME_MAP[wanted->type],
            };
            plan->refusal_count++;
            plan->blocked = true;
            continue;
        }

        if (wanted->is_key && !found->is_key) {
            plan->refusals[plan->refusal_count] = (NYA_MigrationRefusal){
                .kind     = NYA_MIGRATION_REFUSAL_MOVED_KEY,
                .column   = _nya_migration_copy(arena, wanted->name),
                .found    = "not the table's primary key",
                .expected = "the primary key",
            };
            plan->refusal_count++;
            plan->blocked = true;
        }
    }

    // The other direction, and the refusal that does not stop anything: the column stays, because a
    // drop and a rename are the same difference from here and the ORM names its columns anyway.
    for (u32 i = 0; i < from->count; i++) {
        if (_nya_migration_column_find(to, from->columns[i].name) != nullptr) continue;

        plan->refusals[plan->refusal_count] = (NYA_MigrationRefusal){
            .kind     = NYA_MIGRATION_REFUSAL_REMOVED_COLUMN,
            .column   = _nya_migration_copy(arena, from->columns[i].name),
            .found    = "a column no field of the struct describes",
            .expected = "nothing, and it is left alone",
        };
        plan->refusal_count++;
    }

    return NYA_OK;
}
