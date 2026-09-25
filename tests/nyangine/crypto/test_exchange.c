/**
 * X25519: crypto_exchange.h. RFC 7748's vectors, section 5.2's scalar multiplications and iterations and
 * section 6.1's Alice and Bob; then the public keys of low order a hostile peer sends to force a shared
 * secret everyone knows, each of which must be refused; then agreement as a law.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** Cases per law. Each is four scalar multiplications, so fewer than the cheap laws get. */
#define CASES 300

/** Fixed, so the suite is the same run every time. */
#define SEED 0x7832353531392121ULL

/* HELPERS */

static u8 hex_digit(char c) {
    if (c >= '0' && c <= '9') return (u8)(c - '0');
    if (c >= 'a' && c <= 'f') return (u8)(c - 'a' + 10);
    nya_unreachable();
}

static void from_hex(NYA_ConstCString hex, OUT u8* out, u64 size) {
    nya_assert(strlen(hex) == size * 2, "a %zu digit vector for %llu bytes", strlen(hex), (unsigned long long)size);

    for (u64 i = 0; i < size; i++) out[i] = (u8)((hex_digit(hex[i * 2]) << 4) | hex_digit(hex[(i * 2) + 1]));
}

static void check_bytes(const u8* got, NYA_ConstCString expected_hex, NYA_ConstCString what) {
    u8 expected[NYA_CRYPTO_EXCHANGE_KEY_BYTES] = { 0 };
    from_hex(expected_hex, expected, sizeof(expected));

    nya_assert(nya_memcmp(got, expected, sizeof(expected)) == 0, "%s is not the published value", what);
}

/** RFC 7748's X25519(k, u), through the typed API: `k` as a secret key and `u` as a public one. */
static b8 x25519(const u8 k[NYA_CRYPTO_EXCHANGE_KEY_BYTES], const u8 u[NYA_CRYPTO_EXCHANGE_KEY_BYTES], OUT u8 out[NYA_CRYPTO_EXCHANGE_KEY_BYTES]) {
    NYA_CryptoExchangeSecretKey secret = { 0 };
    NYA_CryptoExchangePublicKey public = { 0 };
    NYA_CryptoSharedSecret      shared = { 0 };

    nya_memcpy(secret.bytes, k, NYA_CRYPTO_EXCHANGE_KEY_BYTES);
    nya_memcpy(public.bytes, u, NYA_CRYPTO_EXCHANGE_KEY_BYTES);

    b8 accepted = nya_crypto_exchange(&secret, &public, &shared);
    nya_memcpy(out, shared.bytes, NYA_CRYPTO_EXCHANGE_KEY_BYTES);

    return accepted;
}

/* LAWS */

/** Two pairs from any secrets arrive at the same shared secret from either side. */
static b8 law_both_sides_agree(NYA_Property* property) {
    NYA_CryptoExchangeSecretKey alice_secret = { 0 };
    NYA_CryptoExchangeSecretKey bob_secret   = { 0 };
    nya_property_draw_bytes(property, alice_secret.bytes, sizeof(alice_secret.bytes));
    nya_property_draw_bytes(property, bob_secret.bytes, sizeof(bob_secret.bytes));

    NYA_CryptoExchangeKeyPair alice = { 0 };
    NYA_CryptoExchangeKeyPair bob   = { 0 };
    nya_crypto_exchange_key_pair_from_secret(&alice_secret, &alice);
    nya_crypto_exchange_key_pair_from_secret(&bob_secret, &bob);

    NYA_CryptoSharedSecret from_alice = { 0 };
    NYA_CryptoSharedSecret from_bob   = { 0 };
    b8                     alice_ok   = nya_crypto_exchange(&alice.secret_key, &bob.public_key, &from_alice);
    b8                     bob_ok     = nya_crypto_exchange(&bob.secret_key, &alice.public_key, &from_bob);

    return alice_ok && bob_ok && nya_crypto_equals(from_alice.bytes, from_bob.bytes, sizeof(from_alice.bytes));
}

/* TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    printf("TEST: the scalar multiplications RFC 7748 section 5.2 prints\n");
    {
        u8 k[32];
        u8 u[32];
        u8 out[32];

        from_hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, sizeof(k));
        from_hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, sizeof(u));
        nya_assert(x25519(k, u, out));
        check_bytes(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", "the first vector");

        // the u-coordinate's top bit is set, which RFC 7748 says to mask rather than reject.
        from_hex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k, sizeof(k));
        from_hex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u, sizeof(u));
        nya_assert(x25519(k, u, out));
        check_bytes(out, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", "the second vector");

        printf("  both matched\n");
    }

    printf("TEST: the iterations RFC 7748 section 5.2 prints\n");
    {
        // k and u start as the base point; each round k becomes X25519(k, u) and u the old k.
        u8 k[32] = { 9 };
        u8 u[32] = { 9 };

        for (u32 round = 1; round <= 1000; round++) {
            u8 next[32];
            nya_assert(x25519(k, u, next));
            nya_memcpy(u, k, sizeof(u));
            nya_memcpy(k, next, sizeof(k));

            if (round == 1) check_bytes(k, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", "one iteration");
        }

        check_bytes(k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", "a thousand iterations");

        // the million iteration vector is left out: minutes under the sanitizers, for no new coverage.
        printf("  one and a thousand iterations matched\n");
    }

    printf("TEST: Alice and Bob, RFC 7748 section 6.1\n");
    {
        NYA_CryptoExchangeSecretKey alice_secret = { 0 };
        NYA_CryptoExchangeSecretKey bob_secret   = { 0 };
        from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice_secret.bytes, sizeof(alice_secret.bytes));
        from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob_secret.bytes, sizeof(bob_secret.bytes));

        NYA_CryptoExchangeKeyPair alice = { 0 };
        NYA_CryptoExchangeKeyPair bob   = { 0 };
        nya_crypto_exchange_key_pair_from_secret(&alice_secret, &alice);
        nya_crypto_exchange_key_pair_from_secret(&bob_secret, &bob);

        check_bytes(alice.public_key.bytes, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", "Alice's public key");
        check_bytes(bob.public_key.bytes, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", "Bob's public key");
        nya_assert(nya_memcmp(alice.secret_key.bytes, alice_secret.bytes, sizeof(alice_secret.bytes)) == 0, "the pair keeps the secret it came from");

        NYA_CryptoSharedSecret from_alice = { 0 };
        NYA_CryptoSharedSecret from_bob   = { 0 };
        nya_assert(nya_crypto_exchange(&alice.secret_key, &bob.public_key, &from_alice));
        nya_assert(nya_crypto_exchange(&bob.secret_key, &alice.public_key, &from_bob));

        check_bytes(from_alice.bytes, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "the shared secret, from Alice");
        check_bytes(from_bob.bytes, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "the shared secret, from Bob");

        nya_crypto_exchange_key_pair_destroy(&alice);
        nya_assert(nya_is_zeroed(alice), "a destroyed pair is wiped");

        // destroying twice, and destroying nothing, are both no-ops.
        nya_crypto_exchange_key_pair_destroy(&alice);
        nya_crypto_exchange_key_pair_destroy(nullptr);

        printf("  both public keys and the shared secret matched from both sides\n");
    }

    printf("TEST: public keys of low order are refused\n");
    {
        NYA_CryptoExchangeKeyPair mine = { 0 };
        nya_assert(nya_crypto_exchange_key_pair_create(&mine).ok);
        defer nya_crypto_exchange_key_pair_destroy(&mine);

        /* Points of order dividing the cofactor, the list published with Curve25519 (cr.yp.to/ecdh.html) and used by every library's test suite: 0, 1, the two of order 8, p - 1, p and p + 1. Each sends the shared secret to zero whatever our secret key is. */
        NYA_ConstCString low_order[] = {
            "0000000000000000000000000000000000000000000000000000000000000000", "0100000000000000000000000000000000000000000000000000000000000000",
            "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800", "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f", "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        };

        for (u32 i = 0; i < nya_carray_length(low_order); i++) {
            NYA_CryptoExchangePublicKey hostile = { 0 };
            from_hex(low_order[i], hostile.bytes, sizeof(hostile.bytes));

            NYA_CryptoSharedSecret shared = { 0 };
            nya_assert(!nya_crypto_exchange(&mine.secret_key, &hostile, &shared), "low order point %u was accepted", i);
            nya_assert(nya_is_zeroed(shared), "a refused exchange leaves a zero secret");
        }

        printf("  all %zu refused\n", nya_carray_length(low_order));
    }

    printf("TEST: the laws of agreement\n");
    {
        failures += nya_property_check("both sides of an exchange agree", CASES, SEED, law_both_sides_agree);

        printf("  held over %d cases\n", CASES);
    }

    printf("%s: test_exchange (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
