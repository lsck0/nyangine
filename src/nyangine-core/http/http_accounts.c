#include <stdio.h>
#include <string.h>

#include "nyangine-core/accounts/accounts.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_encoding.h"
#include "nyangine-core/crypto/crypto_totp.h"
#include "nyangine-core/db/db_orm.h"
#include "nyangine-core/http/http_accounts.h"
#include "nyangine-core/http/http_auth.h"
#include "nyangine-core/http/http_cookie.h"
#include "nyangine-core/http/http_message.h"
#include "nyangine-core/http/http_seal.h"
#include "nyangine-core/http/http_totp.h"
#include "nyangine-std/os/os_random.h"

// CONSTANTS

/** The second-factor table, one row per account, opened on the config's database beside the accounts tables. */
#define TOTP_TABLE "account_totp"

// The "password was right, code still owed" cookie: a __Host- cookie sealed with the config key (content unreadable/unforgeable), good 5 min — long enough to read a code, short enough a captured one is stale.
#define LOGIN_COOKIE        "__Host-login"
#define LOGIN_SEAL_LABEL    "accounts-login-totp"
#define LOGIN_PENDING_TTL_S 300

// Passkey ceremonies carry "who this flow is about" the same sealed-cookie way, under two separate labels so a register cookie can't open a login slot nor be replayed as the TOTP one; 5 min each (the challenge's own TTL).
#define PASSKEY_REGISTER_COOKIE     "__Host-passkey-register"
#define PASSKEY_REGISTER_SEAL_LABEL "accounts-passkey-register"
#define PASSKEY_LOGIN_COOKIE        "__Host-passkey-login"
#define PASSKEY_LOGIN_SEAL_LABEL    "accounts-passkey-login"
#define PASSKEY_PENDING_TTL_S       ((s64)NYA_ACCOUNTS_PASSKEY_CHALLENGE_TTL_S)

// Secret and recovery hashes are bytes but the ORM stores only INTEGER/REAL/TEXT, so held as text (secret as base32, ten recovery hashes as comma-joined base64url); decoded on use, never logged.
#define TOTP_SECRET_TEXT   NYA_HTTP_TOTP_SECRET_TEXT_BYTES // 32 base32 characters and a terminator.
#define TOTP_RECOVERY_TEXT 512                             // Ten base64url hashes with commas fit inside this.

// PRIVATE TYPES

// The second factor as a row: secret and recovery codes as text, the guard as three integers, and whether enrolment was confirmed; one per account, keyed by account id.
typedef struct {
    s64  owner; // The account id, and the key: one second factor per account.
    char secret[TOTP_SECRET_TEXT];
    s64  last_counter;     // NYA_HttpTotpGuard.last_counter, the replay guard.
    s64  attempts;         // NYA_HttpTotpGuard.attempts in the current window.
    s64  window_started_s; // NYA_HttpTotpGuard.window_started_s.
    s64  confirmed;        // Zero until one code has verified against the secret; see the enrol/confirm ceremony.
    char recovery[TOTP_RECOVERY_TEXT];
} _NYA_HttpAccountsTotpRow;

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_ACCOUNTS_SECRET_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = TOTP_SECRET_TEXT,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = TOTP_SECRET_TEXT,
};

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_ACCOUNTS_RECOVERY_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = TOTP_RECOVERY_TEXT,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = TOTP_RECOVERY_TEXT,
};

NYA_INTERNAL const NYA_ReflectField _NYA_HTTP_ACCOUNTS_TOTP_FIELDS[] = {
    { .name = "owner", .type = nya_reflect_of(s64), .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, owner), .is_key = true },
    { .name = "secret", .type = &_NYA_HTTP_ACCOUNTS_SECRET_ARRAY, .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, secret) },
    { .name = "last_counter", .type = nya_reflect_of(s64), .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, last_counter) },
    { .name = "attempts", .type = nya_reflect_of(s64), .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, attempts) },
    { .name = "window_started_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, window_started_s) },
    { .name = "confirmed", .type = nya_reflect_of(s64), .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, confirmed) },
    { .name = "recovery", .type = &_NYA_HTTP_ACCOUNTS_RECOVERY_ARRAY, .offset = nya_offsetof(_NYA_HttpAccountsTotpRow, recovery) },
};

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_ACCOUNTS_TOTP_MODEL = {
    .name        = "_NYA_HttpAccountsTotpRow",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(_NYA_HttpAccountsTotpRow),
    .alignment   = alignof(_NYA_HttpAccountsTotpRow),
    .fields      = _NYA_HTTP_ACCOUNTS_TOTP_FIELDS,
    .field_count = nya_carray_length(_NYA_HTTP_ACCOUNTS_TOTP_FIELDS),
};

// STATE

// The module's one state: the config a mount opened with and the second-factor table; handlers are plain function pointers with no user data, so what they read lives here. One process mounts once, like accounts.
NYA_INTERNAL struct {
    b8                    open;
    NYA_HttpAccountsConfig config;
    NYA_OrmTable*         totp;
} _STATE = { 0 };

// REQUEST HELPERS

/** Reads `username` and `password` out of a JSON body, refusing anything that is not both strings. */
NYA_INTERNAL b8 request_credentials(NYA_HttpExchange* exchange, OUT NYA_ConstCString* out_username, OUT NYA_ConstCString* out_password) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return false;

    NYA_Value* username = nya_object_get(body, "username");
    NYA_Value* password = nya_object_get(body, "password");

    if (username == nullptr || username->type != NYA_TYPE_STRING) return false;
    if (password == nullptr || password->type != NYA_TYPE_STRING) return false;

    *out_username = username->as_string;
    *out_password = password->as_string;

    return true;
}

/**
 * The same, plus the optional `invite` code an INVITE policy needs. `out_invite` is null when the body
 * carries none, which OPEN ignores and INVITE refuses; the policy is the one thing that reads it.
 * */
NYA_INTERNAL b8 request_registration(
    NYA_HttpExchange* exchange, OUT NYA_ConstCString* out_username, OUT NYA_ConstCString* out_password, OUT NYA_ConstCString* out_invite
) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return false;

    NYA_Value* username = nya_object_get(body, "username");
    NYA_Value* password = nya_object_get(body, "password");

    if (username == nullptr || username->type != NYA_TYPE_STRING) return false;
    if (password == nullptr || password->type != NYA_TYPE_STRING) return false;

    *out_username = username->as_string;
    *out_password = password->as_string;

    NYA_Value* invite = nya_object_get(body, "invite");
    *out_invite       = (invite != nullptr && invite->type == NYA_TYPE_STRING) ? invite->as_string : nullptr;

    return true;
}

/** The peer's address as the server sees it, for the session's own record of where it was opened. */
NYA_INTERNAL NYA_ConstCString request_address(NYA_HttpExchange* exchange) {
    return exchange->address[0] != '\0' ? exchange->address : "unknown";
}

/** Reads a single `code` string out of a JSON body, for a second factor submission. */
NYA_INTERNAL b8 request_code(NYA_HttpExchange* exchange, OUT NYA_ConstCString* out_code) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return false;

    NYA_Value* code = nya_object_get(body, "code");
    if (code == nullptr || code->type != NYA_TYPE_STRING || code->as_string[0] == '\0') return false;

    *out_code = code->as_string;
    return true;
}

// SESSION HELPERS

/** Sets the session cookie from a token, with the attributes a session cookie must have. */
NYA_INTERNAL NYA_HttpStatus set_session_cookie(NYA_HttpExchange* exchange, NYA_ConstCString token) {
    NYA_Error set = nya_http_response_cookie(exchange->response,
                                             &(NYA_HttpCookie){
                                                 .name      = NYA_HTTP_SESSION_COOKIE,
                                                 .value     = token,
                                                 .max_age_s = NYA_ACCOUNTS_SESSION_IDLE_S,
                                                 .http_only = true,
                                                 .secure    = true,
                                                 .same_site = _STATE.config.session_same_site,
                                             });

    return set.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Opens a session for a user and sets it as the session cookie. The full login, once every factor is in. */
NYA_INTERNAL NYA_HttpStatus issue_session(NYA_HttpExchange* exchange, u64 user_id) {
    NYA_ConstCString agent = nya_http_request_header(exchange->request, "user-agent");

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_issue(exchange->arena, user_id, request_address(exchange), agent, &session).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return set_session_cookie(exchange, session.token);
}

/** Seals "this account passed its password" into the pending-login cookie. */
NYA_INTERNAL NYA_HttpStatus set_pending_cookie(NYA_HttpExchange* exchange, u64 user_id) {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };

    if (!nya_http_seal(_STATE.config.login_seal_secret, _STATE.config.login_seal_secret_size, LOGIN_SEAL_LABEL, (const u8*)&user_id, sizeof(user_id),
                       LOGIN_PENDING_TTL_S, token, sizeof(token))
             .ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Error set = nya_http_response_cookie(exchange->response,
                                             &(NYA_HttpCookie){
                                                 .name      = LOGIN_COOKIE,
                                                 .value     = token,
                                                 .max_age_s = LOGIN_PENDING_TTL_S,
                                                 .http_only = true,
                                                 .secure    = true,
                                                 .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                             });

    return set.ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Opens the pending-login cookie, or false when there is none, it was tampered with, or it has expired. */
NYA_INTERNAL b8 read_pending_cookie(NYA_HttpExchange* exchange, OUT u64* out_user_id) {
    *out_user_id = 0;

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, LOGIN_COOKIE, &cookie)) return false;

    u64 user_id = 0;
    u64 size    = 0;

    if (!nya_http_unseal(_STATE.config.login_seal_secret, _STATE.config.login_seal_secret_size, LOGIN_SEAL_LABEL, cookie.text, cookie.size,
                         (u8*)&user_id, sizeof(user_id), &size)) {
        return false;
    }

    if (size != sizeof(user_id)) return false;

    *out_user_id = user_id;
    return true;
}

/**
 * Seals a user id into a `__Host-` cookie under `label`, good for `ttl_s` seconds.
 *
 * The one shape the passkey ceremonies keep their "who this flow is about" in — a register cookie set on
 * the signed-out path, a login cookie naming the account a challenge was minted for. `__Host-`, HttpOnly,
 * Secure and SameSite=Strict, because it is a bearer of the same weight as the pending-login cookie and
 * carries none of it to script or across a site.
 * */
NYA_INTERNAL b8 set_sealed_user_cookie(NYA_HttpExchange* exchange, NYA_ConstCString name, NYA_ConstCString label, u64 user_id, u64 ttl_s) {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };

    if (!nya_http_seal(_STATE.config.login_seal_secret, _STATE.config.login_seal_secret_size, label, (const u8*)&user_id, sizeof(user_id), ttl_s, token,
                       sizeof(token))
             .ok) {
        return false;
    }

    return nya_http_response_cookie(exchange->response,
                                    &(NYA_HttpCookie){
                                        .name      = name,
                                        .value     = token,
                                        .max_age_s = ttl_s,
                                        .http_only = true,
                                        .secure    = true,
                                        .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                    })
        .ok;
}

/** Opens a sealed user-id cookie set by set_sealed_user_cookie, or false when there is none, it is forged, or it has expired. */
NYA_INTERNAL b8 read_sealed_user_cookie(NYA_HttpExchange* exchange, NYA_ConstCString name, NYA_ConstCString label, OUT u64* out_user_id) {
    *out_user_id = 0;

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, name, &cookie)) return false;

    u64 user_id = 0;
    u64 size    = 0;

    if (!nya_http_unseal(_STATE.config.login_seal_secret, _STATE.config.login_seal_secret_size, label, cookie.text, cookie.size, (u8*)&user_id,
                         sizeof(user_id), &size)) {
        return false;
    }

    if (size != sizeof(user_id)) return false;

    *out_user_id = user_id;
    return true;
}

// SECOND FACTOR HELPERS

/** Whether a submission is an authenticator code — exactly six digits — rather than a recovery code. */
NYA_INTERNAL b8 code_is_totp_shaped(NYA_ConstCString code) {
    if (strlen(code) != NYA_CRYPTO_TOTP_DIGITS) return false;

    for (u32 index = 0; index < NYA_CRYPTO_TOTP_DIGITS; index++) {
        if (code[index] < '0' || code[index] > '9') return false;
    }

    return true;
}

/** The guard the http_totp calls take, read out of a stored row. Plain data, no secret in it. */
NYA_INTERNAL NYA_HttpTotpGuard totp_guard_of(const _NYA_HttpAccountsTotpRow* row) {
    return (NYA_HttpTotpGuard){
        .last_counter     = (u64)row->last_counter,
        .attempts         = (u32)row->attempts,
        .window_started_s = (u64)row->window_started_s,
    };
}

/** Writes an advanced guard back into a row, so the replay guard and the rate limit survive the request. */
NYA_INTERNAL void totp_guard_store(_NYA_HttpAccountsTotpRow* row, const NYA_HttpTotpGuard* guard) {
    row->last_counter     = (s64)guard->last_counter;
    row->attempts         = (s64)guard->attempts;
    row->window_started_s = (s64)guard->window_started_s;
}

/** The stored base32 secret back to its twenty bytes. False on a row this module did not write. */
NYA_INTERNAL b8 totp_secret_decode(const _NYA_HttpAccountsTotpRow* row, OUT NYA_CryptoTotpSecret* out_secret) {
    nya_memset(out_secret, 0, sizeof(*out_secret));

    u64 size = 0;
    if (!nya_crypto_base32_decode(row->secret, strlen(row->secret), out_secret->bytes, sizeof(out_secret->bytes), &size).ok) return false;

    return size == sizeof(out_secret->bytes);
}

/** The ten recovery hashes as one comma-joined base64url string, for the text column. */
NYA_INTERNAL b8 totp_recovery_encode(const NYA_HttpTotpRecoveryHash hashes[NYA_HTTP_TOTP_RECOVERY_CODES], OUT char* out_text, u64 capacity) {
    u64 written = 0;

    for (u32 index = 0; index < NYA_HTTP_TOTP_RECOVERY_CODES; index++) {
        char encoded[64] = { 0 };
        u64  size        = 0;

        if (!nya_crypto_base64url_encode(hashes[index].bytes, sizeof(hashes[index].bytes), encoded, sizeof(encoded), &size)) return false;

        s32 count = snprintf(out_text + written, capacity - written, "%s%s", index > 0 ? "," : "", encoded);
        if (count < 0 || (u64)count >= capacity - written) return false;

        written += (u64)count;
    }

    return true;
}

/** The comma-joined base64url string back into the ten hashes. Missing entries stay zero, which match nothing. */
NYA_INTERNAL b8 totp_recovery_decode(const char* text, OUT NYA_HttpTotpRecoveryHash hashes[NYA_HTTP_TOTP_RECOVERY_CODES]) {
    nya_memset(hashes, 0, sizeof(NYA_HttpTotpRecoveryHash) * NYA_HTTP_TOTP_RECOVERY_CODES);

    if (text == nullptr || text[0] == '\0') return true;

    const char* cursor = text;

    for (u32 index = 0; index < NYA_HTTP_TOTP_RECOVERY_CODES; index++) {
        const char* comma  = strchr(cursor, ',');
        u64         length = comma != nullptr ? (u64)(comma - cursor) : strlen(cursor);
        u64         size   = 0;

        if (!nya_crypto_base64url_decode(cursor, length, hashes[index].bytes, sizeof(hashes[index].bytes), &size)) return false;
        if (size != sizeof(hashes[index].bytes)) return false;

        if (comma == nullptr) break;
        cursor = comma + 1;
    }

    return true;
}

/** The account's second factor row, or false when there is none. */
NYA_INTERNAL b8 totp_find(NYA_HttpExchange* exchange, u64 owner, OUT _NYA_HttpAccountsTotpRow* out_row) {
    nya_memset(out_row, 0, sizeof(*out_row));
    return nya_orm_find(_STATE.totp, exchange->arena, nya_sql_s64((s64)owner), out_row).ok;
}

/** Whether the account has a confirmed second factor, which is what makes a login two steps. */
NYA_INTERNAL b8 totp_is_enrolled(NYA_HttpExchange* exchange, u64 owner) {
    _NYA_HttpAccountsTotpRow row = { 0 };
    return totp_find(exchange, owner, &row) && row.confirmed != 0;
}

// HANDLERS: ACCOUNTS

/** Registration, under the config's policy. A taken name, a short password or a bad invite says which. */
NYA_INTERNAL NYA_HttpStatus handle_register(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;
    NYA_ConstCString invite   = nullptr;

    if (!request_registration(exchange, &username, &password, &invite)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user = { 0 };
    NYA_Error       made = nya_account_register(exchange->arena, _STATE.config.registration, username, password, invite, &user);

    if (!made.ok) {
        // A taken name, bad password or refused invite are the caller's to fix, so they're told; anything else is a server fault. Registration isn't a login: no reason to hide. See accounts_invite.h.
        if (made.kind == NYA_ERROR_ALREADY_EXISTS || made.kind == NYA_ERROR_INVALID_ARGUMENT || made.kind == NYA_ERROR_PERMISSION_DENIED
            || made.kind == NYA_ERROR_NOT_FOUND) {
            return NYA_HTTP_STATUS_UNPROCESSABLE;
        }

        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

/** Answers a login with "the password was right, now send a code", the pending state sealed in a cookie. */
NYA_INTERNAL NYA_HttpStatus answer_second_factor_required(NYA_HttpExchange* exchange, u64 user_id) {
    NYA_HttpStatus sealed = set_pending_cookie(exchange, user_id);
    if (sealed != NYA_HTTP_STATUS_OK) return sealed;

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "second_factor_required", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * A password. On success either a session in a cookie, or — when the account has a second factor — the
 * "code still owed" state, sealed in the pending-login cookie, and no session until a code answers it.
 *
 * The throttle in accounts slows a guessing spree whatever the outcome, and one refusal covers every way
 * the password step can fail so nothing here tells a guesser which usernames exist.
 * */
NYA_INTERNAL NYA_HttpStatus handle_login(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;

    if (!request_credentials(exchange, &username, &password)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user    = { 0 };
    NYA_Error       allowed = nya_account_authenticate(exchange->arena, username, password, request_address(exchange), &user);

    // One answer for every failure (wrong password, no such user, disabled, throttled) so the response tells a guesser nothing; 401, since credentials were refused.
    if (!allowed.ok) return NYA_HTTP_STATUS_UNAUTHORIZED;

    // A confirmed second factor makes this only the first of two steps: hold the account in a sealed cookie and issue nothing actionable until the code answers.
    if (totp_is_enrolled(exchange, user.id)) return answer_second_factor_required(exchange, user.id);

    return issue_session(exchange, user.id);
}

/**
 * The second step: a code against the account the pending-login cookie names, and a session only if it
 * verifies. A submitted value is tried as an authenticator code and then, failing that, as a recovery
 * code; either way the guard is written back so the replay guard and the rate limit hold across requests.
 *
 * One refusal — 401 — for a wrong code, an expired or forged pending cookie and a code for an account
 * with no second factor alike, and 429 when the guard is spent, which is the only verdict that says
 * anything and says only "later".
 * */
NYA_INTERNAL NYA_HttpStatus handle_login_totp(NYA_HttpExchange* exchange) {
    u64 user_id = 0;
    if (!read_pending_cookie(exchange, &user_id)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_ConstCString code = nullptr;
    if (!request_code(exchange, &code)) return NYA_HTTP_STATUS_BAD_REQUEST;

    _NYA_HttpAccountsTotpRow row = { 0 };
    if (!totp_find(exchange, user_id, &row) || row.confirmed == 0) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_CryptoTotpSecret secret = { 0 };
    if (!totp_secret_decode(&row, &secret)) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    defer nya_crypto_totp_secret_destroy(&secret);

    NYA_HttpTotpGuard guard = totp_guard_of(&row);

    // Six digits read as an authenticator code, anything else as a recovery code: the forms never overlap, so routing on shape spends one attempt and never distinguishes them in the answer; a redeemed recovery code is zeroed and the row rewritten, and a failed re-encode refuses rather than issuing a session on an unsaved spend.
    NYA_HttpTotpVerdict verdict = NYA_HTTP_TOTP_REFUSED;

    if (code_is_totp_shaped(code)) {
        verdict = nya_http_totp_verify(&guard, &secret, code, exchange->now_s);
    } else {
        NYA_HttpTotpRecoveryHash hashes[NYA_HTTP_TOTP_RECOVERY_CODES] = { 0 };

        if (totp_recovery_decode(row.recovery, hashes)) {
            verdict = nya_http_totp_recovery_redeem(&guard, hashes, NYA_HTTP_TOTP_RECOVERY_CODES, code, exchange->now_s);

            if (verdict == NYA_HTTP_TOTP_ACCEPTED && !totp_recovery_encode(hashes, row.recovery, sizeof(row.recovery))) {
                return NYA_HTTP_STATUS_INTERNAL_ERROR;
            }
        }
    }

    // The guard moved whatever the verdict (an attempt spent, or a counter advanced), so it's written back first, or the replay guard forgets and the rate limit resets.
    totp_guard_store(&row, &guard);
    if (!nya_orm_update(_STATE.totp, &row).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    if (verdict == NYA_HTTP_TOTP_RATE_LIMITED) return NYA_HTTP_STATUS_TOO_MANY_REQUESTS;
    if (verdict != NYA_HTTP_TOTP_ACCEPTED) return NYA_HTTP_STATUS_UNAUTHORIZED;

    // The factor is proved: clear the pending cookie so it cannot be replayed, and issue the real session.
    (void)nya_http_response_cookie_clear(exchange->response, LOGIN_COOKIE, "/", true);

    return issue_session(exchange, user_id);
}

/** Signing out: the session row is revoked, not only the cookie cleared, so the token cannot be reused. */
NYA_INTERNAL NYA_HttpStatus handle_logout(NYA_HttpExchange* exchange) {
    NYA_HttpCookieValue cookie = { 0 };

    if (nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) {
        char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };

        if (cookie.size < sizeof(token)) {
            nya_memcpy(token, cookie.text, cookie.size);

            NYA_AccountSession session = { 0 };

            // Validate to find the row, then revoke it; an already-invalid cookie is nothing to revoke, but clearing it is still right.
            if (nya_account_session_validate(exchange->arena, token, &session).ok) {
                (void)nya_account_session_revoke(exchange->arena, session.id);
            }
        }
    }

    NYA_Error cleared = nya_http_response_cookie_clear(exchange->response, NYA_HTTP_SESSION_COOKIE, "/", true);

    return cleared.ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Who the cookie says you are. 401 when it says nobody. */
NYA_INTERNAL NYA_HttpStatus handle_session(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* body = nya_object_create(exchange->arena);

    nya_object_add(body, "id", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = user.id });
    nya_object_add(body, "username", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user.username });
    nya_object_add(body, "display", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user.display });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

// HANDLERS: THE SECOND FACTOR

/**
 * Begins enrolling a second factor for the signed-in account.
 *
 * A secret, the otpauth URI a phone scans, and the recovery codes, all shown once in this one response —
 * every field of it `@redact`ed so none reaches a log. Nothing is switched on yet: the row is stored
 * unconfirmed, and RFC 6238 enrolment is not finished until the confirm route proves the phone and this
 * server compute the same code. 409 when a confirmed factor is already there, so one cannot be silently
 * replaced.
 * */
NYA_INTERNAL NYA_HttpStatus handle_totp_enrol(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    _NYA_HttpAccountsTotpRow existing = { 0 };
    b8                       present  = totp_find(exchange, user.id, &existing);
    if (present && existing.confirmed != 0) return NYA_HTTP_STATUS_CONFLICT;

    NYA_HttpTotpEnrolment enrolment = { 0 };
    if (!nya_http_totp_enrol_create(_STATE.config.totp_issuer, user.username, &enrolment).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    defer nya_http_totp_enrol_destroy(&enrolment);

    // A fresh, unconfirmed row: the secret as base32, a guard that has accepted nothing, no confirmation.
    _NYA_HttpAccountsTotpRow row = { .owner = (s64)user.id, .confirmed = 0 };
    (void)snprintf(row.secret, sizeof(row.secret), "%s", enrolment.secret_base32);

    if (!totp_recovery_encode(enrolment.recovery_hash, row.recovery, sizeof(row.recovery))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error stored = present ? nya_orm_update(_STATE.totp, &row) : nya_orm_insert(_STATE.totp, &row);
    if (!stored.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // The one answer that carries the secret; the DTO's fields are all `@redact`, so the request log keeps none of it. See http_totp.h and http_log.h.
    NYA_HttpTotpEnrolmentDto dto = { 0 };
    (void)snprintf(dto.uri, sizeof(dto.uri), "%s", enrolment.uri);
    (void)snprintf(dto.secret, sizeof(dto.secret), "%s", enrolment.secret_base32);

    for (u32 index = 0; index < NYA_HTTP_TOTP_RECOVERY_CODES; index++) {
        (void)snprintf(dto.recovery[index].code, sizeof(dto.recovery[index].code), "%s", enrolment.recovery[index]);
    }

    return nya_http_response_reflect(exchange->response, exchange->arena, nya_reflect_of(NYA_HttpTotpEnrolmentDto), &dto).ok ? NYA_HTTP_STATUS_OK
                                                                                                                            : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * Finishes enrolment: one code against the pending secret, and the factor is on.
 *
 * The same verify and the same guard the login's second step uses, so a code cannot be replayed here to
 * confirm and there to log in. 401 for a wrong code, 429 when the guard is spent, 404 when there is no
 * enrolment in progress, 409 when it is already confirmed.
 * */
NYA_INTERNAL NYA_HttpStatus handle_totp_confirm(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!nya_http_accounts_caller(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_ConstCString code = nullptr;
    if (!request_code(exchange, &code)) return NYA_HTTP_STATUS_BAD_REQUEST;

    _NYA_HttpAccountsTotpRow row = { 0 };
    if (!totp_find(exchange, user.id, &row)) return NYA_HTTP_STATUS_NOT_FOUND;
    if (row.confirmed != 0) return NYA_HTTP_STATUS_CONFLICT;

    NYA_CryptoTotpSecret secret = { 0 };
    if (!totp_secret_decode(&row, &secret)) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    defer nya_crypto_totp_secret_destroy(&secret);

    NYA_HttpTotpGuard   guard   = totp_guard_of(&row);
    NYA_HttpTotpVerdict verdict = nya_http_totp_verify(&guard, &secret, code, exchange->now_s);

    totp_guard_store(&row, &guard);
    if (verdict == NYA_HTTP_TOTP_ACCEPTED) row.confirmed = 1;

    if (!nya_orm_update(_STATE.totp, &row).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    if (verdict == NYA_HTTP_TOTP_RATE_LIMITED) return NYA_HTTP_STATUS_TOO_MANY_REQUESTS;
    if (verdict != NYA_HTTP_TOTP_ACCEPTED) return NYA_HTTP_STATUS_UNAUTHORIZED;

    return NYA_HTTP_STATUS_NO_CONTENT;
}

// PASSKEY HELPERS

/** An optional string field of a JSON body, or null when it is absent or not a string. */
NYA_INTERNAL NYA_ConstCString passkey_body_string(NYA_Object* body, NYA_ConstCString key) {
    if (body == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(body, (NYA_CString)key);
    return (value != nullptr && value->type == NYA_TYPE_STRING) ? value->as_string : nullptr;
}

/**
 * A base64url string field decoded into arena bytes, bounded by what a WebAuthn part may be.
 *
 * The wire carries a clientDataJSON, an attestation object, an authenticator data and a signature as
 * base64url text; each is decoded here into a fixed buffer no larger than NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES,
 * so a hostile body cannot make this allocate without limit and a part longer than that is refused rather
 * than truncated. False for a missing field, a non-string, or text that does not decode inside the bound.
 * */
NYA_INTERNAL b8 passkey_body_bytes(NYA_Object* body, NYA_ConstCString key, NYA_Arena* arena, OUT const u8** out_bytes, OUT u64* out_size) {
    *out_bytes = nullptr;
    *out_size  = 0;

    NYA_ConstCString text = passkey_body_string(body, key);
    if (text == nullptr) return false;

    u64 text_size = strlen(text);
    if (text_size == 0 || text_size > (u64)NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES * 2) return false;

    u8* buffer = nya_arena_alloc(arena, NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES);
    if (buffer == nullptr) return false;

    u64 size = 0;
    if (!nya_crypto_base64url_decode(text, text_size, buffer, NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES, &size)) return false;

    *out_bytes = buffer;
    *out_size  = size;
    return true;
}

/**
 * The `navigator.credentials.create` options a registration begins with: the challenge, the relying party
 * id, the user handle and label, and the one algorithm this verifies.
 *
 * `pubKeyCredParams` is `[-8]` alone, so a compliant client only ever mints an Ed25519 credential — the
 * curve accounts_passkey can check; an ES256 one would be refused at finish. The user handle is the account
 * id as base64url, the opaque `user.id` WebAuthn wants, echoed back as an assertion's userHandle.
 * */
NYA_INTERNAL NYA_HttpStatus passkey_creation_options(NYA_HttpExchange* exchange, const NYA_AccountUser* user, NYA_ConstCString challenge) {
    char handle[NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_TEXT] = { 0 };
    u64  handle_size                                  = 0;
    if (!nya_crypto_base64url_encode((const u8*)&user->id, sizeof(user->id), handle, sizeof(handle), &handle_size)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "challenge", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)challenge });
    nya_object_add(body, "rp_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)_STATE.config.passkey_rp_id });

    NYA_Object* user_object = nya_object_create(exchange->arena);
    nya_object_add(user_object, "id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)handle });
    nya_object_add(user_object, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user->username });
    nya_object_add(user_object, "display", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)user->display });
    nya_object_add(body, "user", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *user_object });

    NYA_ArrayᐸNYA_Valueᐳ* params = nya_array_create(exchange->arena, NYA_Value);
    NYA_Object*          algorithm = nya_object_create(exchange->arena);
    nya_object_add(algorithm, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "public-key" });
    nya_object_add(algorithm, "alg", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = NYA_ACCOUNTS_PASSKEY_COSE_ALG_EDDSA });
    nya_array_add(params, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *algorithm }));
    nya_object_add(body, "pub_key_cred_params", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *params });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * The `navigator.credentials.get` options a login begins with: the challenge, the relying party id, and
 * the credentials the account may sign with.
 *
 * A `user_id` of zero is the decoy an unknown or disabled username gets — no credentials are listed, and
 * the challenge is one that was never stored, so the finish refuses it exactly as a wrong assertion for a
 * real account. The `allowCredentials` list is the ids from nya_account_passkey_list; a resident
 * (discoverable) credential still works through this flow, the username only saying which account the
 * challenge is bound to.
 * */
NYA_INTERNAL NYA_HttpStatus passkey_request_options(NYA_HttpExchange* exchange, u64 user_id, NYA_ConstCString challenge) {
    NYA_Object* body = nya_object_create(exchange->arena);
    nya_object_add(body, "challenge", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)challenge });
    nya_object_add(body, "rp_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)_STATE.config.passkey_rp_id });

    NYA_ArrayᐸNYA_Valueᐳ* allow = nya_array_create(exchange->arena, NYA_Value);

    if (user_id != 0) {
        NYA_AccountPasskey* list  = nullptr;
        u32                 count = 0;
        if (!nya_account_passkey_list(exchange->arena, user_id, &list, &count).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

        for (u32 index = 0; index < count; index++) {
            NYA_Object* descriptor = nya_object_create(exchange->arena);
            nya_object_add(descriptor, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "public-key" });
            nya_object_add(descriptor, "id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)list[index].credential_id });
            nya_array_add(allow, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *descriptor }));
        }
    }

    nya_object_add(body, "allow_credentials", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *allow });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

// HANDLERS: PASSKEYS, THE PASSWORDLESS FACTOR

/**
 * Begins enrolling a passkey, and answers the creation options and a challenge.
 *
 * Signed in, it adds a device to the caller's account. Signed out, it is a passwordless sign-up: an
 * account is created with no password and the enrolment is bound to it in a sealed `__Host-passkey-register`
 * cookie, so the finish knows which brand-new account the credential is for without a session existing yet.
 * The signed-out path only runs under an OPEN registration policy — under INVITE or CLOSED it is 403, so a
 * passkey sign-up is never a way past the invite gate; those policies want a registered, signed-in account
 * before a passkey is added.
 * */
NYA_INTERNAL NYA_HttpStatus handle_passkey_register_begin(NYA_HttpExchange* exchange) {
    // The body is optional on the signed-in path and carries the username on the signed-out one; a failed parse leaves it null, which the signed-out path checks.
    NYA_Object* body = nullptr;
    (void)nya_http_request_document(exchange->request, exchange->arena, &body);

    NYA_AccountUser user      = { 0 };
    b8              signed_in = nya_http_accounts_caller(exchange, &user);

    if (!signed_in) {
        if (_STATE.config.registration != NYA_ACCOUNT_REGISTRATION_OPEN) return NYA_HTTP_STATUS_FORBIDDEN;

        NYA_ConstCString username = passkey_body_string(body, "username");
        if (username == nullptr || username[0] == '\0') return NYA_HTTP_STATUS_BAD_REQUEST;

        // A passwordless account: the password column stays empty so nya_account_authenticate refuses it, and the passkey is the only way in until a password is set. A taken/malformed name is the caller's to fix — a sign-up, not a login.
        NYA_Error made = nya_account_create_without_password(exchange->arena, username, &user);
        if (!made.ok) {
            if (made.kind == NYA_ERROR_ALREADY_EXISTS || made.kind == NYA_ERROR_INVALID_ARGUMENT) return NYA_HTTP_STATUS_UNPROCESSABLE;
            return NYA_HTTP_STATUS_INTERNAL_ERROR;
        }
    }

    NYA_AccountPasskeyChallenge challenge = { 0 };
    if (!nya_account_passkey_register_begin(exchange->arena, user.id, &challenge).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // Only the signed-out path needs the binding cookie; a signed-in enrolment is bound by the session the finish revalidates, so it sets nothing here.
    if (!signed_in && !set_sealed_user_cookie(exchange, PASSKEY_REGISTER_COOKIE, PASSKEY_REGISTER_SEAL_LABEL, user.id, (u64)PASSKEY_PENDING_TTL_S))
        return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return passkey_creation_options(exchange, &user, challenge.challenge);
}

/**
 * Finishes enrolling a passkey: verifies the `navigator.credentials.create` response and stores the
 * credential.
 *
 * Who is enrolling is the session when there is one, else the sealed register cookie the begin set — so a
 * passwordless sign-up is finished by the same account the begin created, and nobody else. On that
 * signed-out path a stored credential is proof of possession of the new account, so the register cookie is
 * cleared and the session cookie set: the sign-up ends logged in, exactly as a password registration
 * followed by a login would. The relying party checked is the config's, not a request header's.
 * */
NYA_INTERNAL NYA_HttpStatus handle_passkey_register_finish(NYA_HttpExchange* exchange) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user       = { 0 };
    b8              signed_in  = nya_http_accounts_caller(exchange, &user);
    u64             user_id    = user.id;
    b8              via_cookie = false;

    if (!signed_in) {
        if (!read_sealed_user_cookie(exchange, PASSKEY_REGISTER_COOKIE, PASSKEY_REGISTER_SEAL_LABEL, &user_id)) return NYA_HTTP_STATUS_UNAUTHORIZED;
        via_cookie = true;
    }

    const u8* client_data      = nullptr;
    u64       client_data_size = 0;
    const u8* attestation      = nullptr;
    u64       attestation_size = 0;

    if (!passkey_body_bytes(body, "client_data_json", exchange->arena, &client_data, &client_data_size)) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (!passkey_body_bytes(body, "attestation_object", exchange->arena, &attestation, &attestation_size)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountPasskeyRegistration request = {
        .rp_id                   = _STATE.config.passkey_rp_id,
        .origin                  = _STATE.config.passkey_origin,
        .client_data_json        = client_data,
        .client_data_json_size   = client_data_size,
        .attestation_object      = attestation,
        .attestation_object_size = attestation_size,
        .name                    = passkey_body_string(body, "name"),
    };

    NYA_Error stored = nya_account_passkey_register_finish(exchange->arena, user_id, &request, nullptr);
    if (!stored.ok) {
        // A registration is the account's own, so it may know how its response was wrong; a mismatched challenge or origin stays opaque, since that's the anti-phishing check.
        switch (stored.kind) {
            case NYA_ERROR_ALREADY_EXISTS:    return NYA_HTTP_STATUS_CONFLICT;
            case NYA_ERROR_NOT_SUPPORTED:     return NYA_HTTP_STATUS_UNPROCESSABLE;
            case NYA_ERROR_PARSE:
            case NYA_ERROR_INVALID_ARGUMENT:  return NYA_HTTP_STATUS_BAD_REQUEST;
            case NYA_ERROR_PERMISSION_DENIED: return NYA_HTTP_STATUS_UNAUTHORIZED;
            default:                          return NYA_HTTP_STATUS_INTERNAL_ERROR;
        }
    }

    if (via_cookie) {
        (void)nya_http_response_cookie_clear(exchange->response, PASSKEY_REGISTER_COOKIE, "/", true);
        return issue_session(exchange, user_id);
    }

    return NYA_HTTP_STATUS_NO_CONTENT;
}

/**
 * Begins a passwordless login for a username, and answers an assertion challenge and the account's
 * credentials.
 *
 * The username names the account so the challenge can be bound to it and stored single-use; an unknown or
 * disabled one is not told apart in the answer — it gets a decoy challenge that was never stored and no
 * credentials, the same shape a real account gets, and its finish refuses uniformly. The challenge and the
 * account it is for are sealed into a `__Host-passkey-login` cookie the finish reads, so the assertion is
 * only ever checked against the account the challenge was minted for.
 * */
NYA_INTERNAL NYA_HttpStatus handle_passkey_login_begin(NYA_HttpExchange* exchange) {
    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_ConstCString username = passkey_body_string(body, "username");
    if (username == nullptr || username[0] == '\0') return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user  = { 0 };
    b8              known = nya_account_find(exchange->arena, username, &user).ok && !user.disabled;

    if (!known) {
        // A decoy: a random challenge never stored and a cookie naming user zero — same shape, same cost, failing just as a wrong assertion for a real account would, so no enumeration.
        u8 bytes[NYA_ACCOUNTS_PASSKEY_CHALLENGE_BYTES] = { 0 };
        if (!nya_os_random_bytes(bytes, sizeof(bytes))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

        char challenge[NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT] = { 0 };
        u64  length                                         = 0;
        if (!nya_crypto_base64url_encode(bytes, sizeof(bytes), challenge, sizeof(challenge), &length)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

        if (!set_sealed_user_cookie(exchange, PASSKEY_LOGIN_COOKIE, PASSKEY_LOGIN_SEAL_LABEL, 0, (u64)PASSKEY_PENDING_TTL_S)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

        return passkey_request_options(exchange, 0, challenge);
    }

    NYA_AccountPasskeyChallenge challenge = { 0 };
    if (!nya_account_passkey_assert_begin(exchange->arena, user.id, &challenge).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    if (!set_sealed_user_cookie(exchange, PASSKEY_LOGIN_COOKIE, PASSKEY_LOGIN_SEAL_LABEL, user.id, (u64)PASSKEY_PENDING_TTL_S)) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return passkey_request_options(exchange, user.id, challenge.challenge);
}

/**
 * Finishes a passwordless login: verifies the `navigator.credentials.get` response and, on success, sets
 * the `__Host-session` cookie — no password anywhere in it.
 *
 * The account is the one the login cookie names; the cookie is cleared whatever the outcome, so a captured
 * one cannot be paired with a replayed assertion on a later request — and the challenge behind it is
 * single-use in the store anyway. The one refusal, 401, covers every way an assertion is not valid, the
 * decoy cookie for an unknown username (user zero) included, so the answer says nothing a guesser can use.
 * The session cookie set here is the very one the password login sets, with the same flags.
 * */
NYA_INTERNAL NYA_HttpStatus handle_passkey_login_finish(NYA_HttpExchange* exchange) {
    u64 user_id     = 0;
    b8  have_cookie = read_sealed_user_cookie(exchange, PASSKEY_LOGIN_COOKIE, PASSKEY_LOGIN_SEAL_LABEL, &user_id);

    // Spend the pending-login cookie now, before anything can go wrong, so it is one request's use only.
    (void)nya_http_response_cookie_clear(exchange->response, PASSKEY_LOGIN_COOKIE, "/", true);

    if (!have_cookie) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* body = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &body).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_ConstCString credential_id = passkey_body_string(body, "credential_id");
    if (credential_id == nullptr || credential_id[0] == '\0') return NYA_HTTP_STATUS_BAD_REQUEST;

    const u8* client_data             = nullptr;
    u64       client_data_size        = 0;
    const u8* authenticator_data      = nullptr;
    u64       authenticator_data_size = 0;
    const u8* signature               = nullptr;
    u64       signature_size          = 0;

    if (!passkey_body_bytes(body, "client_data_json", exchange->arena, &client_data, &client_data_size)) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (!passkey_body_bytes(body, "authenticator_data", exchange->arena, &authenticator_data, &authenticator_data_size)) return NYA_HTTP_STATUS_BAD_REQUEST;
    if (!passkey_body_bytes(body, "signature", exchange->arena, &signature, &signature_size)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountPasskeyAssertion request = {
        .rp_id                   = _STATE.config.passkey_rp_id,
        .origin                  = _STATE.config.passkey_origin,
        .credential_id           = credential_id,
        .client_data_json        = client_data,
        .client_data_json_size   = client_data_size,
        .authenticator_data      = authenticator_data,
        .authenticator_data_size = authenticator_data_size,
        .signature               = signature,
        .signature_size          = signature_size,
    };

    // accounts_passkey answers one refusal for every invalid case (unknown credential, spent/forged challenge, bad origin or RP id hash, clear user-present, bad signature, non-climbing counter, decoy user zero); this route doesn't soften it.
    if (!nya_account_passkey_assert_finish(exchange->arena, user_id, &request, nullptr).ok) return NYA_HTTP_STATUS_UNAUTHORIZED;

    return issue_session(exchange, user_id);
}

// ROUTES

// Every handler touches the one database on the ticking thread, so every route is MAIN: no route runs on a worker, and two requests never race the same rows.
NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_ACCOUNTS_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_REGISTER_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_register,
      .summary = "Makes an account", .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_LOGIN_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login,
      .summary = "Logs in; sets the session cookie, or asks for a second factor",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_LOGIN_TOTP_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login_totp,
      .summary = "Answers the second factor and finishes the login",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_TOO_MANY_REQUESTS, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_LOGOUT_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_logout,
      .summary = "Revokes the session and clears the cookie", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_GET, .path = NYA_HTTP_ACCOUNTS_SESSION_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_session,
      .summary = "Who the cookie says you are", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_TOTP_ENROL_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_totp_enrol,
      .summary = "Begins enrolling a second factor", .response_type = nya_reflect_of(NYA_HttpTotpEnrolmentDto),
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_CONFLICT, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_TOTP_CONFIRM_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_totp_confirm,
      .summary = "Confirms a pending second factor with one code",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_NOT_FOUND,
                    NYA_HTTP_STATUS_CONFLICT, NYA_HTTP_STATUS_TOO_MANY_REQUESTS, NYA_HTTP_STATUS_FORBIDDEN } },

    // The passkey routes are the last PASSKEY_ROUTE_COUNT entries, so the password-only router names the array short of them and the passwordless one names all of it (see nya_http_accounts_open); keep them at the end or the counts stop lining up.
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_PASSKEY_REGISTER_BEGIN_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_passkey_register_begin,
      .summary = "Begins enrolling a passkey; opens a passwordless account when signed out",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_PASSKEY_REGISTER_FINISH_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_passkey_register_finish,
      .summary = "Stores the passkey; logs a new passwordless account in",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_CONFLICT,
                    NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_PASSKEY_LOGIN_BEGIN_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_passkey_login_begin,
      .summary = "An assertion challenge and the account's credentials, for a username",
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = NYA_HTTP_ACCOUNTS_PASSKEY_LOGIN_FINISH_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_passkey_login_finish,
      .summary = "Verifies the assertion and sets the session cookie; no password",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN } },
};

/** The passkey routes at the tail of the array, the count the two routers differ by. */
#define PASSKEY_ROUTE_COUNT 4

// Two views on the one array: the password-only router stops before the passkey routes, the passwordless one names the whole array; nya_http_accounts_open returns whichever the config asked for, so a mount with no relying party answers no passkey path and OpenAPI shows only what mounts.
NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_ACCOUNTS_ROUTER = {
    .name        = "accounts",
    .routes      = _NYA_HTTP_ACCOUNTS_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_ACCOUNTS_ROUTES) - PASSKEY_ROUTE_COUNT,
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_ACCOUNTS_ROUTER_PASSKEY = {
    .name        = "accounts",
    .routes      = _NYA_HTTP_ACCOUNTS_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_ACCOUNTS_ROUTES),
};

// PUBLIC API IMPLEMENTATION

b8 nya_http_accounts_caller(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) {
    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    if (!_STATE.open) return false;

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) return false;

    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    if (cookie.size >= sizeof(token)) return false;

    nya_memcpy(token, cookie.text, cookie.size);

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_validate(exchange->arena, token, &session).ok) return false;

    return nya_account_find_by_id(exchange->arena, session.user_id, out_user).ok;
}

const NYA_HttpRouter* nya_http_accounts_open(NYA_HttpAccountsConfig config) {
    if (_STATE.open) {
        nya_log_error("The accounts routes are already mounted; one process mounts them once.");
        return nullptr;
    }

    if (config.arena == nullptr || config.database == nullptr) {
        nya_log_error("The accounts routes need both an arena and a database.");
        return nullptr;
    }

    if (!nya_accounts_is_open()) {
        nya_log_error("nya_accounts_open must run before the accounts routes are mounted.");
        return nullptr;
    }

    if (config.totp_issuer == nullptr || config.totp_issuer[0] == '\0') {
        nya_log_error("The accounts routes need a TOTP issuer for the authenticator label.");
        return nullptr;
    }

    if (config.login_seal_secret == nullptr || config.login_seal_secret_size < NYA_HTTP_SEAL_MIN_SECRET_BYTES) {
        nya_log_error("The accounts routes need a login seal secret of at least %d bytes.", NYA_HTTP_SEAL_MIN_SECRET_BYTES);
        return nullptr;
    }

    if (config.session_same_site == NYA_HTTP_SAME_SITE_NONE) {
        nya_log_error("A session cookie may not be SameSite=None; it is what lets a cross-site request carry it.");
        return nullptr;
    }

    // The relying party is optional but paired — both or neither; half of it is a check that couldn't run, so it's refused rather than mounted broken. With both, the passkey routes mount alongside the rest.
    b8 has_rp_id  = config.passkey_rp_id != nullptr && config.passkey_rp_id[0] != '\0';
    b8 has_origin = config.passkey_origin != nullptr && config.passkey_origin[0] != '\0';

    if (has_rp_id != has_origin) {
        nya_log_error("The passkey routes need both a relying party id and an origin, or neither.");
        return nullptr;
    }

    b8 passkeys = has_rp_id && has_origin;

    NYA_OrmTable* totp = nullptr;
    if (!nya_orm_open(config.arena, config.database, &_NYA_HTTP_ACCOUNTS_TOTP_MODEL, TOTP_TABLE, &totp).ok) {
        nya_log_error("Could not open the accounts second-factor table.");
        return nullptr;
    }

    if (!nya_orm_schema_migrate(totp).ok) {
        nya_orm_close(totp);
        nya_log_error("Could not migrate the accounts second-factor table.");
        return nullptr;
    }

    _STATE.config = config;
    _STATE.totp   = totp;
    _STATE.open   = true;

    return passkeys ? &_NYA_HTTP_ACCOUNTS_ROUTER_PASSKEY : &_NYA_HTTP_ACCOUNTS_ROUTER;
}

void nya_http_accounts_close(void) {
    if (!_STATE.open) return;

    nya_orm_close(_STATE.totp);
    nya_memset(&_STATE, 0, sizeof(_STATE));
}
