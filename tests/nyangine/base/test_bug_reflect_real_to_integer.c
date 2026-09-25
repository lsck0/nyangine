/**
 * Regression test for nya_reflect_value_to_s64 casting any real to an s64. A NaN, an infinity or a
 * number past the range is undefined behaviour as a cast, and a document picks the number: the binary
 * .nya fuzzer found it with a NaN, and `{"enabled": 1e300}` reached it over HTTP the same way.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_bug_reflect_real_to_integer");
    defer      nya_arena_destroy(arena);

    printf("TEST: a real with no s64 is refused, not cast\n");
    {
        s64 integer = 0;

        nya_assert(!nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = __builtin_nanf("") }, &integer));
        nya_assert(!nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = __builtin_inf() }, &integer));
        nya_assert(!nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = -__builtin_inf() }, &integer));
        nya_assert(!nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 1.0e300 }, &integer));
        nya_assert(!nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 9223372036854775808.0 }, &integer), "2^63 is one past");

        // the edges that do exist still convert, and the truncation a hand written 3.0 relies on stays.
        nya_assert(nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = -9223372036854775808.0 }, &integer) && integer == S64_MIN);
        nya_assert(nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = 3.0F }, &integer) && integer == 3);
        nya_assert(nya_reflect_value_to_s64((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = -2.75 }, &integer) && integer == -2);
        printf("  PASSED\n");
    }

    printf("TEST: the same through a JSON body into a DTO\n");
    {
        NYA_ConstCString json = "{\"enabled\": 1e300}";

        NYA_Object* document = nullptr;
        NYA_EXPECT(nya_deserialize(arena, (const u8*)json, strlen(json), NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &document));

        // reported as a value the field cannot hold, and left alone rather than written with garbage.
        nya_assert(nya_reflect_check(nya_reflect_of(NYA_HttpAccountingDto), document, nullptr, nullptr) == 1);

        NYA_HttpAccountingDto accounting = { .enabled = false };
        (void)nya_reflect_from_object(nya_reflect_of(NYA_HttpAccountingDto), &accounting, document);
        nya_assert(accounting.enabled == false);
        printf("  PASSED\n");
    }

    printf("PASSED: test_bug_reflect_real_to_integer (0 failures)\n");
    return EXIT_SUCCESS;
}
