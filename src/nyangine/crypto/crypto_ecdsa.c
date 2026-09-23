#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/crypto/crypto_ecdsa.h"
#include "nyangine/crypto/crypto_hash.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE CURVE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * P-256, as four 64-bit limbs per number, least significant first. Two moduli matter and they are not
 * interchangeable: `p` is the field the points live in, `n` is the order of the group and the modulus
 * the signature's own arithmetic happens in. Mixing them is the classic way to write a verifier that
 * accepts a forgery, so every function below says which one it works in.
 *
 * The constants that end in _MONT are in Montgomery form already — value * 2^256 mod p — because that
 * is the only form the multiplication here produces, and converting them at startup would be work
 * done once per process for numbers that never change. They were computed from the curve parameters
 * in FIPS 186-4; the generator and `b` are that document's own.
 */

/** The field prime: 2^256 - 2^224 + 2^192 + 2^96 - 1. */
NYA_INTERNAL const u64 _NYA_P256_P[4] = { 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL, 0x0000000000000000ULL, 0xFFFFFFFF00000001ULL };

/** The group order. */
NYA_INTERNAL const u64 _NYA_P256_N[4] = { 0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL };

/** -p^-1 mod 2^64 and -n^-1 mod 2^64, which is what makes Montgomery's reduction a multiply. */
#define _NYA_P256_P_N0 0x0000000000000001ULL
#define _NYA_P256_N_N0 0xCCD1C8AAEE00BC4FULL

/** 2^512 mod p and mod n: what a number is multiplied by to enter Montgomery form. */
NYA_INTERNAL const u64 _NYA_P256_R2_P[4] = { 0x0000000000000003ULL, 0xFFFFFFFBFFFFFFFFULL, 0xFFFFFFFFFFFFFFFEULL, 0x00000004FFFFFFFDULL };
NYA_INTERNAL const u64 _NYA_P256_R2_N[4] = { 0x83244C95BE79EEA2ULL, 0x4699799C49BD6FA6ULL, 0x2845B2392B6BEC59ULL, 0x66E12D94F3D95620ULL };

/** One, in Montgomery form modulo p. */
NYA_INTERNAL const u64 _NYA_P256_ONE_MONT[4] = { 0x0000000000000001ULL, 0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFEULL };

/** The curve's `b`, in Montgomery form. `a` is -3 and is written as a subtraction rather than stored. */
NYA_INTERNAL const u64 _NYA_P256_B_MONT[4] = { 0xD89CDF6229C4BDDFULL, 0xACF005CD78843090ULL, 0xE5A220ABF7212ED6ULL, 0xDC30061D04874834ULL };

/** The generator, in Montgomery form. */
NYA_INTERNAL const u64 _NYA_P256_GX_MONT[4] = { 0x79E730D418A9143CULL, 0x75BA95FC5FEDB601ULL, 0x79FB732B77622510ULL, 0x18905F76A53755C6ULL };
NYA_INTERNAL const u64 _NYA_P256_GY_MONT[4] = { 0xDDF25357CE95560AULL, 0x8B4AB8E4BA19E45CULL, 0xD2E88688DD21F325ULL, 0x8571FF1825885D85ULL };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A point in Jacobian coordinates, where the affine point is (X/Z^2, Y/Z^3).
 *
 * Doubling and adding this way need no inversion, which is the expensive operation here: one
 * inversion at the end instead of one per step is most of why a verification is milliseconds rather
 * than a second. Z of zero is the point at infinity, which is the identity and has no affine form.
 * */
typedef struct {
    u64 x[4];
    u64 y[4];
    u64 z[4];
} _NYA_P256Point;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The field arithmetic. Every one of these wraps on purpose, as the RSA file's does and for the same
 * reason: modular arithmetic is wrapping arithmetic, and the carry is the thing being computed.
 */

/** Whether `a` is less than `b`. */
NYA_INTERNAL b8 _nya_p256_less(const u64* a, const u64* b) __attr_no_discard;

/** Whether `a` is zero. */
NYA_INTERNAL b8 _nya_p256_is_zero(const u64* a) __attr_no_discard;

/** Whether `a` equals `b`. */
NYA_INTERNAL b8 _nya_p256_equals(const u64* a, const u64* b) __attr_no_discard;

/** `out = a + b mod modulus`. */
NYA_INTERNAL void _nya_p256_add(OUT u64* out, const u64* a, const u64* b, const u64* modulus);

/** `out = a - b mod modulus`. */
NYA_INTERNAL void _nya_p256_subtract(OUT u64* out, const u64* a, const u64* b, const u64* modulus);

/** `out = a * b * 2^-256 mod modulus`: Montgomery multiplication, four limbs, CIOS. */
NYA_INTERNAL void _nya_p256_multiply(OUT u64* out, const u64* a, const u64* b, const u64* modulus, u64 n0);

/** `out = a ^ exponent mod modulus`, both in Montgomery form. Used for inversion by Fermat. */
NYA_INTERNAL void _nya_p256_power(OUT u64* out, const u64* a, const u64* exponent, const u64* modulus, u64 n0);

/** `out = a^-1 mod modulus`, both in Montgomery form, by raising to modulus - 2. */
NYA_INTERNAL void _nya_p256_inverse(OUT u64* out, const u64* a, const u64* modulus, u64 n0);

/** Big endian bytes into limbs, which is the only direction anything here needs. */
NYA_INTERNAL void _nya_p256_from_bytes(OUT u64* out, const u8* bytes);

/* The group arithmetic, all of it in the field modulo p and in Montgomery form. */

/** `out = 2 * point`, the doubling for a curve whose `a` is -3. */
NYA_INTERNAL void _nya_p256_double(OUT _NYA_P256Point* out, const _NYA_P256Point* point);

/** `out = a + b`, where `b` is affine — which is what both points in a verification are. */
NYA_INTERNAL void _nya_p256_add_affine(OUT _NYA_P256Point* out, const _NYA_P256Point* a, const u64* bx, const u64* by);

/** The affine x of `point`, or false for the point at infinity, which has none. */
NYA_INTERNAL b8 _nya_p256_affine_x(const _NYA_P256Point* point, OUT u64* out_x) __attr_no_discard;

/** Whether (x, y) satisfies y^2 = x^3 - 3x + b, which is what it means to be on this curve. */
NYA_INTERNAL b8 _nya_p256_is_on_curve(const u64* x, const u64* y) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_crypto_ecdsa_public_key_from_xy(const u8* x, u64 x_size, const u8* y, u64 y_size, NYA_CryptoEcdsaPublicKey* out_key) {
    nya_assert(out_key != nullptr);

    nya_memset(out_key, 0, sizeof(NYA_CryptoEcdsaPublicKey));

    if (x == nullptr || y == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a point is two coordinates, and one is missing");

    // A JWKS coordinate is exactly the field size, base64url of thirty-two bytes; a shorter one is a
    // provider that stripped a leading zero, which is legal enough to accept by padding it.
    if (x_size > NYA_CRYPTO_ECDSA_COORDINATE_BYTES || y_size > NYA_CRYPTO_ECDSA_COORDINATE_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is larger than a P-256 coordinate");
    }

    u8 padded_x[NYA_CRYPTO_ECDSA_COORDINATE_BYTES] = { 0 };
    u8 padded_y[NYA_CRYPTO_ECDSA_COORDINATE_BYTES] = { 0 };

    nya_memcpy(padded_x + (NYA_CRYPTO_ECDSA_COORDINATE_BYTES - x_size), x, x_size);
    nya_memcpy(padded_y + (NYA_CRYPTO_ECDSA_COORDINATE_BYTES - y_size), y, y_size);

    u64 raw_x[4] = { 0 };
    u64 raw_y[4] = { 0 };

    _nya_p256_from_bytes(raw_x, padded_x);
    _nya_p256_from_bytes(raw_y, padded_y);

    // A coordinate at or above the prime is not a field element, and a verifier that reduced it would
    // be accepting a second spelling of a key.
    if (!_nya_p256_less(raw_x, _NYA_P256_P) || !_nya_p256_less(raw_y, _NYA_P256_P)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a coordinate is not below the field prime");
    }

    // (0, 0) is how the point at infinity is spelled in this encoding, and it is not a public key: it
    // would make every signature verify.
    if (_nya_p256_is_zero(raw_x) && _nya_p256_is_zero(raw_y)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the point at infinity is not a key");

    _nya_p256_multiply(out_key->x, raw_x, _NYA_P256_R2_P, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(out_key->y, raw_y, _NYA_P256_R2_P, _NYA_P256_P, _NYA_P256_P_N0);

    /*
     * On the curve, which is the whole of invalid-curve attack prevention: a point that satisfies some
     * other curve's equation lives in a group whose order may be smooth, and arithmetic with it leaks
     * whatever it touches. One multiplication to check, once per key.
     */
    if (!_nya_p256_is_on_curve(out_key->x, out_key->y)) {
        nya_memset(out_key, 0, sizeof(NYA_CryptoEcdsaPublicKey));

        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that point is not on P-256");
    }

    return NYA_OK;
}

b8 nya_crypto_ecdsa_verify_sha256(const NYA_CryptoEcdsaPublicKey* key, const u8* message, u64 message_size, const u8* signature, u64 signature_size) {
    nya_assert(key != nullptr);
    nya_assert(message != nullptr || message_size == 0);
    nya_assert(signature != nullptr || signature_size == 0);

    // The JWS form and only it: `r` then `s`, each the coordinate size. DER is a different encoding
    // and is somebody else's to unwrap; see the header.
    if (signature_size != NYA_CRYPTO_ECDSA_SIGNATURE_BYTES) return false;

    // A zeroed key never read is not one to verify with.
    if (_nya_p256_is_zero(key->x) && _nya_p256_is_zero(key->y)) return false;

    u64 r[4] = { 0 };
    u64 s[4] = { 0 };

    _nya_p256_from_bytes(r, signature);
    _nya_p256_from_bytes(s, signature + NYA_CRYPTO_ECDSA_COORDINATE_BYTES);

    // Both halves live modulo n and must be in 1..n-1. Zero is where a signature of all zeroes lands,
    // and anything at or above n is a second spelling of a valid one.
    if (_nya_p256_is_zero(r) || _nya_p256_is_zero(s)) return false;
    if (!_nya_p256_less(r, _NYA_P256_N) || !_nya_p256_less(s, _NYA_P256_N)) return false;

    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(message, message_size, &digest);

    u64 e[4] = { 0 };
    _nya_p256_from_bytes(e, digest.bytes);

    /*
     * The hash is the same width as n here, so the standard's "leftmost bits" is the whole of it, and
     * what remains is the reduction: a digest above n is taken modulo n, which is one subtraction
     * because a 256-bit number cannot be more than one multiple of n past it.
     */
    if (!_nya_p256_less(e, _NYA_P256_N)) _nya_p256_subtract(e, e, _NYA_P256_N, _NYA_P256_N);

    // into Montgomery form modulo n, where the signature's arithmetic happens.
    u64 e_mont[4] = { 0 };
    u64 r_mont[4] = { 0 };
    u64 s_mont[4] = { 0 };

    _nya_p256_multiply(e_mont, e, _NYA_P256_R2_N, _NYA_P256_N, _NYA_P256_N_N0);
    _nya_p256_multiply(r_mont, r, _NYA_P256_R2_N, _NYA_P256_N, _NYA_P256_N_N0);
    _nya_p256_multiply(s_mont, s, _NYA_P256_R2_N, _NYA_P256_N, _NYA_P256_N_N0);

    u64 w[4] = { 0 };
    _nya_p256_inverse(w, s_mont, _NYA_P256_N, _NYA_P256_N_N0);

    u64 u1_mont[4] = { 0 };
    u64 u2_mont[4] = { 0 };

    _nya_p256_multiply(u1_mont, e_mont, w, _NYA_P256_N, _NYA_P256_N_N0);
    _nya_p256_multiply(u2_mont, r_mont, w, _NYA_P256_N, _NYA_P256_N_N0);

    // and out of it, because a scalar is read bit by bit and Montgomery form is a different number.
    u64 one[4] = { 1, 0, 0, 0 };
    u64 u1[4]  = { 0 };
    u64 u2[4]  = { 0 };

    _nya_p256_multiply(u1, u1_mont, one, _NYA_P256_N, _NYA_P256_N_N0);
    _nya_p256_multiply(u2, u2_mont, one, _NYA_P256_N, _NYA_P256_N_N0);

    /*
     * u1 * G + u2 * Q, by doubling once per bit and adding whichever of the two points that bit calls
     * for. Two additions per bit rather than Shamir's trick with a precomputed table: this runs once
     * per signature on public data, and a table is a thing to get wrong for a millisecond nobody is
     * waiting on.
     */
    _NYA_P256Point sum = { 0 };

    for (u32 bit = 256; bit > 0; bit--) {
        u32 index = (bit - 1) / 64;
        u32 shift = (bit - 1) % 64;

        _NYA_P256Point doubled = { 0 };
        _nya_p256_double(&doubled, &sum);
        sum = doubled;

        if (((u1[index] >> shift) & 1ULL) != 0) {
            _NYA_P256Point added = { 0 };
            _nya_p256_add_affine(&added, &sum, _NYA_P256_GX_MONT, _NYA_P256_GY_MONT);
            sum = added;
        }

        if (((u2[index] >> shift) & 1ULL) != 0) {
            _NYA_P256Point added = { 0 };
            _nya_p256_add_affine(&added, &sum, key->x, key->y);
            sum = added;
        }
    }

    u64 x[4] = { 0 };

    // The point at infinity has no x, and a signature that lands there verifies nothing.
    if (!_nya_p256_affine_x(&sum, x)) return false;

    // x lives modulo p and r modulo n, so the comparison is against x reduced into n's range — one
    // subtraction, for the same reason the digest needed at most one.
    if (!_nya_p256_less(x, _NYA_P256_N)) _nya_p256_subtract(x, x, _NYA_P256_N, _NYA_P256_N);

    return _nya_p256_equals(x, r);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_p256_less(const u64* a, const u64* b) {
    for (u32 index = 4; index > 0; index--) {
        if (a[index - 1] != b[index - 1]) return a[index - 1] < b[index - 1];
    }

    return false;
}

b8 _nya_p256_is_zero(const u64* a) {
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}

b8 _nya_p256_equals(const u64* a, const u64* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

__attr_no_sanitize("unsigned-integer-overflow") void _nya_p256_add(u64* out, const u64* a, const u64* b, const u64* modulus) {
    u64 carry = 0;
    u64 sum[4] = { 0 };

    for (u32 index = 0; index < 4; index++) {
        u128 total = (u128)a[index] + (u128)b[index] + (u128)carry;

        sum[index] = (u64)total;
        carry      = (u64)(total >> 64);
    }

    // One subtraction is enough: both inputs are already below the modulus, so their sum is below
    // twice it. The carry out of the top is what says the sum passed 2^256 and is certainly above.
    if (carry != 0 || !_nya_p256_less(sum, modulus)) {
        u64 borrow = 0;

        for (u32 index = 0; index < 4; index++) {
            u64 left  = sum[index];
            u64 right = modulus[index] + borrow;

            borrow = (right < modulus[index]) || (left < right) ? 1 : 0;

            sum[index] = left - right;
        }
    }

    for (u32 index = 0; index < 4; index++) out[index] = sum[index];
}

__attr_no_sanitize("unsigned-integer-overflow") void _nya_p256_subtract(u64* out, const u64* a, const u64* b, const u64* modulus) {
    u64 difference[4] = { 0 };
    u64 borrow        = 0;

    for (u32 index = 0; index < 4; index++) {
        u64 left  = a[index];
        u64 right = b[index] + borrow;

        borrow = (right < b[index]) || (left < right) ? 1 : 0;

        difference[index] = left - right;
    }

    // It went below zero, so the modulus comes back: the result wrapped at 2^256 and adding the
    // modulus lands it where it belongs.
    if (borrow != 0) {
        u64 carry = 0;

        for (u32 index = 0; index < 4; index++) {
            u128 total = (u128)difference[index] + (u128)modulus[index] + (u128)carry;

            difference[index] = (u64)total;
            carry             = (u64)(total >> 64);
        }
    }

    for (u32 index = 0; index < 4; index++) out[index] = difference[index];
}

__attr_no_sanitize("unsigned-integer-overflow") void _nya_p256_multiply(u64* out, const u64* a, const u64* b, const u64* modulus, u64 n0) {
    u64 product[6] = { 0 };

    for (u32 i = 0; i < 4; i++) {
        u64 carry = 0;

        for (u32 j = 0; j < 4; j++) {
            u128 sum = (u128)a[i] * (u128)b[j] + (u128)product[j] + (u128)carry;

            product[j] = (u64)sum;
            carry      = (u64)(sum >> 64);
        }

        u128 top = (u128)product[4] + (u128)carry;

        product[4] = (u64)top;
        product[5] = (u64)(top >> 64);

        // The multiple of the modulus that clears the bottom limb, which is what earns the shift.
        u64 m      = product[0] * n0;
        u64 carry2 = 0;

        for (u32 j = 0; j < 4; j++) {
            u128 sum = (u128)m * (u128)modulus[j] + (u128)product[j] + (u128)carry2;

            product[j] = (u64)sum;
            carry2     = (u64)(sum >> 64);
        }

        u128 rest = (u128)product[4] + (u128)carry2;

        product[4]  = (u64)rest;
        product[5] += (u64)(rest >> 64);

        for (u32 j = 0; j < 5; j++) product[j] = product[j + 1];

        product[5] = 0;
    }

    if (product[4] != 0 || !_nya_p256_less(product, modulus)) {
        u64 borrow = 0;

        for (u32 index = 0; index < 4; index++) {
            u64 left  = product[index];
            u64 right = modulus[index] + borrow;

            borrow = (right < modulus[index]) || (left < right) ? 1 : 0;

            product[index] = left - right;
        }
    }

    for (u32 index = 0; index < 4; index++) out[index] = product[index];
}

void _nya_p256_power(u64* out, const u64* a, const u64* exponent, const u64* modulus, u64 n0) {
    // one in Montgomery form for this modulus, which is what the running result starts at.
    u64 result[4] = { 0 };
    u64 one[4]    = { 1, 0, 0, 0 };

    _nya_p256_multiply(result, one, modulus == _NYA_P256_P ? _NYA_P256_R2_P : _NYA_P256_R2_N, modulus, n0);

    for (u32 bit = 256; bit > 0; bit--) {
        u32 index = (bit - 1) / 64;
        u32 shift = (bit - 1) % 64;

        u64 squared[4] = { 0 };
        _nya_p256_multiply(squared, result, result, modulus, n0);

        for (u32 limb = 0; limb < 4; limb++) result[limb] = squared[limb];

        if (((exponent[index] >> shift) & 1ULL) == 0) continue;

        u64 multiplied[4] = { 0 };
        _nya_p256_multiply(multiplied, result, a, modulus, n0);

        for (u32 limb = 0; limb < 4; limb++) result[limb] = multiplied[limb];
    }

    for (u32 limb = 0; limb < 4; limb++) out[limb] = result[limb];
}

__attr_no_sanitize("unsigned-integer-overflow") void _nya_p256_inverse(u64* out, const u64* a, const u64* modulus, u64 n0) {
    /*
     * By Fermat: a^(m-2) is a's inverse when m is prime, and both moduli here are. An extended
     * Euclid would be faster and is also where inversion implementations go wrong, in branches whose
     * count depends on the value; this one is the same 256 squarings whatever it is given.
     */
    u64 exponent[4] = { 0 };
    u64 borrow      = 0;
    u64 two[4]      = { 2, 0, 0, 0 };

    for (u32 index = 0; index < 4; index++) {
        u64 left  = modulus[index];
        u64 right = two[index] + borrow;

        borrow = (right < two[index]) || (left < right) ? 1 : 0;

        exponent[index] = left - right;
    }

    _nya_p256_power(out, a, exponent, modulus, n0);
}

void _nya_p256_from_bytes(u64* out, const u8* bytes) {
    for (u32 index = 0; index < 4; index++) {
        u64 limb = 0;

        for (u32 byte = 0; byte < 8; byte++) limb = (limb << 8) | bytes[(index * 8) + byte];

        // big endian on the wire, least significant limb first in memory.
        out[3 - index] = limb;
    }
}

void _nya_p256_double(_NYA_P256Point* out, const _NYA_P256Point* point) {
    // The identity doubles to itself, and the formulas below divide by zero on it.
    if (_nya_p256_is_zero(point->z)) {
        nya_memset(out, 0, sizeof(_NYA_P256Point));
        return;
    }

    /*
     * The standard Jacobian doubling for a = -3, which is what lets `alpha` be computed with one
     * multiplication instead of a squaring and a separate term:
     *
     *   delta = Z^2, gamma = Y^2, beta = X * gamma
     *   alpha = 3 * (X - delta) * (X + delta)
     *   X' = alpha^2 - 8 * beta
     *   Z' = (Y + Z)^2 - gamma - delta
     *   Y' = alpha * (4 * beta - X') - 8 * gamma^2
     */
    u64 delta[4] = { 0 };
    u64 gamma[4] = { 0 };
    u64 beta[4]  = { 0 };
    u64 alpha[4] = { 0 };
    u64 tmp[4]   = { 0 };
    u64 tmp2[4]  = { 0 };

    _nya_p256_multiply(delta, point->z, point->z, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(gamma, point->y, point->y, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(beta, point->x, gamma, _NYA_P256_P, _NYA_P256_P_N0);

    _nya_p256_subtract(tmp, point->x, delta, _NYA_P256_P);
    _nya_p256_add(tmp2, point->x, delta, _NYA_P256_P);
    _nya_p256_multiply(alpha, tmp, tmp2, _NYA_P256_P, _NYA_P256_P_N0);

    _nya_p256_add(tmp, alpha, alpha, _NYA_P256_P);
    _nya_p256_add(alpha, tmp, alpha, _NYA_P256_P);

    u64 x[4] = { 0 };
    u64 y[4] = { 0 };
    u64 z[4] = { 0 };

    _nya_p256_multiply(x, alpha, alpha, _NYA_P256_P, _NYA_P256_P_N0);

    // eight betas, by doubling three times.
    _nya_p256_add(tmp, beta, beta, _NYA_P256_P);
    _nya_p256_add(tmp2, tmp, tmp, _NYA_P256_P);
    _nya_p256_add(tmp, tmp2, tmp2, _NYA_P256_P);

    _nya_p256_subtract(x, x, tmp, _NYA_P256_P);

    _nya_p256_add(tmp, point->y, point->z, _NYA_P256_P);
    _nya_p256_multiply(z, tmp, tmp, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_subtract(z, z, gamma, _NYA_P256_P);
    _nya_p256_subtract(z, z, delta, _NYA_P256_P);

    // four betas minus the new x, times alpha.
    _nya_p256_add(tmp, beta, beta, _NYA_P256_P);
    _nya_p256_add(tmp2, tmp, tmp, _NYA_P256_P);
    _nya_p256_subtract(tmp2, tmp2, x, _NYA_P256_P);
    _nya_p256_multiply(y, alpha, tmp2, _NYA_P256_P, _NYA_P256_P_N0);

    // eight gamma squared, again by doubling.
    _nya_p256_multiply(tmp, gamma, gamma, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_add(tmp2, tmp, tmp, _NYA_P256_P);
    _nya_p256_add(tmp, tmp2, tmp2, _NYA_P256_P);
    _nya_p256_add(tmp2, tmp, tmp, _NYA_P256_P);

    _nya_p256_subtract(y, y, tmp2, _NYA_P256_P);

    for (u32 index = 0; index < 4; index++) {
        out->x[index] = x[index];
        out->y[index] = y[index];
        out->z[index] = z[index];
    }
}

void _nya_p256_add_affine(_NYA_P256Point* out, const _NYA_P256Point* a, const u64* bx, const u64* by) {
    // Adding to the identity is the other point, lifted into Jacobian with Z of one.
    if (_nya_p256_is_zero(a->z)) {
        for (u32 index = 0; index < 4; index++) {
            out->x[index] = bx[index];
            out->y[index] = by[index];
            out->z[index] = _NYA_P256_ONE_MONT[index];
        }

        return;
    }

    /*
     * Jacobian plus affine:
     *
     *   z1z1 = Z1^2, u2 = X2 * z1z1, s2 = Y2 * Z1 * z1z1
     *   h = u2 - X1, r = 2 * (s2 - Y1)
     */
    u64 z1z1[4] = { 0 };
    u64 u2[4]   = { 0 };
    u64 s2[4]   = { 0 };
    u64 h[4]    = { 0 };
    u64 r[4]    = { 0 };
    u64 tmp[4]  = { 0 };

    _nya_p256_multiply(z1z1, a->z, a->z, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(u2, bx, z1z1, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(tmp, by, a->z, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(s2, tmp, z1z1, _NYA_P256_P, _NYA_P256_P_N0);

    _nya_p256_subtract(h, u2, a->x, _NYA_P256_P);
    _nya_p256_subtract(r, s2, a->y, _NYA_P256_P);

    // The same point: the addition formula divides by zero there, so it becomes a doubling. The other
    // half of that case — the same x with the opposite y — is the identity, which is Z of zero.
    if (_nya_p256_is_zero(h)) {
        if (_nya_p256_is_zero(r)) {
            _nya_p256_double(out, a);
            return;
        }

        nya_memset(out, 0, sizeof(_NYA_P256Point));
        return;
    }

    _nya_p256_add(r, r, r, _NYA_P256_P);

    u64 i[4] = { 0 };
    u64 j[4] = { 0 };
    u64 v[4] = { 0 };

    _nya_p256_add(tmp, h, h, _NYA_P256_P);
    _nya_p256_multiply(i, tmp, tmp, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(j, h, i, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(v, a->x, i, _NYA_P256_P, _NYA_P256_P_N0);

    u64 x[4] = { 0 };
    u64 y[4] = { 0 };
    u64 z[4] = { 0 };

    _nya_p256_multiply(x, r, r, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_subtract(x, x, j, _NYA_P256_P);
    _nya_p256_subtract(x, x, v, _NYA_P256_P);
    _nya_p256_subtract(x, x, v, _NYA_P256_P);

    _nya_p256_subtract(tmp, v, x, _NYA_P256_P);
    _nya_p256_multiply(y, r, tmp, _NYA_P256_P, _NYA_P256_P_N0);

    _nya_p256_multiply(tmp, a->y, j, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_add(tmp, tmp, tmp, _NYA_P256_P);
    _nya_p256_subtract(y, y, tmp, _NYA_P256_P);

    _nya_p256_add(tmp, a->z, h, _NYA_P256_P);
    _nya_p256_multiply(z, tmp, tmp, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_subtract(z, z, z1z1, _NYA_P256_P);
    _nya_p256_multiply(tmp, h, h, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_subtract(z, z, tmp, _NYA_P256_P);

    for (u32 index = 0; index < 4; index++) {
        out->x[index] = x[index];
        out->y[index] = y[index];
        out->z[index] = z[index];
    }
}

b8 _nya_p256_affine_x(const _NYA_P256Point* point, u64* out_x) {
    if (_nya_p256_is_zero(point->z)) return false;

    // x = X / Z^2, which is the one inversion a verification pays for.
    u64 inverse[4]  = { 0 };
    u64 inverse2[4] = { 0 };
    u64 affine[4]   = { 0 };
    u64 one[4]      = { 1, 0, 0, 0 };

    _nya_p256_inverse(inverse, point->z, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(inverse2, inverse, inverse, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(affine, point->x, inverse2, _NYA_P256_P, _NYA_P256_P_N0);

    // and out of Montgomery form, because what comes next is a comparison against a plain number.
    _nya_p256_multiply(out_x, affine, one, _NYA_P256_P, _NYA_P256_P_N0);

    return true;
}

b8 _nya_p256_is_on_curve(const u64* x, const u64* y) {
    // y^2 = x^3 - 3x + b, every term in Montgomery form.
    u64 left[4]  = { 0 };
    u64 right[4] = { 0 };
    u64 tmp[4]   = { 0 };

    _nya_p256_multiply(left, y, y, _NYA_P256_P, _NYA_P256_P_N0);

    _nya_p256_multiply(right, x, x, _NYA_P256_P, _NYA_P256_P_N0);
    _nya_p256_multiply(right, right, x, _NYA_P256_P, _NYA_P256_P_N0);

    _nya_p256_add(tmp, x, x, _NYA_P256_P);
    _nya_p256_add(tmp, tmp, x, _NYA_P256_P);

    _nya_p256_subtract(right, right, tmp, _NYA_P256_P);
    _nya_p256_add(right, right, _NYA_P256_B_MONT, _NYA_P256_P);

    return _nya_p256_equals(left, right);
}
