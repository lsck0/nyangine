/**
 * The db module's ORM: a schema from a reflection, every value bound rather than
 * formatted, rows back into structs through nya_reflect_from_object, and a drifted table refused.
 *
 * The described types here are written out by hand rather than annotated, because the generator only
 * scans the engine and the game. They are the same tables it emits; what the generator does with
 * `@key` is tested in tests/nyangine/base/test_reflection_generated.c against a real annotated type.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE DESCRIBED TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Characters a name holds, terminator included. Small, so the truncation edge is reachable. */
#define NAME_MAX 24

typedef struct {
  s64  id;
  u32  score;
  f64  ratio;
  b8   flag;
  char name[NAME_MAX];
} TestRow;

static const NYA_TypeReflection TEST_ROW_NAME_ARRAY = {
  .name          = "char[]",
  .kind          = NYA_REFLECT_ARRAY,
  .size          = sizeof(((TestRow*)nullptr)->name),
  .alignment     = alignof(char),
  .element       = nya_reflect_of(char),
  .element_count = NAME_MAX,
};

static const NYA_ReflectField TEST_ROW_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(TestRow, id), .is_key = true },
  { .name = "score", .type = nya_reflect_of(u32), .offset = nya_offsetof(TestRow, score) },
  { .name = "ratio", .type = nya_reflect_of(f64), .offset = nya_offsetof(TestRow, ratio) },
  { .name = "flag", .type = nya_reflect_of(b8), .offset = nya_offsetof(TestRow, flag) },
  { .name = "name", .type = &TEST_ROW_NAME_ARRAY, .offset = nya_offsetof(TestRow, name) },
};

static const NYA_TypeReflection TEST_ROW = {
  .name        = "TestRow",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(TestRow),
  .alignment   = alignof(TestRow),
  .fields      = TEST_ROW_FIELDS,
  .field_count = nya_carray_length(TEST_ROW_FIELDS),
};

/** A text key, which is never assigned by the database and may not be empty. */
typedef struct {
  char slot[NAME_MAX];
  s32  value;
} TestSlot;

static const NYA_TypeReflection TEST_SLOT_NAME_ARRAY = {
  .name          = "char[]",
  .kind          = NYA_REFLECT_ARRAY,
  .size          = sizeof(((TestSlot*)nullptr)->slot),
  .alignment     = alignof(char),
  .element       = nya_reflect_of(char),
  .element_count = NAME_MAX,
};

static const NYA_ReflectField TEST_SLOT_FIELDS[] = {
  { .name = "slot", .type = &TEST_SLOT_NAME_ARRAY, .offset = nya_offsetof(TestSlot, slot), .is_key = true },
  { .name = "value", .type = nya_reflect_of(s32), .offset = nya_offsetof(TestSlot, value) },
};

static const NYA_TypeReflection TEST_SLOT = {
  .name        = "TestSlot",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(TestSlot),
  .alignment   = alignof(TestSlot),
  .fields      = TEST_SLOT_FIELDS,
  .field_count = nya_carray_length(TEST_SLOT_FIELDS),
};

/** COUNT is deliberately left out of the variants, so a mood of 2 is a value with no name. */
typedef enum {
  TEST_MOOD_CALM,
  TEST_MOOD_ANGRY,
  TEST_MOOD_COUNT,
} TestMood;

static const NYA_ReflectVariant TEST_MOOD_VARIANTS[] = {
  { .name = "TEST_MOOD_CALM", .value = TEST_MOOD_CALM },
  { .name = "TEST_MOOD_ANGRY", .value = TEST_MOOD_ANGRY },
};

static const NYA_TypeReflection TEST_MOOD = {
  .name          = "TestMood",
  .kind          = NYA_REFLECT_ENUM,
  .size          = sizeof(TestMood),
  .alignment     = alignof(TestMood),
  .primitive     = NYA_TYPE_S32,
  .variants      = TEST_MOOD_VARIANTS,
  .variant_count = nya_carray_length(TEST_MOOD_VARIANTS),
};

typedef struct {
  u32      id;
  TestMood mood;
} TestFeeling;

static const NYA_ReflectField TEST_FEELING_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(u32), .offset = nya_offsetof(TestFeeling, id), .is_key = true },
  { .name = "mood", .type = &TEST_MOOD, .offset = nya_offsetof(TestFeeling, mood) },
};

static const NYA_TypeReflection TEST_FEELING = {
  .name        = "TestFeeling",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(TestFeeling),
  .alignment   = alignof(TestFeeling),
  .fields      = TEST_FEELING_FIELDS,
  .field_count = nya_carray_length(TEST_FEELING_FIELDS),
};

/*
 * The types nya_orm_open has to refuse.
 */

static const NYA_ReflectField NO_KEY_FIELDS[] = {
  { .name = "score", .type = nya_reflect_of(u32), .offset = 0 },
};

static const NYA_TypeReflection NO_KEY = {
  .name        = "NoKey",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(u32),
  .alignment   = alignof(u32),
  .fields      = NO_KEY_FIELDS,
  .field_count = nya_carray_length(NO_KEY_FIELDS),
};

typedef struct {
  u32       id;
  NYA_Color tint;
} Nested;

static const NYA_ReflectField NESTED_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(u32), .offset = nya_offsetof(Nested, id), .is_key = true },
  { .name = "tint", .type = nya_reflect_of(NYA_Color), .offset = nya_offsetof(Nested, tint) },
};

static const NYA_TypeReflection NESTED = {
  .name        = "Nested",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(Nested),
  .alignment   = alignof(Nested),
  .fields      = NESTED_FIELDS,
  .field_count = nya_carray_length(NESTED_FIELDS),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE ROUND TRIP LAW
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Cases the law runs. Each is an insert and a select against a real database. */
#define CASES 500

/** Fixed, so the suite is the same run every time; see testing_property.h. */
#define SEED 0x6F726D6E79613031ULL

/** The table the law writes to. A law draws its inputs and nothing else, so the table is here. */
static NYA_OrmTable* law_table = nullptr;

/**
 * Writing a struct and reading it back gives the same struct, with the key the database assigned.
 * Every field kind the mapping covers is in it: an integer, a real, a boolean and text.
 * */
static b8 law_row_round_trips(NYA_Property* property) {
  TestRow written = {
    .score = (u32)nya_property_draw_u64(property),
    .ratio = (f64)nya_property_draw_f32(property, -1.0e6F, 1.0e6F),
    .flag  = nya_property_draw_bool(property, 50),
  };

  NYA_CString drawn = nya_property_draw_text(property, NAME_MAX - 1);
  (void)snprintf(written.name, sizeof(written.name), "%s", drawn);

  NYA_Error inserted = nya_orm_insert(law_table, &written);
  if (!inserted.ok) {
    nya_property_note(property, "insert: %s", (NYA_ConstCString)inserted.message);
    return false;
  }

  // The key was zero, so the database chose one and wrote it back. A row with no identity cannot be
  // read again, so this is part of the law rather than a separate check.
  if (written.id == 0) {
    nya_property_note(property, "the assigned key was not written back");
    return false;
  }

  TestRow   read  = { 0 };
  NYA_Error found = nya_orm_find(law_table, property->allocator, nya_sql_s64(written.id), &read);
  if (!found.ok) {
    nya_property_note(property, "find: %s", (NYA_ConstCString)found.message);
    return false;
  }

  if (read.id != written.id || read.score != written.score || read.ratio != written.ratio || read.flag != written.flag) {
    nya_property_note(property, "id/score/ratio/flag came back as " FMTs64 "/" FMTu32 "/%f/%d", read.id, read.score, read.ratio,
                      (s32)read.flag);
    return false;
  }

  if (!nya_string_equals(read.name, written.name)) {
    nya_property_note(property, "name went in as '%s' and came back as '%s'", written.name, read.name);
    return false;
  }

  return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TESTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Opens a fresh in memory database. Each test gets its own, so none can pollute another. */
static NYA_Database* open_memory(NYA_Arena* arena) {
  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
  return db;
}

/** Counts what nya_orm_schema_check reports, so a test can assert on the number and not install one. */
static void count_problems(NYA_ConstCString column, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
  nya_log_info("  drift: '%s' is %s, expected %s", column, found, expected);
  (*(u32*)user_data)++;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_orm");
  defer      nya_arena_destroy(arena);

  // TEST: a type that cannot be a table is refused at open, by name
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* table = nullptr;

    // The point of refusing here: none of these touch the database, so the mistake is reported when
    // the type is bound rather than at the first insert of a real row.
    nya_assert(nya_orm_open(arena, db, &NO_KEY, "rows", &table).kind == NYA_ERROR_INVALID_ARGUMENT, "a type with no @key is not a table");
    nya_assert(nya_orm_open(arena, db, &NESTED, "rows", &table).kind == NYA_ERROR_NOT_SUPPORTED, "a nested struct has no column type");
    nya_assert(nya_orm_open(arena, db, nya_reflect_of(NYA_Color), "rows", &table).kind == NYA_ERROR_INVALID_ARGUMENT, "NYA_Color has no key");
    nya_assert(nya_orm_open(arena, db, nya_reflect_of(u32), "rows", &table).kind == NYA_ERROR_NOT_SUPPORTED, "a primitive is not a row");
    nya_assert(nya_orm_open(arena, db, &TEST_ROW, nullptr, &table).kind == NYA_ERROR_INVALID_ARGUMENT);

    // An identifier cannot be bound, so it is the one thing parsed rather than trusted.
    nya_assert(nya_orm_open(arena, db, &TEST_ROW, "rows; DROP TABLE rows", &table).kind == NYA_ERROR_INVALID_ARGUMENT, "not an identifier");
    nya_assert(nya_orm_open(arena, db, &TEST_ROW, "1rows", &table).kind == NYA_ERROR_INVALID_ARGUMENT, "a leading digit is refused");
    nya_assert(nya_orm_open(arena, db, &TEST_ROW, "ro ws", &table).kind == NYA_ERROR_INVALID_ARGUMENT, "a space is refused");
  }

  // TEST: the schema is the struct, and the fresh table agrees with it
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &rows));
    defer nya_orm_close(rows);

    nya_assert(nya_string_equals(rows->sql_create,
                                 "CREATE TABLE IF NOT EXISTS rows (id INTEGER PRIMARY KEY, score INTEGER, ratio REAL, flag INTEGER, "
                                 "name TEXT)"),
               "got %s", rows->sql_create);

    // Every value is a `?`. No call site anywhere can put one into the string.
    nya_assert(nya_string_equals(rows->sql_insert, "INSERT INTO rows (id, score, ratio, flag, name) VALUES (?, ?, ?, ?, ?)"), "got %s",
               rows->sql_insert);
    nya_assert(nya_string_equals(rows->sql_update, "UPDATE rows SET score = ?, ratio = ?, flag = ?, name = ? WHERE id = ?"));

    NYA_EXPECT(nya_orm_schema_create(rows));

    u32 problems = 0;
    nya_assert(nya_orm_schema_check(rows, count_problems, &problems) == 0, "a table this module just created disagrees with its struct");
    nya_assert(problems == 0);

    // The partner, and it is as final as it sounds.
    NYA_EXPECT(nya_orm_schema_destroy(rows));
    nya_assert(nya_orm_schema_check(rows, nullptr, nullptr) == 1, "the dropped table is reported as missing");
  }

  // TEST: insert, find, update, delete, one row at a time
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &rows));
    defer nya_orm_close(rows);

    NYA_EXPECT(nya_orm_schema_create(rows));

    TestRow first = { .score = 10, .ratio = 0.5, .flag = true, .name = "first" };
    NYA_EXPECT(nya_orm_insert(rows, &first));
    nya_assert(first.id == 1, "the assigned rowid was written back, got " FMTs64, first.id);

    TestRow second = { .score = 20, .ratio = 1.5, .name = "second" };
    NYA_EXPECT(nya_orm_insert(rows, &second));
    nya_assert(second.id == 2);

    // A key the caller chose is stored as it is rather than replaced.
    TestRow chosen = { .id = 99, .score = 30, .name = "chosen" };
    NYA_EXPECT(nya_orm_insert(rows, &chosen));
    nya_assert(chosen.id == 99);

    TestRow read = { 0 };
    NYA_EXPECT(nya_orm_find(rows, arena, nya_sql_s64(1), &read));
    nya_assert(read.score == 10 && read.ratio == 0.5 && read.flag && nya_string_equals(read.name, "first"));

    first.score = 11;
    NYA_EXPECT(nya_orm_update(rows, &first));

    NYA_EXPECT(nya_orm_find(rows, arena, nya_sql_s64(1), &read));
    nya_assert(read.score == 11, "the update did not land, got " FMTu32, read.score);

    // Absence is in the return, never in the data: none of these is a silent success.
    nya_assert(nya_orm_find(rows, arena, nya_sql_s64(404), &read).kind == NYA_ERROR_NOT_FOUND);
    nya_assert(nya_orm_delete(rows, nya_sql_s64(404)).kind == NYA_ERROR_NOT_FOUND);

    TestRow absent = { .id = 404, .score = 1 };
    nya_assert(nya_orm_update(rows, &absent).kind == NYA_ERROR_NOT_FOUND, "an update is not an upsert");

    // A key of the wrong shape matches no row, which looks exactly like a row that is not there.
    nya_assert(nya_orm_delete(rows, nya_sql_text("1")).kind == NYA_ERROR_INVALID_ARGUMENT, "a text key against an integer one is refused");

    NYA_EXPECT(nya_orm_delete(rows, nya_sql_s64(2)));
    nya_assert(nya_orm_find(rows, arena, nya_sql_s64(2), &read).kind == NYA_ERROR_NOT_FOUND);
  }

  // TEST: select, with and without clauses, into an array of structs
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &rows));
    defer nya_orm_close(rows);

    NYA_EXPECT(nya_orm_schema_create(rows));

    for (u32 i = 0; i < 5; i++) {
      TestRow row = { .score = i * 10 };
      (void)snprintf(row.name, sizeof(row.name), "row %u", i);
      NYA_EXPECT(nya_orm_insert(rows, &row));
    }

    void* instances = nullptr;
    u32   count     = 0;

    NYA_EXPECT(nya_orm_select(rows, arena, nullptr, nullptr, 0, &instances, &count));
    nya_assert(count == 5, "no clauses is every row, got " FMTu32, count);

    // The clauses are SQL and the values are bound to the `?` in them, which is the whole rule.
    NYA_SqlValue above[] = { nya_sql_s64(15) };
    NYA_EXPECT(nya_orm_select(rows, arena, "WHERE score > ? ORDER BY score DESC", above, 1, &instances, &count));
    nya_assert(count == 3, "got " FMTu32, count);

    TestRow* best = nya_orm_at(rows, instances, 0);
    nya_assert(best->score == 40, "the ORDER BY was honoured, got " FMTu32, best->score);
    nya_assert(nya_string_equals(best->name, "row 4"));

    // No rows is success with a count of zero, not an error.
    NYA_SqlValue impossible[] = { nya_sql_s64(1000) };
    NYA_EXPECT(nya_orm_select(rows, arena, "WHERE score > ?", impossible, 1, &instances, &count));
    nya_assert(count == 0 && instances == nullptr);

    // The count is checked against the statement before anything runs; see nya_sql_query.
    nya_assert(nya_orm_select(rows, arena, "WHERE score > ?", nullptr, 0, &instances, &count).kind == NYA_ERROR_INVALID_ARGUMENT);
  }

  // TEST: a value never becomes SQL
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &rows));
    defer nya_orm_close(rows);

    NYA_EXPECT(nya_orm_schema_create(rows));

    // Interpolated into a statement this closes the literal and drops the table. Bound, it is a name.
    TestRow hostile = { .name = "'); DROP TABLE" };
    NYA_EXPECT(nya_orm_insert(rows, &hostile));

    TestRow read = { 0 };
    NYA_EXPECT(nya_orm_find(rows, arena, nya_sql_s64(hostile.id), &read));
    nya_assert(nya_string_equals(read.name, "'); DROP TABLE"), "stored verbatim, as data");
  }

  // TEST: a text key is never assigned, and never empty
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* slots = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_SLOT, "slots", &slots));
    defer nya_orm_close(slots);

    nya_assert(nya_string_equals(slots->sql_create, "CREATE TABLE IF NOT EXISTS slots (slot TEXT PRIMARY KEY, value INTEGER)"), "got %s",
               slots->sql_create);
    nya_assert(slots->sql_insert_assigned == nullptr, "nothing assigns a text key");

    NYA_EXPECT(nya_orm_schema_create(slots));

    TestSlot named = { .slot = "autosave", .value = 7 };
    NYA_EXPECT(nya_orm_insert(slots, &named));

    TestSlot read = { 0 };
    NYA_EXPECT(nya_orm_find(slots, arena, nya_sql_text("autosave"), &read));
    nya_assert(read.value == 7 && nya_string_equals(read.slot, "autosave"));

    // A row has to be findable, so a key that names nothing is refused rather than stored.
    TestSlot anonymous = { .value = 1 };
    nya_assert(nya_orm_insert(slots, &anonymous).kind == NYA_ERROR_INVALID_ARGUMENT, "an empty text key is refused");

    nya_assert(nya_orm_find(slots, arena, nya_sql_s64(1), &read).kind == NYA_ERROR_INVALID_ARGUMENT, "an integer key against a text one");
  }

  // TEST: an enum is stored as its variant's name, and an unnamed value is refused
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* feelings = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_FEELING, "feelings", &feelings));
    defer nya_orm_close(feelings);

    nya_assert(nya_string_equals(feelings->sql_create, "CREATE TABLE IF NOT EXISTS feelings (id INTEGER PRIMARY KEY, mood TEXT)"), "got %s",
               feelings->sql_create);

    NYA_EXPECT(nya_orm_schema_create(feelings));

    TestFeeling angry = { .mood = TEST_MOOD_ANGRY };
    NYA_EXPECT(nya_orm_insert(feelings, &angry));

    TestFeeling read = { 0 };
    NYA_EXPECT(nya_orm_find(feelings, arena, nya_sql_s64(angry.id), &read));
    nya_assert(read.mood == TEST_MOOD_ANGRY, "the enum did not survive as a name");

    // The name rather than the number, so renumbering the enum cannot reinterpret an old row.
    NYA_SqlResult raw = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT mood FROM feelings", nullptr, 0, &raw));
    nya_assert(nya_string_equals(nya_object_get(raw.rows->items[0], "mood")->as_string, "TEST_MOOD_ANGRY"));

    // A value the enum has no name for would come back as text naming no variant and be dropped on
    // the way into the struct, so it is refused on the way in instead.
    TestFeeling nameless = { .mood = TEST_MOOD_COUNT };
    nya_assert(nya_orm_insert(feelings, &nameless).kind == NYA_ERROR_INVALID_ARGUMENT, "an unnamed enum value is refused");
  }

  // TEST: a drifted table is reported precisely and never written to
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    // What an older build left behind: the key is not the key, one column is missing and one holds a
    // type whose affinity would convert every value that went through it.
    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE rows (id INTEGER, score TEXT, ratio REAL, flag INTEGER)"));

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &rows));
    defer nya_orm_close(rows);

    u32 problems = 0;
    nya_assert(nya_orm_schema_check(rows, count_problems, &problems) == 3, "a missing column, a wrong type and a key that is not one");
    nya_assert(problems == 3);

    // CREATE TABLE IF NOT EXISTS says nothing about a table that is already there, so the check is
    // what decides. Refused rather than written to; see the migration note in orm.h.
    nya_assert(nya_orm_schema_create(rows).kind == NYA_ERROR_CORRUPT, "a drifted table is not worked against");

    // A column no field describes is a warning and nothing more: every statement names its columns,
    // so one this build never heard of is neither read nor written.
    NYA_EXPECT(nya_sql_exec(db, "DROP TABLE rows"));
    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE rows (id INTEGER PRIMARY KEY, score INTEGER, ratio REAL, flag INTEGER, name TEXT, "
                                "ended TEXT DEFAULT CURRENT_TIMESTAMP)"));

    problems = 0;
    nya_assert(nya_orm_schema_check(rows, count_problems, &problems) == 1, "the extra column is reported");
    NYA_EXPECT(nya_orm_schema_create(rows));

    TestRow row = { .score = 1, .name = "still works" };
    NYA_EXPECT(nya_orm_insert(rows, &row));

    // And the column this build never heard of kept its default, untouched.
    NYA_SqlResult ended = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT ended FROM rows", nullptr, 0, &ended));
    nya_assert(nya_object_get(ended.rows->items[0], "ended")->type == NYA_TYPE_STRING, "the default ran, so nothing overwrote it");
  }

  // TEST: closing is idempotent and safe on null
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &rows));

    nya_orm_close(nullptr);
    nya_orm_close(rows);
    nya_orm_close(rows);

    nya_assert(rows->database == nullptr, "a closed table holds no connection");
  }

  // TEST: the round trip is a law, not an example
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_orm_open(arena, db, &TEST_ROW, "rows", &law_table));
    defer nya_orm_close(law_table);

    NYA_EXPECT(nya_orm_schema_create(law_table));

    u32 failures = nya_property_check("a written row reads back as the struct that was written", CASES, SEED, law_row_round_trips);
    nya_assert(failures == 0, "the round trip does not hold");
  }

  printf("PASSED: test_db_orm\n");
  return 0;
}
