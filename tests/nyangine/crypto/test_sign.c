/**
 * Ed25519: crypto_sign.h. Every Ed25519 vector RFC 8032 section 7.1 prints, from the empty message to the
 * 1023 byte one; then every alteration of message, signature and key, each of which must be refused,
 * including a signature whose scalar is not reduced; then signing and refusing as laws.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Cases per law. Each signs and verifies, so fewer than the cheap laws get. */
#define CASES 300

/** Fixed, so the suite is the same run every time. */
#define SEED 0x6564323535313921ULL

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VECTORS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_ConstCString name;
    NYA_ConstCString seed;
    NYA_ConstCString public_key;
    NYA_ConstCString message;
    NYA_ConstCString signature;
} Vector;

/** RFC 8032 section 7.1, extracted from the RFC's text rather than retyped. */
static const Vector VECTORS[] = {
    {
     .name       = "TEST 1",
     .seed       = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     .public_key = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     .message    = "",
     .signature =
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", },
    {
     .name       = "TEST 2",
     .seed       = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     .public_key = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     .message    = "72",
     .signature =
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", },
    {
     .name       = "TEST 3",
     .seed       = "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     .public_key = "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     .message    = "af82",
     .signature =
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a", },
    {
     .name       = "TEST 1024",
     .seed       = "f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5",
     .public_key = "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e",
     .message = "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
                   "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
                   "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4feba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
                   "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbefefd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
                   "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed185ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
                   "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
                   "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
                   "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128bab27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
                   "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429ec96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
                   "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
                   "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34dff7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
                   "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
                   "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a32ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
                   "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5fb93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
                   "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
                   "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0",     .signature =
            "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03", },
    {
     .name       = "TEST SHA(abc)",
     .seed       = "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
     .public_key = "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     .message = "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     .signature =
            "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704", },
};

/**
 * The order of the base point, L = 2^252 + 27742317777372353535851937790883648493, little endian. RFC 8032
 * section 5.1 requires a verifier to refuse a signature whose S is not below it.
 * */
static const u8 GROUP_ORDER[32] = {
    0xED, 0xD3, 0xF5, 0x5C, 0x1A, 0x63, 0x12, 0x58, 0xD6, 0x9C, 0xF7, 0xA2, 0xDE, 0xF9, 0xDE, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static u8 hex_digit(char c) {
    if (c >= '0' && c <= '9') return (u8)(c - '0');
    if (c >= 'a' && c <= 'f') return (u8)(c - 'a' + 10);
    nya_unreachable();
}

static void from_hex(NYA_ConstCString hex, OUT u8* out, u64 size) {
    nya_assert(strlen(hex) == size * 2, "a %zu digit vector for %llu bytes", strlen(hex), (unsigned long long)size);

    for (u64 i = 0; i < size; i++) out[i] = (u8)((hex_digit(hex[i * 2]) << 4) | hex_digit(hex[(i * 2) + 1]));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_CryptoSignKeyPair pair;
    u8                    message[256];
    u32                   size;
    NYA_CryptoSignature   signature;
} Signed;

static void draw_signed(NYA_Property* property, OUT Signed* out) {
    NYA_CryptoKey32 seed = { 0 };
    nya_property_draw_bytes(property, seed.bytes, sizeof(seed.bytes));
    nya_crypto_sign_key_pair_from_seed(&seed, &out->pair);

    out->size = (u32)nya_property_draw_below(property, sizeof(out->message) + 1);
    nya_property_draw_bytes(property, out->message, out->size);

    nya_crypto_sign(&out->pair.secret_key, out->message, out->size, &out->signature);
}

/** Whatever is signed verifies under the signer's public key. */
static b8 law_signatures_verify(NYA_Property* property) {
    Signed signed_message = { 0 };
    draw_signed(property, &signed_message);

    nya_property_note(property, "a %u byte message", signed_message.size);
    return nya_crypto_sign_verify(&signed_message.pair.public_key, signed_message.message, signed_message.size, &signed_message.signature);
}

/** One flipped bit in the message, the signature or the public key is refused. */
static b8 law_any_flipped_bit_is_refused(NYA_Property* property) {
    Signed signed_message = { 0 };
    draw_signed(property, &signed_message);

    u64 bits = ((u64)signed_message.size + NYA_CRYPTO_SIGNATURE_BYTES + NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES) * 8;
    u64 bit  = nya_property_draw_below(property, bits);
    u64 byte = bit / 8;
    u8  mask = (u8)(1U << (bit % 8));

    if (byte < signed_message.size) {
        signed_message.message[byte] ^= mask;
    } else if (byte < (u64)signed_message.size + NYA_CRYPTO_SIGNATURE_BYTES) {
        signed_message.signature.bytes[byte - signed_message.size] ^= mask;
    } else {
        signed_message.pair.public_key.bytes[byte - signed_message.size - NYA_CRYPTO_SIGNATURE_BYTES] ^= mask;
    }

    nya_property_note(property, "bit " FMTu64 " of " FMTu64 " was accepted", bit, bits);
    return !nya_crypto_sign_verify(&signed_message.pair.public_key, signed_message.message, signed_message.size, &signed_message.signature);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TESTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_sign");
    defer      nya_arena_destroy(arena);

    u32 failures = 0;

    printf("TEST: every Ed25519 vector RFC 8032 prints\n");
    {
        for (u32 v = 0; v < nya_carray_length(VECTORS); v++) {
            const Vector* vector = &VECTORS[v];

            NYA_CryptoKey32         seed       = { 0 };
            NYA_CryptoSignPublicKey public_key = { 0 };
            NYA_CryptoSignature     expected   = { 0 };
            from_hex(vector->seed, seed.bytes, sizeof(seed.bytes));
            from_hex(vector->public_key, public_key.bytes, sizeof(public_key.bytes));
            from_hex(vector->signature, expected.bytes, sizeof(expected.bytes));

            u64 size    = strlen(vector->message) / 2;
            u8* message = size > 0 ? nya_arena_alloc(arena, size) : nullptr;
            if (size > 0) from_hex(vector->message, message, size);

            NYA_CryptoSignKeyPair pair = { 0 };
            nya_crypto_sign_key_pair_from_seed(&seed, &pair);
            nya_assert(nya_memcmp(pair.public_key.bytes, public_key.bytes, sizeof(public_key.bytes)) == 0, "%s: the public key", vector->name);

            NYA_CryptoSignature signature = { 0 };
            nya_crypto_sign(&pair.secret_key, message, size, &signature);
            nya_assert(nya_memcmp(signature.bytes, expected.bytes, sizeof(expected.bytes)) == 0, "%s: the signature", vector->name);

            nya_assert(nya_crypto_sign_verify(&public_key, message, size, &expected), "%s: the published signature did not verify", vector->name);

            nya_crypto_sign_key_pair_destroy(&pair);
            nya_assert(nya_is_zeroed(pair), "a destroyed pair is wiped");
        }

        printf("  all %zu matched: public key, signature and verification\n", nya_carray_length(VECTORS));
    }

    printf("TEST: altered signatures, messages and keys are refused\n");
    {
        const Vector* vector = &VECTORS[2];

        NYA_CryptoSignPublicKey public_key = { 0 };
        NYA_CryptoSignature     signature  = { 0 };
        u8                      message[2] = { 0 };
        from_hex(vector->public_key, public_key.bytes, sizeof(public_key.bytes));
        from_hex(vector->signature, signature.bytes, sizeof(signature.bytes));
        from_hex(vector->message, message, sizeof(message));

        nya_assert(nya_crypto_sign_verify(&public_key, message, sizeof(message), &signature));

        // R, the first half, and S, the second.
        NYA_CryptoSignature altered  = signature;
        altered.bytes[0]            ^= 0x01U;
        nya_assert(!nya_crypto_sign_verify(&public_key, message, sizeof(message), &altered), "an altered R was accepted");

        altered            = signature;
        altered.bytes[40] ^= 0x01U;
        nya_assert(!nya_crypto_sign_verify(&public_key, message, sizeof(message), &altered), "an altered S was accepted");

        // S + L is the same scalar mod L, so a verifier that skips the range check accepts it, and one
        // signature gains a second spelling. RFC 8032 section 5.1.7 says to refuse it.
        altered   = signature;
        u32 carry = 0;
        for (u32 i = 0; i < 32; i++) {
            u32 sum               = (u32)altered.bytes[32 + i] + GROUP_ORDER[i] + carry;
            altered.bytes[32 + i] = (u8)(sum & 0xFFU);
            carry                 = sum >> 8;
        }
        nya_assert(carry == 0, "S + L fits 256 bits because S is below L");
        nya_assert(!nya_crypto_sign_verify(&public_key, message, sizeof(message), &altered), "S + L was accepted");

        // the message, one bit, one byte shorter, one byte longer.
        u8 changed[3]  = { message[0], message[1], 0 };
        changed[1]    ^= 0x80U;
        nya_assert(!nya_crypto_sign_verify(&public_key, changed, 2, &signature), "an altered message was accepted");
        nya_assert(!nya_crypto_sign_verify(&public_key, message, 1, &signature), "a truncated message was accepted");
        changed[1] ^= 0x80U;
        nya_assert(!nya_crypto_sign_verify(&public_key, changed, 3, &signature), "an extended message was accepted");

        // another key, and the all zero key, which is no key at all.
        NYA_CryptoSignPublicKey other  = public_key;
        other.bytes[31]               ^= 0x01U;
        nya_assert(!nya_crypto_sign_verify(&other, message, sizeof(message), &signature), "another key was accepted");

        NYA_CryptoSignPublicKey zero = { 0 };
        nya_assert(!nya_crypto_sign_verify(&zero, message, sizeof(message), &signature), "the zero key was accepted");

        // an all zero signature under the zero key: the classic forgery against a lax verifier.
        NYA_CryptoSignature nothing = { 0 };
        nya_assert(!nya_crypto_sign_verify(&zero, message, sizeof(message), &nothing), "a zero signature under a zero key was accepted");

        /*
         * Keys of small order, whichever sign bit they carry: y = 1 is the identity, y = 0 a point of order
         * four and y = p - 1 one of order two. Under the identity, R = identity and S = 0 satisfy the
         * equation for every message, so this is the forgery at its plainest.
         */
        NYA_ConstCString small_order[] = {
            "0100000000000000000000000000000000000000000000000000000000000000", "0100000000000000000000000000000000000000000000000000000000000080",
            "0000000000000000000000000000000000000000000000000000000000000080", "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        };

        NYA_CryptoSignature identity_forgery = { 0 };
        identity_forgery.bytes[0]            = 0x01;

        for (u32 i = 0; i < nya_carray_length(small_order); i++) {
            NYA_CryptoSignPublicKey weak = { 0 };
            from_hex(small_order[i], weak.bytes, sizeof(weak.bytes));

            nya_assert(
                !nya_crypto_sign_verify(&weak, message, sizeof(message), &identity_forgery),
                "small order key %u took the identity forgery",
                i
            );
            nya_assert(!nya_crypto_sign_verify(&weak, message, sizeof(message), &nothing), "small order key %u took a zero signature", i);
        }

        printf("  R, S, S + L, message, truncation, extension, another key, and small order keys all refused\n");
    }

    printf("TEST: fresh pairs from the system's random source\n");
    {
        NYA_CryptoSignKeyPair first  = { 0 };
        NYA_CryptoSignKeyPair second = { 0 };
        nya_assert(nya_crypto_sign_key_pair_create(&first).ok);
        nya_assert(nya_crypto_sign_key_pair_create(&second).ok);
        defer nya_crypto_sign_key_pair_destroy(&first);
        defer nya_crypto_sign_key_pair_destroy(&second);

        nya_assert(nya_memcmp(first.public_key.bytes, second.public_key.bytes, sizeof(first.public_key.bytes)) != 0, "two fresh pairs were equal");

        NYA_CryptoSignature signature = { 0 };
        nya_crypto_sign(&first.secret_key, (const u8*)"hello", 5, &signature);
        nya_assert(nya_crypto_sign_verify(&first.public_key, (const u8*)"hello", 5, &signature));
        nya_assert(!nya_crypto_sign_verify(&second.public_key, (const u8*)"hello", 5, &signature), "a signature verified under another pair");

        printf("  two differ, and each verifies only its own\n");
    }

    printf("TEST: the laws of signing\n");
    {
        failures += nya_property_check("what is signed verifies", CASES, SEED, law_signatures_verify);
        failures += nya_property_check("any flipped bit is refused", CASES, SEED, law_any_flipped_bit_is_refused);

        printf("  both laws held over %d cases\n", CASES);
    }

    printf("%s: test_sign (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
