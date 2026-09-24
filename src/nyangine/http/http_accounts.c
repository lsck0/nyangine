#include <stdio.h>
#include <string.h>

#include "nyangine/accounts/accounts.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_totp.h"
#include "nyangine/db/db_orm.h"
#include "nyangine/http/http_accounts.h"
#include "nyangine/http/http_auth.h"
#include "nyangine/http/http_cookie.h"
#include "nyangine/http/http_message.h"
#include "nyangine/http/http_seal.h"
#include "nyangine/http/http_totp.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The second-factor table, one row per account, opened on the config's database beside the accounts tables. */
#define TOTP_TABLE "account_totp"

/*
 * The cookie that carries "this password was right, the code is still owed", and how it is sealed.
 *
 * A `__Host-` cookie like the session, so a browser pins it to this exact host, and sealed with the
 * config's key so its only content — which account is half way in — cannot be read or forged by whoever
 * holds it. Good for five minutes: long enough to read a code off a phone, short enough that a captured
 * one is worthless by the time it is replayed.
 */
#define LOGIN_COOKIE        "__Host-login"
#define LOGIN_SEAL_LABEL    "accounts-login-totp"
#define LOGIN_PENDING_TTL_S 300

/*
 * The secret and the recovery hashes are bytes, and the ORM keeps INTEGER, REAL or TEXT and nothing else
 * (see db_orm.h), so they are held as text a column can carry: the secret as the same base32 an
 * authenticator reads, the ten recovery hashes as base64url joined by commas. Both are decoded back to
 * bytes the moment they are used and never logged.
 */
#define TOTP_SECRET_TEXT   NYA_HTTP_TOTP_SECRET_TEXT_BYTES // 32 base32 characters and a terminator.
#define TOTP_RECOVERY_TEXT 512                             // Ten base64url hashes with commas fit inside this.

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The second factor as a row: the secret and the recovery codes as text, the guard as three integers,
 * and whether the enrolment was ever confirmed. Stored per account, keyed by the account id.
 */
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The module's one state: the config a mount was opened with and the second-factor table it opened. A
 * route's handler is a plain function pointer with no user data, so what it reads is here rather than
 * threaded through the exchange; one process mounts once, exactly as `accounts` opens once.
 */
NYA_INTERNAL struct {
    b8                    open;
    NYA_HttpAccountsConfig config;
    NYA_OrmTable*         totp;
} _STATE = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * REQUEST HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SESSION HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SECOND FACTOR HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: ACCOUNTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registration, under the config's policy. A taken name, a short password or a bad invite says which. */
NYA_INTERNAL NYA_HttpStatus handle_register(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;
    NYA_ConstCString invite   = nullptr;

    if (!request_registration(exchange, &username, &password, &invite)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user = { 0 };
    NYA_Error       made = nya_account_register(exchange->arena, _STATE.config.registration, username, password, invite, &user);

    if (!made.ok) {
        // A taken name, a bad password and a refused invite are the caller's to fix, so they are told;
        // anything else is a server fault the caller can do nothing about. Registration is not a login:
        // the person is not yet an attacker to hide the reason from. See accounts_invite.h.
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

    // One answer for every way it fails — wrong password, no such user, disabled, throttled — so the
    // response says nothing a guesser can use. 401, since it is the credentials that were refused.
    if (!allowed.ok) return NYA_HTTP_STATUS_UNAUTHORIZED;

    // A confirmed second factor makes this only the first of two steps: hold the account in a sealed
    // cookie and issue nothing a request can act with until the second step answers with a code.
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

    // A six-digit code is read as an authenticator code, anything else as a recovery code off paper: the
    // two forms never overlap, so routing on the shape spends one attempt rather than two and never tells
    // the two apart in the answer. Redeeming a recovery code zeroes the matched hash, so on success the
    // row's recovery column is rewritten from the hashes — and if that re-encode ever failed the code
    // would be spent in memory but live on disk, so a failure there refuses rather than issuing a session
    // on an unsaved spend.
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

    // The guard moved whatever the verdict — an attempt was spent, or a counter advanced — so it is
    // written back before anything else, or the replay guard forgets and the rate limit resets.
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

            // Validate to find the row, then revoke it. A cookie that is already invalid is nothing to
            // revoke, and clearing it is still the right thing to do.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: THE SECOND FACTOR
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    // The one answer that carries the secret. The DTO's fields are all `@redact`, so the request log that
    // sees this response keeps none of it; see http_totp.h and http_log.h.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ROUTES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every handler touches the one database on the ticking thread, so every route is MAIN: no route here
 * runs on a worker, and two requests never race the same rows.
 */
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
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_ACCOUNTS_ROUTER = {
    .name        = "accounts",
    .routes      = _NYA_HTTP_ACCOUNTS_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_ACCOUNTS_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    return &_NYA_HTTP_ACCOUNTS_ROUTER;
}

void nya_http_accounts_close(void) {
    if (!_STATE.open) return;

    nya_orm_close(_STATE.totp);
    nya_memset(&_STATE, 0, sizeof(_STATE));
}
