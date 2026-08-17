/**
 * The serde dispatch layer, the JSON format, and format detection.
 *
 * test_object covers the nya format through nya_serialize/nya_deserialize already. Everything here
 * is what that misses: the JSON reader and writer, which had no coverage at all, and the format
 * guesser that decides which of the two a caller gets.
 *
 * Round trips through JSON are lossy on purpose — the format has one number type and no type
 * annotations — so the assertions below expect the documented mapping in serde_json.h rather than
 * the types that went in.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Round trips `object` through `format` and hands back what came out. */
static NYA_Object* roundtrip(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFormat format, NYA_SerdeFlags flags) {
  NYA_String* text     = nya_serialize(arena, object, format, flags);
  NYA_Object* restored = nullptr;
  NYA_EXPECT(nya_deserialize(arena, text->items, text->length, format, flags, &restored));
  nya_assert(restored != nullptr);
  return restored;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_serde");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_serde_detect_format
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_serde_detect_format\n");
  {
    NYA_ConstCString json_doc  = "{ \"a\": 1 }";
    NYA_ConstCString json_ws   = "  \n\t { \"a\": 1 }";
    NYA_ConstCString nya_doc   = "nya 2 12345\n{ a: u32 1; }\n";
    const u8         obfusc[]  = { 0xA7, 0x00, 0x01 };
    NYA_ConstCString garbage   = "not either of them";

    nya_assert(nya_serde_detect_format((const u8*)json_doc, strlen(json_doc)) == NYA_SERDE_FORMAT_JSON);
    nya_assert(nya_serde_detect_format((const u8*)json_ws, strlen(json_ws)) == NYA_SERDE_FORMAT_JSON);
    nya_assert(nya_serde_detect_format((const u8*)nya_doc, strlen(nya_doc)) == NYA_SERDE_FORMAT_NYA);

    // The obfuscated nya magic is not valid at the start of any text format, so it is decisive.
    nya_assert(nya_serde_detect_format(obfusc, sizeof(obfusc)) == NYA_SERDE_FORMAT_NYA);

    // Unknown is reported as COUNT rather than defaulting to a format and failing later.
    nya_assert(nya_serde_detect_format((const u8*)garbage, strlen(garbage)) == NYA_SERDE_FORMAT_COUNT);
    nya_assert(nya_serde_detect_format(nullptr, 0) == NYA_SERDE_FORMAT_COUNT);
    nya_assert(nya_serde_detect_format((const u8*)"", 0) == NYA_SERDE_FORMAT_COUNT);
    nya_assert(nya_serde_detect_format((const u8*)"   ", 3) == NYA_SERDE_FORMAT_COUNT);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSON round trip, with the documented lossy type mapping
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSON round trip\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "flag", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
    nya_object_set(obj, "count", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 42 });
    nya_object_set(obj, "ratio", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 0.5 });
    nya_object_set(obj, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "nyangine" });
    nya_object_set(obj, "nothing", (NYA_Value){ .type = NYA_TYPE_NULL });

    NYA_Object* back = roundtrip(arena, obj, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);

    // true/false comes back as b8, which is the one integer-ish type JSON keeps exactly.
    nya_assert(nya_object_get(back, "flag")->type == NYA_TYPE_B8);
    nya_assert(nya_object_get(back, "flag")->as_b8 == true);

    // A u32 written as a number cannot come back a u32: JSON has one number type.
    nya_assert(nya_object_get(back, "count")->type == NYA_TYPE_S64);
    nya_assert(nya_object_get(back, "count")->as_s64 == 42);

    nya_assert(nya_object_get(back, "ratio")->type == NYA_TYPE_F64);
    nya_assert(nya_object_get(back, "ratio")->as_f64 == 0.5);

    nya_assert(nya_object_get(back, "name")->type == NYA_TYPE_STRING);
    nya_assert(strcmp(nya_object_get(back, "name")->as_string, "nyangine") == 0);

    nya_assert(nya_object_get(back, "nothing")->type == NYA_TYPE_NULL);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSON pretty and compact carry the same data
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSON pretty vs compact\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "a", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1 });
    nya_object_set(obj, "b", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "two" });

    NYA_String* compact = nya_serialize(arena, obj, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    NYA_String* pretty  = nya_serialize(arena, obj, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);

    nya_assert(compact->length > 0);
    nya_assert(pretty->length > compact->length);   // pretty is the one with whitespace in it
    nya_assert(nya_string_contains(pretty, "\n"));
    nya_assert(!nya_string_contains(compact, "\n"));

    NYA_Object* from_compact = nullptr;
    NYA_Object* from_pretty  = nullptr;
    NYA_EXPECT(nya_deserialize(arena, compact->items, compact->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &from_compact));
    NYA_EXPECT(nya_deserialize(arena, pretty->items, pretty->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &from_pretty));

    nya_assert(nya_object_get(from_compact, "a")->as_s64 == 1);
    nya_assert(nya_object_get(from_pretty, "a")->as_s64 == 1);
    nya_assert(strcmp(nya_object_get(from_pretty, "b")->as_string, "two") == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSON nesting, objects and arrays
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSON nesting\n");
  {
    NYA_Object* inner = nya_object_create(arena);
    nya_object_set(inner, "depth", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 2 });

    NYA_ArrayᐸNYA_Valueᐳ* items = nya_array_create(arena, NYA_Value);
    nya_array_push_back(items, ((NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 10 }));
    nya_array_push_back(items, ((NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 20 }));

    NYA_Object* outer = nya_object_create(arena);
    nya_object_set(outer, "inner", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *inner });
    nya_object_set(outer, "items", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *items });

    NYA_Object* back = roundtrip(arena, outer, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);

    NYA_Value* got_inner = nya_object_get(back, "inner");
    nya_assert(got_inner != nullptr);
    nya_assert(got_inner->type == NYA_TYPE_OBJECT);
    nya_assert(nya_object_get(&got_inner->as_object, "depth")->as_s64 == 2);

    NYA_Value* got_items = nya_object_get(back, "items");
    nya_assert(got_items != nullptr);
    nya_assert(got_items->type == NYA_TYPE_ARRAY);
    nya_assert(got_items->as_array.length == 2);
    nya_assert(got_items->as_array.items[0].as_s64 == 10);
    nya_assert(got_items->as_array.items[1].as_s64 == 20);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSON string escaping survives a round trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSON escaping\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "quote", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "he said \"hi\"" });
    nya_object_set(obj, "slash", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "a\\b" });
    nya_object_set(obj, "lines", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "one\ntwo\tthree" });

    NYA_Object* back = roundtrip(arena, obj, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);

    nya_assert(strcmp(nya_object_get(back, "quote")->as_string, "he said \"hi\"") == 0);
    nya_assert(strcmp(nya_object_get(back, "slash")->as_string, "a\\b") == 0);
    nya_assert(strcmp(nya_object_get(back, "lines")->as_string, "one\ntwo\tthree") == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: malformed JSON is reported, not accepted
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: malformed JSON\n");
  {
    NYA_ConstCString bad[] = {
      "",                       // nothing at all
      "   ",                    // only whitespace
      "{",                      // unterminated object
      "}",                      // no opening brace
      "{ \"a\" 1 }",            // missing colon
      "{ \"a\": }",             // missing value
      "{ \"a\": 1,, }",         // stray comma
      "{ \"a\": \"unclosed }",  // unterminated string
      "[1, 2, 3]",              // a bare array is not an object document
      "nonsense",
    };

    for (u64 i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      NYA_Object* out = (NYA_Object*)1;
      NYA_Error   e   = nya_deserialize(arena, (const u8*)bad[i], strlen(bad[i]), NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &out);
      nya_assert(!e.ok, "expected \"%s\" to be rejected", bad[i]);
      nya_assert(out == nullptr, "a rejected parse must not hand back an object (\"%s\")", bad[i]);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nesting deeper than NYA_SERDE_JSON_DEPTH_MAX fails rather than blowing the stack
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSON depth limit\n");
  {
    NYA_String* deep = nya_string_create(arena);
    for (u32 i = 0; i < NYA_SERDE_JSON_DEPTH_MAX + 16; i++) nya_string_extend(deep, "{\"a\":");
    nya_string_extend(deep, "1");
    for (u32 i = 0; i < NYA_SERDE_JSON_DEPTH_MAX + 16; i++) nya_string_extend(deep, "}");

    NYA_Object* out = (NYA_Object*)1;
    NYA_Error   e   = nya_deserialize(arena, deep->items, deep->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &out);
    nya_assert(!e.ok);
    nya_assert(out == nullptr);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty object round trips through both formats
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: empty object, both formats\n");
  {
    NYA_Object* empty = nya_object_create(arena);

    NYA_Object* via_json = roundtrip(arena, empty, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    nya_assert(via_json->length == 0);

    NYA_Object* via_nya = roundtrip(arena, empty, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    nya_assert(via_nya->length == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: detection agrees with what each serializer actually produces
  //
  // The point of the guesser is that a caller can hand it bytes off disk without being told which
  // format they are, so it has to recognise this build's own output.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: detection agrees with serialization\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "k", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 7 });

    NYA_SerdeFlags variants[] = { NYA_SERDE_NONE, NYA_SERDE_PRETTY };

    for (u64 i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
      NYA_String* as_json = nya_serialize(arena, obj, NYA_SERDE_FORMAT_JSON, variants[i]);
      nya_assert(nya_serde_detect_format(as_json->items, as_json->length) == NYA_SERDE_FORMAT_JSON);

      NYA_String* as_nya = nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, variants[i]);
      nya_assert(nya_serde_detect_format(as_nya->items, as_nya->length) == NYA_SERDE_FORMAT_NYA);
    }

    // Obfuscated output too, which is the branch the leading magic byte exists for.
    NYA_String* obfuscated = nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_OBFUSCATE);
    nya_assert(nya_serde_detect_format(obfuscated->items, obfuscated->length) == NYA_SERDE_FORMAT_NYA);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSONC accepts comments
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSONC comments\n");
  {
    NYA_ConstCString documents[] = {
      "// leading line comment\n{ \"a\": 1 }",
      "/* leading block */ { \"a\": 1 }",
      "{ \"a\": 1 } // trailing\n",
      "{ \"a\": /* inline */ 1 }",
      "{\n  // about a\n  \"a\": 1\n}",
      "{ \"a\": 1 /* after the value */ }",
      "/* one */ /* two */ { \"a\": 1 }",
      "{ \"a\": 1 }\n/* trailing block */",
      "/* multi\n   line\n   block */ { \"a\": 1 }",
    };

    for (u64 i = 0; i < sizeof(documents) / sizeof(documents[0]); i++) {
      NYA_Object* out = nullptr;
      NYA_Error   e   = nya_deserialize(arena, (const u8*)documents[i], strlen(documents[i]), NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &out);
      nya_assert(e.ok, "JSONC rejected \"%s\": %s", documents[i], (const char*)e.message);
      nya_assert(out != nullptr);
      nya_assert(nya_object_get(out, "a") != nullptr, "the comment ate the data in \"%s\"", documents[i]);
      nya_assert(nya_object_get(out, "a")->as_s64 == 1);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSONC accepts one trailing comma
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSONC trailing commas\n");
  {
    NYA_Object* out = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (const u8*)"{ \"a\": 1, }", 11, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &out));
    nya_assert(out->length == 1);
    nya_assert(nya_object_get(out, "a")->as_s64 == 1);

    NYA_ConstCString nested = "{ \"xs\": [1, 2, 3,], \"b\": 2, }";
    NYA_EXPECT(nya_deserialize(arena, (const u8*)nested, strlen(nested), NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &out));
    nya_assert(nya_object_get(out, "xs")->as_array.length == 3, "the trailing comma added a phantom element");
    nya_assert(nya_object_get(out, "b")->as_s64 == 2);

    // One comma, not any number of them, and not a comma standing in for a value.
    NYA_ConstCString still_bad[] = { "{ \"a\": 1,, }", "{ , }", "[1,,2]", "{ \"a\": , }" };
    for (u64 i = 0; i < sizeof(still_bad) / sizeof(still_bad[0]); i++) {
      NYA_Object* bad = (NYA_Object*)1;
      NYA_Error   e   = nya_deserialize(arena, (const u8*)still_bad[i], strlen(still_bad[i]), NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &bad);
      nya_assert(!e.ok, "JSONC accepted \"%s\"", still_bad[i]);
      nya_assert(bad == nullptr);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: strict JSON is unchanged by any of this
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSON stays strict\n");
  {
    NYA_ConstCString rejected_by_json[] = {
      "// comment\n{ \"a\": 1 }",
      "/* comment */ { \"a\": 1 }",
      "{ \"a\": 1 } // trailing\n",
      "{ \"a\": /* inline */ 1 }",
      "{ \"a\": 1, }",
      "[1, 2,]",
    };

    for (u64 i = 0; i < sizeof(rejected_by_json) / sizeof(rejected_by_json[0]); i++) {
      NYA_Object* out = (NYA_Object*)1;
      NYA_Error   e   = nya_deserialize(arena, (const u8*)rejected_by_json[i], strlen(rejected_by_json[i]), NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &out);
      nya_assert(!e.ok, "strict JSON accepted \"%s\"", rejected_by_json[i]);
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: JSONC is a superset, and writes plain JSON
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSONC superset and output\n");
  {
    NYA_Object* obj = nya_object_create(arena);
    nya_object_set(obj, "n", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 5 });
    nya_object_set(obj, "s", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "text" });

    // Writing JSONC produces exactly what writing JSON does: no comments invented, no trailing comma.
    NYA_String* as_json  = nya_serialize(arena, obj, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);
    NYA_String* as_jsonc = nya_serialize(arena, obj, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_PRETTY);
    nya_assert(nya_string_equals(as_json, as_jsonc), "JSONC output differs from JSON output");
    nya_assert(!nya_string_contains(as_jsonc, "//"));

    // So what JSONC writes, strict JSON reads back.
    NYA_Object* back = nullptr;
    NYA_EXPECT(nya_deserialize(arena, as_jsonc->items, as_jsonc->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &back));
    nya_assert(nya_object_get(back, "n")->as_s64 == 5);

    // And everything strict JSON reads, JSONC reads identically.
    NYA_Object* via_jsonc = nullptr;
    NYA_EXPECT(nya_deserialize(arena, as_json->items, as_json->length, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &via_jsonc));
    nya_assert(nya_object_get(via_jsonc, "n")->as_s64 == 5);
    nya_assert(strcmp(nya_object_get(via_jsonc, "s")->as_string, "text") == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a slash that is not a comment stays data
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: slashes inside strings\n");
  {
    NYA_ConstCString document = "{ \"url\": \"http://example.com/a\", \"path\": \"a/*b*/c\" }";
    NYA_Object*      out      = nullptr;
    NYA_EXPECT(nya_deserialize(arena, (const u8*)document, strlen(document), NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &out));

    nya_assert(strcmp(nya_object_get(out, "url")->as_string, "http://example.com/a") == 0, "a // inside a string was treated as a comment");
    nya_assert(strcmp(nya_object_get(out, "path")->as_string, "a/*b*/c") == 0, "a block comment marker inside a string was stripped");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: detection reports JSONC only when it can tell
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: JSONC detection\n");
  {
    NYA_ConstCString leading_line  = "// note\n{ \"a\": 1 }";
    NYA_ConstCString leading_block = "/* note */ { \"a\": 1 }";
    NYA_ConstCString inner_comment = "{ // note\n \"a\": 1 }";

    nya_assert(nya_serde_detect_format((const u8*)leading_line, strlen(leading_line)) == NYA_SERDE_FORMAT_JSONC);
    nya_assert(nya_serde_detect_format((const u8*)leading_block, strlen(leading_block)) == NYA_SERDE_FORMAT_JSONC);

    // Undecidable from the opening bytes, and documented as such: this reports JSON.
    nya_assert(nya_serde_detect_format((const u8*)inner_comment, strlen(inner_comment)) == NYA_SERDE_FORMAT_JSON);

    // A bare division sign is not a comment and not a document.
    nya_assert(nya_serde_detect_format((const u8*)"/ 2", 3) == NYA_SERDE_FORMAT_COUNT);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: dispatch rejects a format it does not know
  // ─────────────────────────────────────────────────────────────────────────────""
  printf("TEST: unknown format panics\n");
  {
    NYA_Object* obj = nya_object_create(arena);

    nya_expect_crash((void)nya_serialize(arena, obj, NYA_SERDE_FORMAT_COUNT, NYA_SERDE_NONE));
    nya_assert(nya_crash_caught() != nullptr);

    NYA_Object* out = nullptr;
    nya_expect_crash((void)nya_deserialize(arena, (const u8*)"{}", 2, NYA_SERDE_FORMAT_COUNT, NYA_SERDE_NONE, &out));
    nya_assert(nya_crash_caught() != nullptr);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  printf("PASSED: test_serde\n");
  return 0;
}
