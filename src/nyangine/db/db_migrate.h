/**
 * @file db_migrate.h
 *
 * What a table and a described type differ by, and which of those differences a machine may act on.
 *
 * A migration here is derived, never written: the schema is the reflection, so the difference between
 * two reflections — or between a reflection and the table that is actually in the database — is the
 * only statement of what changed that cannot drift from the code. A hand written migration file is a
 * second statement of the same fact, and the two disagree the first time somebody forgets one.
 *
 * ```c
 * NYA_MigrationPlan* plan = nullptr;
 * NYA_TRY(nya_migration_plan_from_table(notes, arena, &plan));
 *
 * for (u32 i = 0; i < plan->refusal_count; i++) {
 *     nya_log_warn("%s: %s", plan->refusals[i].column, nya_migration_refusal_reason(plan->refusals[i].kind));
 * }
 *
 * NYA_TRY(nya_migration_apply(database, plan));
 * ```
 *
 * nya_orm_schema_migrate is those three steps in one call, and is what a program uses at startup.
 *
 * ─────────────────────────────────────────────────────────
 * WHAT IT WILL DO, AND WHAT IT REFUSES
 * ─────────────────────────────────────────────────────────
 *
 * Derivable, because there is exactly one thing the difference can mean:
 *
 * | Difference                                  | What is run                                  |
 * | :------------------------------------------ | :------------------------------------------- |
 * | the table is not there                      | `CREATE TABLE`, from the type                 |
 * | the type has a column the table has not      | `ALTER TABLE ... ADD COLUMN`, with no default |
 *
 * An added column is NULL in every row that was already there, and NULL loads as zero, because
 * nya_orm_select zeroes a struct before it fills it. That is the whole of the data migration, and it
 * is why nothing here writes values into rows.
 *
 * Refused, each naming the column, because deriving it would mean guessing:
 *
 * - **A column the type no longer describes.** It is a drop or it is a rename, and nothing in either
 *   reflection says which: the two are the same difference seen from here. A rename guessed wrong
 *   moves one column's data into another's, and a drop guessed wrong deletes it. So the column is
 *   left exactly where it is and reported. Every statement the ORM runs names its columns, so a
 *   column this build does not describe is never read and never written, and leaving it costs a row
 *   some bytes and nothing else. This is the one refusal that does not stop a migration.
 * - **A column whose type changed.** `TEXT` to `INTEGER` is a conversion, and a conversion is a
 *   decision about what an unconvertible value becomes. It stops the migration, because the ORM would
 *   otherwise write one type into a column declared as another and read back something else.
 * - **A primary key that moved**, or a type whose key column the table has not got. Both mean
 *   rebuilding the table under a new key, which is a copy, and a copy of somebody's data is not a
 *   thing to derive. It stops the migration.
 *
 * What to do about a refusal is the caller's, and the three honest answers are the ones db_orm.h
 * already gives: write the statement yourself with nya_sql_exec, copy the rows into a new table and
 * rename it, or delete the file and start again.
 *
 * ─────────────────────────────────────────────────────────
 * WHAT ELSE TO KNOW
 * ─────────────────────────────────────────────────────────
 *
 * - **All of it or none of it.** nya_migration_apply runs its steps inside one transaction and rolls
 *   back if any of them fails, so a disk that fills halfway through leaves the schema as it was.
 * - **Nothing runs when anything is refused**, other than the removed column above: a plan that is
 *   part derivable is not applied in part, because a table left between two schemas is a table
 *   nobody wrote code for.
 * - **A plan is data.** Building one touches the database only to read `PRAGMA table_info`, and
 *   nya_migration_plan_from_types touches it not at all, so a test or a build step can ask what a
 *   change would do to a deployed schema without having one.
 * - **Every identifier is parsed**, column names included, exactly as db_orm.h says about the table
 *   name: an identifier cannot be a bound parameter, so nothing reaches a statement unchecked.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_types.h"
#include "nyangine/db/db_orm.h"
#include "nyangine/db/db_sql.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_MigrationStepKind    NYA_MigrationStepKind;
typedef enum NYA_MigrationRefusalKind NYA_MigrationRefusalKind;
typedef struct NYA_MigrationStep      NYA_MigrationStep;
typedef struct NYA_MigrationRefusal   NYA_MigrationRefusal;
typedef struct NYA_MigrationPlan      NYA_MigrationPlan;

/** What a derivable difference turns into. One statement each; see the table in this file's block. */
enum NYA_MigrationStepKind {
    NYA_MIGRATION_STEP_CREATE_TABLE,
    NYA_MIGRATION_STEP_ADD_COLUMN,
    NYA_MIGRATION_STEP_COUNT,
};

/** Why a difference was not turned into a statement. */
enum NYA_MigrationRefusalKind {
    /** The table has a column the type does not describe: a drop or a rename, and they look alike. */
    NYA_MIGRATION_REFUSAL_REMOVED_COLUMN,

    /** The column is there under another type, which is a conversion nobody stated. */
    NYA_MIGRATION_REFUSAL_RETYPED_COLUMN,

    /** The key is another column, or is not in the table at all. Either way the table is rebuilt. */
    NYA_MIGRATION_REFUSAL_MOVED_KEY,

    NYA_MIGRATION_REFUSAL_COUNT,
};

/** One statement the plan will run, already built. */
struct NYA_MigrationStep {
    NYA_MigrationStepKind kind;

    /** The column it is about, or the table for a create. Lives in the plan's arena. */
    NYA_ConstCString subject;

    /** The statement itself. Built from the reflection and the parsed identifiers, never from data. */
    NYA_ConstCString sql;
};

/** One difference the plan would not derive a statement for. */
struct NYA_MigrationRefusal {
    NYA_MigrationRefusalKind kind;

    NYA_ConstCString column;

    /** What the table has and what the type wanted, in the words the ORM's drift check uses. */
    NYA_ConstCString found;
    NYA_ConstCString expected;
};

/**
 * The whole difference: what would be run, and what was refused. Plain data in the arena it was built
 * in, so it can be logged, asserted on in a test or printed before anything touches the database.
 * */
struct NYA_MigrationPlan {
    char table[NYA_ORM_NAME_MAX];

    /** One per added column, or the single create, so the type's column ceiling is the bound. */
    NYA_MigrationStep steps[NYA_ORM_COLUMN_MAX];
    u32               step_count;

    /**
     * Twice the ceiling, because each side can contribute one refusal per column: the type's columns
     * can each be retyped or moved, and the table's can each be one the type no longer describes.
     * */
    NYA_MigrationRefusal refusals[NYA_ORM_COLUMN_MAX * 2];
    u32                  refusal_count;

    /**
     * Whether a refusal stops the migration. A removed column alone does not; a retyped column or a
     * moved key does. nya_migration_apply reads this and runs nothing at all when it is set.
     * */
    b8 blocked;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The sentence a refusal is reported with, so every caller says the same thing about it. */
NYA_API NYA_ConstCString nya_migration_refusal_reason(NYA_MigrationRefusalKind kind) __attr_no_discard;

/**
 * What `table`'s type and the table actually in the database differ by. Reads `PRAGMA table_info` and
 * nothing else; a table that is not there is a plan with one create in it.
 * */
NYA_API NYA_Error nya_migration_plan_from_table(NYA_OrmTable* table, NYA_Arena* arena, OUT NYA_MigrationPlan** out_plan) __attr_no_discard;

/**
 * What two versions of one described type differ by, with no database anywhere: `from` is the type as
 * it was, `to` the type as it is, and `table_name` what the rows live in. The same diff the call
 * above runs, which is what makes it a fair answer to "what would this change do to a deployed
 * schema" — in a test, or in a step that asks before a release.
 *
 * A null `from` means the table does not exist yet, so the plan is the create.
 * */
NYA_API NYA_Error nya_migration_plan_from_types(
    NYA_Arena*                arena,
    const NYA_TypeReflection* from,
    const NYA_TypeReflection* to,
    NYA_ConstCString          table_name,
    OUT NYA_MigrationPlan**   out_plan
) __attr_no_discard;

/**
 * Runs the plan's steps inside one transaction, or nothing at all when the plan is blocked, in which
 * case the error names the column and why it was refused. A plan with no steps is success.
 * */
NYA_API NYA_Error nya_migration_apply(NYA_Database* database, const NYA_MigrationPlan* plan) __attr_no_discard;
