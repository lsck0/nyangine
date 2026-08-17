/**
 * Known-answer vectors for FNV-1a and SipHash-2-4.
 *
 * test_crc.c already pins CRC-8/16/32/64 against the standard "123456789" check values. Neither
 * FNV-1a nor SipHash had the same treatment, and SipHash is the one that matters most: it is what
 * base_integrity.c computes its code baseline and its binary MAC with, so an implementation that is
 * merely *a* hash rather than *the* hash would still look fine to every test in the tree while
 * failing to be the primitive the integrity check claims.
 *
 * SipHash-2-4 reference vectors: key 000102...0f, input the byte sequence 00 01 02 ... of the given
 * length. The reference implementation supplies the key as bytes; this API takes two u64 halves, so
 * the key is the little-endian reading of those sixteen bytes.
 */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Key bytes 00..0f read little endian, which is what the reference implementation does. */
#define SIPHASH_KEY_LOW  0x0706050403020100ULL
#define SIPHASH_KEY_HIGH 0x0F0E0D0C0B0A0908ULL

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: FNV-1a 64 against its published values
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: fnv1a known answers\n");
  {
    struct {
      NYA_ConstCString input;
      u64              expected;
    } cases[] = {
      {      "", 0xCBF29CE484222325ULL }, // the offset basis, returned unchanged for empty input
      {     "a", 0xAF63DC4C8601EC8CULL },
      {"foobar", 0x85944171F73967E8ULL },
    };

    for (u64 i = 0; i < nya_carray_length(cases); i++) {
      u64 got = nya_hash_fnv1a(cases[i].input, strlen(cases[i].input));
      nya_check(got == cases[i].expected, "fnv1a(\"%s\") = 0x%016llX, expected 0x%016llX", cases[i].input, (unsigned long long)got, (unsigned long long)cases[i].expected);
    }

    // The cstring overload has to agree with the explicit-length one.
    nya_check(nya_hash_fnv1a("foobar") == nya_hash_fnv1a("foobar", 6), "the fnv1a overloads disagree");
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: SipHash-2-4 against the reference vectors
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: siphash known answers\n");
  {
    u8 input[16];
    for (u32 i = 0; i < sizeof(input); i++) input[i] = (u8)i;

    struct {
      u64 length;
      u64 expected;
    } cases[] = {
      { 0, 0x726FDB47DD0E0E31ULL },
      { 1, 0x74F839C593DC67FDULL },
      { 2, 0x0D6C8009D9A94F5AULL },
      { 3, 0x85676696D7FB7E2DULL },
      { 4, 0xCF2794E0277187B7ULL },
      { 8, 0x93F5F5799A932462ULL },
      { 15, 0xA129CA6149BE45E5ULL },
    };

    for (u64 i = 0; i < nya_carray_length(cases); i++) {
      u64 got = nya_siphash(input, cases[i].length, SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH);
      nya_check(
        got == cases[i].expected,
        "siphash(len %llu) = 0x%016llX, expected 0x%016llX",
        (unsigned long long)cases[i].length,
        (unsigned long long)got,
        (unsigned long long)cases[i].expected
      );
    }
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the properties the integrity check depends on
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: siphash properties\n");
  {
    u8 data[64];
    for (u32 i = 0; i < sizeof(data); i++) data[i] = (u8)(i * 7 + 1);

    u64 base = nya_siphash(data, sizeof(data), SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH);

    nya_check(base == nya_siphash(data, sizeof(data), SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH), "siphash is not deterministic");

    // A different key must give a different digest, or the key is not being mixed in.
    nya_check(base != nya_siphash(data, sizeof(data), SIPHASH_KEY_LOW ^ 1ULL, SIPHASH_KEY_HIGH), "the low key half does not affect the digest");
    nya_check(base != nya_siphash(data, sizeof(data), SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH ^ 1ULL), "the high key half does not affect the digest");

    // Flipping any single bit of the input must change the digest — this is the property that makes
    // it a tamper check rather than a checksum.
    for (u32 byte = 0; byte < sizeof(data); byte++) {
      for (u32 bit = 0; bit < 8; bit++) {
        data[byte] ^= (u8)(1U << bit);
        u64 altered = nya_siphash(data, sizeof(data), SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH);
        data[byte] ^= (u8)(1U << bit);

        nya_check(altered != base, "flipping bit %u of byte %u left the digest unchanged", bit, byte);
      }
    }

    // Trailing zero bytes must not collide with a shorter input: that is what the length in the
    // tail block is for.
    u8 short_input[4] = { 1, 2, 3, 4 };
    u8 long_input[8]  = { 1, 2, 3, 4, 0, 0, 0, 0 };
    nya_check(
      nya_siphash(short_input, 4, SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH) != nya_siphash(long_input, 8, SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH),
      "an input and the same input padded with zeroes hash alike"
    );

    // A zero length input with a null pointer is explicitly permitted by the assertion.
    (void)nya_siphash(nullptr, 0, SIPHASH_KEY_LOW, SIPHASH_KEY_HIGH);
  }
  printf("  done\n");

  printf("%s: test_hash_vectors (" FMTu32 " failures)\n", nya_check_failures() == 0 ? "PASSED" : "FAILED", nya_check_failures());
  return nya_check_failures() == 0 ? 0 : 1;
}
