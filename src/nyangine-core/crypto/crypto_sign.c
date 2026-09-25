#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-core/crypto/crypto_sign.h"
#include "nyangine-std/os/os_random.h"
#include "monocypher.h"
#include "optional/monocypher-ed25519.h"

// PRIVATE API DECLARATION

/** Whether a public key is a small-order point: RFC 8032 admits them, but an all-zero sig then forges. */
NYA_INTERNAL b8 _nya_crypto_sign_key_is_small_order(const NYA_CryptoSignPublicKey* public_key) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_crypto_sign_key_pair_create(OUT NYA_CryptoSignKeyPair* out_key_pair) {
    nya_assert(out_key_pair != nullptr);

    *out_key_pair = (NYA_CryptoSignKeyPair){ 0 };

    NYA_CryptoKey32 seed = { 0 };
    defer           nya_crypto_key_destroy(&seed);

    NYA_TRY(nya_crypto_key_create(&seed));
    nya_crypto_sign_key_pair_from_seed(&seed, out_key_pair);

    return NYA_OK;
}

void nya_crypto_sign_key_pair_destroy(NYA_CryptoSignKeyPair* key_pair) {
    if (key_pair == nullptr) return;

    nya_crypto_wipe(key_pair, sizeof(*key_pair));
}

void nya_crypto_sign_key_pair_from_seed(const NYA_CryptoKey32* seed, OUT NYA_CryptoSignKeyPair* out_key_pair) {
    nya_assert(seed != nullptr);
    nya_assert(out_key_pair != nullptr);

    // monocypher wipes the seed it is given, and the caller's is const and still theirs, so it gets a copy.
    NYA_CryptoKey32 copy = *seed;
    crypto_ed25519_key_pair(out_key_pair->secret_key.bytes, out_key_pair->public_key.bytes, copy.bytes);
    nya_crypto_key_destroy(&copy);
}

void nya_crypto_sign(const NYA_CryptoSignSecretKey* secret_key, const u8* message, u64 size, OUT NYA_CryptoSignature* out_signature) {
    nya_assert(secret_key != nullptr);
    nya_assert(message != nullptr || size == 0);
    nya_assert(out_signature != nullptr);

    // an empty message is legal and RFC 8032's first vector; monocypher still wants a pointer to it.
    u8 none = 0;
    crypto_ed25519_sign(out_signature->bytes, secret_key->bytes, message != nullptr ? message : &none, size);
}

b8 nya_crypto_sign_verify(const NYA_CryptoSignPublicKey* public_key, const u8* message, u64 size, const NYA_CryptoSignature* signature) {
    nya_assert(public_key != nullptr);
    nya_assert(message != nullptr || size == 0);
    nya_assert(signature != nullptr);

    if (_nya_crypto_sign_key_is_small_order(public_key)) return false;

    u8 none = 0;
    return crypto_ed25519_check(signature->bytes, public_key->bytes, message != nullptr ? message : &none, size) == 0;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_crypto_sign_key_is_small_order(const NYA_CryptoSignPublicKey* public_key) {
    // Curve25519 keeps order and X25519 clamps scalars to a multiple of 8, so the product is zero for these points.
    u8 montgomery[NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES] = { 0 };
    crypto_eddsa_to_x25519(montgomery, public_key->bytes);

    // any scalar would do; clamped, this one is 2^254, a multiple of 8 and not of the group order.
    u8 scalar[NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES]  = { 1 };
    u8 product[NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES] = { 0 };
    crypto_x25519(product, scalar, montgomery);

    u8 any = 0;
    for (u32 i = 0; i < sizeof(product); i++) any |= product[i];

    return any == 0;
}
