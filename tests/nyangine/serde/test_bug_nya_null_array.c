/**
 * Regression test for an array of nulls in the text .nya. The writer spells it `null[] [null, null]`
 * and the reader took the leading `null` for the whole value, then failed on the '[' after it, so a
 * document holding one could be written and never read back.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_bug_nya_null_array");
    defer      nya_arena_destroy(arena);

    printf("TEST: an array of nulls round trips through the text form\n");
    {
        NYA_ArrayᐸNYA_Valueᐳ* nulls = nya_array_create(arena, NYA_Value);
        nya_array_push_back(nulls, ((NYA_Value){ .type = NYA_TYPE_NULL }));
        nya_array_push_back(nulls, ((NYA_Value){ .type = NYA_TYPE_NULL }));

        NYA_Object* object = nya_object_create(arena);
        nya_object_set(object, "nulls", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *nulls });
        nya_object_set(object, "after", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 7 });

        NYA_String* text = nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);

        NYA_Object* restored = nullptr;
        NYA_EXPECT(nya_deserialize(arena, text->items, text->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &restored));

        NYA_Value* back = nya_object_get(restored, "nulls");
        nya_assert(back != nullptr && back->type == NYA_TYPE_ARRAY && back->as_array.length == 2);
        nya_assert(back->as_array.items[0].type == NYA_TYPE_NULL && back->as_array.items[1].type == NYA_TYPE_NULL);
        nya_assert(nya_object_get(restored, "after")->as_u32 == 7, "and the member after it still reads");

        // a plain null member is still a null, not the start of an array.
        NYA_ConstCString plain = "nya 2 0 { a: null; b: u8 1; }";
        NYA_EXPECT(nya_deserialize(arena, (const u8*)plain, strlen(plain), NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &restored));
        nya_assert(nya_object_get(restored, "a")->type == NYA_TYPE_NULL && nya_object_get(restored, "b")->as_u8 == 1);
        printf("  PASSED\n");
    }

    printf("PASSED: test_bug_nya_null_array (0 failures)\n");
    return EXIT_SUCCESS;
}
