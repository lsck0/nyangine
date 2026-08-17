/**
 * The reflection runtime, driven by hand written tables.
 *
 * Hand written on purpose. src/build/reflection.c has to emit exactly this shape, so writing it out
 * once by hand says what the generator's target *is* — and it means the runtime is proven before the
 * generator exists rather than the two being debugged against each other.
 *
 * What it defends:
 *
 * - **A walk ends at primitives.** A struct inside a struct inside a vector resolves all the way
 *   down, which is the whole premise.
 * - **Offsets come from the compiler.** Every table below uses nya_offsetof and sizeof, so this also
 *   checks that a padded struct is described correctly without anyone modelling padding.
 * - **Round tripping is lossless** for everything the design says it covers, and leaves alone
 *   everything it says it does not.
 * - **Enums survive renumbering**, because they are written as names.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────
 * THE TYPES UNDER TEST
 * ─────────────────────────────────────────────────────────
 */

typedef enum {
  TEST_KIND_NONE  = 0,
  TEST_KIND_CRATE = 1,
  TEST_KIND_BOX   = 7,  // deliberately not contiguous
} TestKind;

typedef enum {
  TEST_FLAG_NONE      = 0,
  TEST_FLAG_VISIBLE   = 1ULL << 0,
  TEST_FLAG_SOLID     = 1ULL << 1,
  TEST_FLAG_SELECTED  = 1ULL << 2,
} TestFlag;

typedef struct {
  f32 r, g, b, a;
} TestColor;

typedef struct {
  TestColor color;
  f32       size;
} TestVisual;

typedef struct {
  f32x3      position;
  TestVisual visual;
  TestKind   kind;
  u64        flags;
  char       name[16];
  s32        health;
  b8         alive;
  f64        weight;
} TestEntity;

/*
 * ─────────────────────────────────────────────────────────
 * THE TABLES, AS THE GENERATOR WILL EMIT THEM
 * ─────────────────────────────────────────────────────────
 */

static const NYA_TypeReflection _NYA_REFLECT_f32 = {
  .name = "f32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f32), .alignment = alignof(f32), .primitive = NYA_TYPE_F32,
};

static const NYA_TypeReflection _NYA_REFLECT_f64 = {
  .name = "f64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f64), .alignment = alignof(f64), .primitive = NYA_TYPE_F64,
};

static const NYA_TypeReflection _NYA_REFLECT_s32 = {
  .name = "s32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s32), .alignment = alignof(s32), .primitive = NYA_TYPE_S32,
};

static const NYA_TypeReflection _NYA_REFLECT_u64 = {
  .name = "u64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u64), .alignment = alignof(u64), .primitive = NYA_TYPE_U64,
};

static const NYA_TypeReflection _NYA_REFLECT_b8 = {
  .name = "b8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b8), .alignment = alignof(b8), .primitive = NYA_TYPE_B8,
};

static const NYA_TypeReflection _NYA_REFLECT_char = {
  .name = "char", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char), .alignment = alignof(char), .primitive = NYA_TYPE_CHAR,
};

/** A clang extended vector: three floats in sixteen bytes. See NYA_REFLECT_VECTOR. */
static const NYA_TypeReflection _NYA_REFLECT_f32x3 = {
  .name          = "f32x3",
  .kind          = NYA_REFLECT_VECTOR,
  .size          = sizeof(f32x3),
  .alignment     = alignof(f32x3),
  .element       = &_NYA_REFLECT_f32,
  .element_count = 3,
};

static const NYA_ReflectVariant _NYA_REFLECT_TestKind_VARIANTS[] = {
  { .name = "TEST_KIND_NONE", .value = 0 },
  { .name = "TEST_KIND_CRATE", .value = 1 },
  { .name = "TEST_KIND_BOX", .value = 7 },
};

static const NYA_TypeReflection _NYA_REFLECT_TestKind = {
  .name          = "TestKind",
  .kind          = NYA_REFLECT_ENUM,
  .size          = sizeof(TestKind),
  .alignment     = alignof(TestKind),
  .primitive     = NYA_TYPE_U32,
  .variants      = _NYA_REFLECT_TestKind_VARIANTS,
  .variant_count = 3,
};

static const NYA_ReflectVariant _NYA_REFLECT_TestFlag_VARIANTS[] = {
  { .name = "TEST_FLAG_NONE", .value = 0 },
  { .name = "TEST_FLAG_VISIBLE", .value = 1 },
  { .name = "TEST_FLAG_SOLID", .value = 2 },
  { .name = "TEST_FLAG_SELECTED", .value = 4 },
};

static const NYA_TypeReflection _NYA_REFLECT_TestFlag = {
  .name          = "TestFlag",
  .kind          = NYA_REFLECT_ENUM,
  .size          = sizeof(u64),
  .alignment     = alignof(u64),
  .primitive     = NYA_TYPE_U64,
  .variants      = _NYA_REFLECT_TestFlag_VARIANTS,
  .variant_count = 4,
  .is_bitflags   = true,
};

static const NYA_ReflectField _NYA_REFLECT_TestColor_FIELDS[] = {
  { .name = "r", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(TestColor, r) },
  { .name = "g", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(TestColor, g) },
  { .name = "b", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(TestColor, b) },
  { .name = "a", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(TestColor, a) },
};

static const NYA_TypeReflection _NYA_REFLECT_TestColor = {
  .name        = "TestColor",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(TestColor),
  .alignment   = alignof(TestColor),
  .fields      = _NYA_REFLECT_TestColor_FIELDS,
  .field_count = 4,
};

static const NYA_ReflectField _NYA_REFLECT_TestVisual_FIELDS[] = {
  { .name = "color", .type = &_NYA_REFLECT_TestColor, .offset = nya_offsetof(TestVisual, color), .hint = NYA_HINT_COLOR },
  { .name = "size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(TestVisual, size) },
};

static const NYA_TypeReflection _NYA_REFLECT_TestVisual = {
  .name        = "TestVisual",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(TestVisual),
  .alignment   = alignof(TestVisual),
  .fields      = _NYA_REFLECT_TestVisual_FIELDS,
  .field_count = 2,
};

/** `char name[16]`, which to_object writes as text rather than as sixteen numbers. */
static const NYA_TypeReflection _NYA_REFLECT_char_16 = {
  .name          = "char[16]",
  .kind          = NYA_REFLECT_ARRAY,
  .size          = sizeof(char[16]),
  .alignment     = alignof(char[16]),
  .element       = &_NYA_REFLECT_char,
  .element_count = 16,
};

static const NYA_ReflectField _NYA_REFLECT_TestEntity_FIELDS[] = {
  { .name = "position", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(TestEntity, position), .hint = NYA_HINT_POSITION },
  { .name = "visual", .type = &_NYA_REFLECT_TestVisual, .offset = nya_offsetof(TestEntity, visual) },
  { .name = "kind", .type = &_NYA_REFLECT_TestKind, .offset = nya_offsetof(TestEntity, kind) },
  { .name = "flags", .type = &_NYA_REFLECT_TestFlag, .offset = nya_offsetof(TestEntity, flags), .hint = NYA_HINT_BITFLAGS },
  { .name = "name", .type = &_NYA_REFLECT_char_16, .offset = nya_offsetof(TestEntity, name) },
  { .name = "health", .type = &_NYA_REFLECT_s32, .offset = nya_offsetof(TestEntity, health) },
  { .name = "alive", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(TestEntity, alive) },
  { .name = "weight", .type = &_NYA_REFLECT_f64, .offset = nya_offsetof(TestEntity, weight) },
};

static const NYA_TypeReflection _NYA_REFLECT_TestEntity = {
  .name        = "TestEntity",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(TestEntity),
  .alignment   = alignof(TestEntity),
  .fields      = _NYA_REFLECT_TestEntity_FIELDS,
  .field_count = 8,
};

/** Proves on_apply runs, and runs after the fields are in place. */
static u32 APPLY_CALLS      = 0;
static s32 APPLY_SAW_HEALTH = 0;

static NYA_Error test_apply(void* instance) {
  APPLY_CALLS++;
  APPLY_SAW_HEALTH = ((TestEntity*)instance)->health;

  return NYA_OK;
}

static TestEntity sample(void) {
  TestEntity entity = {
    .position = { 1.5F, -2.0F, 3.25F },
    .visual   = { .color = { 0.1F, 0.2F, 0.3F, 1.0F }, .size = 4.5F },
    .kind     = TEST_KIND_BOX,
    .flags    = TEST_FLAG_VISIBLE | TEST_FLAG_SELECTED,
    .health   = -42,
    .alive    = true,
    .weight   = 12.75,
  };

  (void)snprintf(entity.name, sizeof(entity.name), "crate");

  return entity;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_reflection");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: fields, paths and offsets
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: lookup\n");
  {
    const NYA_ReflectField* health = nya_reflect_field(&_NYA_REFLECT_TestEntity, "health");

    nya_assert(health != nullptr, "the field is missing");
    nya_assert(health->type == &_NYA_REFLECT_s32);
    nya_assert(health->offset == nya_offsetof(TestEntity, health));

    nya_assert(nya_reflect_field(&_NYA_REFLECT_TestEntity, "nope") == nullptr, "an absent field must not be invented");

    // A path resolves through nested structs and hands back the address at the same time, which is
    // the pair an inspector needs and the reason it is one call.
    TestEntity entity = sample();

    void*                   address = nullptr;
    const NYA_ReflectField* green   = nya_reflect_path(&_NYA_REFLECT_TestEntity, "visual.color.g", &entity, &address);

    nya_assert(green != nullptr, "a dotted path did not resolve");
    nya_assert(green->type == &_NYA_REFLECT_f32);
    nya_assert(address == &entity.visual.color.g, "the path resolved to the wrong address");

    // The address the compiler computed and the one reflection computed must agree, which is what
    // says nya_offsetof survived the trip through the table.
    nya_assert(nya_reflect_field_pointer(&entity, health) == &entity.health);

    // A path that runs through a primitive has nowhere to go.
    nya_assert(nya_reflect_path(&_NYA_REFLECT_TestEntity, "health.nope", &entity, nullptr) == nullptr);

    // Schema-only lookup, with no instance to touch.
    nya_assert(nya_reflect_path(&_NYA_REFLECT_TestEntity, "visual.size", nullptr, nullptr) != nullptr);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: reading and writing primitives
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: primitives\n");
  {
    TestEntity entity = sample();

    NYA_Value health = nya_reflect_read(&_NYA_REFLECT_s32, &entity.health);
    nya_assert(health.type == NYA_TYPE_S32);
    nya_assert(health.as_s32 == -42);

    // Coercion: a number that came out of a file is whatever the reader made of it, not what the
    // field wants. An integer must land in an f64 field.
    nya_assert(nya_reflect_write(&_NYA_REFLECT_f64, &entity.weight, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 7 }));
    nya_assert(entity.weight == 7.0, "an integer did not widen into a float field");

    // And the other direction, for a whole number written with a decimal point.
    nya_assert(nya_reflect_write(&_NYA_REFLECT_s32, &entity.health, (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 3.0 }));
    nya_assert(entity.health == 3);

    // A string is not a number and must not silently become one.
    nya_assert(!nya_reflect_write(&_NYA_REFLECT_s32, &entity.health, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "x" }));
    nya_assert(entity.health == 3, "a rejected write still modified the field");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: enums by name
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: enums\n");
  {
    nya_assert(nya_string_equals(nya_reflect_variant_name(&_NYA_REFLECT_TestKind, 7), "TEST_KIND_BOX"));
    nya_assert(nya_reflect_variant_name(&_NYA_REFLECT_TestKind, 99) == nullptr, "an unnamed value must not be invented");

    s64 value = 0;
    nya_assert(nya_reflect_variant_value(&_NYA_REFLECT_TestKind, "TEST_KIND_CRATE", &value));
    nya_assert(value == 1);

    // Zero is a real variant value, so the lookup reports success separately rather than by
    // returning zero for "not found".
    nya_assert(nya_reflect_variant_value(&_NYA_REFLECT_TestKind, "TEST_KIND_NONE", &value));
    nya_assert(value == 0);
    nya_assert(!nya_reflect_variant_value(&_NYA_REFLECT_TestKind, "NOPE", &value));

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: to_object walks all the way down
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: to_object\n");
  {
    TestEntity  entity = sample();
    NYA_Object* object = nya_reflect_to_object(arena, &_NYA_REFLECT_TestEntity, &entity);

    nya_assert(object != nullptr);

    // A vector becomes an array of its elements, and its stride is its element size even though its
    // own size is padded to sixteen.
    NYA_Value* position = nya_object_get(object, "position");
    nya_assert(position != nullptr && position->type == NYA_TYPE_ARRAY);
    nya_assert(position->as_array.length == 3, "a three element vector produced %llu values",
               (unsigned long long)position->as_array.length);
    nya_assert(position->as_array.items[0].as_f32 == 1.5F);
    nya_assert(position->as_array.items[2].as_f32 == 3.25F);

    // A nested struct becomes a nested object, recursively.
    NYA_Value* visual = nya_object_get(object, "visual");
    nya_assert(visual != nullptr && visual->type == NYA_TYPE_OBJECT);

    NYA_Value* color = nya_object_get(&visual->as_object, "color");
    nya_assert(color != nullptr && color->type == NYA_TYPE_OBJECT);

    NYA_Value* blue = nya_object_get(&color->as_object, "b");
    nya_assert(blue != nullptr && blue->as_f32 == 0.3F, "the walk did not reach the third level");

    // An enum is a name, not a number: that is what survives the enum being renumbered.
    NYA_Value* kind = nya_object_get(object, "kind");
    nya_assert(kind != nullptr && kind->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(kind->as_string, "TEST_KIND_BOX"));

    // Flags are the list of names that are set, and only those.
    NYA_Value* flags = nya_object_get(object, "flags");
    nya_assert(flags != nullptr && flags->type == NYA_TYPE_ARRAY);
    nya_assert(flags->as_array.length == 2, "expected two flags, got %llu", (unsigned long long)flags->as_array.length);
    nya_assert(nya_string_equals(flags->as_array.items[0].as_string, "TEST_FLAG_VISIBLE"));
    nya_assert(nya_string_equals(flags->as_array.items[1].as_string, "TEST_FLAG_SELECTED"));

    // A char array is text.
    NYA_Value* name = nya_object_get(object, "name");
    nya_assert(name != nullptr && name->type == NYA_TYPE_STRING, "a char array was not written as text");
    nya_assert(nya_string_equals(name->as_string, "crate"));

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the round trip is lossless
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: round trip\n");
  {
    TestEntity  original = sample();
    NYA_Object* object   = nya_reflect_to_object(arena, &_NYA_REFLECT_TestEntity, &original);

    TestEntity restored = { 0 };
    NYA_EXPECT(nya_reflect_from_object(&_NYA_REFLECT_TestEntity, &restored, object));

    nya_assert(restored.position[0] == original.position[0]);
    nya_assert(restored.position[1] == original.position[1]);
    nya_assert(restored.position[2] == original.position[2]);
    nya_assert(restored.visual.color.b == original.visual.color.b);
    nya_assert(restored.visual.size == original.visual.size);
    nya_assert(restored.kind == TEST_KIND_BOX, "the enum did not come back as itself");
    nya_assert(restored.flags == original.flags, "the flags did not come back as themselves");
    nya_assert(nya_string_equals(restored.name, "crate"));
    nya_assert(restored.health == original.health);
    nya_assert(restored.alive == original.alive);
    nya_assert(restored.weight == original.weight);

    // Through a real file, since that is where the type information is actually lost and rebuilt.
    NYA_String* json = nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);
    nya_assert(json != nullptr);

    NYA_Object* reparsed = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (const u8*)json->items, json->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &reparsed));

    TestEntity from_file = { 0 };
    NYA_EXPECT(nya_reflect_from_object(&_NYA_REFLECT_TestEntity, &from_file, reparsed));

    nya_assert(from_file.kind == TEST_KIND_BOX, "the enum did not survive JSON");
    nya_assert(from_file.flags == original.flags, "the flags did not survive JSON");
    nya_assert(from_file.health == original.health, "a negative integer did not survive JSON");
    nya_assert(from_file.weight == original.weight);
    nya_assert(nya_string_equals(from_file.name, "crate"));
    nya_assert(from_file.position[2] == original.position[2], "a vector element did not survive JSON");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: loading is partial, deliberately
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: partial load\n");
  {
    // What an older save looks like against a newer struct: it mentions one field and knows nothing
    // about the rest.
    NYA_Object* partial = nya_object_create(arena);
    nya_object_set(partial, "health", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = 5 });
    nya_object_set(partial, "unknown_field", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });

    TestEntity entity = sample();

    NYA_EXPECT(nya_reflect_from_object(&_NYA_REFLECT_TestEntity, &entity, partial));

    nya_assert(entity.health == 5, "the mentioned field was not written");
    nya_assert(entity.weight == 12.75, "an unmentioned field was zeroed instead of left alone");
    nya_assert(entity.kind == TEST_KIND_BOX, "an unmentioned enum was cleared");
    nya_assert(nya_string_equals(entity.name, "crate"), "an unmentioned string was cleared");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a string longer than the array is truncated, not overrun
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: bounded char array\n");
  {
    NYA_Object* object = nya_object_create(arena);
    nya_object_set(object, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "a name far longer than sixteen bytes" });

    TestEntity entity = { 0 };
    NYA_EXPECT(nya_reflect_from_object(&_NYA_REFLECT_TestEntity, &entity, object));

    nya_assert(strlen(entity.name) == 15, "expected truncation to fifteen plus a terminator, got %llu",
               (unsigned long long)strlen(entity.name));
    nya_assert(entity.name[15] == '\0', "the array was not terminated");
    nya_assert(nya_string_equals(entity.name, "a name far long"));

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: on_apply runs last
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: apply hook\n");
  {
    // A copy of the table with the hook set, so the other tests stay free of it.
    NYA_TypeReflection with_hook = _NYA_REFLECT_TestEntity;
    with_hook.on_apply           = test_apply;

    NYA_Object* object = nya_object_create(arena);
    nya_object_set(object, "health", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = 77 });

    TestEntity entity = { 0 };

    APPLY_CALLS = 0;
    NYA_EXPECT(nya_reflect_from_object(&with_hook, &entity, object));

    nya_assert(APPLY_CALLS == 1, "the hook ran %u times", APPLY_CALLS);
    nya_assert(APPLY_SAW_HEALTH == 77, "the hook ran before the fields were written");

    printf("  PASSED\n");
  }

  printf("PASSED: test_reflection\n");

  return 0;
}
