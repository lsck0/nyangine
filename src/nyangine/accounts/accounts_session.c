#include <stdio.h>
#include <string.h>

#include "nyangine/accounts/accounts_session.h"
#include "nyangine/accounts/accounts_user.h"
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

/** SHA-256 of a token, as lower case hex: what a row stores and what a lookup searches. */
NYA_INTERNAL void _nya_account_session_hash(NYA_ConstCString token, OUT char* out_hex, u64 capacity);

/** Whether a session is usable right now: not revoked, and inside both expiries. */
NYA_INTERNAL b8 _nya_account_session_is_live(const NYA_AccountSession* session, u64 now_s) __attr_no_discard;

/** Ends the oldest sessions of a user until at most `keep` live ones are left. */
NYA_INTERNAL NYA_Error _nya_account_session_trim(NYA_Arena* arena, u64 user_id, u32 keep) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_account_session_issue(NYA_Arena* arena, u64 user_id, NYA_ConstCString address, NYA_ConstCString agent, NYA_AccountSession* out_session) {
    nya_assert(arena != nullptr && out_session != nullptr);

    nya_memset(out_session, 0, sizeof(NYA_AccountSession));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    // The user has to be there and has to be allowed in: a session for a disabled account would be a
    // way around nya_account_disable.
    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, user_id, &user));

    if (user.disabled) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that account is disabled");

    // Room first, so the trim never ends the session this call is about to hand back.
    NYA_TRY(_nya_account_session_trim(arena, user_id, NYA_ACCOUNTS_MAX_SESSIONS_PER_USER - 1));

    u8 secret[NYA_ACCOUNTS_TOKEN_BYTES] = { 0 };
    if (!nya_os_random_bytes(secret, sizeof(secret))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    NYA_AccountSession session = { 0 };
    u64                length  = 0;

    if (!nya_crypto_base64url_encode(secret, sizeof(secret), session.token, sizeof(session.token), &length)) {
        nya_crypto_wipe(secret, sizeof(secret));
        return nya_error(NYA_ERROR_NOT_OK, "the session token could not be encoded");
    }

    nya_crypto_wipe(secret, sizeof(secret));

    _nya_account_session_hash(session.token, session.token_hash, sizeof(session.token_hash));

    u64 now_s = nya_clock_get_timestamp_s();

    session.user_id      = user_id;
    session.created_at_s = now_s;
    session.used_at_s    = now_s;
    session.expires_at_s = now_s + NYA_ACCOUNTS_SESSION_IDLE_S;

    (void)snprintf(session.address, sizeof(session.address), "%s", address != nullptr ? address : "");
    (void)snprintf(session.agent, sizeof(session.agent), "%s", agent != nullptr ? agent : "");

    NYA_TRY(nya_orm_insert(_NYA_ACCOUNTS.sessions, &session));

    *out_session = session;

    return NYA_OK;
}

NYA_Error nya_account_session_validate(NYA_Arena* arena, NYA_ConstCString token, NYA_AccountSession* out_session) {
    nya_assert(arena != nullptr && out_session != nullptr);

    nya_memset(out_session, 0, sizeof(NYA_AccountSession));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    // One refusal for every way this fails, as accounts.h describes: which of them happened is what
    // somebody feeding tokens in wants to learn.
    NYA_Error refused = nya_error(NYA_ERROR_PERMISSION_DENIED, "that session is not valid");

    if (token == nullptr || token[0] == '\0') return refused;

    char hex[NYA_ACCOUNTS_TOKEN_HASH_BYTES] = { 0 };
    _nya_account_session_hash(token, hex, sizeof(hex));

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.sessions, arena, "WHERE token_hash = ? LIMIT 1", (NYA_SqlValue[]){ nya_sql_text(hex) }, 1, &rows, &count));

    if (count == 0) return refused;

    const NYA_AccountSession* row = nya_orm_at(_NYA_ACCOUNTS.sessions, rows, 0);

    NYA_AccountSession session = *row;

    u64 now_s = nya_clock_get_timestamp_s();

    if (!_nya_account_session_is_live(&session, now_s)) return refused;

    // The user is read on every validation rather than cached, so disabling an account ends what it
    // can do on the next request rather than on the next login.
    NYA_AccountUser user = { 0 };
    if (!nya_account_find_by_id(arena, session.user_id, &user).ok) return refused;
    if (user.disabled) return refused;

    /*
     * The idle expiry moves out, bounded by the absolute one: a session in use stays alive, and one
     * that is not stops on its own. Written once a minute at most — a row write on every request of
     * a busy server would be the session table doing more work than the request.
     */
    u64 pushed = now_s + NYA_ACCOUNTS_SESSION_IDLE_S;
    u64 ceiling = session.created_at_s + NYA_ACCOUNTS_SESSION_ABSOLUTE_S;

    if (pushed > ceiling) pushed = ceiling;

    if (now_s > session.used_at_s + 60) {
        session.used_at_s    = now_s;
        session.expires_at_s = pushed;

        NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.sessions, &session));
    }

    *out_session = session;

    // The token never travels back out: what the caller handed in is what it already has.
    nya_memset(out_session->token, 0, sizeof(out_session->token));

    return NYA_OK;
}

NYA_Error nya_account_session_revoke(NYA_Arena* arena, u64 session_id) {
    nya_assert(arena != nullptr);

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    NYA_AccountSession session = { 0 };
    NYA_TRY(nya_orm_find(_NYA_ACCOUNTS.sessions, arena, nya_sql_s64((s64)session_id), &session));

    if (session.revoked) return NYA_OK;

    session.revoked = true;

    return nya_orm_update(_NYA_ACCOUNTS.sessions, &session);
}

NYA_Error nya_account_session_revoke_all(NYA_Arena* arena, u64 user_id, u32* out_ended) {
    nya_assert(arena != nullptr && out_ended != nullptr);

    *out_ended = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.sessions, arena, "WHERE user_id = ? AND revoked = 0", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count));

    for (u32 index = 0; index < count; index++) {
        NYA_AccountSession* session = nya_orm_at(_NYA_ACCOUNTS.sessions, rows, index);

        session->revoked = true;

        NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.sessions, session));

        *out_ended += 1;
    }

    return NYA_OK;
}

NYA_Error nya_account_session_list(NYA_Arena* arena, u64 user_id, NYA_AccountSession** out_sessions, u32* out_count) {
    nya_assert(arena != nullptr && out_sessions != nullptr && out_count != nullptr);

    *out_sessions = nullptr;
    *out_count    = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.sessions, 
        arena, "WHERE user_id = ? ORDER BY created_at_s DESC, id DESC", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count
    ));

    *out_sessions = (NYA_AccountSession*)rows;
    *out_count    = count;

    return NYA_OK;
}

NYA_Error nya_account_session_purge(NYA_Arena* arena, u64 user_id, u32* out_removed) {
    nya_assert(arena != nullptr && out_removed != nullptr);

    *out_removed = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;

    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.sessions, arena, "WHERE user_id = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count));

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountSession* session = nya_orm_at(_NYA_ACCOUNTS.sessions, rows, index);

        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.sessions, nya_sql_s64((s64)session->id)));

        *out_removed += 1;
    }

    return NYA_OK;
}

NYA_Error nya_account_session_prune(NYA_Arena* arena, u64 keep_for_s, u32* out_removed) {
    nya_assert(arena != nullptr && out_removed != nullptr);

    *out_removed = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    u64 now_s = nya_clock_get_timestamp_s();
    u64 cutoff = now_s > keep_for_s ? now_s - keep_for_s : 0;

    void* rows  = nullptr;
    u32   count = 0;

    // Expired before the cutoff, or revoked and last used before it: a revoked row has no expiry that
    // moved, so its last use is the only date it has.
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.sessions, 
        arena,
        "WHERE (expires_at_s <= ?) OR (revoked = 1 AND used_at_s <= ?)",
        (NYA_SqlValue[]){ nya_sql_s64((s64)cutoff), nya_sql_s64((s64)cutoff) },
        2,
        &rows,
        &count
    ));

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountSession* session = nya_orm_at(_NYA_ACCOUNTS.sessions, rows, index);

        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.sessions, nya_sql_s64((s64)session->id)));

        *out_removed += 1;
    }

    return NYA_OK;
}

NYA_Error nya_account_session_sweep(NYA_Arena* arena, u32* out_ended, u32* out_removed) {
    nya_assert(arena != nullptr && out_ended != nullptr && out_removed != nullptr);

    *out_ended   = 0;
    *out_removed = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    u64 now_s = nya_clock_get_timestamp_s();

    /*
     * First, the abandoned: a session unused past the idle window is invalid by its expiry already, but
     * the row still reads as live. Revoked here so a list stops calling it a session, which is what the
     * reference's "closed as abandoned after 30 days" trigger did.
     */
    u64 idle_cutoff = now_s > NYA_ACCOUNTS_SESSION_IDLE_S ? now_s - NYA_ACCOUNTS_SESSION_IDLE_S : 0;

    void* abandoned = nullptr;
    u32   count     = 0;

    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.sessions, arena, "WHERE revoked = 0 AND used_at_s <= ?", (NYA_SqlValue[]){ nya_sql_s64((s64)idle_cutoff) }, 1, &abandoned, &count
    ));

    for (u32 index = 0; index < count; index++) {
        NYA_AccountSession* session = nya_orm_at(_NYA_ACCOUNTS.sessions, abandoned, index);

        session->revoked = true;

        NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.sessions, session));

        *out_ended += 1;
    }

    /*
     * Then the bound on how many dead rows a user keeps. Ordered oldest first, every revoked row past
     * the newest NYA_ACCOUNTS_SESSION_KEEP_REVOKED for its user is deleted — so a person with years of
     * logins carries a list that stops growing rather than one that never forgets.
     */
    void* revoked = nullptr;
    u32   revoked_count = 0;

    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.sessions, arena, "WHERE revoked = 1 ORDER BY user_id ASC, used_at_s DESC, id DESC", nullptr, 0, &revoked, &revoked_count
    ));

    u64 current_user = 0;
    u32 kept         = 0;

    for (u32 index = 0; index < revoked_count; index++) {
        const NYA_AccountSession* session = nya_orm_at(_NYA_ACCOUNTS.sessions, revoked, index);

        // The rows arrive grouped by user, newest first within each; the counter resets at each user, so
        // "kept" is how many of this user's revoked rows have already been seen this group.
        if (session->user_id != current_user) {
            current_user = session->user_id;
            kept         = 0;
        }

        kept++;

        if (kept <= NYA_ACCOUNTS_SESSION_KEEP_REVOKED) continue;

        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.sessions, nya_sql_s64((s64)session->id)));

        *out_removed += 1;
    }

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_account_session_hash(NYA_ConstCString token, char* out_hex, u64 capacity) {
    nya_assert(out_hex != nullptr && capacity > ((u64)NYA_CRYPTO_SHA256_BYTES * 2));

    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256((const u8*)token, strlen(token), &digest);

    static const char HEX[] = "0123456789abcdef";

    for (u64 index = 0; index < NYA_CRYPTO_SHA256_BYTES; index++) {
        out_hex[(index * 2)]     = HEX[digest.bytes[index] >> 4];
        out_hex[(index * 2) + 1] = HEX[digest.bytes[index] & 0x0F];
    }

    out_hex[(u64)NYA_CRYPTO_SHA256_BYTES * 2] = '\0';
}

b8 _nya_account_session_is_live(const NYA_AccountSession* session, u64 now_s) {
    if (session->revoked) return false;
    if (now_s >= session->expires_at_s) return false;

    // The absolute bound is checked against the start rather than against the stored expiry, so a
    // clock that went backwards or a row that was written by an older build cannot extend it.
    if (now_s >= session->created_at_s + NYA_ACCOUNTS_SESSION_ABSOLUTE_S) return false;

    return true;
}

NYA_Error _nya_account_session_trim(NYA_Arena* arena, u64 user_id, u32 keep) {
    void* rows  = nullptr;
    u32   count = 0;

    // Oldest first, so the ones ended are the ones least recently used.
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.sessions, 
        arena, "WHERE user_id = ? AND revoked = 0 ORDER BY used_at_s ASC, id ASC", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count
    ));

    u64 now_s = nya_clock_get_timestamp_s();

    // Only the live ones count against the limit: an expired row is already not a session.
    u32 live = 0;
    for (u32 index = 0; index < count; index++) {
        if (_nya_account_session_is_live(nya_orm_at(_NYA_ACCOUNTS.sessions, rows, index), now_s)) live++;
    }

    for (u32 index = 0; index < count && live > keep; index++) {
        NYA_AccountSession* session = nya_orm_at(_NYA_ACCOUNTS.sessions, rows, index);

        if (!_nya_account_session_is_live(session, now_s)) continue;

        session->revoked = true;

        NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.sessions, session));

        live--;
    }

    return NYA_OK;
}
