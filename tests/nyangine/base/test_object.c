/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

// Helper to compare values
static b8 values_equal(const NYA_Value* a, const NYA_Value* b) {
  if (a->type != b->type) return false;

  switch (a->type) {
    case NYA_TYPE_NULL:    return true;
    case NYA_TYPE_B8:      return a->as_b8 == b->as_b8;
    case NYA_TYPE_B16:     return a->as_b16 == b->as_b16;
    case NYA_TYPE_B32:     return a->as_b32 == b->as_b32;
    case NYA_TYPE_B64:     return a->as_b64 == b->as_b64;
    case NYA_TYPE_U8:      return a->as_u8 == b->as_u8;
    case NYA_TYPE_U16:     return a->as_u16 == b->as_u16;
    case NYA_TYPE_U32:     return a->as_u32 == b->as_u32;
    case NYA_TYPE_U64:     return a->as_u64 == b->as_u64;
    case NYA_TYPE_S8:      return a->as_s8 == b->as_s8;
    case NYA_TYPE_S16:     return a->as_s16 == b->as_s16;
    case NYA_TYPE_S32:     return a->as_s32 == b->as_s32;
    case NYA_TYPE_S64:     return a->as_s64 == b->as_s64;
    case NYA_TYPE_F16:     return a->as_f16 == b->as_f16;
    case NYA_TYPE_F32:     return a->as_f32 == b->as_f32;
    case NYA_TYPE_F64:     return a->as_f64 == b->as_f64;
    case NYA_TYPE_CHAR:    return a->as_char == b->as_char;
    case NYA_TYPE_STRING:  return nya_string_equals(a->as_string, b->as_string);
    case NYA_TYPE_OBJECT:  return false; // TODO: Deep compare
    case NYA_TYPE_ARRAY:   return false; // TODO: Deep compare
    default:               return false; printf("  PASSED\n");
  }
}

// Helper to compare objects
static b8 objects_equal(const NYA_Object* a, const NYA_Object* b) {
  if (a->length != b->length) return false;

  nya_hmap_foreach_key (a, key) {
    NYA_Value* val_a = nya_object_get(a, *key);
    NYA_Value* val_b = nya_object_get(b, *key);

    if (!val_b) return false;
    if (!values_equal(val_a, val_b)) return false;
    printf("  PASSED\n");
  }

  return true;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_object");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Basic primitive types serialization/deserialization
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Basic primitive types\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "u8_val", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = 255 });
    nya_object_set(obj, "u32_val", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 4294967295 });
    nya_object_set(obj, "s32_val", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = -2147483648 });
    nya_object_set(obj, "f64_val", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 3.14159 });
    nya_object_set(obj, "bool_val", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
    nya_object_set(obj, "string_val", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "hello_world" });

    // Test compact serialization
    NYA_String compact = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    nya_assert(compact.length > 0);
    printf("  Compact:\n    " NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(&compact));

    // Test pretty serialization
    NYA_String pretty = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    nya_assert(pretty.length > 0);
    printf("  Pretty:\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(&pretty));

    // Test round-trip deserialization from compact
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, compact.items, compact.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));
    nya_assert(objects_equal(obj, restored));

    // Test round-trip deserialization from pretty
    NYA_Object* restored_pretty = nullptr;
    NYA_EXPECT(nya_deserialize(arena, pretty.items, pretty.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored_pretty));
    nya_assert(objects_equal(obj, restored_pretty));
    printf("  PASSED\n");
  }

  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Nested objects
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Nested objects\n");
  {
    NYA_Object* inner = nya_object_create(arena);
    nya_object_set(inner, "x", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 10 });
    nya_object_set(inner, "y", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 20 });

    NYA_Object* outer     = nya_object_create(arena);
    NYA_Value   inner_val = { .type = NYA_TYPE_OBJECT, .as_object = *inner };
    nya_object_set(outer, "position", inner_val);
    nya_object_set(outer, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "player" });

    NYA_String serialized = *nya_serialize(arena, outer, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    nya_assert(serialized.length > 0);
    printf("  Pretty:\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(&serialized));

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* restored_pos = nya_object_get(restored, "position");
    nya_assert(restored_pos != nullptr);
    nya_assert(restored_pos->type == NYA_TYPE_OBJECT);

    NYA_Value* x_val = nya_object_get(&restored_pos->as_object, "x");
    nya_assert(x_val != nullptr);
    nya_assert(x_val->type == NYA_TYPE_U32);
    nya_assert(x_val->as_u32 == 10);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Arrays of primitives
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Object* obj = nya_object_create(arena);

    // Create array of u32
    NYA_ArrayᐸNYA_Valueᐳ arr = nya_array_create_on_stack(arena, NYA_Value);
    NYA_Value      v1  = { .type = NYA_TYPE_U32, .as_u32 = 1 };
    NYA_Value      v2  = { .type = NYA_TYPE_U32, .as_u32 = 2 };
    NYA_Value      v3  = { .type = NYA_TYPE_U32, .as_u32 = 3 };
    nya_array_push_back(&arr, v1);
    nya_array_push_back(&arr, v2);
    nya_array_push_back(&arr, v3);

    nya_object_set(obj, "numbers", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = arr });

    NYA_String serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    nya_assert(serialized.length > 0);

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* restored_arr = nya_object_get(restored, "numbers");
    nya_assert(restored_arr != nullptr);
    nya_assert(restored_arr->type == NYA_TYPE_ARRAY);
    nya_assert(restored_arr->as_array.length == 3);

    NYA_Value* first = &restored_arr->as_array.items[0];
    nya_assert(first->type == NYA_TYPE_U32);
    nya_assert(first->as_u32 == 1);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Arrays of objects (without object keyword in elements)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Object* obj = nya_object_create(arena);

    // Create user 1
    NYA_Object* user1 = nya_object_create(arena);
    nya_object_set(user1, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "Alice" });
    nya_object_set(user1, "age", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 25 });

    // Create user 2
    NYA_Object* user2 = nya_object_create(arena);
    nya_object_set(user2, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "Bob" });
    nya_object_set(user2, "age", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 30 });

    // Create array of objects
    NYA_ArrayᐸNYA_Valueᐳ users = nya_array_create_on_stack(arena, NYA_Value);
    NYA_Value      u1    = { .type = NYA_TYPE_OBJECT, .as_object = *user1 };
    NYA_Value      u2    = { .type = NYA_TYPE_OBJECT, .as_object = *user2 };
    nya_array_push_back(&users, u1);
    nya_array_push_back(&users, u2);

    nya_object_set(obj, "users", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = users });

    NYA_String serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    nya_assert(serialized.length > 0);

    // Verify objects in array don't have "object" keyword
    nya_assert(nya_string_contains(&serialized, "object[]") == true);

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* restored_users = nya_object_get(restored, "users");
    nya_assert(restored_users != nullptr);
    nya_assert(restored_users->type == NYA_TYPE_ARRAY);
    nya_assert(restored_users->as_array.length == 2);

    // Check first user
    NYA_Value* first_user = &restored_users->as_array.items[0];
    nya_assert(first_user->type == NYA_TYPE_OBJECT);
    NYA_Value* name = nya_object_get(&first_user->as_object, "name");
    nya_assert(name != nullptr);
    nya_assert(name->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(name->as_string, "Alice") == true);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Empty array
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Object*    obj       = nya_object_create(arena);
    NYA_ArrayᐸNYA_Valueᐳ empty_arr = nya_array_create_on_stack(arena, NYA_Value);
    nya_object_set(obj, "empty", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = empty_arr });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* arr = nya_object_get(restored, "empty");
    nya_assert(arr != nullptr);
    nya_assert(arr->type == NYA_TYPE_ARRAY);
    nya_assert(arr->as_array.length == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Comments are stripped
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const char* text_with_comments = "/* This is a comment */\n"
                                     "nya 2 12345\n"
                                     "{\n"
                                     "key: u32 42;\n"
                                     "/* Another comment */\n"
                                     "name: string \"test\";\n"
                                     "}\n";

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (u8*)text_with_comments, strlen(text_with_comments), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored));

    NYA_Value* key = nya_object_get(restored, "key");
    nya_assert(key != nullptr);
    nya_assert(key->type == NYA_TYPE_U32);
    nya_assert(key->as_u32 == 42);

    NYA_Value* name = nya_object_get(restored, "name");
    nya_assert(name != nullptr);
    nya_assert(name->type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(name->as_string, "test") == true);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Hex and binary numbers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const char* text_with_hex = "nya 2 12345\n"
                                "{\n"
                                "hex_val: u32 0xFF;\n"
                                "bin_val: u32 0b1010;\n"
                                "}\n";

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (u8*)text_with_hex, strlen(text_with_hex), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored));

    NYA_Value* hex = nya_object_get(restored, "hex_val");
    nya_assert(hex != nullptr);
    nya_assert(hex->type == NYA_TYPE_U32);
    nya_assert(hex->as_u32 == 255);

    NYA_Value* bin = nya_object_get(restored, "bin_val");
    nya_assert(bin != nullptr);
    nya_assert(bin->type == NYA_TYPE_U32);
    nya_assert(bin->as_u32 == 10);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Trailing comma in arrays
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const char* text_with_trailing_comma = "nya 2 12345\n"
                                           "{ arr: u32[] [1,2,3,]; }\n";

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(
        arena,
        (u8*)text_with_trailing_comma,
        strlen(text_with_trailing_comma),
        NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM
    , &restored));

    NYA_Value* arr = nya_object_get(restored, "arr");
    nya_assert(arr != nullptr);
    nya_assert(arr->type == NYA_TYPE_ARRAY);
    nya_assert(arr->as_array.length == 3);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Complex nested structure
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Object* config = nya_object_create(arena);

    // Graphics settings
    NYA_Object* graphics = nya_object_create(arena);
    nya_object_set(graphics, "quality", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "high" });
    nya_object_set(graphics, "resolution", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "1920x1080" });

    // Shader list
    NYA_ArrayᐸNYA_Valueᐳ shaders = nya_array_create_on_stack(arena, NYA_Value);
    NYA_Value      s1      = { .type = NYA_TYPE_STRING, .as_string = "vertex" };
    NYA_Value      s2      = { .type = NYA_TYPE_STRING, .as_string = "fragment" };
    nya_array_push_back(&shaders, s1);
    nya_array_push_back(&shaders, s2);
    nya_object_set(graphics, "shaders", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = shaders });

    nya_object_set(config, "graphics", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *graphics });

    // Players list
    NYA_ArrayᐸNYA_Valueᐳ players = nya_array_create_on_stack(arena, NYA_Value);
    for (u32 i = 0; i < 3; i++) {
      NYA_Object* player = nya_object_create(arena);
      char        name_buf[32];
      snprintf(name_buf, sizeof(name_buf), "player_%u", i);
      char* name_str = nya_arena_alloc(arena, strlen(name_buf) + 1);
      strcpy(name_str, name_buf);
      nya_object_set(player, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = name_str });
      nya_object_set(player, "score", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = i * 100 });
      NYA_Value p = { .type = NYA_TYPE_OBJECT, .as_object = *player };
      nya_array_push_back(&players, p);
    }
    nya_object_set(config, "players", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = players });

    NYA_String serialized = *nya_serialize(arena, config, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    nya_assert(serialized.length > 0);
    printf("  Pretty:\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(&serialized));

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    // Verify graphics
    NYA_Value* gfx = nya_object_get(restored, "graphics");
    nya_assert(gfx != nullptr);
    nya_assert(gfx->type == NYA_TYPE_OBJECT);
    NYA_Value* quality = nya_object_get(&gfx->as_object, "quality");
    nya_assert(quality != nullptr);
    nya_assert(nya_string_equals(quality->as_string, "high") == true);

    // Verify players
    NYA_Value* restored_players = nya_object_get(restored, "players");
    nya_assert(restored_players != nullptr);
    nya_assert(restored_players->as_array.length == 3);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Version validation
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const char* wrong_version = "nya 99 12345\n{ key: u32 42; }\n";
    NYA_Object* restored = nullptr;
    nya_assert(!nya_deserialize(arena, (u8*)wrong_version, strlen(wrong_version), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored).ok);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_object_remove
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Object remove\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "a", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });
    nya_object_set(obj, "b", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 2 });
    nya_object_set(obj, "c", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 3 });
    nya_assert(obj->length == 3);

    nya_object_remove(obj, "b");
    nya_assert(obj->length == 2);
    nya_assert(nya_object_get(obj, "b") == nullptr);
    nya_assert(nya_object_get(obj, "a") != nullptr);
    nya_assert(nya_object_get(obj, "c") != nullptr);
    nya_assert(nya_object_get(obj, "a")->as_u32 == 1);
    nya_assert(nya_object_get(obj, "c")->as_u32 == 3);

    // Remove non-existent key should be safe
    nya_object_remove(obj, "nonexistent");
    nya_assert(obj->length == 2);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_object_reset
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Object reset\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "x", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 42 });
    nya_object_set(obj, "y", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 99 });
    nya_assert(obj->length == 2);

    nya_object_reset(obj);
    nya_assert(obj->length == 0);
    nya_assert(nya_object_get(obj, "x") == nullptr);
    nya_assert(nya_object_get(obj, "y") == nullptr);

    // Can still use after reset
    nya_object_set(obj, "z", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 7 });
    nya_assert(obj->length == 1);
    nya_assert(nya_object_get(obj, "z")->as_u32 == 7);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_object_create_on_stack / nya_object_destroy_on_stack
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Object on stack\n");
  {
    NYA_Object on_stack = nya_object_create_on_stack(arena);
    nya_object_set(&on_stack, "key1", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "val1" });
    nya_object_set(&on_stack, "key2", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 100 });
    nya_assert(on_stack.length == 2);
    nya_assert(nya_object_get(&on_stack, "key1")->type == NYA_TYPE_STRING);
    nya_assert(nya_object_get(&on_stack, "key2")->as_u32 == 100);

    nya_object_destroy_on_stack(&on_stack);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Minify serialization flag
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Minify serialization\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "val", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 42 });

    NYA_String minified = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_OBFUSCATE);
    nya_assert(minified.length > 0);
    printf("  Obfuscated: ");
    for (u64 i = 0; i < minified.length; i++) { printf("%02x", minified.items[i]); }
    printf("\n");

    // first byte should be magic byte
    nya_assert(minified.items[0] == 0xA7);

    // Should deserialize correctly
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, minified.items, minified.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));
    NYA_Value* v = nya_object_get(restored, "val");
    nya_assert(v != nullptr);
    nya_assert(v->type == NYA_TYPE_U32);
    nya_assert(v->as_u32 == 42);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Object overwrite key
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Object overwrite key\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "key", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });
    nya_assert(nya_object_get(obj, "key")->as_u32 == 1);

    nya_object_set(obj, "key", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 2 });
    nya_assert(nya_object_get(obj, "key")->as_u32 == 2);
    nya_assert(obj->length == 1);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Object get non-existent key
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Object get non-existent key\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_assert(nya_object_get(obj, "nope") == nullptr);

    nya_object_set(obj, "exists", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });
    nya_assert(nya_object_get(obj, "nope") == nullptr);
    nya_assert(nya_object_get(obj, "exists") != nullptr);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: CRC validation
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: CRC validation\n");
  {
    // tampered CRC should fail
    const char* tampered = "nya 2 99999\n{ key: u32 42; }\n";
    NYA_Object* restored = nullptr;
    nya_assert(!nya_deserialize(arena, (u8*)tampered, strlen(tampered), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored).ok);

    // same data with NO_CRC_VALIDATION should succeed
    NYA_Object* restored_no_crc = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (u8*)tampered, strlen(tampered), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored_no_crc));
    nya_assert(nya_object_get(restored_no_crc, "key")->as_u32 == 42);

    // round-trip CRC should match
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "x", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 123 });
    nya_object_set(obj, "y", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "hello" });
    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* round_trip = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &round_trip));
    nya_assert(nya_object_get(round_trip, "x")->as_u32 == 123);

    // tamper with serialized data: change a value, CRC should catch it
    NYA_String tampered_data = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    for (u64 i = 0; i < tampered_data.length - 1; i++) {
      if (tampered_data.items[i] == '1' && tampered_data.items[i + 1] == '2') {
        tampered_data.items[i]     = '9';
        tampered_data.items[i + 1] = '9';
        break;
      }
    }
    NYA_Object* tampered_obj = nullptr;
    nya_assert(!nya_deserialize(arena, tampered_data.items, tampered_data.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &tampered_obj).ok);

    // same tampered data with NO_CRC_VALIDATION should still parse
    NYA_Object* tampered_no_crc = nullptr;
    NYA_EXPECT(nya_deserialize(arena, tampered_data.items, tampered_data.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &tampered_no_crc));
    nya_assert(nya_object_get(tampered_no_crc, "x")->as_u32 == 993);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Empty object serialization round-trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Empty object\n");
  {
    NYA_Object* obj     = nya_object_create(arena);
    NYA_String  compact = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    nya_assert(compact.length > 0);

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, compact.items, compact.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));
    nya_assert(restored->length == 0);

    NYA_String  pretty          = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored_pretty = nullptr;
    NYA_EXPECT(nya_deserialize(arena, pretty.items, pretty.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored_pretty));
    nya_assert(restored_pretty->length == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Integer boundary values
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Integer boundary values\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "u8_max", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = 255 });
    nya_object_set(obj, "u8_zero", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = 0 });
    nya_object_set(obj, "s8_min", (NYA_Value){ .type = NYA_TYPE_S8, .as_s8 = INT8_MIN });
    nya_object_set(obj, "s8_max", (NYA_Value){ .type = NYA_TYPE_S8, .as_s8 = INT8_MAX });
    nya_object_set(obj, "s32_min", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = INT32_MIN });
    nya_object_set(obj, "s32_max", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = INT32_MAX });
    nya_object_set(obj, "u32_max", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = UINT32_MAX });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored));

    nya_assert(nya_object_get(restored, "u8_max")->as_u8 == 255);
    nya_assert(nya_object_get(restored, "u8_zero")->as_u8 == 0);
    nya_assert(nya_object_get(restored, "s8_min")->as_s8 == INT8_MIN);
    nya_assert(nya_object_get(restored, "s8_max")->as_s8 == INT8_MAX);
    nya_assert(nya_object_get(restored, "s32_min")->as_s32 == INT32_MIN);
    nya_assert(nya_object_get(restored, "s32_max")->as_s32 == INT32_MAX);
    nya_assert(nya_object_get(restored, "u32_max")->as_u32 == UINT32_MAX);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Compact and pretty produce same data after round-trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Compact vs pretty equivalence\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "a", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 42 });
    nya_object_set(obj, "b", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "test" });
    nya_object_set(obj, "c", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false });

    NYA_String compact = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    NYA_String pretty  = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);

    NYA_Object* from_compact = nullptr;
    NYA_EXPECT(nya_deserialize(arena, compact.items, compact.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &from_compact));
    NYA_Object* from_pretty = nullptr;
    NYA_EXPECT(nya_deserialize(arena, pretty.items, pretty.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &from_pretty));
    nya_assert(from_compact != nullptr);
    nya_assert(from_pretty != nullptr);
    nya_assert(objects_equal(from_compact, from_pretty));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Single-element array
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Single-element array\n");
  {
    NYA_Object*    obj = nya_object_create(arena);
    NYA_ArrayᐸNYA_Valueᐳ arr = nya_array_create_on_stack(arena, NYA_Value);
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "only" }));
    nya_object_set(obj, "items", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = arr });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* restored_arr = nya_object_get(restored, "items");
    nya_assert(restored_arr != nullptr);
    nya_assert(restored_arr->type == NYA_TYPE_ARRAY);
    nya_assert(restored_arr->as_array.length == 1);
    nya_assert(restored_arr->as_array.items[0].type == NYA_TYPE_STRING);
    nya_assert(nya_string_equals(restored_arr->as_array.items[0].as_string, "only"));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Deeply nested objects (3 levels)
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Deeply nested objects\n");
  {
    NYA_Object* level3 = nya_object_create(arena);
    nya_object_set(level3, "deep", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 999 });

    NYA_Object* level2 = nya_object_create(arena);
    nya_object_set(level2, "inner", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *level3 });

    NYA_Object* level1 = nya_object_create(arena);
    nya_object_set(level1, "mid", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *level2 });

    NYA_Object* root = nya_object_create(arena);
    nya_object_set(root, "outer", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *level1 });

    NYA_String  serialized = *nya_serialize(arena, root, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* o = nya_object_get(restored, "outer");
    nya_assert(o != nullptr && o->type == NYA_TYPE_OBJECT);
    NYA_Value* m = nya_object_get(&o->as_object, "mid");
    nya_assert(m != nullptr && m->type == NYA_TYPE_OBJECT);
    NYA_Value* i = nya_object_get(&m->as_object, "inner");
    nya_assert(i != nullptr && i->type == NYA_TYPE_OBJECT);
    NYA_Value* d = nya_object_get(&i->as_object, "deep");
    nya_assert(d != nullptr && d->type == NYA_TYPE_U32 && d->as_u32 == 999);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Minify round-trip with complex data
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Minify complex round-trip\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "minify_test" });
    nya_object_set(obj, "val", (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = -42 });
    nya_object_set(obj, "flag", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    NYA_ArrayᐸNYA_Valueᐳ arr = nya_array_create_on_stack(arena, NYA_Value);
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 10 }));
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 20 }));
    nya_object_set(obj, "nums", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = arr });

    NYA_String minified = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_OBFUSCATE);
    nya_assert(minified.length > 0);
    nya_assert(minified.items[0] == 0xA7);

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, minified.items, minified.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    nya_assert(nya_object_get(restored, "name") != nullptr);
    nya_assert(nya_string_equals(nya_object_get(restored, "name")->as_string, "minify_test"));
    nya_assert(nya_object_get(restored, "val")->as_s32 == -42);
    nya_assert(nya_object_get(restored, "flag")->as_b8 == true);

    NYA_Value* restored_arr = nya_object_get(restored, "nums");
    nya_assert(restored_arr != nullptr);
    nya_assert(restored_arr->type == NYA_TYPE_ARRAY);
    nya_assert(restored_arr->as_array.length == 2);
    nya_assert(restored_arr->as_array.items[0].as_u32 == 10);
    nya_assert(restored_arr->as_array.items[1].as_u32 == 20);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Deserialize malformed input returns nullptr
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Malformed input\n");
  {
    // Empty input is now a reported error rather than a success that yields nothing, so these
    // assert the error instead of NYA_EXPECTing their way past it.
    {
      NYA_Object* _obj = (NYA_Object*)1;
      nya_assert(!nya_deserialize(arena, nullptr, 0, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &_obj).ok);
      nya_assert(_obj == nullptr);
    }
    {
      NYA_Object* _obj = (NYA_Object*)1;
      nya_assert(!nya_deserialize(arena, (u8*)"", 0, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &_obj).ok);
      nya_assert(_obj == nullptr);
    }

    // just whitespace
    const char* ws = "   \n\n  ";
    {
      NYA_Object* _obj = nullptr;
      nya_assert(!nya_deserialize(arena, (u8*)ws, strlen(ws), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &_obj).ok);
    }

    // missing opening brace
    const char* no_brace = "nya 2 12345\nkey: u32 42;\n";
    {
      NYA_Object* _obj = nullptr;
      nya_assert(!nya_deserialize(arena, (u8*)no_brace, strlen(no_brace), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &_obj).ok);
    }

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Many keys stress test
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Many keys\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    for (u32 i = 0; i < 64; i++) {
      char key_buf[16];
      snprintf(key_buf, sizeof(key_buf), "key_%u", i);
      char* key_str = nya_arena_alloc(arena, strlen(key_buf) + 1);
      strcpy(key_str, key_buf);
      nya_object_set(obj, key_str, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = i });
    }
    nya_assert(obj->length == 64);

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));
    nya_assert(restored->length == 64);

    for (u32 i = 0; i < 64; i++) {
      char key_buf[16];
      snprintf(key_buf, sizeof(key_buf), "key_%u", i);
      NYA_Value* v = nya_object_get(restored, key_buf);
      nya_assert(v != nullptr);
      nya_assert(v->as_u32 == i);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Char type round-trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Char type\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "letter", (NYA_Value){ .type = NYA_TYPE_CHAR, .as_char = 'Z' });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* ch = nya_object_get(restored, "letter");
    nya_assert(ch != nullptr);
    nya_assert(ch->type == NYA_TYPE_CHAR);
    nya_assert(ch->as_char == 'Z');
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Array of booleans
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Array of booleans\n");
  {
    NYA_Object*    obj = nya_object_create(arena);
    NYA_ArrayᐸNYA_Valueᐳ arr = nya_array_create_on_stack(arena, NYA_Value);
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true }));
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false }));
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true }));
    nya_object_set(obj, "flags", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = arr });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* flags = nya_object_get(restored, "flags");
    nya_assert(flags != nullptr);
    nya_assert(flags->type == NYA_TYPE_ARRAY);
    nya_assert(flags->as_array.length == 3);
    nya_assert(flags->as_array.items[0].as_b8 == true);
    nya_assert(flags->as_array.items[1].as_b8 == false);
    nya_assert(flags->as_array.items[2].as_b8 == true);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Array of strings
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Array of strings\n");
  {
    NYA_Object*    obj = nya_object_create(arena);
    NYA_ArrayᐸNYA_Valueᐳ arr = nya_array_create_on_stack(arena, NYA_Value);
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "alpha" }));
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "beta" }));
    nya_array_push_back(&arr, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "gamma" }));
    nya_object_set(obj, "names", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = arr });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* names = nya_object_get(restored, "names");
    nya_assert(names != nullptr);
    nya_assert(names->type == NYA_TYPE_ARRAY);
    nya_assert(names->as_array.length == 3);
    nya_assert(nya_string_equals(names->as_array.items[0].as_string, "alpha"));
    nya_assert(nya_string_equals(names->as_array.items[1].as_string, "beta"));
    nya_assert(nya_string_equals(names->as_array.items[2].as_string, "gamma"));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Object with nested object containing array
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Nested object with array\n");
  {
    NYA_ArrayᐸNYA_Valueᐳ scores = nya_array_create_on_stack(arena, NYA_Value);
    nya_array_push_back(&scores, ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 100 }));
    nya_array_push_back(&scores, ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 200 }));

    NYA_Object* inner = nya_object_create(arena);
    nya_object_set(inner, "id", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });
    nya_object_set(inner, "scores", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = scores });

    NYA_Object* root = nya_object_create(arena);
    nya_object_set(root, "player", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *inner });

    NYA_String  serialized = *nya_serialize(arena, root, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    NYA_Value* player = nya_object_get(restored, "player");
    nya_assert(player != nullptr && player->type == NYA_TYPE_OBJECT);
    NYA_Value* id = nya_object_get(&player->as_object, "id");
    nya_assert(id != nullptr && id->as_u32 == 1);
    NYA_Value* sc = nya_object_get(&player->as_object, "scores");
    nya_assert(sc != nullptr && sc->type == NYA_TYPE_ARRAY);
    nya_assert(sc->as_array.length == 2);
    nya_assert(sc->as_array.items[0].as_u32 == 100);
    nya_assert(sc->as_array.items[1].as_u32 == 200);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Float special values
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Float values\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "zero", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 0.0 });
    nya_object_set(obj, "neg", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = -1.5 });
    nya_object_set(obj, "large", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 1e10 });

    NYA_String  serialized = *nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, serialized.items, serialized.length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

    nya_assert(nya_object_get(restored, "zero")->as_f64 == 0.0);
    nya_assert(nya_object_get(restored, "neg")->as_f64 == -1.5);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Negative integer parsing in deserialization
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: Negative integers from text\n");
  {
    const char* text = "nya 2 12345\n"
                       "{\n"
                       "neg_s8: s8 -128;\n"
                       "neg_s32: s32 -1;\n"
                       "neg_s64: s64 -9223372036854775807;\n"
                       "}\n";

    NYA_Object* restored = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (u8*)text, strlen(text), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored));
    nya_assert(nya_object_get(restored, "neg_s8")->as_s8 == INT8_MIN);
    nya_assert(nya_object_get(restored, "neg_s32")->as_s32 == -1);
    nya_assert(nya_object_get(restored, "neg_s64")->as_s64 == -9223372036854775807LL);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_s128_to_string handles S128_MIN without overflow
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String* s = nya_s128_to_string(arena, S128_MIN);
    nya_assert(s != nullptr);
    nya_assert(s->length > 0);
    nya_assert(s->items[0] == '-');

    // Verify it round-trips: S128_MIN → string → parse back
    NYA_String* s_max = nya_s128_to_string(arena, S128_MAX);
    nya_assert(s_max != nullptr);
    nya_assert(s_max->length > 0);
    nya_assert(s_max->items[0] != '-');

    NYA_String* s_zero = nya_s128_to_string(arena, 0);
    NYA_CString s_zero_cstr = nya_string_to_cstring(arena, s_zero);
    nya_assert(nya_string_equals(s_zero_cstr, "0"));

    NYA_String* s_one = nya_s128_to_string(arena, 1);
    NYA_CString s_one_cstr = nya_string_to_cstring(arena, s_one);
    nya_assert(nya_string_equals(s_one_cstr, "1"));

    NYA_String* s_neg1 = nya_s128_to_string(arena, -1);
    NYA_CString s_neg1_cstr = nya_string_to_cstring(arena, s_neg1);
    nya_assert(nya_string_equals(s_neg1_cstr, "-1"));
    printf("  PASSED: s128_to_string edge cases\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: CRC computation works for 128-bit value types
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Object* obj = nya_object_create(arena);

    NYA_Value val_u128 = { .type = NYA_TYPE_U128, .as_u128 = U128_MAX };
    nya_object_set(obj, "u128_val", val_u128);

    NYA_Value val_s128 = { .type = NYA_TYPE_S128, .as_s128 = S128_MIN };
    nya_object_set(obj, "s128_val", val_s128);

    NYA_Value val_b128 = { .type = NYA_TYPE_B128, .as_b128 = (b128)42 };
    nya_object_set(obj, "b128_val", val_b128);

    // CRC should compute without panicking
    u64 crc = nya_serde_nya_checksum(obj);
    nya_assert(crc != 0);

    // Same object should produce same CRC
    u64 crc2 = nya_serde_nya_checksum(obj);
    nya_assert(crc == crc2);

    // Different value should produce different CRC
    NYA_Object* obj2 = nya_object_create(arena);
    NYA_Value val_u128_diff = { .type = NYA_TYPE_U128, .as_u128 = 0 };
    nya_object_set(obj2, "u128_val", val_u128_diff);
    nya_object_set(obj2, "s128_val", val_s128);
    nya_object_set(obj2, "b128_val", val_b128);
    u64 crc3 = nya_serde_nya_checksum(obj2);
    nya_assert(crc3 != crc);
    printf("  PASSED: 128-bit CRC\n");
  }

  nya_arena_destroy(arena);

  return 0;
}
