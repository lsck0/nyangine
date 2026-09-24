/**
 * @file examples/accounts_api/main.c
 *
 * The accounts module over HTTP: register, log in, a session in a cookie, and notes that belong to the
 * person who wrote them.
 *
 * ```
 * ./build run example accounts_api                 # serves on 127.0.0.1:47810 until interrupted
 * ./accounts_api.example --port 8080
 * ./accounts_api.example --certificate cert.pem --key key.pem   # over https
 * ```
 *
 * From another terminal (a cookie jar is how a browser keeps the session):
 *
 * ```
 * curl -c jar -X POST localhost:47810/api/register -d '{"username":"ada","password":"a long passphrase"}'
 * curl -c jar -b jar -X POST localhost:47810/api/login -d '{"username":"ada","password":"a long passphrase"}'
 * curl -b jar localhost:47810/api/me
 * curl -b jar -X POST  localhost:47810/api/notes -d '{"text":"my first note"}'
 * curl -b jar -X QUERY localhost:47810/api/notes -d '{}'          # only ada's notes
 * curl -b jar -X DELETE localhost:47810/api/notes -d '{"id":1}'
 * curl -b jar localhost:47810/api/sessions                        # signed-in devices
 * curl -b jar -X POST localhost:47810/api/logout
 * ```
 *
 * ## The second factor: a login in two steps
 *
 * An account may enrol a TOTP authenticator, after which its login is two requests. The first checks the
 * password and, finding a second factor, answers `{"second_factor_required": true}` and a sealed
 * `__Host-login` cookie instead of a session; the second answers a code and gets the session.
 *
 * ```
 * curl -b jar -c jar -X POST localhost:47810/api/totp/enrol     # returns the otpauth URI, the secret, recovery codes
 * curl -b jar -c jar -X POST localhost:47810/api/totp/confirm -d '{"code":"123456"}'   # one code turns it on
 *
 * curl -c jar -X POST localhost:47810/api/login -d '{"username":"ada","password":"a long passphrase"}'   # -> second_factor_required
 * curl -b jar -c jar -X POST localhost:47810/api/login/totp -d '{"code":"123456"}'     # -> the session cookie
 * ```
 *
 * A recovery code off paper is accepted at `/api/login/totp` in place of a code, and spent when it is.
 *
 * ## What this example is for
 *
 * `web_server` shows the router, the DTOs and the OpenAPI document with no user store — its second
 * factor "stands for both", as its own comment says. This one is the other half: a real `accounts`
 * database, a real login, a session that is a row rather than a signed claim, and authorization that
 * knows who owns what.
 *
 * ## The thing worth reading for: notes belong to people
 *
 * Every note has an `owner`. A note is only ever shown to, or deleted by, the account that wrote it —
 * `notes_query` filters on the owner and `notes_delete` refuses an id that is not the caller's. That is
 * the check a scanner probes for as IDOR: log in as one user, ask for another user's note by id, and a
 * server that answers it has handed one person another's data. Here the answer is 404, the same as for
 * an id that does not exist, because whether somebody else's note exists is not the caller's business
 * either.
 *
 * ## The session is a row, not a claim
 *
 * Logging in issues an `accounts` session — an opaque token, hashed in the database — and puts it in a
 * `__Host-session` cookie: `HttpOnly`, `Secure`, `SameSite=Strict`, `Path=/`. Every request validates
 * it against the row, so revoking a session (logging out, or from the devices list) ends it at once,
 * which a signed token living minutes cannot do. The throttle in `accounts` slows a password-guessing
 * login the same way whoever is guessing gets slowed.
 *
 * ## Not encrypted on disk
 *
 * The database is under the save root and is not encrypted (see db.h): the password hashes in it are
 * Argon2id and safe to leak, but do not put anything else here that would matter if somebody read the
 * file until SQLCipher is vendored.
 * */

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#include "SDL3/SDL_init.h"

// The notes resource, split into its three shapes and their conversions — the worked example of
// "Model, SO, DTO". note_so.h pulls note_model.h (the row) and note_dto.h (the wire), and holds the
// four conversions between them. Only note_dto.h would compile into the web profile; the other two
// carry the guard that refuses to.
#include "notes/note_so.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS AND STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define DEFAULT_PORT 47810

/** Notes one account may hold, so the file cannot grow without bound. */
#define NOTES_PER_USER 1000

NYA_INTERNAL volatile sig_atomic_t RUNNING = 1;

NYA_INTERNAL NYA_Arena*    DB_ARENA = nullptr;
NYA_INTERNAL NYA_Database* DB       = nullptr;
NYA_INTERNAL NYA_OrmTable* NOTES    = nullptr;

/** The second factor a person enrolled, keyed by account: the secret, the replay guard, the recovery codes. */
NYA_INTERNAL NYA_OrmTable* TOTP = nullptr;

/**
 * The key the "password accepted, second factor still owed" cookie is sealed under.
 *
 * A fresh random key made at startup, not the token signing secret and not from the environment: the
 * pending-login cookie lives five minutes and never has to survive a restart, so a per-process key is
 * exactly enough and keeps the example runnable with nothing to configure. See http_seal.h.
 * */
NYA_INTERNAL u8 LOGIN_SEAL_SECRET[32] = { 0 };

// The note's three shapes — the AccountNote row (Model), the Note the program works with (SO) and the
// NoteDtoV1 that crosses the wire (DTO) — and the conversions between them now live in notes/, one
// directory for the one resource. See notes/note_model.h, notes/note_so.h, notes/note_dto.h.

/*
 * The second factor as a row: the secret and the recovery codes as text, the guard as three integers,
 * and whether the enrolment was ever confirmed. Stored per account, keyed by the account id.
 *
 * The secret and the recovery hashes are bytes, and the ORM keeps INTEGER, REAL or TEXT and nothing
 * else (see db_orm.h), so they are held as text a column can carry: the secret as the same base32 an
 * authenticator reads, the ten recovery hashes as base64url joined by commas. Both are decoded back to
 * bytes the moment they are used and never logged. A real engine would grow a column type for this; an
 * example works within what the ORM has today.
 */
#define TOTP_SECRET_TEXT   NYA_HTTP_TOTP_SECRET_TEXT_BYTES // 32 base32 characters and a terminator.
#define TOTP_RECOVERY_TEXT 512                             // Ten base64url hashes with commas fit inside this.

typedef struct {
    s64  owner; // The account id, and the key: one second factor per account.
    char secret[TOTP_SECRET_TEXT];
    s64  last_counter;     // NYA_HttpTotpGuard.last_counter, the replay guard.
    s64  attempts;         // NYA_HttpTotpGuard.attempts in the current window.
    s64  window_started_s; // NYA_HttpTotpGuard.window_started_s.
    s64  confirmed;        // Zero until one code has verified against the secret; see the enrol/confirm ceremony.
    char recovery[TOTP_RECOVERY_TEXT];
} TotpRow;

NYA_INTERNAL const NYA_TypeReflection TOTP_SECRET_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = TOTP_SECRET_TEXT,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = TOTP_SECRET_TEXT,
};

NYA_INTERNAL const NYA_TypeReflection TOTP_RECOVERY_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = TOTP_RECOVERY_TEXT,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = TOTP_RECOVERY_TEXT,
};

NYA_INTERNAL const NYA_ReflectField TOTP_FIELDS[] = {
    { .name = "owner", .type = nya_reflect_of(s64), .offset = nya_offsetof(TotpRow, owner), .is_key = true },
    { .name = "secret", .type = &TOTP_SECRET_ARRAY, .offset = nya_offsetof(TotpRow, secret) },
    { .name = "last_counter", .type = nya_reflect_of(s64), .offset = nya_offsetof(TotpRow, last_counter) },
    { .name = "attempts", .type = nya_reflect_of(s64), .offset = nya_offsetof(TotpRow, attempts) },
    { .name = "window_started_s", .type = nya_reflect_of(s64), .offset = nya_offsetof(TotpRow, window_started_s) },
    { .name = "confirmed", .type = nya_reflect_of(s64), .offset = nya_offsetof(TotpRow, confirmed) },
    { .name = "recovery", .type = &TOTP_RECOVERY_ARRAY, .offset = nya_offsetof(TotpRow, recovery) },
};

NYA_INTERNAL const NYA_TypeReflection TOTP_MODEL = {
    .name        = "TotpRow",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(TotpRow),
    .alignment   = alignof(TotpRow),
    .fields      = TOTP_FIELDS,
    .field_count = nya_carray_length(TOTP_FIELDS),
};

#define REGISTER_PATH    "/api/register"
#define LOGIN_PATH       "/api/login"
#define LOGIN_TOTP_PATH  "/api/login/totp"
#define LOGOUT_PATH      "/api/logout"
#define ME_PATH          "/api/me"
#define NOTES_PATH       "/api/notes"
#define SESSIONS_PATH    "/api/sessions"
#define TOTP_ENROL_PATH  "/api/totp/enrol"
#define TOTP_CONFIRM_PATH "/api/totp/confirm"

/** The program's name, as it appears in the authenticator's list beside the account. */
#define TOTP_ISSUER "accounts_api"

/**
 * The cookie that carries "this password was right, the code is still owed", and how it is sealed.
 *
 * It is a `__Host-` cookie like the session, so a browser pins it to this exact host, and it is sealed
 * with LOGIN_SEAL_SECRET, so its only content — which account is half way in — cannot be read or forged
 * by whoever holds it. It is good for five minutes: long enough to read a code off a phone, short enough
 * that a captured one is worthless by the time it is replayed.
 * */
#define LOGIN_COOKIE        "__Host-login"
#define LOGIN_SEAL_LABEL    "login-totp"
#define LOGIN_PENDING_TTL_S 300

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SESSION HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The account this request's cookie names, or false with the 401 already the caller's to return.
 *
 * Reads the session token out of the `__Host-session` cookie and validates it against the row, so a
 * revoked, expired or forged cookie is nobody. The cookie value is copied into a NUL-terminated buffer
 * because the parser hands back a view that is not terminated.
 * */
NYA_INTERNAL b8 request_account(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) {
    nya_memset(out_user, 0, sizeof(NYA_AccountUser));

    NYA_HttpCookieValue cookie = { 0 };
    if (!nya_http_cookie_read(exchange->request, NYA_HTTP_SESSION_COOKIE, &cookie)) return false;

    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES] = { 0 };
    if (cookie.size >= sizeof(token)) return false;

    nya_memcpy(token, cookie.text, cookie.size);

    NYA_AccountSession session = { 0 };
    if (!nya_account_session_validate(exchange->arena, token, &session).ok) return false;

    return nya_account_find_by_id(exchange->arena, session.user_id, out_user).ok;
}

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
NYA_INTERNAL NYA_HttpTotpGuard totp_guard_of(const TotpRow* row) {
    return (NYA_HttpTotpGuard){
        .last_counter     = (u64)row->last_counter,
        .attempts         = (u32)row->attempts,
        .window_started_s = (u64)row->window_started_s,
    };
}

/** Writes an advanced guard back into a row, so the replay guard and the rate limit survive the request. */
NYA_INTERNAL void totp_guard_store(TotpRow* row, const NYA_HttpTotpGuard* guard) {
    row->last_counter     = (s64)guard->last_counter;
    row->attempts         = (s64)guard->attempts;
    row->window_started_s = (s64)guard->window_started_s;
}

/** The stored base32 secret back to its twenty bytes. False on a row this program did not write. */
NYA_INTERNAL b8 totp_secret_decode(const TotpRow* row, OUT NYA_CryptoTotpSecret* out_secret) {
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

/** The account's second factor row, and whether it is a confirmed one. */
NYA_INTERNAL b8 totp_find(NYA_HttpExchange* exchange, u64 owner, OUT TotpRow* out_row) {
    nya_memset(out_row, 0, sizeof(*out_row));
    return nya_orm_find(TOTP, exchange->arena, nya_sql_s64((s64)owner), out_row).ok;
}

/** Whether the account has a confirmed second factor, which is what makes a login two steps. */
NYA_INTERNAL b8 totp_is_enrolled(NYA_HttpExchange* exchange, u64 owner) {
    TotpRow row = { 0 };
    return totp_find(exchange, owner, &row) && row.confirmed != 0;
}

/** Seals "this account passed its password" into the pending-login cookie. */
NYA_INTERNAL NYA_HttpStatus set_pending_cookie(NYA_HttpExchange* exchange, u64 user_id) {
    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };

    if (!nya_http_seal(LOGIN_SEAL_SECRET, sizeof(LOGIN_SEAL_SECRET), LOGIN_SEAL_LABEL, (const u8*)&user_id, sizeof(user_id), LOGIN_PENDING_TTL_S,
                       token, sizeof(token))
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

    if (!nya_http_unseal(LOGIN_SEAL_SECRET, sizeof(LOGIN_SEAL_SECRET), LOGIN_SEAL_LABEL, cookie.text, cookie.size, (u8*)&user_id, sizeof(user_id),
                         &size)) {
        return false;
    }

    if (size != sizeof(user_id)) return false;

    *out_user_id = user_id;
    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: ACCOUNTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registration, open to anybody in this example. A taken name or a short password says which. */
NYA_INTERNAL NYA_HttpStatus handle_register(NYA_HttpExchange* exchange) {
    NYA_ConstCString username = nullptr;
    NYA_ConstCString password = nullptr;

    if (!request_credentials(exchange, &username, &password)) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_AccountUser user = { 0 };
    NYA_Error       made = nya_account_register(exchange->arena, NYA_ACCOUNT_REGISTRATION_OPEN, username, password, nullptr, &user);

    if (!made.ok) {
        // A taken name and a bad password are the caller's to fix, so they are told; anything else is a
        // server fault the caller can do nothing about.
        if (made.kind == NYA_ERROR_ALREADY_EXISTS || made.kind == NYA_ERROR_INVALID_ARGUMENT) return NYA_HTTP_STATUS_UNPROCESSABLE;

        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

/** Sets the session cookie from a token, with the attributes a session cookie must have. */
NYA_INTERNAL NYA_HttpStatus set_session_cookie(NYA_HttpExchange* exchange, NYA_ConstCString token) {
    NYA_Error set = nya_http_response_cookie(exchange->response,
                                             &(NYA_HttpCookie){
                                                 .name      = NYA_HTTP_SESSION_COOKIE,
                                                 .value     = token,
                                                 .max_age_s = NYA_ACCOUNTS_SESSION_IDLE_S,
                                                 .http_only = true,
                                                 .secure    = true,
                                                 .same_site = NYA_HTTP_SAME_SITE_STRICT,
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
    // cookie and issue nothing a request can act with until POST /api/login/totp answers with a code.
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

    TotpRow row = { 0 };
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
    if (!nya_orm_update(TOTP, &row).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

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
NYA_INTERNAL NYA_HttpStatus handle_me(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

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
 * unconfirmed, and RFC 6238 enrolment is not finished until POST /api/totp/confirm proves the phone and
 * this server compute the same code. 409 when a confirmed factor is already there, so one cannot be
 * silently replaced.
 * */
NYA_INTERNAL NYA_HttpStatus handle_totp_enrol(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    TotpRow existing = { 0 };
    b8      present  = totp_find(exchange, user.id, &existing);
    if (present && existing.confirmed != 0) return NYA_HTTP_STATUS_CONFLICT;

    NYA_HttpTotpEnrolment enrolment = { 0 };
    if (!nya_http_totp_enrol_create(TOTP_ISSUER, user.username, &enrolment).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    defer nya_http_totp_enrol_destroy(&enrolment);

    // A fresh, unconfirmed row: the secret as base32, a guard that has accepted nothing, no confirmation.
    TotpRow row = { .owner = (s64)user.id, .confirmed = 0 };
    (void)snprintf(row.secret, sizeof(row.secret), "%s", enrolment.secret_base32);

    if (!totp_recovery_encode(enrolment.recovery_hash, row.recovery, sizeof(row.recovery))) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Error stored = present ? nya_orm_update(TOTP, &row) : nya_orm_insert(TOTP, &row);
    if (!stored.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // The one answer that carries the secret. The DTO's fields are all `@redact`, so the request log
    // that sees this response keeps none of it; see http_totp.h and http_log.h.
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
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_ConstCString code = nullptr;
    if (!request_code(exchange, &code)) return NYA_HTTP_STATUS_BAD_REQUEST;

    TotpRow row = { 0 };
    if (!totp_find(exchange, user.id, &row)) return NYA_HTTP_STATUS_NOT_FOUND;
    if (row.confirmed != 0) return NYA_HTTP_STATUS_CONFLICT;

    NYA_CryptoTotpSecret secret = { 0 };
    if (!totp_secret_decode(&row, &secret)) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    defer nya_crypto_totp_secret_destroy(&secret);

    NYA_HttpTotpGuard   guard   = totp_guard_of(&row);
    NYA_HttpTotpVerdict verdict = nya_http_totp_verify(&guard, &secret, code, exchange->now_s);

    totp_guard_store(&row, &guard);
    if (verdict == NYA_HTTP_TOTP_ACCEPTED) row.confirmed = 1;

    if (!nya_orm_update(TOTP, &row).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    if (verdict == NYA_HTTP_TOTP_RATE_LIMITED) return NYA_HTTP_STATUS_TOO_MANY_REQUESTS;
    if (verdict != NYA_HTTP_TOTP_ACCEPTED) return NYA_HTTP_STATUS_UNAUTHORIZED;

    return NYA_HTTP_STATUS_NO_CONTENT;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: NOTES, WHICH BELONG TO PEOPLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One stored row as the JSON the client sees — the DTO, rendered through the DTO's own reflection.
 *
 * The row goes Model → SO → DTO before it is written, so `owner` falls away where the DTO has no field
 * for it: the owner is not the reader's to see. The wire shape is decided by NOTE_DTO_V1_REFLECT and
 * nothing else, exactly as the ORM's shape is decided by NOTE_MODEL.
 * */
NYA_INTERNAL NYA_Value note_dto_value(NYA_Arena* arena, const AccountNote* row) {
    Note      so  = note_so_from_model(row);
    NoteDtoV1 dto = note_dto_from_so(&so);

    NYA_Object* object = nya_reflect_to_object(arena, &NOTE_DTO_V1_REFLECT, &dto);
    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *object };
}

/** The caller's own notes, and nobody else's. The filter is on the owner, in the query. */
NYA_INTERNAL NYA_HttpStatus handle_notes_query(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    void* rows  = nullptr;
    u32   count = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ? ORDER BY id", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &rows, &count).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_Object*          body  = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* notes = nya_array_create(exchange->arena, NYA_Value);

    for (u32 index = 0; index < count; index++) {
        const AccountNote* note = nya_orm_at(NOTES, rows, index);

        NYA_Value value = note_dto_value(exchange->arena, note);
        nya_array_add(notes, value);
    }

    nya_object_add(body, "count", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = notes->length });
    nya_object_add(body, "notes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *notes });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Adds a note owned by the caller. */
NYA_INTERNAL NYA_HttpStatus handle_notes_post(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    // The request body is read as the DTO through its reflection, then parsed into an SO — which is
    // where the untrusted text is checked and where the *server*, not the client, fills in the owner
    // and the timestamp. A client cannot claim a note it did not write: note_so_from_dto ignores any
    // owner a DTO might carry, because the DTO has no such field to carry.
    NoteDtoV1 dto = { 0 };
    if (!nya_http_request_reflect(exchange->request, exchange->arena, &NOTE_DTO_V1_REFLECT, &dto).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    Note so = { 0 };
    if (!note_so_from_dto(&dto, (s64)user.id, exchange->now_s, &so).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    void* existing = nullptr;
    u32   held     = 0;

    if (!nya_orm_select(NOTES, exchange->arena, "WHERE owner = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)user.id) }, 1, &existing, &held).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    if (held >= NOTES_PER_USER) return NYA_HTTP_STATUS_UNPROCESSABLE;

    AccountNote note = note_model_from_so(&so);
    if (!nya_orm_insert(NOTES, &note).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR; // writes the assigned id back into note.

    // The stored row back out as the DTO, so the client reads exactly what a later query would return.
    Note      stored_so  = note_so_from_model(&note);
    NoteDtoV1 stored_dto = note_dto_from_so(&stored_so);

    return nya_http_response_reflect(exchange->response, exchange->arena, &NOTE_DTO_V1_REFLECT, &stored_dto).ok ? NYA_HTTP_STATUS_CREATED
                                                                                                                : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/**
 * Deletes one of the caller's notes by id — and only one of the caller's.
 *
 * This is the IDOR check. A note that exists but belongs to somebody else answers 404, the same as a
 * note that does not exist, because "that is not yours" and "there is no such note" are the same
 * sentence to somebody who has no business knowing either way.
 * */
NYA_INTERNAL NYA_HttpStatus handle_notes_delete(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id = nya_object_get(incoming, "id");
    if (id == nullptr || (id->type != NYA_TYPE_U64 && id->type != NYA_TYPE_S64)) return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 note_id = id->type == NYA_TYPE_U64 ? id->as_u64 : (u64)id->as_s64;

    AccountNote note = { 0 };
    if (!nya_orm_find(NOTES, exchange->arena, nya_sql_s64((s64)note_id), &note).ok) return NYA_HTTP_STATUS_NOT_FOUND;

    // The owner check, and the whole point of the example: not yours is the same answer as not there.
    if (note.owner != (s64)user.id) return NYA_HTTP_STATUS_NOT_FOUND;

    if (!nya_orm_delete(NOTES, nya_sql_s64((s64)note_id)).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_NO_CONTENT;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLERS: SIGNED-IN DEVICES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The caller's own sessions, the "signed-in devices" list. Never a token, only where and when. */
NYA_INTERNAL NYA_HttpStatus handle_sessions_list(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_AccountSession* sessions = nullptr;
    u32                 count    = 0;

    if (!nya_account_session_list(exchange->arena, user.id, &sessions, &count).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    NYA_Object*          body = nya_object_create(exchange->arena);
    NYA_ArrayᐸNYA_Valueᐳ* list = nya_array_create(exchange->arena, NYA_Value);

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountSession* session = &sessions[index];

        NYA_Object* row = nya_object_create(exchange->arena);

        nya_object_add(row, "id", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = session->id });
        nya_object_add(row, "address", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)session->address });
        nya_object_add(row, "agent", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)session->agent });
        nya_object_add(row, "created_at_s", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = session->created_at_s });
        nya_object_add(row, "used_at_s", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = session->used_at_s });
        nya_object_add(row, "revoked", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = session->revoked });

        NYA_Value row_value = { .type = NYA_TYPE_OBJECT, .as_object = *row };
        nya_array_add(list, row_value);
    }

    nya_object_add(body, "sessions", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *list });

    return nya_http_response_json(exchange->response, exchange->arena, body).ok ? NYA_HTTP_STATUS_OK : NYA_HTTP_STATUS_INTERNAL_ERROR;
}

/** Revokes one of the caller's own sessions by id. Somebody else's is not found, like a note. */
NYA_INTERNAL NYA_HttpStatus handle_sessions_delete(NYA_HttpExchange* exchange) {
    NYA_AccountUser user = { 0 };
    if (!request_account(exchange, &user)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    NYA_Object* incoming = nullptr;
    if (!nya_http_request_document(exchange->request, exchange->arena, &incoming).ok) return NYA_HTTP_STATUS_BAD_REQUEST;

    NYA_Value* id = nya_object_get(incoming, "id");
    if (id == nullptr || (id->type != NYA_TYPE_U64 && id->type != NYA_TYPE_S64)) return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 session_id = id->type == NYA_TYPE_U64 ? id->as_u64 : (u64)id->as_s64;

    // The session has to be one of the caller's own: revoking by id alone would let anybody end
    // anybody's session, which is the same IDOR the notes have.
    NYA_AccountSession* sessions = nullptr;
    u32                 count    = 0;

    if (!nya_account_session_list(exchange->arena, user.id, &sessions, &count).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    for (u32 index = 0; index < count; index++) {
        if (sessions[index].id != session_id) continue;

        return nya_account_session_revoke(exchange->arena, session_id).ok ? NYA_HTTP_STATUS_NO_CONTENT : NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_NOT_FOUND;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ROUTES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every handler touches the one database on the ticking thread, so the whole table is MAIN: no route
 * here runs on a worker, and two requests never race the same rows.
 */
NYA_INTERNAL const NYA_HttpRoute ACCOUNT_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_POST, .path = REGISTER_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_register,
      .summary = "Makes an account", .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = LOGIN_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login,
      .summary = "Logs in; sets the session cookie, or asks for a second factor",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = LOGIN_TOTP_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_login_totp,
      .summary = "Answers the second factor and finishes the login",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_TOO_MANY_REQUESTS, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = LOGOUT_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_logout,
      .summary = "Revokes the session and clears the cookie", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_GET, .path = ME_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_me,
      .summary = "Who the cookie says you are", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
};

NYA_INTERNAL const NYA_HttpRouter ACCOUNT_ROUTER = {
    .name = "accounts", .routes = ACCOUNT_ROUTES, .route_count = nya_carray_length(ACCOUNT_ROUTES),
};

NYA_INTERNAL const NYA_HttpRoute NOTE_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_QUERY, .path = NOTES_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_notes_query,
      .summary = "Your own notes", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
    { .method = NYA_HTTP_METHOD_POST, .path = NOTES_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_notes_post,
      .summary = "Adds a note you own", .statuses = { NYA_HTTP_STATUS_CREATED, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_UNPROCESSABLE, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_DELETE, .path = NOTES_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_notes_delete,
      .summary = "Deletes a note you own", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_FORBIDDEN } },
};

NYA_INTERNAL const NYA_HttpRouter NOTE_ROUTER = {
    .name = "notes", .routes = NOTE_ROUTES, .route_count = nya_carray_length(NOTE_ROUTES),
};

NYA_INTERNAL const NYA_HttpRoute SESSION_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_GET, .path = SESSIONS_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_sessions_list,
      .summary = "Your signed-in devices", .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED } },
    { .method = NYA_HTTP_METHOD_DELETE, .path = SESSIONS_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_sessions_delete,
      .summary = "Revokes one of your sessions", .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_NOT_FOUND, NYA_HTTP_STATUS_FORBIDDEN } },
};

NYA_INTERNAL const NYA_HttpRouter SESSION_ROUTER = {
    .name = "sessions", .routes = SESSION_ROUTES, .route_count = nya_carray_length(SESSION_ROUTES),
};

NYA_INTERNAL const NYA_HttpRoute TOTP_ROUTES[] = {
    { .method = NYA_HTTP_METHOD_POST, .path = TOTP_ENROL_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_totp_enrol,
      .summary = "Begins enrolling a second factor", .response_type = nya_reflect_of(NYA_HttpTotpEnrolmentDto),
      .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_CONFLICT, NYA_HTTP_STATUS_FORBIDDEN } },
    { .method = NYA_HTTP_METHOD_POST, .path = TOTP_CONFIRM_PATH, .affinity = NYA_HTTP_AFFINITY_MAIN, .handler = handle_totp_confirm,
      .summary = "Confirms a pending second factor with one code",
      .statuses = { NYA_HTTP_STATUS_NO_CONTENT, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_UNAUTHORIZED, NYA_HTTP_STATUS_NOT_FOUND,
                    NYA_HTTP_STATUS_CONFLICT, NYA_HTTP_STATUS_TOO_MANY_REQUESTS, NYA_HTTP_STATUS_FORBIDDEN } },
};

NYA_INTERNAL const NYA_HttpRouter TOTP_ROUTER = {
    .name = "totp", .routes = TOTP_ROUTES, .route_count = nya_carray_length(TOTP_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void stop(int signal_number) {
    nya_unused(signal_number);
    RUNNING = 0;
}

s32 main(s32 argc, char** argv) {
    u16 port = DEFAULT_PORT;

    NYA_ConstCString certificate_path = "";
    NYA_ConstCString key_path         = "";

    for (s32 i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--certificate") == 0) certificate_path = argv[i + 1];
        if (strcmp(argv[i], "--key") == 0) key_path = argv[i + 1];
        if (strcmp(argv[i], "--port") != 0) continue;

        if (!nya_type_parse(NYA_TYPE_U16, (const u8*)argv[i + 1], strlen(argv[i + 1]), &port)) {
            nya_log_error("--port expects a number from 0 to 65535, got '%s'.", argv[i + 1]);
            return EXIT_FAILURE;
        }
    }

    b8 secure = certificate_path[0] != '\0' || key_path[0] != '\0';

    nya_log_level_set(NYA_LOG_LEVEL_INFO);
    (void)signal(SIGINT, stop);

    if (!SDL_Init(0)) {
        nya_log_error("SDL could not start: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    defer SDL_Quit();

    // No window, no renderer, no frame loop: an app instance for the systems to hang off, the callback
    // and event registries the save and http systems hook into, and then the database. See web_server.
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_EXPECT(nya_system_events_init(), "while starting the event registry");
    defer nya_system_events_deinit();

    NYA_Error saves = nya_system_save_init();
    if (!saves.ok) {
        nya_log_error("No save root, so there is nowhere to keep the accounts: %s", (NYA_ConstCString)saves.message);
        return EXIT_FAILURE;
    }
    defer nya_system_save_deinit();

    DB_ARENA = nya_arena_create(.name = "accounts_db");
    defer    nya_arena_destroy(DB_ARENA);

    NYA_Error stored = nya_save_database_open(DB_ARENA, "accounts.db", &DB);
    if (!stored.ok) {
        nya_log_error("Could not open the accounts database: %s", (NYA_ConstCString)stored.message);
        return EXIT_FAILURE;
    }
    defer nya_sql_close(DB);

    // The accounts module and its six tables, on this database.
    NYA_EXPECT(nya_accounts_open(DB_ARENA, DB), "while opening the accounts tables");
    defer nya_accounts_close();

    // The notes table, owned per account.
    NYA_EXPECT(nya_orm_open(DB_ARENA, DB, &NOTE_MODEL, "notes", &NOTES), "while binding the note model");
    defer nya_orm_close(NOTES);
    NYA_EXPECT(nya_orm_schema_migrate(NOTES), "while bringing the notes table level with the model");

    // The second-factor table, one row per account, and the key the pending-login cookie is sealed with.
    NYA_EXPECT(nya_orm_open(DB_ARENA, DB, &TOTP_MODEL, "totp", &TOTP), "while binding the second-factor model");
    defer nya_orm_close(TOTP);
    NYA_EXPECT(nya_orm_schema_migrate(TOTP), "while bringing the second-factor table level with the model");

    if (!nya_os_random_bytes(LOGIN_SEAL_SECRET, sizeof(LOGIN_SEAL_SECRET))) {
        nya_log_error("No entropy for the pending-login key, so the second factor cannot be sealed.");
        return EXIT_FAILURE;
    }
    defer nya_memset(LOGIN_SEAL_SECRET, 0, sizeof(LOGIN_SEAL_SECRET));

    nya_http_log_config_set((NYA_HttpLogConfig){ .level = NYA_HTTP_LOG_HEADERS, .address = NYA_HTTP_LOG_ADDRESS_NETWORK });

    NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
                   .port             = port,
                   .workers          = 2,
                   .certificate_path = certificate_path,
                   .key_path         = key_path,
               }),
               "while starting the server");
    defer nya_system_http_deinit();

    NYA_EXPECT(nya_http_server_merge(&ACCOUNT_ROUTER), "while mounting the accounts routes");
    NYA_EXPECT(nya_http_server_merge(&TOTP_ROUTER), "while mounting the second-factor routes");
    NYA_EXPECT(nya_http_server_merge(&NOTE_ROUTER), "while mounting the notes routes");
    NYA_EXPECT(nya_http_server_merge(&SESSION_ROUTER), "while mounting the sessions routes");
    NYA_EXPECT(nya_http_server_merge(nya_http_openapi_router()), "while mounting the OpenAPI document");

    nya_log_info("accounts_api on %s://127.0.0.1:%u — register, log in, and keep notes that are yours. ctrl-c to stop.",
                 secure ? "https" : "http", nya_http_server_port());

    // The housekeeping a real server runs on a timer, run once at start so a long-lived database does
    // not carry dead rows forever. A production server would call these hourly.
    u32 ended = 0, removed = 0;
    (void)nya_account_session_sweep(DB_ARENA, &ended, &removed);

    while (RUNNING) {
        nya_system_http_tick();
        nya_os_time_sleep_ms(2);
    }

    nya_log_info("Stopping.");

    return EXIT_SUCCESS;
}
