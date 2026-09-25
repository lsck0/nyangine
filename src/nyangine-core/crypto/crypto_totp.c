#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-core/crypto/crypto_totp.h"
#include "nyangine-std/os/os_random.h"

// PRIVATE API DECLARATION

/** RFC 4226 counts in big endian over eight bytes, whatever the host does. */
#define _NYA_CRYPTO_TOTP_COUNTER_BYTES 8

/** 10^NYA_CRYPTO_TOTP_DIGITS, written out rather than computed so nothing rounds it. */
#define _NYA_CRYPTO_TOTP_MODULUS 1000000U

/** Section 5.3's dynamic truncation: the offset is the low nibble of the last byte of the tag. */
NYA_INTERNAL u32 _nya_crypto_totp_truncate(const NYA_CryptoSha1Digest* tag) __attr_no_discard;

// PRIVATE API IMPLEMENTATION

u32 _nya_crypto_totp_truncate(const NYA_CryptoSha1Digest* tag) {
    u32 offset = tag->bytes[NYA_CRYPTO_SHA1_BYTES - 1] & 0x0FU;

    // top bit masked off (RFC's 0x7F) so the number reads the same on a machine that treats it as signed.
    return ((u32)(tag->bytes[offset] & 0x7FU) << 24) | ((u32)tag->bytes[offset + 1] << 16) | ((u32)tag->bytes[offset + 2] << 8) |
           (u32)tag->bytes[offset + 3];
}

// PUBLIC API IMPLEMENTATION

NYA_Error nya_crypto_totp_secret_create(OUT NYA_CryptoTotpSecret* out_secret) {
    nya_assert(out_secret != nullptr);

    if (!nya_os_random_bytes(out_secret->bytes, sizeof(out_secret->bytes))) {
        nya_crypto_wipe(out_secret, sizeof(*out_secret));
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");
    }

    return NYA_OK;
}

void nya_crypto_totp_secret_destroy(NYA_CryptoTotpSecret* secret) {
    if (secret == nullptr) return;

    nya_crypto_wipe(secret, sizeof(*secret));
}

u64 nya_crypto_totp_counter(u64 unix_s) {
    // T0 is the epoch itself, so there is no time before it to subtract past and this cannot wrap.
    return (unix_s - NYA_CRYPTO_TOTP_EPOCH_S) / NYA_CRYPTO_TOTP_STEP_S;
}

void nya_crypto_totp_code(const u8* key, u64 key_size, u64 counter, OUT char out_code[NYA_CRYPTO_TOTP_CODE_BYTES]) {
    nya_assert(key != nullptr || key_size == 0);
    nya_assert(out_code != nullptr);

    u8 message[_NYA_CRYPTO_TOTP_COUNTER_BYTES] = { 0 };
    for (u32 i = 0; i < _NYA_CRYPTO_TOTP_COUNTER_BYTES; i++) message[i] = (u8)(counter >> (56U - (8U * i)));

    NYA_CryptoSha1Digest tag = { 0 };
    nya_crypto_hmac_sha1(key, key_size, message, sizeof(message), &tag);

    u32 value = _nya_crypto_totp_truncate(&tag) % _NYA_CRYPTO_TOTP_MODULUS;

    // written from the last digit back, so a code keeps its leading zeros (004135, not 4135).
    out_code[NYA_CRYPTO_TOTP_DIGITS] = '\0';
    for (u32 i = NYA_CRYPTO_TOTP_DIGITS; i > 0; i--) {
        out_code[i - 1] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    // the tag is a MAC under the secret and the block is what it covers; neither outlives the call.
    nya_crypto_wipe(&tag, sizeof(tag));
    nya_crypto_wipe(message, sizeof(message));
}

b8 nya_crypto_totp_code_equals(const char a[NYA_CRYPTO_TOTP_CODE_BYTES], const char b[NYA_CRYPTO_TOTP_CODE_BYTES]) {
    nya_assert(a != nullptr);
    nya_assert(b != nullptr);

    return nya_crypto_equals((const u8*)a, (const u8*)b, NYA_CRYPTO_TOTP_CODE_BYTES);
}
