/**
 * Regression test for silent number truncation in _nya_serde_json_parse_number (serde_json.c).
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Well past the 192 byte buffer, and past what an s64 can hold, so it parses as f64. */
#define DIGIT_COUNT 240

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_json_number");

  // "1000...0", DIGIT_COUNT digits: 1e239.
  NYA_String* digits = nya_string_create(arena);
  nya_string_push_back(digits, '1');
  for (u32 i = 1; i < DIGIT_COUNT; i++) nya_string_push_back(digits, '0');

  NYA_String* document = nya_string_sprintf(arena, "{\"big\":" NYA_FMT_STRING "}", NYA_FMT_STRING_ARG(digits));

  NYA_Object* parsed = nullptr;
  NYA_Error   result = nya_serde_json_deserialize(arena, document->items, document->length, 0, &parsed);

  // Refusing the number is a fine outcome. Accepting it and changing it is not.
  if (!result.ok) {
    nya_log_info("PASSED: an over long number is rejected rather than truncated");
    nya_arena_destroy(arena);
    return 0;
  }

  NYA_Value* value = nya_object_get(parsed, "big");
  nya_assert(value != nullptr);
  nya_assert(value->type == NYA_TYPE_F64, "a number past s64 should arrive as f64");

  f64 expected = strtod(nya_string_to_cstring(arena, digits), nullptr);
  nya_assert(
      value->as_f64 == expected,
      "parsed %.17g but the document says %.17g — the digits were truncated to fit the parser's buffer",
      value->as_f64,
      expected
  );

  nya_arena_destroy(arena);

  nya_log_info("PASSED: a long JSON number keeps its value");
  return 0;
}
