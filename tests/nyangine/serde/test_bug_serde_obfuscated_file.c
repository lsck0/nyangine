/**
 * Regression test for nya_serde_save_file truncating an obfuscated document.
 *
 * The obfuscated nya format is base64 XORed against a repeating key, so a zero byte appears wherever
 * the key matches the encoded character. nya_serde_detect_format identifies the format by its
 * leading 0xA7 for exactly that reason — the payload is binary, not text.
 *
 * The save path converted the document to a C string before writing, which made nya_file_write
 * measure it with strlen. Everything past the first zero byte was dropped, and the resulting file
 * failed to load.
 *
 * Written with enough fields that some encoded byte lands on the key: a short document may contain
 * no zero at all, which is why this loops over sizes rather than testing one.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena*       arena = nya_arena_create(.name = "test_bug_serde_obfuscated_file");
  NYA_ConstCString path  = "./test_obfuscated.nya";

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an obfuscated document survives a save and load
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: obfuscated save/load round trip\n");
  {
    b8 saw_a_zero_byte = false;

    for (u32 field_count = 1; field_count <= 40; field_count++) {
      NYA_Object* object = nya_object_create(arena);

      for (u32 i = 0; i < field_count; i++) {
        NYA_String* key = nya_string_sprintf(arena, "field_%u", i);
        nya_object_set(object, nya_string_to_cstring(arena, key), ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = (i * 37U) + 11U }));
      }

      // Whether this document actually contains a zero byte decides whether it exercises the bug.
      NYA_String* encoded = nya_serde_nya_serialize(arena, object, NYA_SERDE_OBFUSCATE);
      for (u64 i = 0; i < encoded->length; i++) {
        if (encoded->items[i] == 0) saw_a_zero_byte = true;
      }

      NYA_EXPECT(nya_serde_save_file(object, path, NYA_SERDE_OBFUSCATE), "while saving with %u fields", field_count);

      // The file on disk has to be the whole document, not its prefix.
      NYA_String* on_disk = nya_string_create(arena);
      NYA_EXPECT(nya_file_read(path, on_disk), "while reading back the saved file");

      nya_assert(
        on_disk->length == encoded->length,
        "%u fields: the file is " FMTu64 " bytes but the document is " FMTu64,
        field_count,
        on_disk->length,
        encoded->length
      );

      NYA_Object* loaded = nullptr;
      NYA_EXPECT(nya_serde_load_file(arena, path, NYA_SERDE_NONE, &loaded), "while loading back %u fields", field_count);

      for (u32 i = 0; i < field_count; i++) {
        NYA_String*      key   = nya_string_sprintf(arena, "field_%u", i);
        const NYA_Value* value = nya_object_get(loaded, nya_string_to_cstring(arena, key));

        nya_assert(value != nullptr, "%u fields: field_%u is missing after the round trip", field_count, i);
        nya_assert(value->as_u32 == (i * 37U) + 11U, "%u fields: field_%u came back wrong", field_count, i);
      }

      NYA_EXPECT(nya_filesystem_delete(path));
    }

    // If no document in the sweep contained a zero byte, the test never exercised the truncation and
    // would pass on the broken code too.
    nya_assert(saw_a_zero_byte, "no obfuscated document in the sweep contained a zero byte; this test proves nothing");
  }
  printf("  PASSED\n");

  nya_arena_destroy(arena);

  printf("PASSED: test_bug_serde_obfuscated_file\n");
  return 0;
}
