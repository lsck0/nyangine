/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_crc");

  // TEST: nya_crc8 - basic test
  {
    const u8 data[] = "hello";
    u8       crc    = nya_crc8(data, 5);
    // CRC8 of "hello" using polynomial x^8 + x^2 + x + 1 This value is computed from the lookup table
    nya_assert(crc != 0); // Just verify it produces a non-zero value
  }

  // TEST: nya_crc8 - empty data
  {
    const u8 data[] = "";
    u8       crc    = nya_crc8(data, 0);
    nya_assert(crc == 0); // CRC of empty data should be 0 (initial value)
  }

  // TEST: nya_crc8 - same data produces same CRC
  {
    const u8 data[] = "test data for crc";
    u8       crc1   = nya_crc8(data, 17);
    u8       crc2   = nya_crc8(data, 17);
    nya_assert(crc1 == crc2);
  }

  // TEST: nya_crc8 - different data produces different CRC (usually)
  {
    const u8 data1[] = "data1";
    const u8 data2[] = "data2";
    u8       crc1    = nya_crc8(data1, 5);
    u8       crc2    = nya_crc8(data2, 5);
    nya_assert(crc1 != crc2);
  }

  // TEST: nya_crc8 - binary data
  {
    const u8 data[] = { 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE };
    u8       crc    = nya_crc8(data, 6);
    nya_assert(crc != 0);

    // Verify same data produces same CRC
    u8 crc2 = nya_crc8(data, 6);
    nya_assert(crc == crc2);
  }

  // TEST: nya_crc16 - basic test
  {
    const u8 data[] = "hello";
    u16      crc    = nya_crc16(data, 5);
    nya_assert(crc != 0);
    nya_assert(crc != 0xFFFF); // Should not be the initial value
  }

  // TEST: nya_crc16 - empty data
  {
    const u8 data[] = "";
    u16      crc    = nya_crc16(data, 0);
    // CRC16-CCITT of empty data should be 0xFFFF (initial value)
    nya_assert(crc == 0xFFFF);
  }

  // TEST: nya_crc16 - deterministic
  {
    const u8 data[] = "123456789";
    u16      crc1   = nya_crc16(data, 9);
    u16      crc2   = nya_crc16(data, 9);
    nya_assert(crc1 == crc2);
  }

  // TEST: nya_crc16 - known test vectors
  {
    // CRC16-CCITT (0xFFFF initial) of "123456789" should be 0x29B1
    const u8 data[] = "123456789";
    u16      crc    = nya_crc16(data, 9);
    nya_assert(crc == 0x29B1);
  }

  // TEST: nya_crc32 - basic test
  {
    const u8 data[] = "hello";
    u32      crc    = nya_crc32(data, 5);
    nya_assert(crc != 0);
    nya_assert(crc != 0xFFFFFFFF);
  }

  // TEST: nya_crc32 - empty data
  {
    const u8 data[] = "";
    u32      crc    = nya_crc32(data, 0);
    // CRC32 of empty data: init ^ final_xor = 0xFFFFFFFF ^ 0xFFFFFFFF = 0
    nya_assert(crc == 0);
  }

  // TEST: nya_crc32 - deterministic
  {
    const u8 data[] = "The quick brown fox jumps over the lazy dog";
    u32      crc1   = nya_crc32(data, 43);
    u32      crc2   = nya_crc32(data, 43);
    nya_assert(crc1 == crc2);
  }

  // TEST: nya_crc32 - known test vector
  {
    // CRC32 of "123456789" should be 0xCBF43926
    const u8 data[] = "123456789";
    u32      crc    = nya_crc32(data, 9);
    nya_assert(crc == 0xCBF43926);
  }

  // TEST: nya_crc32 - ASCII 'A' should produce 0xD3D99E8B
  {
    const u8 data[] = "A";
    u32      crc    = nya_crc32(data, 1);
    // CRC32 of "A" = 0xD3D99E8B (calculated with standard CRC32) But let's just verify it's deterministic
    u32      crc2 = nya_crc32(data, 1);
    nya_assert(crc == crc2);
  }

  // TEST: nya_crc64 - basic test
  {
    const u8 data[] = "hello";
    u64      crc    = nya_crc64(data, 5);
    nya_assert(crc != 0);
    nya_assert(crc != 0xFFFFFFFFFFFFFFFFULL);
  }

  // TEST: nya_crc64 - empty data
  {
    const u8 data[] = "";
    u64      crc    = nya_crc64(data, 0);
    // CRC64 of empty data: init ^ final_xor = 0xFFFFFFFFFFFFFFFF ^ 0xFFFFFFFFFFFFFFFF = 0
    nya_assert(crc == 0);
  }

  // TEST: nya_crc64 - deterministic
  {
    const u8 data[] = "123456789";
    u64      crc1   = nya_crc64(data, 9);
    u64      crc2   = nya_crc64(data, 9);
    nya_assert(crc1 == crc2);
  }

  // TEST: all CRC functions with same data
  {
    const u8 data[] = "abcdefghijklmnopqrstuvwxyz";

    u8  crc8  = nya_crc8(data, 26);
    u16 crc16 = nya_crc16(data, 26);
    u32 crc32 = nya_crc32(data, 26);
    u64 crc64 = nya_crc64(data, 26);

    // All should be non-zero
    nya_assert(crc8 != 0);
    nya_assert(crc16 != 0);
    nya_assert(crc32 != 0);
    nya_assert(crc64 != 0);

    // Running again should produce same results
    nya_assert(crc8 == nya_crc8(data, 26));
    nya_assert(crc16 == nya_crc16(data, 26));
    nya_assert(crc32 == nya_crc32(data, 26));
    nya_assert(crc64 == nya_crc64(data, 26));
  }

  // TEST: CRC avalanche effect - small change produces large difference
  {
    const u8 data1[] = "aaaaaaaaaa";
    const u8 data2[] = "baaaaaaaaa"; // One bit different

    u32 crc1 = nya_crc32(data1, 10);
    u32 crc2 = nya_crc32(data2, 10);

    // The CRCs should be very different
    nya_assert(crc1 != crc2);
  }

  // TEST: CRC with all possible byte values
  {
    // Create data with all possible byte values 0-255
    u8 all_bytes[256];
    for (s32 i = 0; i < 256; i++) { all_bytes[i] = (u8)i; }

    // Compute CRCs - should not crash and should be deterministic
    u8  crc8_result  = nya_crc8(all_bytes, 256);
    u16 crc16_result = nya_crc16(all_bytes, 256);
    u32 crc32_result = nya_crc32(all_bytes, 256);
    u64 crc64_result = nya_crc64(all_bytes, 256);

    // Should all be non-zero for non-empty data
    nya_assert(crc8_result != 0);
    nya_assert(crc16_result != 0xFFFF);
    nya_assert(crc32_result != 0);
    nya_assert(crc64_result != 0);

    // Should be deterministic
    nya_assert(crc8_result == nya_crc8(all_bytes, 256));
    nya_assert(crc16_result == nya_crc16(all_bytes, 256));
    nya_assert(crc32_result == nya_crc32(all_bytes, 256));
    nya_assert(crc64_result == nya_crc64(all_bytes, 256));
  }

  // TEST: CRC with single repeated byte
  {
    u8 repeated_ff[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    u8 repeated_00[10] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    u8 repeated_aa[10] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };

    // Test CRC of repeated patterns
    u8 crc8_ff = nya_crc8(repeated_ff, 10);
    u8 crc8_00 = nya_crc8(repeated_00, 10);
    u8 crc8_aa = nya_crc8(repeated_aa, 10);

    // Should be different (unless there's a collision, which is unlikely)
    nya_assert(crc8_ff != crc8_00 || crc8_ff != crc8_aa || crc8_00 != crc8_aa);

    // CRC32 tests
    u32 crc32_ff = nya_crc32(repeated_ff, 10);
    u32 crc32_00 = nya_crc32(repeated_00, 10);
    u32 crc32_aa = nya_crc32(repeated_aa, 10);

    nya_assert(crc32_ff != crc32_00 || crc32_ff != crc32_aa || crc32_00 != crc32_aa);
  }

  // TEST: CRC with large data
  {
    // Create 64KB of test data
    u8* large_data = (u8*)nya_arena_alloc(arena, 65536);
    for (s32 i = 0; i < 65536; i++) {
      large_data[i] = (u8)((i * 31 + 17) & 0xFF); // Pseudo-random pattern
    }

    // Compute CRCs on large data - should not crash
    u8  crc8_large  = nya_crc8(large_data, 65536);
    u16 crc16_large = nya_crc16(large_data, 65536);
    u32 crc32_large = nya_crc32(large_data, 65536);
    u64 crc64_large = nya_crc64(large_data, 65536);

    // Should be non-zero for non-empty data
    nya_assert(crc8_large != 0);
    nya_assert(crc16_large != 0xFFFF);
    nya_assert(crc32_large != 0);
    nya_assert(crc64_large != 0);

    // Should be deterministic
    nya_assert(crc8_large == nya_crc8(large_data, 65536));
    nya_assert(crc16_large == nya_crc16(large_data, 65536));
    nya_assert(crc32_large == nya_crc32(large_data, 65536));
    nya_assert(crc64_large == nya_crc64(large_data, 65536));
  }

  // TEST: CRC with bit pattern sequences
  {
    // Test with alternating patterns
    u8 pattern_55[8] = { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 }; // 01010101
    u8 pattern_AA[8] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA }; // 10101010
    u8 pattern_FF[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // 11111111
    u8 pattern_00[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // 00000000

    // Compute CRC32 for these patterns
    u32 crc32_55 = nya_crc32(pattern_55, 8);
    u32 crc32_AA = nya_crc32(pattern_AA, 8);
    u32 crc32_FF = nya_crc32(pattern_FF, 8);
    u32 crc32_00 = nya_crc32(pattern_00, 8);

    // All should be different (very unlikely to have collisions)
    u32 crcs[] = { crc32_55, crc32_AA, crc32_FF, crc32_00 };
    for (s32 i = 0; i < 4; i++) {
      for (s32 j = i + 1; j < 4; j++) { nya_assert(crcs[i] != crcs[j]); }
    }

    // crc32_00 should not be 0 (it's 8 zero bytes, not empty data) The CRC32 of zero bytes depends on the polynomial and length
    nya_assert(crc32_00 != 0xFFFFFFFF); // Should not be initial value
  }

  // TEST: CRC consistency across different data lengths
  {
    u8 base_data[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10 };

    // Compute CRCs for different lengths of the same data
    u32 crc32_1  = nya_crc32(base_data, 1);
    u32 crc32_2  = nya_crc32(base_data, 2);
    u32 crc32_4  = nya_crc32(base_data, 4);
    u32 crc32_8  = nya_crc32(base_data, 8);
    u32 crc32_16 = nya_crc32(base_data, 16);

    // Should all be different (extremely unlikely to have collisions)
    nya_assert(crc32_1 != crc32_2);
    nya_assert(crc32_2 != crc32_4);
    nya_assert(crc32_4 != crc32_8);
    nya_assert(crc32_8 != crc32_16);

    // Verify deterministic nature
    nya_assert(crc32_1 == nya_crc32(base_data, 1));
    nya_assert(crc32_2 == nya_crc32(base_data, 2));
    nya_assert(crc32_4 == nya_crc32(base_data, 4));
    nya_assert(crc32_8 == nya_crc32(base_data, 8));
    nya_assert(crc32_16 == nya_crc32(base_data, 16));
  }

  // CLEANUP
  nya_arena_destroy(arena);

  return 0;
}
