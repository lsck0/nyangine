/**
 * Regression test for _nya_serde_nya_parse_number silently truncating an over-long literal.
 *
 * The digits were copied into a 192 byte scratch buffer and clamped to whatever fit, so a number
 * longer than that was parsed from its prefix — a value orders of magnitude from what the document
 * said, returned as a success.
 *
 * tests/nyangine/serde/test_bug_json_number.c pins the same fix for the JSON reader, whose comment
 * spells out why clamping is the one indefensible option. This is the .nya reader, where it matters
 * more: it is the save format, and the checksum is computed over the object that was parsed, so a
 * truncated number produces a file that verifies clean and holds the wrong value.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_bug_nya_number_truncation");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a literal past the parser's scratch buffer is refused, not truncated
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: an over-long number in a .nya document\n");
  {
    // 300 digits, comfortably past the 192 byte buffer. Written as an f64 field so the type is one
    // the format actually carries.
    NYA_String* digits = nya_string_create(arena);
    for (u32 i = 0; i < 300; i++) nya_string_push_back(digits, (u8)('1' + (i % 9)));

    NYA_Object* object = nya_object_create(arena);
    nya_object_set(object, "value", ((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 1.0 }));

    NYA_String* document = nya_serde_nya_serialize(arena, object, 0);

    // Splice the long literal in where the 1.0 was written, so the rest of the document — header,
    // checksum line, field name and type — stays exactly what the writer produces.
    NYA_String* patched = nya_string_clone(arena, document);
    nya_string_replace(patched, "0x1p+0", nya_string_to_cstring(arena, digits));

    // If the writer's spelling of 1.0 ever changes, the splice above stops testing anything.
    nya_assert(!nya_string_equals(patched, document), "the splice did not change the document; the writer's number format moved");

    /*
     * NYA_SERDE_NO_CHECKSUM, so the number parser is what decides.
     *
     * Splicing a literal into a finished document invalidates the header checksum, and verification
     * happens first — so without this the document is rejected for the wrong reason and the test
     * passes whether the truncation is fixed or not. It did exactly that on the first attempt.
     */
    NYA_Object* parsed = nullptr;
    NYA_Error   error  = nya_serde_nya_deserialize(arena, patched->items, patched->length, NYA_SERDE_NO_CHECKSUM, &parsed);

    // Either answer is acceptable — refuse the literal, or carry it exactly. Silently returning a
    // different number is not, and that is what this pins.
    if (error.ok) {
      const NYA_Value* value = nya_object_get(parsed, "value");
      nya_assert(value != nullptr, "the document parsed but has no 'value'");

      // Accepted means it must be the number that was written, not its first 190 digits.
      nya_assert(
        value->as_f64 > 1e290,
        "a 300 digit literal parsed as %.6g, which is its truncated prefix rather than its value",
        value->as_f64
      );
      printf("  accepted and carried exactly\n");
    } else {
      printf("  refused: %s\n", (NYA_ConstCString)error.message);
    }
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an ordinary number still round trips
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: an ordinary number is unaffected\n");
  {
    NYA_Object* object = nya_object_create(arena);
    nya_object_set(object, "value", ((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 1234.5 }));

    NYA_String* document = nya_serde_nya_serialize(arena, object, 0);

    NYA_Object* parsed = nullptr;
    NYA_Error   error  = nya_serde_nya_deserialize(arena, document->items, document->length, 0, &parsed);
    nya_assert(error.ok, "an ordinary document failed to parse");

    const NYA_Value* value = nya_object_get(parsed, "value");
    nya_assert(value != nullptr && value->as_f64 == 1234.5, "an ordinary number did not survive the round trip");
  }
  printf("  PASSED\n");

  nya_arena_destroy(arena);

  printf("PASSED: test_bug_nya_number_truncation\n");
  return 0;
}
