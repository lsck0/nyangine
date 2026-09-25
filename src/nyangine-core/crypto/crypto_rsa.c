#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/crypto/crypto_rsa.h"
#include "nyangine-core/crypto/crypto_secret.h"

// PRIVATE TYPES

/** One number, little endian by limb (`limbs[0]` least significant), one fixed size since every value is reduced modulo the key. */
typedef struct {
    u64 limbs[NYA_CRYPTO_RSA_MAX_LIMBS * 2];
} _NYA_CryptoBig;

/** The DigestInfo a PKCS#1 v1.5 SHA-256 signature carries before the hash, a constant to compare against rather than parse. */
NYA_INTERNAL const u8 _NYA_CRYPTO_RSA_SHA256_PREFIX[] = {
    0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};

// PRIVATE API DECLARATION

// The four functions below wrap on purpose: modular arithmetic is wrapping arithmetic.

/** Whether `a` is less than `b`, over `limbs` limbs. */
NYA_INTERNAL b8 _nya_crypto_big_less(const u64* a, const u64* b, u32 limbs) __attr_no_discard;

/** `out = a - b` over `limbs` limbs, and the borrow that came out of the top. */
NYA_INTERNAL u64 _nya_crypto_big_subtract(OUT u64* out, const u64* a, const u64* b, u32 limbs);

/** `value = value * 2 mod modulus`, which is the one reduction this needs that is not Montgomery's. */
NYA_INTERNAL void _nya_crypto_big_double_mod(u64* value, const u64* modulus, u32 limbs);

/** The inverse of `-modulus[0]` modulo 2^64 that Montgomery reduction needs, by Newton's 2-adic iteration (five steps). */
NYA_INTERNAL u64 _nya_crypto_big_inverse64(u64 value) __attr_no_discard;

/** `out = a * b * R^-1 mod modulus`, CIOS Montgomery multiplication; not constant time, as every number here is public. */
NYA_INTERNAL void _nya_crypto_big_montgomery(OUT u64* out, const u64* a, const u64* b, const u64* modulus, u64 inverse, u32 limbs);

/** `out = base ^ exponent mod modulus`, for the small odd exponent a public key carries. */
NYA_INTERNAL void _nya_crypto_big_power_mod(OUT u64* out, const u64* base, u64 exponent, const u64* modulus, u32 limbs);

// PUBLIC API IMPLEMENTATION

NYA_Error nya_crypto_rsa_public_key_from_parts(const u8* modulus, u64 modulus_size, const u8* exponent, u64 exponent_size,
                                               NYA_CryptoRsaPublicKey* out_key) {
    nya_assert(out_key != nullptr);

    nya_memset(out_key, 0, sizeof(NYA_CryptoRsaPublicKey));

    if (modulus == nullptr || exponent == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a public key is two numbers, and one is missing");

    // Both formats (a JWKS member, a certificate's SubjectPublicKeyInfo) may carry a leading zero byte.
    while (modulus_size > 0 && modulus[0] == 0) {
        modulus++;
        modulus_size--;
    }

    while (exponent_size > 0 && exponent[0] == 0) {
        exponent++;
        exponent_size--;
    }

    if (modulus_size == 0 || exponent_size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a public key is two numbers, and one is zero");
    if (exponent_size > 8) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that public exponent is larger than this verifies with");
    if (modulus_size > NYA_CRYPTO_RSA_MAX_BYTES) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that key is larger than %d bits", NYA_CRYPTO_RSA_MAX_BITS);

    // An even modulus is not an RSA modulus and is also the one input Montgomery reduction cannot take.
    if ((modulus[modulus_size - 1] & 1U) == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an RSA modulus is odd");

    u64 value = 0;
    for (u64 index = 0; index < exponent_size; index++) value = (value << 8) | exponent[index];

    // Reject exponents <= 2 or even: one is the identity, two is unusable.
    if (value <= 2 || (value & 1U) == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a public exponent is odd and greater than two");

    out_key->exponent = value;
    out_key->bytes    = (u32)modulus_size;
    out_key->limbs    = (u32)((modulus_size + 7) / 8);

    // big endian bytes into little endian limbs, which is the whole of the conversion.
    for (u64 index = 0; index < modulus_size; index++) {
        u64 byte  = modulus[modulus_size - 1 - index];
        u32 limb  = (u32)(index / 8);
        u32 shift = (u32)((index % 8) * 8);

        out_key->modulus[limb] |= byte << shift;
    }

    u64 top = out_key->modulus[out_key->limbs - 1];

    out_key->bits = (out_key->limbs - 1) * 64;
    while (top > 0) {
        out_key->bits += 1;
        top          >>= 1;
    }

    if (out_key->bits < NYA_CRYPTO_RSA_MIN_BITS) {
        nya_memset(out_key, 0, sizeof(NYA_CryptoRsaPublicKey));

        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a %u bit key is below the %d bit floor and is refused rather than verified with",
                         out_key->bits, NYA_CRYPTO_RSA_MIN_BITS);
    }

    return NYA_OK;
}

b8 nya_crypto_rsa_verify_sha256(const NYA_CryptoRsaPublicKey* key, const u8* message, u64 message_size, const u8* signature, u64 signature_size) {
    nya_assert(key != nullptr);
    nya_assert(message != nullptr || message_size == 0);
    nya_assert(signature != nullptr || signature_size == 0);

    if (key->limbs == 0 || key->bits < NYA_CRYPTO_RSA_MIN_BITS) return false;

    // A signature is exactly as long as the modulus; a short one left-padded would get several spellings.
    if (signature_size != key->bytes) return false;

    _NYA_CryptoBig encrypted = { 0 };

    for (u64 index = 0; index < signature_size; index++) {
        u64 byte  = signature[signature_size - 1 - index];
        u32 limb  = (u32)(index / 8);
        u32 shift = (u32)((index % 8) * 8);

        encrypted.limbs[limb] |= byte << shift;
    }

    // s must be less than n; a larger one is a different number that reduces to the same place.
    if (!_nya_crypto_big_less(encrypted.limbs, key->modulus, key->limbs)) return false;

    _NYA_CryptoBig recovered = { 0 };
    _nya_crypto_big_power_mod(recovered.limbs, encrypted.limbs, key->exponent, key->modulus, key->limbs);

    // The padding built here and compared whole (never parsed): 0x00 0x01, 0xFF fill, 0x00, DigestInfo, hash.
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(message, message_size, &digest);

    u8 expected[NYA_CRYPTO_RSA_MAX_BYTES] = { 0 };

    u64 tail = sizeof(_NYA_CRYPTO_RSA_SHA256_PREFIX) + sizeof(digest.bytes);

    // Eleven is the standard's own floor: two bytes of prefix, eight of padding and the separator.
    if (key->bytes < tail + 11) return false;

    u64 at = 0;

    expected[at++] = 0x00;
    expected[at++] = 0x01;

    while (at < key->bytes - tail - 1) expected[at++] = 0xFF;

    expected[at++] = 0x00;

    nya_memcpy(expected + at, _NYA_CRYPTO_RSA_SHA256_PREFIX, sizeof(_NYA_CRYPTO_RSA_SHA256_PREFIX));
    at += sizeof(_NYA_CRYPTO_RSA_SHA256_PREFIX);

    nya_memcpy(expected + at, digest.bytes, sizeof(digest.bytes));

    u8 actual[NYA_CRYPTO_RSA_MAX_BYTES] = { 0 };

    for (u64 index = 0; index < key->bytes; index++) {
        u32 limb  = (u32)(index / 8);
        u32 shift = (u32)((index % 8) * 8);

        actual[key->bytes - 1 - index] = (u8)((recovered.limbs[limb] >> shift) & 0xFFU);
    }

    // In constant time so a forger cannot measure the comparison a byte at a time.
    return nya_crypto_equals(actual, expected, key->bytes);
}

// PRIVATE API IMPLEMENTATION

b8 _nya_crypto_big_less(const u64* a, const u64* b, u32 limbs) {
    for (u32 index = limbs; index > 0; index--) {
        if (a[index - 1] != b[index - 1]) return a[index - 1] < b[index - 1];
    }

    return false;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 _nya_crypto_big_subtract(u64* out, const u64* a, const u64* b, u32 limbs) {
    u64 borrow = 0;

    for (u32 index = 0; index < limbs; index++) {
        u64 left  = a[index];
        u64 right = b[index];

        u64 difference = left - right - borrow;

        // The borrow out, as the comparison that produces it: one when the subtraction wrapped.
        borrow = (left < right + borrow) || (right + borrow < right) ? 1 : 0;

        out[index] = difference;
    }

    return borrow;
}

void _nya_crypto_big_double_mod(u64* value, const u64* modulus, u32 limbs) {
    u64 carry = 0;

    for (u32 index = 0; index < limbs; index++) {
        u64 limb = value[index];

        value[index] = (limb << 1) | carry;
        carry        = limb >> 63;
    }

    // The doubling may have carried above the top limb, and then the result is certainly above the modulus.
    if (carry != 0 || !_nya_crypto_big_less(value, modulus, limbs)) (void)_nya_crypto_big_subtract(value, value, modulus, limbs);
}

__attr_no_sanitize("unsigned-integer-overflow") u64 _nya_crypto_big_inverse64(u64 value) {
    u64 inverse = value;

    // x * v congruent to 1 mod 2^k, k doubling each step (3, 6, 12, 24, 48, 96): five steps clears 64.
    for (u32 step = 0; step < 5; step++) inverse *= 2 - (value * inverse);

    return inverse;
}

__attr_no_sanitize("unsigned-integer-overflow") void _nya_crypto_big_montgomery(u64* out, const u64* a, const u64* b, const u64* modulus, u64 inverse, u32 limbs) {
    u64 product[(NYA_CRYPTO_RSA_MAX_LIMBS * 2) + 2] = { 0 };

    for (u32 i = 0; i < limbs; i++) {
        u64 carry = 0;

        // a[i] * b, accumulated into the running product.
        for (u32 j = 0; j < limbs; j++) {
            u128 sum = (u128)a[i] * (u128)b[j] + (u128)product[j] + (u128)carry;

            product[j] = (u64)sum;
            carry      = (u64)(sum >> 64);
        }

        u128 top = (u128)product[limbs] + (u128)carry;

        product[limbs]     = (u64)top;
        product[limbs + 1] = (u64)(top >> 64);

        // The reduction: a multiple of the modulus that clears the bottom limb, making the shift below the division.
        u64 m     = product[0] * inverse;
        u64 carry2 = 0;

        for (u32 j = 0; j < limbs; j++) {
            u128 sum = (u128)m * (u128)modulus[j] + (u128)product[j] + (u128)carry2;

            product[j] = (u64)sum;
            carry2     = (u64)(sum >> 64);
        }

        u128 rest = (u128)product[limbs] + (u128)carry2;

        product[limbs]      = (u64)rest;
        product[limbs + 1] += (u64)(rest >> 64);

        // Shift down by one limb, which is the division by the radix the reduction just earned.
        for (u32 j = 0; j <= limbs; j++) product[j] = product[j + 1];

        product[limbs + 1] = 0;
    }

    // At most one subtraction: Montgomery's result is below twice the modulus by construction.
    if (product[limbs] != 0 || !_nya_crypto_big_less(product, modulus, limbs)) {
        (void)_nya_crypto_big_subtract(product, product, modulus, limbs);
    }

    for (u32 index = 0; index < limbs; index++) out[index] = product[index];
}

__attr_no_sanitize("unsigned-integer-overflow") void _nya_crypto_big_power_mod(u64* out, const u64* base, u64 exponent, const u64* modulus, u32 limbs) {
    u64 inverse = _nya_crypto_big_inverse64(0 - modulus[0]);

    // R^2 mod n by doubling one 128*limbs times (R is 2^(64*limbs)).
    u64 r2[NYA_CRYPTO_RSA_MAX_LIMBS * 2] = { 0 };

    r2[0] = 1;

    for (u32 step = 0; step < 128 * limbs; step++) _nya_crypto_big_double_mod(r2, modulus, limbs);

    // into Montgomery form, where the multiply above is the only multiply there is.
    u64 value[NYA_CRYPTO_RSA_MAX_LIMBS * 2]  = { 0 };
    u64 result[NYA_CRYPTO_RSA_MAX_LIMBS * 2] = { 0 };

    _nya_crypto_big_montgomery(value, base, r2, modulus, inverse, limbs);

    // one, in Montgomery form, which is R mod n — and R mod n is what one times R^2 reduces to.
    u64 one[NYA_CRYPTO_RSA_MAX_LIMBS * 2] = { 0 };
    one[0]                                = 1;

    _nya_crypto_big_montgomery(result, one, r2, modulus, inverse, limbs);

    // Square and multiply from the top bit down; the exponent is public (65537), so no need to walk every bit.
    u32 highest = 63;
    while (highest > 0 && ((exponent >> highest) & 1ULL) == 0) highest--;

    for (u32 bit = highest + 1; bit > 0; bit--) {
        u64 squared[NYA_CRYPTO_RSA_MAX_LIMBS * 2] = { 0 };

        _nya_crypto_big_montgomery(squared, result, result, modulus, inverse, limbs);

        for (u32 index = 0; index < limbs; index++) result[index] = squared[index];

        if (((exponent >> (bit - 1)) & 1ULL) == 0) continue;

        u64 multiplied[NYA_CRYPTO_RSA_MAX_LIMBS * 2] = { 0 };

        _nya_crypto_big_montgomery(multiplied, result, value, modulus, inverse, limbs);

        for (u32 index = 0; index < limbs; index++) result[index] = multiplied[index];
    }

    // and back out of Montgomery form, which is one more reduction against one.
    nya_memset(one, 0, sizeof(one));
    one[0] = 1;

    _nya_crypto_big_montgomery(out, result, one, modulus, inverse, limbs);
}
