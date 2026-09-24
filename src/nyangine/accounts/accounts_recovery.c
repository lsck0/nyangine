#include <string.h>

#include "nyangine/accounts/accounts_recovery.h"
#include "nyangine/accounts/accounts_throttle.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/db/db_orm.h"
#include "nyangine/os/os_random.h"

// PRIVATE API DECLARATION

/** SHA-256 of a canonical code, as lower case hex. Shares its shape with the session token hash. */
NYA_INTERNAL void _nya_account_recovery_hash(NYA_ConstCString canonical, OUT char* out_hex, u64 capacity);

/** The canonical form of a typed code: upper case, base32 only, dashes/spaces dropped; false when the result isn't a code's length. */
NYA_INTERNAL b8 _nya_account_recovery_canonical(NYA_ConstCString code, OUT char* out, u64 capacity) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_account_recovery_generate(
    NYA_Arena* arena, u64 account_id, char out_codes[NYA_ACCOUNTS_RECOVERY_CODE_COUNT][NYA_ACCOUNTS_RECOVERY_CODE_TEXT], u32* out_count
) {
    nya_assert(arena != nullptr && out_codes != nullptr && out_count != nullptr);

    *out_count = 0;

    nya_memset(out_codes, 0, sizeof(char[NYA_ACCOUNTS_RECOVERY_CODE_COUNT][NYA_ACCOUNTS_RECOVERY_CODE_TEXT]));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    // The account must exist: codes for a nonexistent account later let somebody into whoever gets that id next.
    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, account_id, &user));

    // A new set replaces the old one entirely, so every code the person had stops working at once.
    void* existing = nullptr;
    u32   had      = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.recovery_codes, arena, "WHERE account_id = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)account_id) }, 1, &existing, &had));

    for (u32 index = 0; index < had; index++) {
        const NYA_AccountRecoveryCode* row = nya_orm_at(_NYA_ACCOUNTS.recovery_codes, existing, index);

        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.recovery_codes, nya_sql_s64((s64)row->id)));
    }

    u64 now_s = nya_clock_get_timestamp_s();

    for (u32 index = 0; index < NYA_ACCOUNTS_RECOVERY_CODE_COUNT; index++) {
        u8 bytes[NYA_ACCOUNTS_RECOVERY_CODE_BYTES] = { 0 };

        if (!nya_os_random_bytes(bytes, sizeof(bytes))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

        // 5 bytes are 8 base32 characters, no padding: the canonical code, and what is hashed.
        char canonical[16] = { 0 };
        u64  length        = 0;

        NYA_TRY(nya_crypto_base32_encode(bytes, sizeof(bytes), canonical, sizeof(canonical), &length));

        nya_crypto_wipe(bytes, sizeof(bytes));

        // Strip base32's trailing '=' padding: the canonical form is the pad-free prefix.
        while (length > 0 && canonical[length - 1] == '=') canonical[--length] = '\0';

        // Shown grouped with a dash for readability; the dash is dropped on the way in, so `XXXX-XXXX` == `XXXXXXXX`.
        (void)snprintf(out_codes[index], NYA_ACCOUNTS_RECOVERY_CODE_TEXT, "%.4s-%.4s", canonical, canonical + 4);

        NYA_AccountRecoveryCode stored = { 0 };

        stored.account_id   = account_id;
        stored.created_at_s = now_s;

        _nya_account_recovery_hash(canonical, stored.code_hash, sizeof(stored.code_hash));

        nya_crypto_wipe(canonical, sizeof(canonical));

        NYA_TRY(nya_orm_insert(_NYA_ACCOUNTS.recovery_codes, &stored));
    }

    *out_count = NYA_ACCOUNTS_RECOVERY_CODE_COUNT;

    return NYA_OK;
}

NYA_Error nya_account_recovery_consume(NYA_Arena* arena, NYA_ConstCString username, NYA_ConstCString code, NYA_ConstCString address, NYA_AccountUser* out_user) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    // One refusal for every way this fails, in the same words a login uses; see accounts.h.
    NYA_Error refused = nya_error(NYA_ERROR_PERMISSION_DENIED, "that username and code do not match an account");

    // Before any work, and it is what makes guessing codes cost the same as guessing passwords.
    u32 wait_s = 0;
    if (nya_account_throttle_check(username, address, &wait_s) == NYA_ACCOUNT_THROTTLE_WAIT) return refused;

    NYA_AccountUser user  = { 0 };
    NYA_Error       found = nya_account_find(arena, username, &user);

    char canonical[16] = { 0 };
    b8   is_a_code     = _nya_account_recovery_canonical(code, canonical, sizeof(canonical));

    // Same hash-and-lookup whether or not the account exists or the code is well-shaped, so all failures cost and say the same.
    char hex[72] = { 0 };
    if (is_a_code) _nya_account_recovery_hash(canonical, hex, sizeof(hex));

    nya_crypto_wipe(canonical, sizeof(canonical));

    b8 matched = false;

    if (found.ok && is_a_code) {
        void* rows  = nullptr;
        u32   count = 0;

        NYA_TRY(nya_orm_select(
            _NYA_ACCOUNTS.recovery_codes, arena, "WHERE account_id = ? AND code_hash = ? LIMIT 1",
            (NYA_SqlValue[]){ nya_sql_s64((s64)user.id), nya_sql_text(hex) }, 2, &rows, &count
        ));

        if (count > 0) {
            const NYA_AccountRecoveryCode* row = nya_orm_at(_NYA_ACCOUNTS.recovery_codes, rows, 0);

            // Single use: the row goes the moment it matches, so the same code cannot be spent twice.
            NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.recovery_codes, nya_sql_s64((s64)row->id)));

            matched = true;
        }
    }

    if (!found.ok || !matched || user.disabled) {
        nya_account_throttle_fail(username, address);
        return refused;
    }

    nya_account_throttle_succeed(username, address);

    *out_user = user;

    return NYA_OK;
}

NYA_Error nya_account_recovery_remaining(NYA_Arena* arena, u64 account_id, u32* out_remaining) {
    nya_assert(arena != nullptr && out_remaining != nullptr);

    *out_remaining = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.recovery_codes, arena, "WHERE account_id = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)account_id) }, 1, &rows, &count));

    *out_remaining = count;

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

void _nya_account_recovery_hash(NYA_ConstCString canonical, char* out_hex, u64 capacity) {
    nya_assert(out_hex != nullptr && capacity > ((u64)NYA_CRYPTO_SHA256_BYTES * 2));

    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256((const u8*)canonical, strlen(canonical), &digest);

    static const char HEX[] = "0123456789abcdef";

    for (u64 index = 0; index < NYA_CRYPTO_SHA256_BYTES; index++) {
        out_hex[(index * 2)]     = HEX[digest.bytes[index] >> 4];
        out_hex[(index * 2) + 1] = HEX[digest.bytes[index] & 0x0F];
    }

    out_hex[(u64)NYA_CRYPTO_SHA256_BYTES * 2] = '\0';
}

b8 _nya_account_recovery_canonical(NYA_ConstCString code, char* out, u64 capacity) {
    out[0] = '\0';

    if (code == nullptr) return false;

    u64 written = 0;

    for (u64 index = 0; code[index] != '\0'; index++) {
        u8 character = (u8)code[index];

        if (character >= 'a' && character <= 'z') character = (u8)(character - 'a' + 'A');

        // The base32 alphabet only: dashes, spaces and stray characters are dropped since a person reads these off paper.
        b8 is_base32 = (character >= 'A' && character <= 'Z') || (character >= '2' && character <= '7');

        if (!is_base32) continue;

        if (written + 1 >= capacity) return false;

        out[written++] = (char)character;
    }

    out[written] = '\0';

    // Exactly what a code is, or it is a typo. 5 bytes of base32 is 8 characters.
    return written == 8;
}
