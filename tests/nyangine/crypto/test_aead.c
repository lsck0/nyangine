/**
 * XChaCha20-Poly1305: crypto_aead.h. The published vector from draft-irtf-cfrg-xchacha appendix A.3.1,
 * which is RFC 8439's AEAD behind an HChaCha20 subkey; then everything an attacker can alter, each of
 * which must be refused with the bytes left as they came; then the round trip and the refusal as laws.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** Cases per law. */
#define CASES 2000

/** Fixed, so the suite is the same run every time. */
#define SEED 0x6165616421212121ULL

/* HELPERS */

static u8 hex_digit(char c) {
    if (c >= '0' && c <= '9') return (u8)(c - '0');
    if (c >= 'a' && c <= 'f') return (u8)(c - 'a' + 10);
    nya_unreachable();
}

/** `hex` into exactly `size` bytes; a vector of the wrong length is a typo in this file. */
static void from_hex(NYA_ConstCString hex, OUT u8* out, u64 size) {
    nya_assert(strlen(hex) == size * 2, "a %zu digit vector for %llu bytes", strlen(hex), (unsigned long long)size);

    for (u64 i = 0; i < size; i++) out[i] = (u8)((hex_digit(hex[i * 2]) << 4) | hex_digit(hex[(i * 2) + 1]));
}

/* LAWS */

typedef struct {
    NYA_CryptoKey32   key;
    NYA_CryptoNonce24 nonce;
    u8                text[256];
    u32               text_size;
    u8                associated[64];
    u32               associated_size;
} Sealed;

static void draw_sealed(NYA_Property* property, OUT Sealed* sealed) {
    nya_property_draw_bytes(property, sealed->key.bytes, sizeof(sealed->key.bytes));
    nya_property_draw_bytes(property, sealed->nonce.bytes, sizeof(sealed->nonce.bytes));

    sealed->text_size       = (u32)nya_property_draw_below(property, sizeof(sealed->text) + 1);
    sealed->associated_size = (u32)nya_property_draw_below(property, sizeof(sealed->associated) + 1);
    nya_property_draw_bytes(property, sealed->text, sealed->text_size);
    nya_property_draw_bytes(property, sealed->associated, sealed->associated_size);
}

static NYA_CryptoAeadMessage message_of(Sealed* sealed) {
    NYA_CryptoAeadMessage message = {
        .text            = sealed->text,
        .text_size       = sealed->text_size,
        .associated      = sealed->associated,
        .associated_size = sealed->associated_size,
    };

    return message;
}

/** Decrypting what was encrypted, under the same key, nonce and associated data, gives the text back. */
static b8 law_round_trip(NYA_Property* property) {
    Sealed sealed = { 0 };
    draw_sealed(property, &sealed);

    u8 original[sizeof(sealed.text)];
    nya_memcpy(original, sealed.text, sealed.text_size);

    NYA_CryptoTag16 tag = { 0 };
    nya_crypto_aead_encrypt(&sealed.key, &sealed.nonce, message_of(&sealed), &tag);

    b8 opened = nya_crypto_aead_decrypt(&sealed.key, &sealed.nonce, message_of(&sealed), &tag);

    nya_property_note(property, "%u bytes of text, %u associated", sealed.text_size, sealed.associated_size);
    return opened && nya_memcmp(original, sealed.text, sealed.text_size) == 0;
}

/**
 * One flipped bit anywhere an attacker reaches, ciphertext, associated data or tag, is refused, and the
 * ciphertext is left exactly as it was.
 * */
static b8 law_any_flipped_bit_is_refused(NYA_Property* property) {
    Sealed sealed = { 0 };
    draw_sealed(property, &sealed);

    NYA_CryptoTag16 tag = { 0 };
    nya_crypto_aead_encrypt(&sealed.key, &sealed.nonce, message_of(&sealed), &tag);

    u64 bits = ((u64)sealed.text_size + sealed.associated_size + sizeof(tag.bytes)) * 8;
    u64 bit  = nya_property_draw_below(property, bits);
    u64 byte = bit / 8;
    u8  mask = (u8)(1U << (bit % 8));

    if (byte < sealed.text_size) {
        sealed.text[byte] ^= mask;
    } else if (byte < (u64)sealed.text_size + sealed.associated_size) {
        sealed.associated[byte - sealed.text_size] ^= mask;
    } else {
        tag.bytes[byte - sealed.text_size - sealed.associated_size] ^= mask;
    }

    u8 before[sizeof(sealed.text)];
    nya_memcpy(before, sealed.text, sealed.text_size);

    b8 opened = nya_crypto_aead_decrypt(&sealed.key, &sealed.nonce, message_of(&sealed), &tag);

    nya_property_note(property, "bit " FMTu64 " of " FMTu64 " was accepted or the text was touched", bit, bits);
    return !opened && nya_memcmp(before, sealed.text, sealed.text_size) == 0;
}

/* TESTS */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    // draft-irtf-cfrg-xchacha-03, appendix A.3.1.
    NYA_ConstCString plaintext_hex =
        "4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a204966204920636f756c64206f6666657220796f75206f"
        "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73637265656e20776f756c642062652069742e";
    NYA_ConstCString ciphertext_hex =
        "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b452"
        "2f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff921f9664c97637da9768812f615c68b13b52e";

    u8                plaintext[114]  = { 0 };
    u8                ciphertext[114] = { 0 };
    u8                associated[12]  = { 0 };
    NYA_CryptoKey32   key             = { 0 };
    NYA_CryptoNonce24 nonce           = { 0 };
    NYA_CryptoTag16   expected_tag    = { 0 };

    from_hex(plaintext_hex, plaintext, sizeof(plaintext));
    from_hex(ciphertext_hex, ciphertext, sizeof(ciphertext));
    from_hex("50515253c0c1c2c3c4c5c6c7", associated, sizeof(associated));
    from_hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key.bytes, sizeof(key.bytes));
    from_hex("404142434445464748494a4b4c4d4e4f5051525354555657", nonce.bytes, sizeof(nonce.bytes));
    from_hex("c0875924c1c7987947deafd8780acf49", expected_tag.bytes, sizeof(expected_tag.bytes));

    printf("TEST: the vector draft-irtf-cfrg-xchacha prints\n");
    {
        u8 text[114];
        nya_memcpy(text, plaintext, sizeof(text));

        NYA_CryptoTag16 tag = { 0 };
        nya_crypto_aead_encrypt(
            &key,
            &nonce,
            (NYA_CryptoAeadMessage){ .text = text, .text_size = sizeof(text), .associated = associated, .associated_size = sizeof(associated) },
            &tag
        );

        nya_assert(nya_memcmp(text, ciphertext, sizeof(text)) == 0, "the ciphertext is not the published one");
        nya_assert(nya_memcmp(tag.bytes, expected_tag.bytes, sizeof(tag.bytes)) == 0, "the tag is not the published one");

        nya_assert(nya_crypto_aead_decrypt(
            &key,
            &nonce,
            (NYA_CryptoAeadMessage){ .text = text, .text_size = sizeof(text), .associated = associated, .associated_size = sizeof(associated) },
            &tag
        ));
        nya_assert(nya_memcmp(text, plaintext, sizeof(text)) == 0, "decrypting the published ciphertext did not give the plaintext");

        printf("  the ciphertext and the tag matched, and the ciphertext decrypted\n");
    }

    printf("TEST: every altered input is refused and the text is left alone\n");
    {
        NYA_CryptoAeadMessage message = { .text            = nullptr,
                                          .text_size       = sizeof(ciphertext),
                                          .associated      = associated,
                                          .associated_size = sizeof(associated) };

        u8 text[114];

        // the ciphertext, first byte and last.
        u64 positions[] = { 0, sizeof(text) - 1 };
        for (u32 i = 0; i < nya_carray_length(positions); i++) {
            nya_memcpy(text, ciphertext, sizeof(text));
            text[positions[i]] ^= 0x01U;

            u8 altered[114];
            nya_memcpy(altered, text, sizeof(text));

            message.text = text;
            nya_assert(!nya_crypto_aead_decrypt(&key, &nonce, message, &expected_tag), "an altered ciphertext was accepted");
            nya_assert(nya_memcmp(text, altered, sizeof(text)) == 0, "a refused ciphertext was touched");
        }

        // the tag.
        nya_memcpy(text, ciphertext, sizeof(text));
        NYA_CryptoTag16 tag                  = expected_tag;
        tag.bytes[NYA_CRYPTO_TAG_BYTES - 1] ^= 0x80U;
        nya_assert(!nya_crypto_aead_decrypt(&key, &nonce, message, &tag), "an altered tag was accepted");

        // the associated data, which travels in the clear and is exactly what a header rewrite changes.
        u8 other_associated[12];
        nya_memcpy(other_associated, associated, sizeof(associated));
        other_associated[0]              ^= 0x01U;
        NYA_CryptoAeadMessage relabelled  = message;
        relabelled.associated             = other_associated;
        nya_assert(!nya_crypto_aead_decrypt(&key, &nonce, relabelled, &expected_tag), "altered associated data was accepted");

        // dropped associated data, which a sloppy parser might hand over as empty.
        NYA_CryptoAeadMessage stripped = message;
        stripped.associated            = nullptr;
        stripped.associated_size       = 0;
        nya_assert(!nya_crypto_aead_decrypt(&key, &nonce, stripped, &expected_tag), "missing associated data was accepted");

        // a truncated ciphertext.
        NYA_CryptoAeadMessage truncated = message;
        truncated.text_size             = sizeof(text) - 1;
        nya_assert(!nya_crypto_aead_decrypt(&key, &nonce, truncated, &expected_tag), "a truncated ciphertext was accepted");

        // the nonce and the key, which is a replay under another counter or a message for someone else.
        NYA_CryptoNonce24 other_nonce                  = nonce;
        other_nonce.bytes[NYA_CRYPTO_NONCE_BYTES - 1] ^= 0x01U;
        nya_assert(!nya_crypto_aead_decrypt(&key, &other_nonce, message, &expected_tag), "another nonce was accepted");

        NYA_CryptoKey32 other_key  = key;
        other_key.bytes[0]        ^= 0x01U;
        nya_assert(!nya_crypto_aead_decrypt(&other_key, &nonce, message, &expected_tag), "another key was accepted");

        nya_assert(nya_memcmp(text, ciphertext, sizeof(text)) == 0, "a refusal touched the ciphertext");

        printf("  ciphertext, tag, associated data, truncation, nonce and key all refused\n");
    }

    printf("TEST: a tag over associated data alone\n");
    {
        NYA_CryptoTag16       tag    = { 0 };
        NYA_CryptoAeadMessage header = { .associated = associated, .associated_size = sizeof(associated) };

        nya_crypto_aead_encrypt(&key, &nonce, header, &tag);
        nya_assert(nya_crypto_aead_decrypt(&key, &nonce, header, &tag));

        u8 other_associated[12];
        nya_memcpy(other_associated, associated, sizeof(associated));
        other_associated[5] ^= 0x10U;
        header.associated    = other_associated;
        nya_assert(!nya_crypto_aead_decrypt(&key, &nonce, header, &tag), "a header tag accepted another header");

        printf("  checked, and refused once the header changed\n");
    }

    printf("TEST: nonces\n");
    {
        NYA_CryptoNonce24 counted = nya_crypto_nonce_from_counter(0x0102030405060708ULL);

        u8 expected[NYA_CRYPTO_NONCE_BYTES] = { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
        nya_assert(nya_memcmp(counted.bytes, expected, sizeof(expected)) == 0, "a counter is little endian in the first eight bytes, zero after");

        NYA_CryptoNonce24 first  = { 0 };
        NYA_CryptoNonce24 second = { 0 };
        nya_assert(nya_crypto_nonce_random(&first).ok);
        nya_assert(nya_crypto_nonce_random(&second).ok);

        // two draws of 192 bits agree with probability 2^-192; agreement here means a broken source.
        nya_assert(nya_memcmp(first.bytes, second.bytes, sizeof(first.bytes)) != 0, "two random nonces were equal");

        printf("  a counter lays out as the network protocol expects, and random ones differ\n");
    }

    printf("TEST: the laws of encryption\n");
    {
        failures += nya_property_check("decrypt undoes encrypt", CASES, SEED, law_round_trip);
        failures += nya_property_check("any flipped bit is refused and touches nothing", CASES, SEED, law_any_flipped_bit_is_refused);

        printf("  both laws held over %d cases\n", CASES);
    }

    printf("%s: test_aead (%u failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
