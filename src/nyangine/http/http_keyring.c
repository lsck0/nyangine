#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/http/http_keyring.h"

// PRIVATE API DECLARATION

/** Removes every key whose verify life has ended, keeping the rest newest-first. Returns how many went. */
NYA_INTERNAL u32 _nya_http_keyring_prune(NYA_HttpKeyring* ring, u64 now_s);

// PUBLIC API IMPLEMENTATION

b8 nya_http_keyring_rotate(NYA_HttpKeyring* ring) {
    nya_assert(ring != nullptr);

    u64 now_s = nya_clock_get_timestamp_s();

    b8 changed = _nya_http_keyring_prune(ring, now_s) > 0;

    // Newest key is index zero (ring kept newest-first); a key is due for replacement once it has sealed a whole rotation window, and nothing is minted before that.
    b8 due = ring->key_count == 0 || now_s >= ring->keys[0].created_at_s + NYA_HTTP_KEYRING_ROTATE_S;

    if (!due) return changed;

    NYA_CryptoKey32 fresh = { 0 };

    // A ring that can't get randomness doesn't mint a bad key: it keeps the ones it has (which still seal and open), and a program watching the return sees no change this tick.
    if (!nya_crypto_key_create(&fresh).ok) return changed;

    // The new key goes to the front and the rest slide down; a full ring drops its oldest, only ever a key already in its last day of verifying — MAX_KEYS is set so a still-useful key is never pushed out under normal rotation.
    u32 keep = ring->key_count < NYA_HTTP_KEYRING_MAX_KEYS ? ring->key_count : NYA_HTTP_KEYRING_MAX_KEYS - 1;

    for (u32 index = keep; index > 0; index--) ring->keys[index] = ring->keys[index - 1];

    nya_memcpy(ring->keys[0].material, fresh.bytes, sizeof(ring->keys[0].material));
    ring->keys[0].created_at_s = now_s;
    ring->keys[0].expires_at_s = now_s + NYA_HTTP_KEYRING_ROTATE_S + NYA_HTTP_KEYRING_VERIFY_TAIL_S;

    ring->key_count = keep + 1;

    nya_crypto_key_destroy(&fresh);

    return true;
}

NYA_Error nya_http_keyring_seal(
    const NYA_HttpKeyring* ring, NYA_ConstCString label, const u8* plaintext, u64 plaintext_size, u64 ttl_s, char* out_token, u64 capacity
) {
    nya_assert(ring != nullptr && out_token != nullptr && capacity > 0);

    out_token[0] = '\0';

    // An empty ring is a rotate that never ran, a start-up bug, so an error rather than a quiet failure to seal.
    if (ring->key_count == 0) return nya_error(NYA_ERROR_NOT_OK, "the keyring is empty; call nya_http_keyring_rotate at start-up");

    return nya_http_seal(ring->keys[0].material, sizeof(ring->keys[0].material), label, plaintext, plaintext_size, ttl_s, out_token, capacity);
}

b8 nya_http_keyring_unseal(
    const NYA_HttpKeyring* ring, NYA_ConstCString label, const char* token, u64 token_size, u8* out_plaintext, u64 capacity, u64* out_size
) {
    nya_assert(ring != nullptr && out_size != nullptr);

    *out_size = 0;

    if (out_plaintext != nullptr && capacity > 0) out_plaintext[0] = 0;

    // Newest first, so the key most tokens were sealed with is tried first; a rotation costs the few tokens still on an old key one extra AEAD open each, not every token.
    for (u32 index = 0; index < ring->key_count; index++) {
        if (nya_http_unseal(ring->keys[index].material, sizeof(ring->keys[index].material), label, token, token_size, out_plaintext, capacity, out_size)) {
            return true;
        }
    }

    return false;
}

u32 nya_http_keyring_count(const NYA_HttpKeyring* ring) {
    nya_assert(ring != nullptr);

    return ring->key_count;
}

void nya_http_keyring_wipe(NYA_HttpKeyring* ring) {
    if (ring == nullptr) return;

    nya_crypto_wipe(ring, sizeof(*ring));
}

// PRIVATE API IMPLEMENTATION

u32 _nya_http_keyring_prune(NYA_HttpKeyring* ring, u64 now_s) {
    u32 kept = 0;

    for (u32 index = 0; index < ring->key_count; index++) {
        // A key past its verify life opens nothing, so it's dropped and its bytes wiped: a key kept longer than useful is only one waiting to leak.
        if (now_s >= ring->keys[index].expires_at_s) {
            nya_crypto_wipe(ring->keys[index].material, sizeof(ring->keys[index].material));
            continue;
        }

        if (kept != index) ring->keys[kept] = ring->keys[index];

        kept++;
    }

    u32 removed = ring->key_count - kept;

    // Whatever slots the survivors left behind are cleared, so no key lingers in the tail of the array.
    for (u32 index = kept; index < ring->key_count; index++) nya_memset(&ring->keys[index], 0, sizeof(ring->keys[index]));

    ring->key_count = kept;

    return removed;
}
