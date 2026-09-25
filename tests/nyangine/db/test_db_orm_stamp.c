/**
 * The ORM's automatic timestamps: a field carrying @created_at or @updated_at is stamped with the
 * engine clock rather than by hand. Insert sets both and writes them back into the struct; update
 * advances @updated_at alone, so @created_at survives. A row with neither attribute is untouched.
 *
 * The clock is driven by hand through nya_instant_source_set, so the stamps are exact times the test
 * chose and the difference between an insert and an update is a value it can assert on, not a race.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/* A CLOCK THE TEST DRIVES */

static s64 g_now_ns = 0;

static NYA_Instant test_clock(void* context) {
  (void)context;
  return (NYA_Instant){ .ns = g_now_ns };
}

/* THE DESCRIBED TYPES */

/** A row whose two timestamps the ORM keeps for it. Written by hand, as the other ORM tests are. */
typedef struct {
  s64 id;
  u32 score;
  s64 created_at;
  s64 updated_at;
} Stamped;

static const NYA_ReflectAttribute CREATED_ATTRIBUTES[] = { { .name = "created_at" } };
static const NYA_ReflectAttribute UPDATED_ATTRIBUTES[] = { { .name = "updated_at" } };

static const NYA_ReflectField STAMPED_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(Stamped, id), .is_key = true },
  { .name = "score", .type = nya_reflect_of(u32), .offset = nya_offsetof(Stamped, score) },
  { .name       = "created_at",
    .type       = nya_reflect_of(s64),
    .offset     = nya_offsetof(Stamped, created_at),
    .attributes = CREATED_ATTRIBUTES,
    .attribute_count = nya_carray_length(CREATED_ATTRIBUTES) },
  { .name       = "updated_at",
    .type       = nya_reflect_of(s64),
    .offset     = nya_offsetof(Stamped, updated_at),
    .attributes = UPDATED_ATTRIBUTES,
    .attribute_count = nya_carray_length(UPDATED_ATTRIBUTES) },
};

static const NYA_TypeReflection STAMPED = {
  .name        = "Stamped",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(Stamped),
  .alignment   = alignof(Stamped),
  .fields      = STAMPED_FIELDS,
  .field_count = nya_carray_length(STAMPED_FIELDS),
};

/** The common row: no timestamp attribute anywhere, so nothing here stamps it. */
typedef struct {
  s64 id;
  s64 created_at;
} Plain;

static const NYA_ReflectField PLAIN_FIELDS[] = {
  { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(Plain, id), .is_key = true },
  { .name = "created_at", .type = nya_reflect_of(s64), .offset = nya_offsetof(Plain, created_at) },
};

static const NYA_TypeReflection PLAIN = {
  .name        = "Plain",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(Plain),
  .alignment   = alignof(Plain),
  .fields      = PLAIN_FIELDS,
  .field_count = nya_carray_length(PLAIN_FIELDS),
};

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_orm_stamp");
  defer      nya_arena_destroy(arena);

  // A large, nonzero base so a stamp is never the zero a plain field starts at. Roughly 2023-11-14.
  g_now_ns = 1700000000LL * NYA_NS_PER_SECOND;
  nya_instant_source_set((NYA_InstantSource){ .now = test_clock });

  // TEST: insert stamps both, update advances @updated_at only, @created_at is preserved
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &STAMPED, "rows", &rows));
    defer nya_orm_close(rows);

    NYA_EXPECT(nya_orm_schema_create(rows));

    s64 inserted_at = g_now_ns;

    // The struct names no time; the ORM fills both, and writes them back as it does an assigned key.
    Stamped row = { .score = 10 };
    NYA_EXPECT(nya_orm_insert(rows, &row));
    nya_assert(row.created_at == inserted_at, "created_at was not stamped on insert, got " FMTs64, row.created_at);
    nya_assert(row.updated_at == inserted_at, "updated_at was not stamped on insert, got " FMTs64, row.updated_at);

    // The row on disk carries the same two.
    Stamped read = { 0 };
    NYA_EXPECT(nya_orm_find(rows, arena, nya_sql_s64(row.id), &read));
    nya_assert(read.created_at == inserted_at && read.updated_at == inserted_at, "the stamps did not reach the row");

    // Time moves on, and an update is made from the row as it was read back.
    g_now_ns += 5LL * NYA_NS_PER_SECOND;
    s64 updated_at = g_now_ns;

    read.score = 20;
    NYA_EXPECT(nya_orm_update(rows, &read));

    Stamped after = { 0 };
    NYA_EXPECT(nya_orm_find(rows, arena, nya_sql_s64(row.id), &after));
    nya_assert(after.score == 20, "the update did not land, got " FMTu32, after.score);
    nya_assert(after.created_at == inserted_at, "created_at was not preserved across the update, got " FMTs64, after.created_at);
    nya_assert(after.updated_at == updated_at, "updated_at did not advance, got " FMTs64, after.updated_at);
    nya_assert(after.updated_at != after.created_at, "the two stamps should differ after an update");
  }

  // TEST: a row carrying neither attribute is untouched, whatever the clock reads
  {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
    defer nya_sql_close(db);

    NYA_OrmTable* rows = nullptr;
    NYA_EXPECT(nya_orm_open(arena, db, &PLAIN, "plain", &rows));
    defer nya_orm_close(rows);

    NYA_EXPECT(nya_orm_schema_create(rows));

    // created_at here is an ordinary column the caller owns: the name is not the attribute.
    Plain row = { .created_at = 42 };
    NYA_EXPECT(nya_orm_insert(rows, &row));
    nya_assert(row.created_at == 42, "a field the attribute never marked was rewritten, got " FMTs64, row.created_at);

    g_now_ns += 5LL * NYA_NS_PER_SECOND;

    row.created_at = 43;
    NYA_EXPECT(nya_orm_update(rows, &row));

    Plain read = { 0 };
    NYA_EXPECT(nya_orm_find(rows, arena, nya_sql_s64(row.id), &read));
    nya_assert(read.created_at == 43, "an unmarked column stored the value it was given, got " FMTs64, read.created_at);
  }

  printf("PASSED: test_db_orm_stamp\n");
  return 0;
}
