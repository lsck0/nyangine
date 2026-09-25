#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-std/os/os_random.h"
#include "monocypher.h"

// PUBLIC API IMPLEMENTATION

NYA_Error nya_crypto_key_create(OUT NYA_CryptoKey32* out_key) {
    nya_assert(out_key != nullptr);

    if (!nya_os_random_bytes(out_key->bytes, sizeof(out_key->bytes))) {
        nya_crypto_wipe(out_key, sizeof(*out_key));
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");
    }

    return NYA_OK;
}

void nya_crypto_key_destroy(NYA_CryptoKey32* key) {
    if (key == nullptr) return;

    nya_crypto_wipe(key, sizeof(*key));
}

b8 nya_crypto_equals(const u8* a, const u8* b, u64 size) {
    nya_assert(a != nullptr || size == 0);
    nya_assert(b != nullptr || size == 0);

    u8 difference = 0;

    for (u64 i = 0; i < size; i++) {
        difference |= (u8)(a[i] ^ b[i]);

        // barrier hides the accumulator from the optimizer, else it could exit the loop early once saturated.
        __asm__ volatile("" : "+r"(difference));
    }

    return difference == 0;
}

void nya_crypto_wipe(void* secret, u64 size) {
    nya_assert(secret != nullptr || size == 0);

    if (size == 0) return;

    crypto_wipe(secret, size);
}
