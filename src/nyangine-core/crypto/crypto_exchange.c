#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_exchange.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-std/os/os_random.h"
#include "monocypher.h"

// PUBLIC API IMPLEMENTATION

NYA_Error nya_crypto_exchange_key_pair_create(OUT NYA_CryptoExchangeKeyPair* out_key_pair) {
    nya_assert(out_key_pair != nullptr);

    *out_key_pair = (NYA_CryptoExchangeKeyPair){ 0 };

    NYA_CryptoExchangeSecretKey secret = { 0 };
    defer                       nya_crypto_wipe(&secret, sizeof(secret));

    if (!nya_os_random_bytes(secret.bytes, sizeof(secret.bytes))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    nya_crypto_exchange_key_pair_from_secret(&secret, out_key_pair);

    return NYA_OK;
}

void nya_crypto_exchange_key_pair_destroy(NYA_CryptoExchangeKeyPair* key_pair) {
    if (key_pair == nullptr) return;

    nya_crypto_wipe(key_pair, sizeof(*key_pair));
}

void nya_crypto_exchange_key_pair_from_secret(const NYA_CryptoExchangeSecretKey* secret_key, OUT NYA_CryptoExchangeKeyPair* out_key_pair) {
    nya_assert(secret_key != nullptr);
    nya_assert(out_key_pair != nullptr);

    // copied first, so a caller may pass the pair's own secret half as `secret_key`.
    NYA_CryptoExchangeSecretKey secret = *secret_key;

    out_key_pair->secret_key = secret;
    crypto_x25519_public_key(out_key_pair->public_key.bytes, secret.bytes);

    nya_crypto_wipe(&secret, sizeof(secret));
}

b8 nya_crypto_exchange(
    const NYA_CryptoExchangeSecretKey* secret_key,
    const NYA_CryptoExchangePublicKey* public_key,
    OUT NYA_CryptoSharedSecret*        out_shared
) {
    nya_assert(secret_key != nullptr);
    nya_assert(public_key != nullptr);
    nya_assert(out_shared != nullptr);

    crypto_x25519(out_shared->bytes, secret_key->bytes, public_key->bytes);

    // every byte folded in regardless (RFC 7748 section 6.1), so the check leaks nothing about zero placement.
    u8 any = 0;
    for (u32 i = 0; i < NYA_CRYPTO_EXCHANGE_KEY_BYTES; i++) any |= out_shared->bytes[i];

    return any != 0;
}
