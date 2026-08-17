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
    nya_assert(r.ok);
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_integrity_verify_file rejects what it cannot check
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(!nya_integrity_verify_file("./_no_such_file_at_all"), "a missing file cannot be valid");

    NYA_ConstCString path = "./_test_integrity_no_sentinel.bin";
    FILE*            file = fopen(path, "wb");
    nya_assert(file != nullptr);
    u8 filler[256];
    memset(filler, 0xAA, sizeof(filler));
    (void)fwrite(filler, 1, sizeof(filler), file);
    (void)fclose(file);

    nya_assert(!nya_integrity_verify_file(path), "a file with no sentinel cannot be valid");
    (void)remove(path);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: _nya_integrity_pe_regions rejects an e_lfanew that would wrap its own bounds check
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // e_lfanew is read straight out of the file, so it is whatever a corrupt or hostile binary says
    // it is. Computed in u32, `pe_offset + 24` wraps for anything this large and the bounds check
    // passes, after which the PE magic is read from far past the end of the buffer.
    u8 image[0x100];
    memset(image, 0, sizeof(image));
    image[0] = 'M';
    image[1] = 'Z';

    const u32 wrapping[] = { 0xFFFFFFFFU, 0xFFFFFFE8U, 0xFFFFFF00U };
    for (u64 index = 0; index < sizeof(wrapping) / sizeof(wrapping[0]); index++) {
      memcpy(&image[0x3C], &wrapping[index], sizeof(u32));

      u64 hashed_len = 0, checksum_offset = 0, security_offset = 0;
      b8  is_pe      = _nya_integrity_pe_regions(image, sizeof(image), &hashed_len, &checksum_offset, &security_offset);
      nya_assert(!is_pe, "e_lfanew of 0x%X is past the end of the buffer and must be rejected", wrapping[index]);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a stamped PE stays valid once a signature is appended to it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * A minimal PE, only as real as the fields the hashing looks at: the e_lfanew pointer, the PE
     * magic, one section header, and a PE32+ optional header long enough to hold a data directory.
     *
     * The point is what Authenticode does to a file after it has been stamped — pad, append a
     * certificate, fill in the security directory, rewrite the checksum — and that none of it
     * changes what the integrity MAC covers.
     */
    const u64 pe_offset       = 0x80;
    const u64 optional_offset = pe_offset + 24;
    const u64 optional_size   = 240;
    const u64 section_offset  = optional_offset + optional_size;
    const u64 section_data    = 0x400;
    const u64 section_size    = 0x200;
    const u64 file_size       = section_data + section_size;

    u8* image = nya_arena_alloc(arena, file_size);
    memset(image, 0, file_size);

    image[0]           = 'M';
    image[1]           = 'Z';
    u32 lfanew         = (u32)pe_offset;
    memcpy(&image[0x3C], &lfanew, sizeof(u32));
    image[pe_offset]   = 'P';
    image[pe_offset + 1] = 'E';

    u16 section_count = 1;
    u16 optional_size_field = (u16)optional_size;
    memcpy(&image[pe_offset + 6], &section_count, sizeof(u16));
    memcpy(&image[pe_offset + 20], &optional_size_field, sizeof(u16));

    u16 magic = 0x20B; // PE32+
    memcpy(&image[optional_offset], &magic, sizeof(u16));

    u32 raw_size = (u32)section_size;
    u32 raw_ptr  = (u32)section_data;
    memcpy(&image[section_offset + 16], &raw_size, sizeof(u32));
    memcpy(&image[section_offset + 20], &raw_ptr, sizeof(u32));

    // The stamp itself, inside the section where the linker would have put it.
    u64 block_offset = section_data + 32;
    memcpy(&image[block_offset], SENTINEL_BEGIN, sizeof(SENTINEL_BEGIN));
    memcpy(&image[block_offset + 8 + 8], SENTINEL_END, sizeof(SENTINEL_END));
    // Something for the hash to actually cover.
    for (u64 i = 0; i < 16; i++) image[section_data + i] = (u8)(i * 7);

    NYA_ConstCString path = "./_test_integrity_fake.exe";
    FILE*            file = fopen(path, "wb");
    nya_assert(file != nullptr);
    (void)fwrite(image, 1, file_size, file);
    (void)fclose(file);

    u64 mac = 0;
    NYA_EXPECT(nya_integrity_patch(path, &mac));
    nya_assert(mac != 0, "patching produced no hash");
    nya_assert(nya_integrity_verify_file(path), "a freshly stamped binary must verify");

    // Now do to it what signing does.
    {
      NYA_String* content = nya_string_create(arena);
      NYA_EXPECT(nya_file_read(path, content));

      u64 signed_length = content->length;
      while (signed_length % 8 != 0) signed_length++; // the alignment padding signing inserts

      u64 certificate_size = 512;
      NYA_String* signed_content = nya_string_create(arena);
      for (u64 i = 0; i < signed_length; i++) {
        nya_string_push_back(signed_content, i < content->length ? content->items[i] : 0);
      }
      for (u64 i = 0; i < certificate_size; i++) nya_string_push_back(signed_content, (u8)(i ^ 0x5A));

      u32 certificate_offset = (u32)signed_length;
      u32 certificate_length = (u32)certificate_size;
      u64 security_offset    = optional_offset + 112 + (4 * 8ULL);
      memcpy(&signed_content->items[security_offset], &certificate_offset, sizeof(u32));
      memcpy(&signed_content->items[security_offset + 4], &certificate_length, sizeof(u32));

      u32 checksum = 0xDEADBEEF;
      memcpy(&signed_content->items[optional_offset + 64], &checksum, sizeof(u32));

      NYA_EXPECT(nya_file_write(path, signed_content));
    }

    nya_assert(nya_integrity_verify_file(path), "signing must not invalidate the stamp");

    // And that the check is still a check: a byte inside the section is covered.
    {
      NYA_String* content = nya_string_create(arena);
      NYA_EXPECT(nya_file_read(path, content));
      content->items[section_data + 4] ^= 0xFF;
      NYA_EXPECT(nya_file_write(path, content));
    }

    nya_assert(!nya_integrity_verify_file(path), "a patched section byte must fail the check");

    (void)remove(path);
  }

  nya_arena_destroy(arena);
  printf("PASSED: test_integrity\n");
  return 0;
}
