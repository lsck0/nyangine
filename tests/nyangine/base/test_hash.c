/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_hash");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hash_fnv1a with data buffer
  // ─────────────────────────────────────────────────────────────────────────────
  u8 data1[] = { 0x01, 0x02, 0x03 };
  u8 data2[] = { 0x01, 0x02, 0x04 };
  u8 data3[] = { 0x01, 0x02, 0x03 };

  u64 h1 = nya_hash_fnv1a(data1, sizeof(data1));
  u64 h2 = nya_hash_fnv1a(data2, sizeof(data2));
  u64 h3 = nya_hash_fnv1a(data3, sizeof(data3));

  // Same data should produce same hash
  nya_assert(h1 == h3);

  // Different data should (usually) produce different hashes
  nya_assert(h1 != h2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hash_fnv1a with cstring
  // ─────────────────────────────────────────────────────────────────────────────
  u64 ch1 = nya_hash_fnv1a("");
  u64 ch2 = nya_hash_fnv1a("hello");
  u64 ch3 = nya_hash_fnv1a("world");
  u64 ch4 = nya_hash_fnv1a("hello");

  // Same string should produce same hash
  nya_assert(ch2 == ch4);

  // Different strings should (usually) produce different hashes
  nya_assert(ch1 != ch2);
  nya_assert(ch2 != ch3);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hash_fnv1a with NYA_String
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String s1 = *nya_string_from(arena, "test");
  NYA_String s2 = *nya_string_from(arena, "another");
  NYA_String s3 = *nya_string_from(arena, "test");

  u64 sh_str1 = nya_hash_fnv1a(s1);
  u64 sh_str2 = nya_hash_fnv1a(s2);
  u64 sh_str3 = nya_hash_fnv1a(s3);

  // Same string content should produce same hash
  nya_assert(sh_str1 == sh_str3);

  // Different strings should (usually) produce different hashes
  nya_assert(sh_str1 != sh_str2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hash_fnv1a with empty string (cstring)
  // ─────────────────────────────────────────────────────────────────────────────
  u64 empty_cstr  = nya_hash_fnv1a("");
  u64 empty_cstr2 = nya_hash_fnv1a("");

  // Empty string should produce consistent hash
  nya_assert(empty_cstr == empty_cstr2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_hash_fnv1a with empty string (NYA_String)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String empty      = *nya_string_create(arena);
  u64        empty_hash = nya_hash_fnv1a(empty);

  // Should handle empty string
  nya_assert(empty_hash == empty_cstr);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash consistency between cstring and NYA_String
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String str         = *nya_string_from(arena, "consistency");
  u64        cstr_hash   = nya_hash_fnv1a("consistency");
  u64        string_hash = nya_hash_fnv1a(str);

  // Same content should produce same hash regardless of type
  nya_assert(cstr_hash == string_hash);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash distribution test
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Arrayᐸu64ᐳ* hashes = nya_array_create(arena, u64);
  for (u32 i = 0; i < 1000; ++i) {
    NYA_String num_str = *nya_string_sprintf(arena, "test_%u", i);
    u64        h       = nya_hash_fnv1a(num_str);
    nya_array_add(hashes, h);
  }

  // Check that we don't have too many collisions
  u32 collisions = 0;
  for (u32 i = 0; i < hashes->length; ++i) {
    for (u32 j = i + 1; j < hashes->length; ++j) {
      if (hashes->items[i] == hashes->items[j]) { collisions++; }
    }
  }

  // With 1000 values and 64-bit hash, collisions should be minimal
  nya_assert(collisions == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash with binary data including null bytes
  // ─────────────────────────────────────────────────────────────────────────────
  u8 binary_data1[] = { 0x00, 0x01, 0x02, 0x03 };
  u8 binary_data2[] = { 0x00, 0x01, 0x02, 0x04 };

  u64 bh1 = nya_hash_fnv1a(binary_data1, sizeof(binary_data1));
  u64 bh2 = nya_hash_fnv1a(binary_data2, sizeof(binary_data2));

  nya_assert(bh1 != bh2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash with special characters
  // ─────────────────────────────────────────────────────────────────────────────
  u64 special1 = nya_hash_fnv1a("hello\tworld");
  u64 special2 = nya_hash_fnv1a("hello\nworld");
  u64 special3 = nya_hash_fnv1a("hello\0world");

  nya_assert(special1 != special2);
  nya_assert(special2 != special3);
  nya_assert(special1 != special3);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash with unicode content
  // ─────────────────────────────────────────────────────────────────────────────
  u64 unicode1 = nya_hash_fnv1a("hello 世界");
  u64 unicode2 = nya_hash_fnv1a("hello 世界");
  u64 unicode3 = nya_hash_fnv1a("hello 世界2");

  nya_assert(unicode1 == unicode2);
  nya_assert(unicode1 != unicode3);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash with very long string
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String long_str = *nya_string_create(arena);
  for (u32 i = 0; i < 10000; ++i) {
    NYA_String part = *nya_string_sprintf(arena, "%u", i);
    nya_string_extend(&long_str, &part);
  }

  u64 long_hash  = nya_hash_fnv1a(long_str);
  u64 long_hash2 = nya_hash_fnv1a(long_str);

  // Consistent hashing of long strings
  nya_assert(long_hash == long_hash2);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash with single character
  // ─────────────────────────────────────────────────────────────────────────────
  u64 char_a = nya_hash_fnv1a("a");
  u64 char_b = nya_hash_fnv1a("b");

  nya_assert(char_a != char_b);

  // Single character cstring vs byte array
  u8  single_byte[] = { 'a' };
  u64 byte_hash     = nya_hash_fnv1a(single_byte, 1);
  nya_assert(char_a == byte_hash);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash with different cases
  // ─────────────────────────────────────────────────────────────────────────────
  u64 upper = nya_hash_fnv1a("HELLO");
  u64 lower = nya_hash_fnv1a("hello");
  u64 mixed = nya_hash_fnv1a("Hello");

  // Different cases should produce different hashes
  nya_assert(upper != lower);
  nya_assert(mixed != lower);
  nya_assert(mixed != upper);

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}
