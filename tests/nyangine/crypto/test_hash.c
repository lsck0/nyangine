/**
 * SHA-256, SHA-1, their HMACs and BLAKE2b, against the answers the standards print.
 *
 * A hash has exactly one correct output per input, so a test that computes it a second way is testing
 * itself. Every case here is a published vector: RFC 6234's test driver (FIPS 180's examples) for the
 * digests, RFC 4231 and RFC 2202 for the tags, RFC 7693 appendices A and E for BLAKE2b. Between them they
 * cover a tail that fits beside the length, a tail that does not, and whole blocks with nothing left over.
 * The laws are about the streaming form, which no vector reaches directly.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Cases per law. Each hashes under a kilobyte, so this is quick even sanitized. */
#define CASES 2000

/** Fixed, so the suite is the same run every time. */
#define SEED 0x6861736865735F21ULL

/* HELPERS */

/** Bytes as lowercase hex, which is how every standard writes its answers down. */
static NYA_CString to_hex(NYA_Arena* arena, const u8* bytes, u64 size) {
    NYA_String* text = nya_string_create(arena);

    for (u64 i = 0; i < size; i++) nya_string_extend_sprintf(text, "%02x", (unsigned)bytes[i]);

    return nya_string_to_cstring(arena, text);
}

static void check_hex(NYA_Arena* arena, NYA_ConstCString what, const u8* bytes, u64 size, NYA_ConstCString expected) {
    NYA_CString got = to_hex(arena, bytes, size);
    nya_assert(nya_string_equals((NYA_ConstCString)got, expected), "%s: got %s, want %s", what, got, expected);
}

static void check_sha256(NYA_Arena* arena, NYA_ConstCString message, u64 repeat, NYA_ConstCString expected) {
    NYA_CryptoSha256 sha256 = { 0 };
    nya_crypto_sha256_begin(&sha256);
    for (u64 i = 0; i < repeat; i++) nya_crypto_sha256_update(&sha256, (const u8*)message, strlen(message));

    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256_end(&sha256, &digest);

    check_hex(arena, "sha256", digest.bytes, sizeof(digest.bytes), expected);

    // the one shot form agrees wherever the message is small enough to hand it whole.
    if (repeat == 1) {
        NYA_CryptoSha256Digest once = { 0 };
        nya_crypto_sha256((const u8*)message, strlen(message), &once);
        check_hex(arena, "sha256 in one call", once.bytes, sizeof(once.bytes), expected);
    }
}

static void check_sha1(NYA_Arena* arena, const u8* message, u64 size, NYA_ConstCString expected) {
    NYA_CryptoSha1Digest digest = { 0 };
    nya_crypto_sha1(message, size, &digest);

    check_hex(arena, "sha1", digest.bytes, sizeof(digest.bytes), expected);
}

static void check_hmac_sha256(NYA_Arena* arena, const u8* key, u64 key_size, const u8* data, u64 size, NYA_ConstCString expected) {
    NYA_CryptoSha256Digest tag = { 0 };
    nya_crypto_hmac_sha256(key, key_size, data, size, &tag);

    // RFC 4231 case 5 prints only the first 128 bits, so the comparison is as long as the answer.
    check_hex(arena, "hmac-sha256", tag.bytes, strlen(expected) / 2, expected);
}

static void check_hmac_sha1(NYA_Arena* arena, const u8* key, u64 key_size, const u8* data, u64 size, NYA_ConstCString expected) {
    NYA_CryptoSha1Digest tag = { 0 };
    nya_crypto_hmac_sha1(key, key_size, data, size, &tag);

    check_hex(arena, "hmac-sha1", tag.bytes, sizeof(tag.bytes), expected);
}

/** RFC 7693 appendix E's deterministic filler, a Fibonacci generator over u32 that wraps by design. */
__attr_no_sanitize("unsigned-integer-overflow") static void selftest_sequence(u8* out, u64 size, u32 seed) {
    u32 a = 0xDEAD4BADU * seed;
    u32 b = 1;

    for (u64 i = 0; i < size; i++) {
        u32 t  = a + b;
        a      = b;
        b      = t;
        out[i] = (u8)((t >> 24) & 0xFFU);
    }
}

/* LAWS */

/** Feeding a message in any pieces gives the digest of feeding it whole. */
static b8 law_sha256_pieces_agree(NYA_Property* property) {
    u8  message[512];
    u32 size = (u32)nya_property_draw_below(property, sizeof(message) + 1);
    nya_property_draw_bytes(property, message, size);

    NYA_CryptoSha256Digest whole = { 0 };
    nya_crypto_sha256(message, size, &whole);

    NYA_CryptoSha256 sha256 = { 0 };
    nya_crypto_sha256_begin(&sha256);

    u32 fed = 0;
    while (fed < size) {
        u32 piece = (u32)nya_property_draw_below(property, (size - fed) + 1);
        nya_crypto_sha256_update(&sha256, message + fed, piece);
        fed += piece;

        // a draw of zero past the end of the entropy would never finish; take the rest in one.
        if (piece == 0 && property->cursor >= property->entropy_length) {
            nya_crypto_sha256_update(&sha256, message + fed, size - fed);
            fed = size;
        }
    }

    NYA_CryptoSha256Digest pieces = { 0 };
    nya_crypto_sha256_end(&sha256, &pieces);

    nya_property_note(property, "a %u byte message", size);
    return nya_crypto_equals(whole.bytes, pieces.bytes, sizeof(whole.bytes));
}

/** One flipped message bit changes the tag, under either HMAC. */
static b8 law_hmac_notices_a_flipped_bit(NYA_Property* property) {
    u8  key[80];
    u8  message[200];
    u32 key_size = (u32)nya_property_draw_below(property, sizeof(key) + 1);
    u32 size     = 1 + (u32)nya_property_draw_below(property, sizeof(message));
    nya_property_draw_bytes(property, key, key_size);
    nya_property_draw_bytes(property, message, size);

    NYA_CryptoSha256Digest before_256 = { 0 };
    NYA_CryptoSha1Digest   before_1   = { 0 };
    nya_crypto_hmac_sha256(key, key_size, message, size, &before_256);
    nya_crypto_hmac_sha1(key, key_size, message, size, &before_1);

    u32 bit           = (u32)nya_property_draw_below(property, (u64)size * 8);
    message[bit / 8] ^= (u8)(1U << (bit % 8));

    NYA_CryptoSha256Digest after_256 = { 0 };
    NYA_CryptoSha1Digest   after_1   = { 0 };
    nya_crypto_hmac_sha256(key, key_size, message, size, &after_256);
    nya_crypto_hmac_sha1(key, key_size, message, size, &after_1);

    nya_property_note(property, "a %u byte key, a %u byte message, bit %u", key_size, size, bit);
    return !nya_crypto_equals(before_256.bytes, after_256.bytes, sizeof(before_256.bytes)) &&
           !nya_crypto_equals(before_1.bytes, after_1.bytes, sizeof(before_1.bytes));
}

/* TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_hash");
    defer      nya_arena_destroy(arena);

    u32 failures = 0;

    printf("TEST: the SHA-256 digests RFC 6234 prints\n");
    {
        // the empty message, whose padding block is the only block there is.
        check_sha256(arena, "", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        // TEST1: three bytes, so the terminator and the length fit beside them.
        check_sha256(arena, "abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        // TEST2_1: fifty six bytes, exactly where the terminator and the length stop fitting and a second padding block is needed. The case a hand written padding gets wrong.
        check_sha256(
            arena,
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
            1,
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );

        // TEST2_2: a hundred and twelve bytes, one whole block and a tail that needs its own.
        check_sha256(
            arena,
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
            1,
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"
        );

        // TEST3: a million 'a', fed one byte at a time, so the buffering sees every offset in a block.
        check_sha256(arena, "a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

        // TEST4: sixty four bytes ten times, whole blocks with no remainder, so the tail is padding alone.
        check_sha256(
            arena,
            "0123456701234567012345670123456701234567012345670123456701234567",
            10,
            "594847328451bdfa85056225462cc1d867d877fb388df0ce35f25ab5562bfbb5"
        );

        printf("  six published digests matched, streamed and whole\n");
    }

    printf("TEST: the SHA-1 digests RFC 6234 prints\n");
    {
        check_sha1(arena, (const u8*)"", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        check_sha1(arena, (const u8*)"abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d");
        check_sha1(arena, (const u8*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

        // TEST3 and TEST4 are repeated inputs, which SHA-1 has no streaming form for; built whole instead.
        u64 million = 1000000;
        u8* message = nya_arena_alloc(arena, million);
        nya_memset(message, 'a', million);
        check_sha1(arena, message, million, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
        nya_arena_free(arena, message, million);

        u8 blocks[640];
        for (u32 i = 0; i < sizeof(blocks); i++) blocks[i] = (u8)('0' + (i % 8));
        check_sha1(arena, blocks, sizeof(blocks), "dea356a2cddd90c7a7ecedc5ebb563934f460452");

        printf("  five published digests matched\n");
    }

    printf("TEST: the HMAC-SHA256 tags RFC 4231 prints\n");
    {
        u8 key_1[20];
        nya_memset(key_1, 0x0B, sizeof(key_1));
        check_hmac_sha256(arena, key_1, sizeof(key_1), (const u8*)"Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

        check_hmac_sha256(
            arena,
            (const u8*)"Jefe",
            4,
            (const u8*)"what do ya want for nothing?",
            28,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );

        u8 key_3[20];
        u8 data_3[50];
        nya_memset(key_3, 0xAA, sizeof(key_3));
        nya_memset(data_3, 0xDD, sizeof(data_3));
        check_hmac_sha256(arena, key_3, sizeof(key_3), data_3, sizeof(data_3), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

        // case 4: key and data together longer than a block.
        u8 key_4[25];
        u8 data_4[50];
        for (u32 i = 0; i < sizeof(key_4); i++) key_4[i] = (u8)(i + 1);
        nya_memset(data_4, 0xCD, sizeof(data_4));
        check_hmac_sha256(arena, key_4, sizeof(key_4), data_4, sizeof(data_4), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

        // case 5: the RFC prints the tag truncated to 128 bits.
        u8 key_5[20];
        nya_memset(key_5, 0x0C, sizeof(key_5));
        check_hmac_sha256(arena, key_5, sizeof(key_5), (const u8*)"Test With Truncation", 20, "a3b6167473100ee06e0c796c2955552b");

        // cases 6 and 7: a key longer than a block, which RFC 2104 says to hash first. The rule a caller should never have to know, and the one a truncating implementation silently gets wrong.
        u8 key_6[131];
        nya_memset(key_6, 0xAA, sizeof(key_6));
        check_hmac_sha256(
            arena,
            key_6,
            sizeof(key_6),
            (const u8*)"Test Using Larger Than Block-Size Key - Hash Key First",
            54,
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
        );

        NYA_ConstCString data_7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed "
                                  "before being used by the HMAC algorithm.";
        check_hmac_sha256(
            arena,
            key_6,
            sizeof(key_6),
            (const u8*)data_7,
            strlen(data_7),
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"
        );

        printf("  all seven published tags matched\n");
    }

    printf("TEST: the HMAC-SHA1 tags RFC 2202 prints\n");
    {
        u8 key_1[20];
        nya_memset(key_1, 0x0B, sizeof(key_1));
        check_hmac_sha1(arena, key_1, sizeof(key_1), (const u8*)"Hi There", 8, "b617318655057264e28bc0b6fb378c8ef146be00");

        check_hmac_sha1(arena, (const u8*)"Jefe", 4, (const u8*)"what do ya want for nothing?", 28, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

        u8 key_3[20];
        u8 data_3[50];
        nya_memset(key_3, 0xAA, sizeof(key_3));
        nya_memset(data_3, 0xDD, sizeof(data_3));
        check_hmac_sha1(arena, key_3, sizeof(key_3), data_3, sizeof(data_3), "125d7342b9ac11cd91a39af48aa17b4f63f175d3");

        u8 key_4[25];
        u8 data_4[50];
        for (u32 i = 0; i < sizeof(key_4); i++) key_4[i] = (u8)(i + 1);
        nya_memset(data_4, 0xCD, sizeof(data_4));
        check_hmac_sha1(arena, key_4, sizeof(key_4), data_4, sizeof(data_4), "4c9007f4026250c6bc8414f9bf50c86c2d7235da");

        u8 key_5[20];
        nya_memset(key_5, 0x0C, sizeof(key_5));
        check_hmac_sha1(arena, key_5, sizeof(key_5), (const u8*)"Test With Truncation", 20, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");

        u8 key_6[80];
        nya_memset(key_6, 0xAA, sizeof(key_6));
        check_hmac_sha1(
            arena,
            key_6,
            sizeof(key_6),
            (const u8*)"Test Using Larger Than Block-Size Key - Hash Key First",
            54,
            "aa4ae5e15272d00e95705637ce8a3b55ed402112"
        );

        NYA_ConstCString data_7 = "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data";
        check_hmac_sha1(arena, key_6, sizeof(key_6), (const u8*)data_7, strlen(data_7), "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");

        printf("  all seven published tags matched\n");
    }

    printf("TEST: BLAKE2b, RFC 7693 appendices A and E\n");
    {
        u8 digest[NYA_CRYPTO_BLAKE2B_BYTES_MAX] = { 0 };
        nya_crypto_blake2b((const u8*)"abc", 3, digest, sizeof(digest));
        check_hex(
            arena,
            "blake2b-512",
            digest,
            sizeof(digest),
            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"
        );

        /* Appendix E: every output length in {20, 32, 48, 64} over every input length in {0, 3, 128, 129, 255, 1024}, unkeyed and keyed, hashed together with BLAKE2b-256. One published answer covers forty eight hashes, both block boundaries and every key length the loop reaches. */
        const u64 output_sizes[] = { 20, 32, 48, 64 };
        const u64 input_sizes[]  = { 0, 3, 128, 129, 255, 1024 };

        u8  input[1024];
        u8  key[64];
        u8  results[2 * 6 * (20 + 32 + 48 + 64)];
        u64 at = 0;

        for (u32 i = 0; i < nya_carray_length(output_sizes); i++) {
            for (u32 j = 0; j < nya_carray_length(input_sizes); j++) {
                u64 out_size = output_sizes[i];
                u64 in_size  = input_sizes[j];

                selftest_sequence(input, in_size, (u32)in_size);
                nya_crypto_blake2b(input, in_size, results + at, out_size);
                at += out_size;

                selftest_sequence(key, out_size, (u32)out_size);
                nya_crypto_blake2b_keyed(key, out_size, input, in_size, results + at, out_size);
                at += out_size;
            }
        }

        nya_assert(at == sizeof(results));

        u8 grand[32] = { 0 };
        nya_crypto_blake2b(results, sizeof(results), grand, sizeof(grand));
        check_hex(arena, "blake2b self-test", grand, sizeof(grand), "c23a7800d98123bd10f506c61e29da5603d763b8bbad2e737f5e765a7bccd475");

        printf("  appendix A and the forty eight hash self-test matched\n");
    }

    printf("TEST: the laws of the streaming form and of the MACs\n");
    {
        failures += nya_property_check("sha256 in any pieces is sha256 whole", CASES, SEED, law_sha256_pieces_agree);
        failures += nya_property_check("one flipped bit changes both hmacs", CASES, SEED, law_hmac_notices_a_flipped_bit);

        printf("  both laws held over %d cases\n", CASES);
    }

    printf("%s: test_hash (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
