/**
 * SHA-256 and HMAC-SHA256, against the answers the standards print.
 *
 * A hash has exactly one correct output per input, so a test that computes it a second way is testing
 * itself. Every case here is a published vector: FIPS 180-2 appendix B for the digests, RFC 4231 for
 * the tags. Between them they cover a tail that fits beside the length, a tail that does not, and a
 * message that is a whole number of blocks.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** A digest as lowercase hex, which is how every standard writes its answers down. */
static NYA_CString to_hex(NYA_Arena* arena, const u8* digest, u64 size) {
  NYA_String* text = nya_string_create(arena);

  for (u64 i = 0; i < size; i++) nya_string_extend_sprintf(text, "%02x", (unsigned)digest[i]);

  return nya_string_to_cstring(arena, text);
}

static void check_digest(NYA_Arena* arena, NYA_ConstCString message, NYA_ConstCString expected) {
  u8 digest[NYA_SHA256_BYTES] = { 0 };
  nya_sha256((const u8*)message, strlen(message), digest);

  NYA_CString got = to_hex(arena, digest, sizeof(digest));
  nya_assert(nya_string_equals((NYA_ConstCString)got, expected), "sha256 of a %zu byte message: got %s, want %s", strlen(message), got, expected);
}

static void check_tag(NYA_Arena* arena, const u8* key, u64 key_size, const u8* data, u64 size, NYA_ConstCString expected) {
  u8 tag[NYA_SHA256_BYTES] = { 0 };
  nya_hmac_sha256(key, key_size, data, size, tag);

  NYA_CString got = to_hex(arena, tag, sizeof(tag));
  nya_assert(nya_string_equals((NYA_ConstCString)got, expected), "hmac-sha256: got %s, want %s", got, expected);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_sha256");
  defer      nya_arena_destroy(arena);

  printf("TEST: the digests FIPS 180-2 prints\n");
  {
    // The empty message, whose padding block is the only block there is.
    check_digest(arena, "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Appendix B.1: three bytes, so the terminator and the length fit beside them in one block.
    check_digest(arena, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Appendix B.2: fifty six bytes, which is exactly the length at which the terminator and the
    // eight byte length no longer fit and a second padding block is required. The case a hand written
    // padding gets wrong.
    check_digest(
        arena,
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );

    // A hundred and twelve bytes: one whole block and a tail that needs its own.
    check_digest(
        arena,
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"
    );

    printf("  four published digests matched, including both sides of the padding boundary\n");
  }

  printf("TEST: a message that is a whole number of blocks\n");
  {
    // Appendix B.3: a million 'a', which is 15625 whole blocks and no remainder at all, so the tail is
    // a padding block on its own. The other case a hand written padding gets wrong.
    u64 size = 1000000;

    u8* message = nya_arena_alloc(arena, size);
    nya_memset(message, 'a', size);

    u8 digest[NYA_SHA256_BYTES] = { 0 };
    nya_sha256(message, size, digest);

    NYA_CString got = to_hex(arena, digest, sizeof(digest));
    nya_assert(nya_string_equals((NYA_ConstCString)got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "got %s", got);

    nya_arena_free(arena, message, size);

    printf("  a million bytes hashed to the published answer\n");
  }

  printf("TEST: the tags RFC 4231 prints\n");
  {
    // Case 1: a twenty byte key, shorter than a block, so it is zero padded.
    u8 key_1[20];
    nya_memset(key_1, 0x0B, sizeof(key_1));
    check_tag(arena, key_1, sizeof(key_1), (const u8*)"Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // Case 2: a four byte key, and the case everyone quotes.
    check_tag(
        arena,
        (const u8*)"Jefe",
        4,
        (const u8*)"what do ya want for nothing?",
        28,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
    );

    // Case 3: fifty bytes of data, so the inner hash spans more than one block.
    u8 key_3[20];
    u8 data_3[50];
    nya_memset(key_3, 0xAA, sizeof(key_3));
    nya_memset(data_3, 0xDD, sizeof(data_3));
    check_tag(arena, key_3, sizeof(key_3), data_3, sizeof(data_3), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // Case 6: a key longer than a block, which RFC 2104 says to hash first. The rule a caller should
    // never have to know, and the one this would silently get wrong by truncating instead.
    u8 key_6[131];
    nya_memset(key_6, 0xAA, sizeof(key_6));
    check_tag(
        arena,
        key_6,
        sizeof(key_6),
        (const u8*)"Test Using Larger Than Block-Size Key - Hash Key First",
        54,
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
    );

    printf("  four published tags matched, including a key longer than a block\n");
  }

  printf("TEST: a comparison that does not say where it stopped\n");
  {
    u8 left[NYA_SHA256_BYTES]  = { 0 };
    u8 right[NYA_SHA256_BYTES] = { 0 };

    nya_sha256((const u8*)"secret", 6, left);
    nya_sha256((const u8*)"secret", 6, right);

    nya_assert(nya_hash_equals_constant_time(left, right, sizeof(left)));

    // A difference in the first byte and a difference in the last both answer false, which is all a
    // caller may learn from it.
    right[0] ^= 0x01U;
    nya_assert(!nya_hash_equals_constant_time(left, right, sizeof(left)));

    right[0]                 ^= 0x01U;
    right[sizeof(right) - 1] ^= 0x80U;
    nya_assert(!nya_hash_equals_constant_time(left, right, sizeof(left)));

    // Nothing to compare is equal, which is the same answer memcmp gives.
    nya_assert(nya_hash_equals_constant_time(left, right, 0));

    printf("  equal, differing first, differing last, and empty\n");
  }

  printf("PASSED: test_sha256 (0 failures)\n");

  return EXIT_SUCCESS;
}
