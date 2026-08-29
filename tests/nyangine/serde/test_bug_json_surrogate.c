/**
 * Regression test for unpaired surrogate escapes in the JSON parser (serde_json.c).
 *
 * A \\uD800..\\uDFFF escape with no partner is not a character. Encoding it as written produced a
 * three byte sequence in that range, which is CESU-8 rather than UTF-8, so one bad escape yielded a
 * string no downstream consumer could decode. Unicode prescribes U+FFFD REPLACEMENT CHARACTER, and
 * that is what is asserted here.
 *
 * Properly paired surrogates must still combine, which the last case covers — the substitution must
 * not be applied before the pairing gets its chance.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** U+FFFD in UTF-8. */
#define REPLACEMENT "\xEF\xBF\xBD"

static NYA_ConstCString parse_one_string(NYA_Arena* arena, NYA_ConstCString document, NYA_CString key) {
  NYA_Object* object = nullptr;
  NYA_Error   result = nya_serde_json_deserialize(arena, (const u8*)document, strlen(document), 0, &object);
  nya_assert(result.ok, "could not parse %s: %s", document, (NYA_ConstCString)result.message);

  NYA_Value* value = nya_object_get(object, key);
  nya_assert(value != nullptr && value->type == NYA_TYPE_STRING);

  return value->as_string;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_json_surrogate");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a high surrogate with nothing after it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString text = parse_one_string(arena, "{\"s\":\"a\\uD800b\"}", "s");
    nya_assert(strcmp(text, "a" REPLACEMENT "b") == 0, "got '%s'", text);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a lone low surrogate
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString text = parse_one_string(arena, "{\"s\":\"a\\uDC00b\"}", "s");
    nya_assert(strcmp(text, "a" REPLACEMENT "b") == 0, "got '%s'", text);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a high surrogate followed by an escape that is not a low one
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString text = parse_one_string(arena, "{\"s\":\"\\uD800\\u0041\"}", "s");
    nya_assert(strcmp(text, REPLACEMENT "A") == 0, "got '%s'", text);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a real pair still combines
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // U+1F600 GRINNING FACE.
    NYA_ConstCString text = parse_one_string(arena, "{\"s\":\"\\uD83D\\uDE00\"}", "s");
    nya_assert(strcmp(text, "\xF0\x9F\x98\x80") == 0, "a valid surrogate pair must still combine, got '%s'", text);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: ordinary BMP escapes are untouched
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ConstCString text = parse_one_string(arena, "{\"s\":\"\\u00E4\\u20AC\"}", "s");
    nya_assert(strcmp(text, "\xC3\xA4\xE2\x82\xAC") == 0, "got '%s'", text);
  }

  nya_arena_destroy(arena);

  nya_log_info("PASSED: unpaired surrogates become U+FFFD and valid pairs still combine");
  return 0;
}
