/**
 * Regression test for the unchecked u128 accumulator in the real number parser (base_types.c).
 *
 * _nya_type_accumulate_digit exists because the integer paths used to accumulate unchecked, and
 * test_bug_types_parse_overflow.c pins that fix. The decimal branch of _nya_type_try_parse_f128
 * did not get the same treatment: it accumulated `integer_part`, `fractional_part` and
 * `fractional_divisor` straight into u128, and a literal with more than 38 significant digits
 * overflowed all three.
 *
 * FLAGS_SANITIZE names unsigned-integer-overflow together with -fno-sanitize-recover=all, so under
 * a sanitized build that aborted the process. Without sanitizers it wrapped, and the wrapped value
 * was returned as the parsed number — the same silent corruption the integer fix was for.
 *
 * This is not an exotic input. nya_serde_json accepts number tokens up to 191 characters and says
 * so deliberately ("f64 saturates to infinity around 309 digits ... nothing representable is being
 * turned away"), so every literal between 39 and 191 digits reaches here from any JSON document.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_bug_types_parse_real_overflow");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a long integer part
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: 42 digit integer part\n");
  {
    // 1.23456789012345678901234567890123456789012e41, comfortably inside f64's range.
    NYA_ConstCString text  = "123456789012345678901234567890123456789012.5";
    f64              value = 0.0;

    nya_assert(nya_type_parse(NYA_TYPE_F64, (const u8*)text, strlen(text), &value), "a 42 digit real failed to parse");
    nya_assert(value > 1.2e41 && value < 1.3e41, "'%s' parsed as %.17g, which is not near 1.234e41", text, value);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a long fractional part, which overflows the divisor rather than the mantissa
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: 41 digit fractional part\n");
  {
    NYA_ConstCString text  = "0.00000000000000000000000000000000000000001";
    f64              value = 1.0;

    nya_assert(nya_type_parse(NYA_TYPE_F64, (const u8*)text, strlen(text), &value), "a 41 digit fraction failed to parse");
    nya_assert(value > 0.9e-41 && value < 1.1e-41, "'%s' parsed as %.17g, which is not near 1e-41", text, value);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the same literal through the JSON reader, which is the user facing path
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a long real in a JSON document\n");
  {
    NYA_ConstCString document = "{\"value\":123456789012345678901234567890123456789012.5}";

    NYA_Object* object = nullptr;
    NYA_Error   error  = nya_serde_json_deserialize(arena, (const u8*)document, strlen(document), 0, &object);
    nya_assert(error.ok, "JSON rejected a 42 digit real");

    const NYA_Value* value = nya_object_get(object, "value");
    nya_assert(value != nullptr, "the parsed object has no 'value'");
    nya_assert(value->type == NYA_TYPE_F64, "'value' came back as %s rather than f64", NYA_TYPE_NAME_MAP[value->type]);
    nya_assert(value->as_f64 > 1.2e41 && value->as_f64 < 1.3e41, "'value' is %.17g, which is not near 1.234e41", value->as_f64);
  }
  printf("  PASSED\n");

  nya_arena_destroy(arena);

  printf("PASSED: test_bug_types_parse_real_overflow\n");
  return 0;
}
