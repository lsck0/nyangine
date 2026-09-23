/**
 * Migrations derived from reflections: what the difference between two versions of a type turns
 * into, what it refuses to turn into anything, and what that does to a table with rows in it.
 *
 * The described types here are written out by hand rather than annotated, for the reason
 * test_db_orm.c gives: the generator scans the engine and the game, not the tests.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FOUR VERSIONS OF ONE TYPE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define TEXT_MAX 32

static const NYA_TypeReflection TEXT_ARRAY = {
  .name          = "char[]",
  .kind          = NYA_REFLECT_ARRAY,
  .size          = TEXT_MAX,
  .alignment     = alignof(char),
  .element       = nya_reflect_of(char),
  .element_count = TEXT_MAX,
};

/** As the table was first written. */
typedef struct {
  s64  id;
  char text[TEXT_MAX];
} NoteV1;

static const NYA_ReflectField NOTE_V1_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteV1, id), .is_key = true },
  { .name = "text", .type = &TEXT_ARRAY, .offset = nya_offsetof(NoteV1, text) },
};

static const NYA_TypeReflection NOTE_V1 = {
  .name        = "NoteV1",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(NoteV1),
  .alignment   = alignof(NoteV1),
  .fields      = NOTE_V1_FIELDS,
  .field_count = nya_carray_length(NOTE_V1_FIELDS),
};

/** The same type, one column later. The whole of what a migration may derive. */
typedef struct {
  s64  id;
  char text[TEXT_MAX];
  s64  written_at_s;
} NoteV2;

static const NYA_ReflectField NOTE_V2_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteV2, id), .is_key = true },
  { .name = "text", .type = &TEXT_ARRAY, .offset = nya_offsetof(NoteV2, text) },
  { .name = "written_at_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteV2, written_at_s) },
};

static const NYA_TypeReflection NOTE_V2 = {
  .name        = "NoteV2",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(NoteV2),
  .alignment   = alignof(NoteV2),
  .fields      = NOTE_V2_FIELDS,
  .field_count = nya_carray_length(NOTE_V2_FIELDS),
};

/** `text` is gone. A drop, or a rename of it to something else: indistinguishable from here. */
typedef struct {
  s64 id;
  s64 written_at_s;
} NoteDropped;

static const NYA_ReflectField NOTE_DROPPED_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteDropped, id), .is_key = true },
  { .name = "written_at_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteDropped, written_at_s) },
};

static const NYA_TypeReflection NOTE_DROPPED = {
  .name        = "NoteDropped",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(NoteDropped),
  .alignment   = alignof(NoteDropped),
  .fields      = NOTE_DROPPED_FIELDS,
  .field_count = nya_carray_length(NOTE_DROPPED_FIELDS),
};

/** `text` is an integer now, which is a conversion nobody wrote down. */
typedef struct {
  s64 id;
  s64 text;
} NoteRetyped;

static const NYA_ReflectField NOTE_RETYPED_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteRetyped, id), .is_key = true },
  { .name = "text", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteRetyped, text) },
};

static const NYA_TypeReflection NOTE_RETYPED = {
  .name        = "NoteRetyped",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(NoteRetyped),
  .alignment   = alignof(NoteRetyped),
  .fields      = NOTE_RETYPED_FIELDS,
  .field_count = nya_carray_length(NOTE_RETYPED_FIELDS),
};

/** The identity moved to another column, which is a rebuild and a copy of every row. */
typedef struct {
  s64  id;
  char text[TEXT_MAX];
} NoteMovedKey;

static const NYA_ReflectField NOTE_MOVED_KEY_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NoteMovedKey, id) },
  { .name = "text", .type = &TEXT_ARRAY, .offset = nya_offsetof(NoteMovedKey, text), .is_key = true },
};

static const NYA_TypeReflection NOTE_MOVED_KEY = {
  .name        = "NoteMovedKey",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(NoteMovedKey),
  .alignment   = alignof(NoteMovedKey),
  .fields      = NOTE_MOVED_KEY_FIELDS,
  .field_count = nya_carray_length(NOTE_MOVED_KEY_FIELDS),
};

/** The one refusal in `plan`, which every refusal test expects exactly one of. */
static const NYA_MigrationRefusal* only_refusal(const NYA_MigrationPlan* plan) {
  nya_assert_eq(plan->refusal_count, 1U);
  return &plan->refusals[0];
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_db_migrate");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: two reflections and no database at all
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A table that is not there is one create and nothing else.
    NYA_MigrationPlan* fresh = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, nullptr, &NOTE_V2, "notes", &fresh));

    nya_assert_eq(fresh->step_count, 1U);
    nya_assert_eq(fresh->refusal_count, 0U);
    nya_assert(fresh->steps[0].kind == NYA_MIGRATION_STEP_CREATE_TABLE);
    nya_assert(nya_string_contains(fresh->steps[0].sql, "CREATE TABLE IF NOT EXISTS notes"), "got %s", fresh->steps[0].sql);
    nya_assert(nya_string_contains(fresh->steps[0].sql, "id INTEGER PRIMARY KEY"), "got %s", fresh->steps[0].sql);

    // A type that has not changed is no migration, which is the case that runs on every startup.
    NYA_MigrationPlan* same = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, &NOTE_V2, &NOTE_V2, "notes", &same));

    nya_assert_eq(same->step_count, 0U);
    nya_assert_eq(same->refusal_count, 0U);
    nya_assert(!same->blocked);

    // The one thing it derives: a column the struct has gained.
    NYA_MigrationPlan* grown = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, &NOTE_V1, &NOTE_V2, "notes", &grown));

    nya_assert_eq(grown->step_count, 1U);
    nya_assert_eq(grown->refusal_count, 0U);
    nya_assert(grown->steps[0].kind == NYA_MIGRATION_STEP_ADD_COLUMN);
    nya_assert(nya_string_equals(grown->steps[0].subject, "written_at_s"));
    nya_assert(nya_string_equals(grown->steps[0].sql, "ALTER TABLE notes ADD COLUMN written_at_s INTEGER"), "got %s", grown->steps[0].sql);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what it refuses to derive, and which refusals stop a migration
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A column the struct no longer describes. Left alone, reported, and not a reason to stop: the
    // ORM names its columns in every statement, so a column it does not know is never touched.
    NYA_MigrationPlan* dropped = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, &NOTE_V2, &NOTE_DROPPED, "notes", &dropped));

    nya_assert_eq(dropped->step_count, 0U);
    nya_assert(only_refusal(dropped)->kind == NYA_MIGRATION_REFUSAL_REMOVED_COLUMN);
    nya_assert(nya_string_equals(dropped->refusals[0].column, "text"));
    nya_assert(!dropped->blocked, "a column left in place does not stop the rest");

    // A column whose type changed. Converting it is a decision, so it stops the migration.
    NYA_MigrationPlan* retyped = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, &NOTE_V1, &NOTE_RETYPED, "notes", &retyped));

    nya_assert_eq(retyped->step_count, 0U);
    nya_assert(only_refusal(retyped)->kind == NYA_MIGRATION_REFUSAL_RETYPED_COLUMN);
    nya_assert(nya_string_equals(retyped->refusals[0].found, "TEXT"));
    nya_assert(nya_string_equals(retyped->refusals[0].expected, "INTEGER"));
    nya_assert(retyped->blocked);

    // The identity moving is a rebuild of the table, which is a copy of somebody's rows.
    NYA_MigrationPlan* moved = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, &NOTE_V1, &NOTE_MOVED_KEY, "notes", &moved));

    nya_assert(only_refusal(moved)->kind == NYA_MIGRATION_REFUSAL_MOVED_KEY);
    nya_assert(nya_string_equals(moved->refusals[0].column, "text"));
    nya_assert(moved->blocked);

    // A rename is a drop and an add seen from here, and it is refused as exactly that rather than
    // guessed at: one column added, one left alone, and nothing that moves the data between them.
    NYA_MigrationPlan* renamed = nullptr;
    NYA_EXPECT(nya_migration_plan_from_types(arena, &NOTE_V1, &NOTE_DROPPED, "notes", &renamed));

    nya_assert_eq(renamed->step_count, 1U);
    nya_assert(nya_string_equals(renamed->steps[0].subject, "written_at_s"));
    nya_assert(only_refusal(renamed)->kind == NYA_MIGRATION_REFUSAL_REMOVED_COLUMN);
    nya_assert(nya_string_equals(renamed->refusals[0].column, "text"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a table with rows in it, grown a column
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_OrmTable* v1 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V1, "notes", &v1));

    // The table is not there, so this is the create.
    NYA_EXPECT(nya_orm_schema_migrate(v1));

    NoteV1 written = { .text = "the first note" };
    NYA_EXPECT(nya_orm_insert(v1, &written));
    nya_assert(written.id != 0, "the database assigned the key");

    nya_orm_close(v1);

    NYA_OrmTable* v2 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V2, "notes", &v2));
    defer nya_orm_close(v2);

    // The plan against a live table is the same difference the two reflections gave.
    NYA_MigrationPlan* plan = nullptr;
    NYA_EXPECT(nya_migration_plan_from_table(v2, arena, &plan));

    nya_assert_eq(plan->step_count, 1U);
    nya_assert(nya_string_equals(plan->steps[0].subject, "written_at_s"));

    NYA_EXPECT(nya_orm_schema_migrate(v2));

    // The row written before the column existed is still a row, and the new column reads as zero:
    // the added column is NULL in it, and nya_orm_find zeroes the struct before it fills it.
    NoteV2 read = { 0 };
    NYA_EXPECT(nya_orm_find(v2, arena, nya_sql_s64(written.id), &read));

    nya_assert(nya_string_equals(read.text, "the first note"), "got '%s'", read.text);
    nya_assert_eq(read.written_at_s, (s64)0);

    // And the second startup does nothing at all, which is what makes it safe to run every time.
    NYA_MigrationPlan* again = nullptr;
    NYA_EXPECT(nya_migration_plan_from_table(v2, arena, &again));

    nya_assert_eq(again->step_count, 0U);
    nya_assert_eq(again->refusal_count, 0U);
    NYA_EXPECT(nya_orm_schema_migrate(v2));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a blocked plan runs none of itself
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_OrmTable* v2 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V2, "notes", &v2));
    NYA_EXPECT(nya_orm_schema_migrate(v2));

    NoteV2 written = { .text = "still here", .written_at_s = 7 };
    NYA_EXPECT(nya_orm_insert(v2, &written));
    nya_orm_close(v2);

    // NoteRetyped wants `text` as an integer and has no `written_at_s`: one refusal that stops the
    // migration and one that does not. The derivable half must not run on its own.
    NYA_OrmTable* retyped = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_RETYPED, "notes", &retyped));

    NYA_MigrationPlan* plan = nullptr;
    NYA_EXPECT(nya_migration_plan_from_table(retyped, arena, &plan));
    nya_assert(plan->blocked);

    NYA_Error applied = nya_migration_apply(db, plan);
    nya_assert(!applied.ok && applied.kind == NYA_ERROR_NOT_SUPPORTED, "a blocked plan is refused, naming the column");
    nya_assert(nya_string_contains((NYA_ConstCString)applied.message, "text"), "got %s", (NYA_ConstCString)applied.message);

    nya_assert(!nya_orm_schema_migrate(retyped).ok, "and the one call form refuses with it");
    nya_orm_close(retyped);

    // The table is what it was, rows included.
    NYA_OrmTable* reopened = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V2, "notes", &reopened));
    defer nya_orm_close(reopened);

    nya_assert_eq(nya_orm_schema_check(reopened, nullptr, nullptr), 0U);

    NoteV2 read = { 0 };
    NYA_EXPECT(nya_orm_find(reopened, arena, nya_sql_s64(written.id), &read));
    nya_assert(nya_string_equals(read.text, "still here"));
    nya_assert_eq(read.written_at_s, (s64)7);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a step that fails takes the whole migration back with it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_OrmTable* v1 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V1, "notes", &v1));
    NYA_EXPECT(nya_orm_schema_migrate(v1));
    nya_orm_close(v1);

    NYA_OrmTable* v2 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V2, "notes", &v2));
    defer nya_orm_close(v2);

    NYA_MigrationPlan* plan = nullptr;
    NYA_EXPECT(nya_migration_plan_from_table(v2, arena, &plan));
    nya_assert_eq(plan->step_count, 1U);

    NYA_EXPECT(nya_migration_apply(db, plan));

    /*
     * The same plan a second time, which is the shape of every way a migration fails halfway: the
     * statement is fine and the database says no. sqlite refuses the duplicate column, and what
     * matters is that the transaction is rolled back rather than left open — an open transaction
     * would hold the schema until the connection closed and make every later statement part of it.
     */
    NYA_Error twice = nya_migration_apply(db, plan);
    nya_assert(!twice.ok, "adding a column that is already there is an error, not a no-op");

    // Proven by starting another transaction: sqlite refuses a nested one, so this only succeeds if
    // the failed migration rolled its own back.
    NYA_EXPECT(nya_sql_transaction_begin(db));
    NYA_EXPECT(nya_sql_transaction_rollback(db));

    nya_assert_eq(nya_orm_schema_check(v2, nullptr, nullptr), 0U);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a database written by an earlier run of this test
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Deliberately not deleted at the end. Every other test here builds its database and throws it
     * away, which proves nothing about the case that actually happens: a server restarting onto the
     * file the last version of it wrote. So this one keeps the file, and the run after it — the next
     * `./build run test`, on a machine that has run the suite before — opens a schema it did not
     * create. It is in .gitignore for that reason.
     */
    NYA_ConstCString path = "./_test_db_reopen.db";

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, path, &db));
    defer nya_sql_close(db);

    // The file the last run left is opened as V1 first, exactly as an older build would: a table a
    // newer build has grown still has every column V1 describes.
    NYA_OrmTable* v1 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V1, "notes", &v1));
    NYA_EXPECT(nya_orm_schema_migrate(v1));

    void* older = nullptr;
    u32   older_count = 0;
    NYA_EXPECT(nya_orm_select(v1, arena, "ORDER BY id", nullptr, 0, &older, &older_count));

    for (u32 i = 0; i < older_count; i++) {
      NoteV1* note = nya_orm_at(v1, older, i);
      nya_assert(note->text[0] != '\0', "row " FMTu32 " of an earlier run came back empty", i);
    }

    nya_orm_close(v1);

    // Then as this build sees it, which is the migration a restart onto a new version runs.
    NYA_OrmTable* v2 = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &NOTE_V2, "notes", &v2));
    defer nya_orm_close(v2);

    NYA_EXPECT(nya_orm_schema_migrate(v2));
    nya_assert_eq(nya_orm_schema_check(v2, nullptr, nullptr), 0U);

    NoteV2 written = { .text = "a run happened", .written_at_s = (s64)nya_clock_get_timestamp_ms() };
    NYA_EXPECT(nya_orm_insert(v2, &written));

    void* rows  = nullptr;
    u32   count = 0;
    NYA_EXPECT(nya_orm_select(v2, arena, "ORDER BY id", nullptr, 0, &rows, &count));

    nya_assert(count == older_count + 1, "the rows of every earlier run are still there");

    // Bounded, because a file that is never deleted is a file that grows forever. The oldest go
    // first, so what is kept is the most recent handful of runs.
    for (u32 i = 0; i + 8 < count; i++) {
      NoteV2* note = nya_orm_at(v2, rows, i);
      NYA_EXPECT(nya_orm_delete(v2, nya_sql_s64(note->id)));
    }
  }

  printf("PASSED: test_db_migrate\n");
  return 0;
}
