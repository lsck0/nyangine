/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

typedef enum {
  SAMPLE_FLAG_A = 1 << 0,
  SAMPLE_FLAG_B = 1 << 1,
  SAMPLE_FLAG_C = 1 << 2,
  SAMPLE_FLAG_D = 1 << 3,
} SAMPLE_FLAGS;

s32 main(void) {
  // TEST: bit utilities - basic operations
  u64 value = 0;
  nya_bit_set(value, BIT5);
  nya_assert(value == 0b10000);
  nya_assert(nya_bit_check(value, BIT5));
  nya_bit_unset(value, BIT5);
  nya_assert(!nya_bit_check(value, BIT5));
  nya_assert(value == 0);
  nya_bit_toggle(value, BIT5);
  nya_assert(value == 0b10000);
  nya_bit_toggle(value, BIT5);
  nya_assert(value == 0);

  // TEST: bit utilities - boundary bits (first and last)
  value = 0;
  nya_bit_set(value, BIT1);
  nya_assert(value == 1);
  nya_assert(nya_bit_check(value, BIT1));
  nya_bit_unset(value, BIT1);
  nya_assert(value == 0);

  value = 0;
  nya_bit_set(value, BIT64);
  nya_assert(value == (1ULL << 63));
  nya_assert(nya_bit_check(value, BIT64));
  nya_bit_unset(value, BIT64);
  nya_assert(value == 0);

  // TEST: bit utilities - multiple bits set
  value = 0;
  nya_bit_set(value, BIT1);
  nya_bit_set(value, BIT8);
  nya_bit_set(value, BIT16);
  nya_assert(nya_bit_check(value, BIT1));
  nya_assert(nya_bit_check(value, BIT8));
  nya_assert(nya_bit_check(value, BIT16));
  nya_assert(!nya_bit_check(value, BIT2));
  nya_bit_toggle(value, BIT1);
  nya_assert(!nya_bit_check(value, BIT1));
  nya_assert(nya_bit_check(value, BIT8));

  // TEST: bitmask utilities
  value = 0;
  nya_bitmask_set(value, BITMASK3);
  nya_assert(value == 0b111);
  nya_assert(nya_bitmask_check(value, BITMASK3));
  nya_bitmask_unset(value, BITMASK3);
  nya_assert(!nya_bitmask_check(value, BITMASK3));
  nya_assert(value == 0);
  nya_bitmask_toggle(value, BITMASK3);
  nya_assert(value == 0b111);
  nya_bitmask_toggle(value, BITMASK3);
  nya_assert(value == 0);

  // TEST: bitmask utilities - larger masks
  value = 0;
  nya_bitmask_set(value, BITMASK8);
  nya_assert(value == 0xFF);
  nya_bitmask_set(value, BITMASK16);
  nya_assert(value == 0xFFFF);
  nya_bitmask_unset(value, BITMASK8);
  nya_assert(value == 0xFF00);

  value = 0;
  nya_bitmask_set(value, BITMASK32);
  nya_assert(value == 0xFFFFFFFFU);
  nya_bitmask_unset(value, BITMASK32);
  nya_assert(value == 0);

  value = 0;
  nya_bitmask_set(value, BITMASK64);
  nya_assert(value == 0xFFFFFFFFFFFFFFFFULL);

  // TEST: bit count (popcount)
  nya_assert(nya_bits_count_u32(0) == 0);
  nya_assert(nya_bits_count_u32(1) == 1);
  nya_assert(nya_bits_count_u32(0b1111) == 4);
  nya_assert(nya_bits_count_u32(0xFF) == 8);
  nya_assert(nya_bits_count_u32(0xFFFF) == 16);
  nya_assert(nya_bits_count_u32(0xFFFFFFFFU) == 32);
  nya_assert(nya_bits_count_u32(0b10101010) == 4);
  nya_assert(nya_bits_count_u32(0x12345678) == 13);

  nya_assert(nya_bits_count_u64(0) == 0);
  nya_assert(nya_bits_count_u64(1) == 1);
  nya_assert(nya_bits_count_u64(0xFFFFFFFFFFFFFFFFULL) == 64);
  nya_assert(nya_bits_count_u64(0x8000000000000000ULL) == 1);
  nya_assert(nya_bits_count_u64(0xAAAAAAAAAAAAAAAAULL) == 32);

  // TEST: count leading zeros (clz)
  nya_assert(nya_bits_clz_u32(1) == 31);
  nya_assert(nya_bits_clz_u32(0x80000000U) == 0);
  nya_assert(nya_bits_clz_u32(0x40000000U) == 1);
  nya_assert(nya_bits_clz_u32(0x00010000U) == 15);
  nya_assert(nya_bits_clz_u32(0xFF) == 24);

  nya_assert(nya_bits_clz_u64(1) == 63);
  nya_assert(nya_bits_clz_u64(0x8000000000000000ULL) == 0);
  nya_assert(nya_bits_clz_u64(0x0000000100000000ULL) == 31);
  nya_assert(nya_bits_clz_u64(0x00FF000000000000ULL) == 8);

  // TEST: count trailing zeros (ctz)
  nya_assert(nya_bits_ctz_u32(1) == 0);
  nya_assert(nya_bits_ctz_u32(2) == 1);
  nya_assert(nya_bits_ctz_u32(4) == 2);
  nya_assert(nya_bits_ctz_u32(0x80000000U) == 31);
  nya_assert(nya_bits_ctz_u32(0x00010000U) == 16);
  nya_assert(nya_bits_ctz_u32(0xFF00) == 8);

  nya_assert(nya_bits_ctz_u64(1) == 0);
  nya_assert(nya_bits_ctz_u64(0x8000000000000000ULL) == 63);
  nya_assert(nya_bits_ctz_u64(0x0000000100000000ULL) == 32);
  nya_assert(nya_bits_ctz_u64(0x00FF000000000000ULL) == 48);

  // TEST: bit flag utilities
  SAMPLE_FLAGS flags = 0;
  nya_flag_set(flags, SAMPLE_FLAG_A);
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_flag_unset(flags, SAMPLE_FLAG_A);
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_flag_toggle(flags, SAMPLE_FLAG_A);
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_flag_toggle(flags, SAMPLE_FLAG_B);
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_assert(!nya_flag_equals(flags, SAMPLE_FLAG_C));
  nya_assert(nya_flag_equals(flags, SAMPLE_FLAG_A | SAMPLE_FLAG_B));

  // TEST: bit flag utilities - all flags
  flags = 0;
  nya_flag_set(flags, SAMPLE_FLAG_A);
  nya_flag_set(flags, SAMPLE_FLAG_B);
  nya_flag_set(flags, SAMPLE_FLAG_C);
  nya_flag_set(flags, SAMPLE_FLAG_D);
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_D));
  nya_assert(nya_flag_equals(flags, SAMPLE_FLAG_A | SAMPLE_FLAG_B | SAMPLE_FLAG_C | SAMPLE_FLAG_D));

  nya_flag_unset(flags, SAMPLE_FLAG_B);
  nya_flag_unset(flags, SAMPLE_FLAG_D);
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_D));

  // TEST: bit flag utilities - toggle multiple
  flags = 0;
  nya_flag_toggle(flags, SAMPLE_FLAG_A | SAMPLE_FLAG_C);
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_B));
  nya_assert(nya_flag_check(flags, SAMPLE_FLAG_C));
  nya_flag_toggle(flags, SAMPLE_FLAG_A | SAMPLE_FLAG_C);
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_A));
  nya_assert(!nya_flag_check(flags, SAMPLE_FLAG_C));

  return 0;
}
