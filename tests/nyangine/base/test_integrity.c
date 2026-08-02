/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_integrity");

  const u8 SENTINEL_BEGIN[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };
  const u8 SENTINEL_END[]   = { 0xBA, 0xBE, 0xCA, 0xFE, 0xDE, 0xAD, 0xBE, 0xEF };

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sentinel constants are correct
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[0] == 0xDE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[1] == 0xAD);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[2] == 0xBE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[3] == 0xEF);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[4] == 0xCA);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[5] == 0xFE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[6] == 0xBA);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_begin[7] == 0xBE);

    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[0] == 0xBA);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[1] == 0xBE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[2] == 0xCA);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[3] == 0xFE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[4] == 0xDE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[5] == 0xAD);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[6] == 0xBE);
    nya_assert(_NYA_INTEGRITY_BLOCK.sentinel_end[7] == 0xEF);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hash slot is initialized to zeros
  // ─────────────────────────────────────────────────────────────────────────────
  {
    for (u64 i = 0; i < _NYA_INTEGRITY_HASH_SIZE; i++) { nya_assert(_NYA_INTEGRITY_BLOCK.hash[i] == 0); }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: struct is packed correctly (24 bytes total)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(sizeof(NYA_IntegrityBlock) == _NYA_INTEGRITY_BLOCK_SIZE);
    nya_assert((u8*)&_NYA_INTEGRITY_BLOCK.hash == (u8*)&_NYA_INTEGRITY_BLOCK.sentinel_begin + _NYA_INTEGRITY_SENTINEL_SIZE);
    nya_assert((u8*)&_NYA_INTEGRITY_BLOCK.sentinel_end == (u8*)&_NYA_INTEGRITY_BLOCK.hash + _NYA_INTEGRITY_HASH_SIZE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sentinel block exists in this test binary
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String   binary = nya_string_create_on_stack(arena);
    NYA_Error   r      = nya_file_read("/proc/self/exe", &binary);
    nya_assert(r.error == NYA_ERROR_NONE);
    nya_assert(binary.length > 0);

    b8 found = false;
    for (u64 i = 0; i + _NYA_INTEGRITY_BLOCK_SIZE <= binary.length; i++) {
      if (memcmp(&binary.items[i], SENTINEL_BEGIN, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;
      u64 end_off = i + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE;
      if (memcmp(&binary.items[end_off], SENTINEL_END, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;
      found = true;
      break;
    }
    nya_assert(found, "Sentinel block should be present in this binary.");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_integrity_assert returns without crashing in debug builds
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_integrity_assert();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: simulated patch + verify on synthetic binary data
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // build a fake binary with some padding, sentinel block, more padding
    u64 prefix_len = 128;
    u64 suffix_len = 256;
    u64 total_len  = prefix_len + _NYA_INTEGRITY_BLOCK_SIZE + suffix_len;
    u8* fake_bin   = nya_arena_alloc(arena, total_len);

    // fill with pseudo-random data
    for (u64 i = 0; i < total_len; i++) fake_bin[i] = (u8)(i * 37 + 13);

    // place sentinels and zero hash slot
    u64 sentinel_off = prefix_len;
    memcpy(&fake_bin[sentinel_off], SENTINEL_BEGIN, _NYA_INTEGRITY_SENTINEL_SIZE);
    memset(&fake_bin[sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE], 0, _NYA_INTEGRITY_HASH_SIZE);
    memcpy(&fake_bin[sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE], SENTINEL_END, _NYA_INTEGRITY_SENTINEL_SIZE);

    // compute CRC64 with hash slot zeroed
    u64 hash_offset = sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE;
    u64 crc         = nya_crc64(fake_bin, total_len);

    // patch the hash in
    memcpy(&fake_bin[hash_offset], &crc, sizeof(u64));

    // verify: zero the hash slot in a copy, recompute, compare
    u8* verify_copy = nya_arena_alloc(arena, total_len);
    memcpy(verify_copy, fake_bin, total_len);
    memset(&verify_copy[hash_offset], 0, _NYA_INTEGRITY_HASH_SIZE);
    u64 recomputed = nya_crc64(verify_copy, total_len);

    u64 stored = 0;
    memcpy(&stored, &fake_bin[hash_offset], sizeof(u64));
    nya_assert(stored == recomputed, "Patched CRC64 should match recomputed value.");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: simulated tamper detection on synthetic binary data
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 prefix_len = 64;
    u64 suffix_len = 128;
    u64 total_len  = prefix_len + _NYA_INTEGRITY_BLOCK_SIZE + suffix_len;
    u8* fake_bin   = nya_arena_alloc(arena, total_len);

    for (u64 i = 0; i < total_len; i++) fake_bin[i] = (u8)(i * 17 + 7);

    u64 sentinel_off = prefix_len;
    memcpy(&fake_bin[sentinel_off], SENTINEL_BEGIN, _NYA_INTEGRITY_SENTINEL_SIZE);
    memset(&fake_bin[sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE], 0, _NYA_INTEGRITY_HASH_SIZE);
    memcpy(&fake_bin[sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE], SENTINEL_END, _NYA_INTEGRITY_SENTINEL_SIZE);

    u64 hash_offset = sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE;
    u64 crc         = nya_crc64(fake_bin, total_len);
    memcpy(&fake_bin[hash_offset], &crc, sizeof(u64));

    // tamper: flip a byte in the prefix
    fake_bin[10] ^= 0xFF;

    // recompute with zeroed hash slot
    u8* tampered_copy = nya_arena_alloc(arena, total_len);
    memcpy(tampered_copy, fake_bin, total_len);
    memset(&tampered_copy[hash_offset], 0, _NYA_INTEGRITY_HASH_SIZE);
    u64 tampered_crc = nya_crc64(tampered_copy, total_len);

    u64 stored = 0;
    memcpy(&stored, &fake_bin[hash_offset], sizeof(u64));
    nya_assert(stored != tampered_crc, "Tampered binary should NOT match stored CRC64.");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sentinel search finds correct offset
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 prefix_len = 200;
    u64 suffix_len = 100;
    u64 total_len  = prefix_len + _NYA_INTEGRITY_BLOCK_SIZE + suffix_len;
    u8* data       = nya_arena_alloc(arena, total_len);
    memset(data, 0x42, total_len);

    u64 sentinel_off = prefix_len;
    memcpy(&data[sentinel_off], SENTINEL_BEGIN, _NYA_INTEGRITY_SENTINEL_SIZE);
    memset(&data[sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE], 0, _NYA_INTEGRITY_HASH_SIZE);
    memcpy(&data[sentinel_off + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE], SENTINEL_END, _NYA_INTEGRITY_SENTINEL_SIZE);

    // manually search (same logic as the hook)
    u64 found_offset = 0;
    b8  found        = false;
    for (u64 i = 0; i + _NYA_INTEGRITY_BLOCK_SIZE <= total_len; i++) {
      if (memcmp(&data[i], SENTINEL_BEGIN, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;
      u64 end_off = i + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE;
      if (memcmp(&data[end_off], SENTINEL_END, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;
      found_offset = i + _NYA_INTEGRITY_SENTINEL_SIZE;
      found        = true;
      break;
    }

    nya_assert(found, "Should find sentinel in synthetic data.");
    nya_assert(found_offset == prefix_len + _NYA_INTEGRITY_SENTINEL_SIZE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: no sentinel found in data without sentinels
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 len  = 512;
    u8* data = nya_arena_alloc(arena, len);
    memset(data, 0xAA, len);

    b8 found = false;
    for (u64 i = 0; i + _NYA_INTEGRITY_BLOCK_SIZE <= len; i++) {
      if (memcmp(&data[i], SENTINEL_BEGIN, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;
      u64 end_off = i + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE;
      if (memcmp(&data[end_off], SENTINEL_END, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;
      found = true;
      break;
    }

    nya_assert(!found, "Should NOT find sentinel in random data.");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: CRC64 consistency — same data always produces same hash
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u8  data[] = "The quick brown fox jumps over the lazy dog";
    u64 crc1   = nya_crc64(data, sizeof(data) - 1);
    u64 crc2   = nya_crc64(data, sizeof(data) - 1);
    nya_assert(crc1 == crc2);
    nya_assert(crc1 != 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sizes are correct
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(_NYA_INTEGRITY_SENTINEL_SIZE == 8);
    nya_assert(_NYA_INTEGRITY_HASH_SIZE == 8);
    nya_assert(_NYA_INTEGRITY_BLOCK_SIZE == 24);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_integrity_find_sentinel returns false for missing sentinel
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u8  data[64];
    memset(data, 0xAA, sizeof(data));
    u64 offset = 0;
    b8  found  = _nya_integrity_find_sentinel(data, sizeof(data), &offset);
    nya_assert(!found, "Should not find sentinel in uniform data.");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_integrity_find_sentinel handles too-short data
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u8  data[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };
    u64 offset  = 0;
    b8  found   = _nya_integrity_find_sentinel(data, sizeof(data), &offset);
    nya_assert(!found, "Data too short to contain full block.");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_integrity_find_sentinel with empty data
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 offset = 0;
    b8  found  = _nya_integrity_find_sentinel((u8*)"", 0, &offset);
    nya_assert(!found, "Should not find sentinel in empty data.");
  }

  nya_arena_destroy(arena);
  printf("PASSED: test_integrity\n");
  return 0;
}
