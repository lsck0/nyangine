#include <string.h>

#include "nyangine/accounts/accounts_invite.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/db/db_orm.h"
#include "nyangine/os/os_random.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** SHA-256 of a canonical invite, as lower case hex. The same shape as the session and recovery hashes. */
NYA_INTERNAL void _nya_account_invite_hash(NYA_ConstCString canonical, OUT char* out_hex, u64 capacity);

/** The canonical form of a typed invite: upper case, base32 only, everything else dropped. */
NYA_INTERNAL b8 _nya_account_invite_canonical(NYA_ConstCString code, OUT char* out, u64 capacity) __attr_no_discard;

/** The live invite for this canonical code, or false when there is no unused, unexpired one. */
NYA_INTERNAL b8 _nya_account_invite_find_live(NYA_Arena* arena, NYA_ConstCString canonical, u64 now_s, OUT NYA_AccountInvite* out_invite) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_account_invite_issue(NYA_Arena* arena, u64 created_by, u64 ttl_s, char* out_code, u64 capacity) {
    nya_assert(arena != nullptr && out_code != nullptr && capacity > 0);

    out_code[0] = '\0';

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");
    if (ttl_s == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an invite needs an expiry; a code that never expires is a standing way in");

    u8 bytes[NYA_ACCOUNTS_INVITE_CODE_BYTES] = { 0 };
    if (!nya_os_random_bytes(bytes, sizeof(bytes))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    // 10 bytes are 16 base32 characters, no padding: the canonical code.
    char canonical[24] = { 0 };
    u64  length        = 0;

    NYA_TRY(nya_crypto_base32_encode(bytes, sizeof(bytes), canonical, sizeof(canonical), &length));

    nya_crypto_wipe(bytes, sizeof(bytes));

    while (length > 0 && canonical[length - 1] == '=') canonical[--length] = '\0';

    // Grouped in fours for a message, dropped again on the way in.
    s32 written = snprintf(out_code, capacity, "%.4s-%.4s-%.4s-%.4s", canonical, canonical + 4, canonical + 8, canonical + 12);

    if (written <= 0 || (u64)written >= capacity) {
        nya_crypto_wipe(canonical, sizeof(canonical));
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the invite code does not fit the buffer");
    }

    NYA_AccountInvite invite = { 0 };

    invite.created_by   = created_by;
    invite.created_at_s = nya_clock_get_timestamp_s();
    invite.expires_at_s = invite.created_at_s + ttl_s;

    _nya_account_invite_hash(canonical, invite.code_hash, sizeof(invite.code_hash));

    nya_crypto_wipe(canonical, sizeof(canonical));

    NYA_Error inserted = nya_orm_insert(_NYA_ACCOUNTS.invites, &invite);

    if (!inserted.ok) {
        nya_crypto_wipe(out_code, capacity);
        return inserted;
    }

    return NYA_OK;
}

NYA_Error nya_account_register(
    NYA_Arena* arena, NYA_AccountRegistration policy, NYA_ConstCString username, NYA_ConstCString password, NYA_ConstCString invite,
    NYA_AccountUser* out_user
) {
    nya_assert(arena != nullptr && out_user != nullptr);

    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    if (policy == NYA_ACCOUNT_REGISTRATION_CLOSED) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "this service does not take sign-ups; an administrator makes accounts");
    }

    NYA_AccountInvite live = { 0 };
    b8                spend = false;

    if (policy == NYA_ACCOUNT_REGISTRATION_INVITE) {
        char canonical[24] = { 0 };

        // An invite that is not even shaped like one is refused before the account is touched, and says
        // so: whoever is registering may know their code was bad.
        if (invite == nullptr || !_nya_account_invite_canonical(invite, canonical, sizeof(canonical))) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "that invite code is not valid");
        }

        if (!_nya_account_invite_find_live(arena, canonical, nya_clock_get_timestamp_s(), &live)) {
            return nya_error(NYA_ERROR_PERMISSION_DENIED, "that invite code is not valid, already used, or expired");
        }

        spend = true;
    }

    // The account first. Only once it is really made is the invite spent, so a taken username or a short
    // password leaves the code for another try rather than burning it.
    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_create(arena, username, password, &user));

    if (spend) {
        live.used_by    = user.id;
        live.used_at_s  = nya_clock_get_timestamp_s();

        NYA_Error updated = nya_orm_update(_NYA_ACCOUNTS.invites, &live);

        if (!updated.ok) {
            // The account exists but the invite could not be marked: undo the account rather than leave
            // a member who came in on a code that still reads as unused.
            (void)nya_account_destroy(arena, user.id);
            return updated;
        }
    }

    *out_user = user;

    return NYA_OK;
}

NYA_Error nya_account_invite_revoke(NYA_Arena* arena, NYA_ConstCString invite) {
    nya_assert(arena != nullptr);

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    char canonical[24] = { 0 };
    if (invite == nullptr || !_nya_account_invite_canonical(invite, canonical, sizeof(canonical))) return nya_error(NYA_ERROR_NOT_FOUND, "no such invite");

    NYA_AccountInvite live = { 0 };
    if (!_nya_account_invite_find_live(arena, canonical, nya_clock_get_timestamp_s(), &live)) return nya_error(NYA_ERROR_NOT_FOUND, "no live invite matches");

    return nya_orm_delete(_NYA_ACCOUNTS.invites, nya_sql_s64((s64)live.id));
}

NYA_Error nya_account_invite_list(NYA_Arena* arena, u64 created_by, NYA_AccountInvite** out_invites, u32* out_count) {
    nya_assert(arena != nullptr && out_invites != nullptr && out_count != nullptr);

    *out_invites = nullptr;
    *out_count   = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.invites, arena, "WHERE created_by = ? ORDER BY created_at_s DESC, id DESC", (NYA_SqlValue[]){ nya_sql_s64((s64)created_by) }, 1, &rows, &count
    ));

    *out_invites = (NYA_AccountInvite*)rows;
    *out_count   = count;

    return NYA_OK;
}

NYA_Error nya_account_invite_prune(NYA_Arena* arena, u64 keep_for_s, u32* out_removed) {
    nya_assert(arena != nullptr && out_removed != nullptr);

    *out_removed = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    u64 now_s  = nya_clock_get_timestamp_s();
    u64 cutoff = now_s > keep_for_s ? now_s - keep_for_s : 0;

    void* rows  = nullptr;
    u32   count = 0;

    // Spent before the cutoff, or expired before it: an unused invite that is still valid is kept.
    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.invites, arena, "WHERE (used_at_s > 0 AND used_at_s <= ?) OR (expires_at_s <= ?)",
        (NYA_SqlValue[]){ nya_sql_s64((s64)cutoff), nya_sql_s64((s64)cutoff) }, 2, &rows, &count
    ));

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountInvite* row = nya_orm_at(_NYA_ACCOUNTS.invites, rows, index);

        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.invites, nya_sql_s64((s64)row->id)));

        *out_removed += 1;
    }

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_account_invite_hash(NYA_ConstCString canonical, char* out_hex, u64 capacity) {
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

b8 _nya_account_invite_canonical(NYA_ConstCString code, char* out, u64 capacity) {
    out[0] = '\0';

    if (code == nullptr) return false;

    u64 written = 0;

    for (u64 index = 0; code[index] != '\0'; index++) {
        u8 character = (u8)code[index];

        if (character >= 'a' && character <= 'z') character = (u8)(character - 'a' + 'A');

        b8 is_base32 = (character >= 'A' && character <= 'Z') || (character >= '2' && character <= '7');
        if (!is_base32) continue;

        if (written + 1 >= capacity) return false;

        out[written++] = (char)character;
    }

    out[written] = '\0';

    // 10 bytes of base32 is 16 characters. Anything else is a typo, not a code.
    return written == 16;
}

b8 _nya_account_invite_find_live(NYA_Arena* arena, NYA_ConstCString canonical, u64 now_s, NYA_AccountInvite* out_invite) {
    nya_memset(out_invite, 0, sizeof(NYA_AccountInvite));

    char hex[72] = { 0 };
    _nya_account_invite_hash(canonical, hex, sizeof(hex));

    void* rows  = nullptr;
    u32   count = 0;

    NYA_Error found = nya_orm_select(
        _NYA_ACCOUNTS.invites, arena, "WHERE code_hash = ? AND used_at_s = 0 AND expires_at_s > ? LIMIT 1",
        (NYA_SqlValue[]){ nya_sql_text(hex), nya_sql_s64((s64)now_s) }, 2, &rows, &count
    );

    if (!found.ok || count == 0) return false;

    const NYA_AccountInvite* row = nya_orm_at(_NYA_ACCOUNTS.invites, rows, 0);

    *out_invite = *row;

    return true;
}
