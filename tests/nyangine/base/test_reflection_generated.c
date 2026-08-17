/**
 * The generated reflection tables, against the real types they describe.
 *
 * test_reflection.c proves the runtime with hand written tables. This proves the other half: that
 * src/build/reflection.c emits tables which agree with the structs the compiler actually laid out.
 *
 * The two failures it exists to catch are the ones no amount of runtime testing would find:
 *
 * - **A field parsed wrong.** `f32 r, g, b, a;` is one declaration and four fields, and a parser that
 *   got that wrong would produce a table that still compiles.
 * - **An offset that does not match the struct.** Emitting `nya_offsetof` is supposed to make that
 *   impossible, so this checks the guarantee rather than trusting it.
 *
 * It includes the game translation unit because that is where the generated tables live — they name
 * GNY_EntityFlags, which the engine cannot see. See the note in gnyame.h.
 **/

#include "nyangine/nyangine.h"
#include "gnyame/gnyame.h"

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_reflection_generated");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a multi-declarator line is four fields, at the right offsets
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: NYA_Color\n");
  {
    const NYA_TypeReflection* color = nya_reflect_of(NYA_Color);

    nya_assert(color->kind == NYA_REFLECT_STRUCT);
    nya_assert(color->size == sizeof(NYA_Color));
    nya_assert(color->field_count == 4, "`f32 r, g, b, a;` produced %u fields", color->field_count);

    // The offsets the generator emitted and the ones the compiler computes must be the same object,
    // which is the whole claim behind emitting nya_offsetof rather than a number.
    NYA_Color instance = { 0 };

    void* address = nullptr;
    nya_assert(nya_reflect_path(color, "b", &instance, &address) != nullptr);
    nya_assert(address == &instance.b, "the generated offset for 'b' is wrong");

    nya_assert(nya_reflect_field(color, "a")->type->primitive == NYA_TYPE_F32);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a C23 enum with an explicit underlying type, detected as flags
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: GNY_EntityFlags\n");
  {
    const NYA_TypeReflection* flags = nya_reflect_of(GNY_EntityFlags);

    nya_assert(flags->kind == NYA_REFLECT_ENUM);
    nya_assert(flags->is_bitflags, "a `1ULL << n` enum was not recognised as flags");
    nya_assert(flags->size == sizeof(GNY_EntityFlags));

    // The variant's value came from the compiler evaluating the shift, not from the generator.
    nya_assert(nya_string_equals(nya_reflect_variant_name(flags, GNY_ENTITY_FLAG_CAMERA_TARGET), "GNY_ENTITY_FLAG_CAMERA_TARGET"));

    s64 value = 0;
    nya_assert(nya_reflect_variant_value(flags, "GNY_ENTITY_FLAG_AUDIBLE", &value));
    nya_assert(value == (s64)GNY_ENTITY_FLAG_AUDIBLE, "the generated variant value disagrees with the enum");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: array extents survive as expressions, and nesting resolves
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: NYA_NetChatMessage\n");
  {
    const NYA_TypeReflection* message = nya_reflect_of(NYA_NetChatMessage);

    const NYA_ReflectField* text = nya_reflect_field(message, "text");
    nya_assert(text != nullptr);
    nya_assert(text->type->kind == NYA_REFLECT_ARRAY);

    // `char text[NYA_NET_CHAT_TEXT_MAX]`: the generator copied the macro's *name* through and the
    // compiler resolved it, so this is what says that trick works.
    nya_assert(text->type->element_count == NYA_NET_CHAT_TEXT_MAX, "the array extent came out as %u",
               text->type->element_count);
    nya_assert(text->type->element->primitive == NYA_TYPE_CHAR);

    // A struct member whose type is another annotated struct points at that struct's own table.
    const NYA_ReflectField* sender = nya_reflect_field(message, "sender");
    nya_assert(sender != nullptr);
    nya_assert(sender->type == nya_reflect_of(NYA_NetPeerId), "the nested type did not link to its own reflection");
    nya_assert(sender->type->field_count == 2);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a real struct round trips through the generated description
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: round trip\n");
  {
    NYA_NetChatMessage original = {
      .sender      = { .index = 3, .generation = 9 },
      .received_ms = 123456,
      .is_system   = true,
    };

    (void)snprintf(original.name, sizeof(original.name), "alice");
    (void)snprintf(original.text, sizeof(original.text), "hello there");

    NYA_Object* object = nya_reflect_to_object(arena, nya_reflect_of(NYA_NetChatMessage), &original);
    nya_assert(object != nullptr);

    NYA_NetChatMessage restored = { 0 };
    NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_NetChatMessage), &restored, object));

    nya_assert(nya_string_equals(restored.name, "alice"));
    nya_assert(nya_string_equals(restored.text, "hello there"));
    nya_assert(restored.received_ms == original.received_ms);
    nya_assert(restored.is_system == original.is_system);
    nya_assert(restored.sender.index == 3, "the nested struct did not round trip");
    nya_assert(restored.sender.generation == 9);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the registry
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: registry\n");
  {
    nya_assert(NYA_REFLECT_TYPE_COUNT > 0, "nothing was generated");
    nya_assert(nya_reflect_find("NYA_Color") == nya_reflect_of(NYA_Color));
    nya_assert(nya_reflect_find("does_not_exist") == nullptr);

    // Every entry is real and self consistent, which is cheap to check across the whole table.
    for (u32 i = 0; i < NYA_REFLECT_TYPE_COUNT; i++) {
      const NYA_TypeReflection* type = NYA_REFLECT_TYPES[i];

      nya_assert(type != nullptr && type->name != nullptr, "entry %u is malformed", i);
      nya_assert(type->size > 0, "'%s' has no size", type->name);
      nya_assert(nya_reflect_find(type->name) == type, "'%s' is not findable by its own name", type->name);
    }

    printf("  %u types\n", NYA_REFLECT_TYPE_COUNT);
    printf("  PASSED\n");
  }

  printf("PASSED: test_reflection_generated\n");

  return 0;
}
