/**
 * @file http_accounts.h
 *
 * The login flow as a router a server merges: register, log in, the second factor, log out, and who the
 * cookie says you are — the `accounts` primitives wired to HTTP once, so an application mounts them
 * rather than writing eight hundred lines of handler again.
 *
 * ```c
 * NYA_TRY(nya_accounts_open(arena, database));
 * defer nya_accounts_close();
 *
 * u8 seal[32] = { 0 };
 * nya_os_random_bytes(seal, sizeof(seal));
 *
 * const NYA_HttpRouter* routes = nya_http_accounts_open((NYA_HttpAccountsConfig){
 *     .arena                  = arena,
 *     .database               = database,
 *     .registration           = NYA_ACCOUNT_REGISTRATION_OPEN,
 *     .totp_issuer            = "my_app",
 *     .login_seal_secret      = seal,
 *     .login_seal_secret_size = sizeof(seal),
 * });
 * if (routes == nullptr) return EXIT_FAILURE;
 * defer nya_http_accounts_close();
 *
 * NYA_TRY(nya_http_server_merge(routes));
 * ```
 *
 * ── what it mounts ──
 *
 * ```
 * POST /api/register       makes an account (through the invite policy the config carries)
 * POST /api/login          a password: the session cookie, or a request for a second factor
 * POST /api/login/totp     the second factor, and then the session cookie
 * POST /api/logout         revokes the session row and clears the cookie
 * GET  /api/session        who the cookie says you are, or 401 when it says nobody
 * POST /api/totp/enrol     begins enrolling an authenticator; shows the secret and recovery codes once
 * POST /api/totp/confirm   one code turns the pending factor on
 * ```
 *
 * And, when the config names a WebAuthn relying party, the passwordless factor — a passkey as a *primary*
 * login, not only a second step:
 *
 * ```
 * POST /api/passkey/register/begin   the creation options and a challenge; signed in it adds a device,
 *                                    signed out it opens a passwordless account under an OPEN policy
 * POST /api/passkey/register/finish  stores the credential, and on the signed-out path logs the new account in
 * POST /api/passkey/login/begin      an assertion challenge and the account's credentials, for a username
 * POST /api/passkey/login/finish     verifies the assertion and sets the session cookie — no password
 * ```
 *
 * ── passwordless is primary, not a second factor ──
 *
 * A password and a passkey are two ways to prove the same account, and neither is required to have the
 * other: an account may be created with a passkey and never hold a password, and one that holds a password
 * may add a passkey and log in with either. The passkey login route sets the same `__Host-session` cookie
 * the password route does, with the same flags, so what a request may do afterwards does not depend on
 * which factor opened the session. The passkey routes only mount when the config carries a relying party
 * (`passkey_rp_id` and `passkey_origin`); a server that wants only passwords sets neither and gets neither.
 *
 * ── where it sits ──
 *
 * In the http module, not the accounts one, because it depends on both and http is the layer that may:
 * `accounts` is deliberately below http (a CLI or a game has users without a web server), so the glue
 * that turns those primitives into routes belongs on the http side of the line. It is the one http file
 * that reaches down into `accounts`, and it is optional — a server built without the db module simply
 * does not include it, exactly as it does not include `accounts`.
 *
 * ── the session cookie ──
 *
 * Logging in issues an `accounts` session — an opaque token, hashed in the database — and puts it in a
 * `__Host-session` cookie: `HttpOnly`, `Secure`, `Path=/`, and `SameSite=Strict` unless the config
 * widens it. Every request validates it against the row, so a logout revokes the row and the same cookie
 * is nobody after. That the cookie carries the `__Host-` prefix and the two flags is not the caller's to
 * turn off: a session cookie a script can read or a page can send cross-site is the bug this module
 * exists to not have. The one attribute a caller may set is `SameSite`, because a login that has to work
 * across a top-level navigation is a real need and Lax still sits behind the origin check dispatch makes.
 *
 * ── what the config decides, and what it does not ──
 *
 * It decides where the tables live, whether registration is open, what an authenticator's label reads,
 * and the key the half-logged-in cookie is sealed under. It does not decide how a password is stored,
 * what a login answers, or how long a session lives — those are `accounts`' and are not weakened here.
 * There is no user-enumeration difference between an unknown user and a wrong password, and every login
 * refusal is the same 401 at the same throttled cost, because the primitives underneath make it so.
 *
 * ── the second factor is stored, so this module owns a table ──
 *
 * http_totp.h stores nothing: it hands the caller a secret and a guard to keep. So this module keeps
 * them, in one row per account in a table it opens on the config's database beside the `accounts`
 * tables. nya_http_accounts_open opens it and nya_http_accounts_close drops it; both are the mirror of
 * nya_accounts_open / nya_accounts_close, and for the same reason.
 *
 * ── one process, one mount ──
 *
 * Like `accounts` itself, this is a singleton: the route table is static and the config is module state,
 * so a process mounts it once. nya_http_accounts_open refuses a second open while one is live. A route's
 * handler is a plain function pointer with no user data — http_router.h keeps it that way on purpose — so
 * the config it reads is where a program's own state already is, which is module scope.
 *
 * Thread safety: none, as with everything over `db`. Every route is NYA_HTTP_AFFINITY_MAIN, so the one
 * database is only ever touched on the ticking thread.
 * */
#pragma once

#include "nyangine-core/accounts/accounts_invite.h"
#include "nyangine-core/accounts/accounts_user.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/db/db_sql.h"
#include "nyangine-core/http/http_cookie.h"
#include "nyangine-core/http/http_router.h"

// CONSTANTS

/** The paths this module answers on. Exported so a client, a test or a reverse proxy names the same ones. */
#define NYA_HTTP_ACCOUNTS_REGISTER_PATH     "/api/register"
#define NYA_HTTP_ACCOUNTS_LOGIN_PATH        "/api/login"
#define NYA_HTTP_ACCOUNTS_LOGIN_TOTP_PATH   "/api/login/totp"
#define NYA_HTTP_ACCOUNTS_LOGOUT_PATH       "/api/logout"
#define NYA_HTTP_ACCOUNTS_SESSION_PATH      "/api/session"
#define NYA_HTTP_ACCOUNTS_TOTP_ENROL_PATH   "/api/totp/enrol"
#define NYA_HTTP_ACCOUNTS_TOTP_CONFIRM_PATH "/api/totp/confirm"

/** The passkey paths, mounted only when the config names a relying party. Exported for the same reason as the rest. */
#define NYA_HTTP_ACCOUNTS_PASSKEY_REGISTER_BEGIN_PATH  "/api/passkey/register/begin"
#define NYA_HTTP_ACCOUNTS_PASSKEY_REGISTER_FINISH_PATH "/api/passkey/register/finish"
#define NYA_HTTP_ACCOUNTS_PASSKEY_LOGIN_BEGIN_PATH     "/api/passkey/login/begin"
#define NYA_HTTP_ACCOUNTS_PASSKEY_LOGIN_FINISH_PATH    "/api/passkey/login/finish"

// TYPES

typedef struct NYA_HttpAccountsConfig NYA_HttpAccountsConfig;

/**
 * What a program hands the login routes: where their tables live, the policy they enforce, and the two
 * seams — a signing key and a cookie attribute — that are a caller's to choose.
 *
 * Everything the routes decide for themselves is deliberately not here; see the file note on what the
 * config does and does not get to say.
 * */
struct NYA_HttpAccountsConfig {
    /**
     * The arena the second-factor table's prepared statements live in, until nya_http_accounts_close.
     * Usually the same one nya_accounts_open was given. Required.
     * */
    NYA_Arena* arena;

    /**
     * The database the second-factor table is opened on. Usually the one the `accounts` tables are on, so
     * a login and its factor commit together. Required.
     * */
    NYA_Database* database;

    /** Who may register: open, by invite only, or nobody. The route enforces it; see accounts_invite.h. */
    NYA_AccountRegistration registration;

    /**
     * The program's name, as an authenticator shows it beside the account in its list. Required: the TOTP
     * routes always mount, and it goes into the otpauth URI's label. See nya_http_totp_enrol_create.
     * */
    NYA_ConstCString totp_issuer;

    /**
     * The key the half-logged-in `__Host-login` cookie is sealed under, and its length.
     *
     * A password that passed but still owes a second factor is held in a sealed cookie for five minutes,
     * never a row; this is the key that seals it. It is a seam, not a stored secret: a program backs it
     * with a per-process random key (the factor need not survive a restart) or with a rotating keyring,
     * whichever it already has. Required, and refused under NYA_HTTP_SEAL_MIN_SECRET_BYTES, because the
     * second-factor step is always mounted and a login that could not seal its pending state would stall.
     * */
    const u8* login_seal_secret;
    u64       login_seal_secret_size;

    /**
     * How the session cookie's `SameSite` reads. Zero is `Strict`, which is the default and what a session
     * cookie should carry; a program whose login must survive a cross-site top-level navigation sets Lax.
     * `None` is refused, because a session cookie sent with any cross-site request is the CSRF this guards.
     * The `__Host-` prefix, `HttpOnly` and `Secure` are not on this struct because they are not negotiable.
     * */
    NYA_HttpSameSite session_same_site;

    /**
     * The WebAuthn relying party the passkey routes check every credential against: the RP id — the
     * effective domain, e.g. "example.com" — and the exact origin, e.g. "https://example.com".
     *
     * Optional and paired. Set both and the four `/api/passkey/…` routes mount, so a passkey is a primary
     * way to log in; set neither and they do not mount at all, which is what a program that wants only
     * passwords leaves them as. Setting one without the other is refused, because a relying party that is
     * half configured is one whose origin or RP id check could not run. These are fixed configuration, not
     * read off a request header, so a forged `Host` or `Origin` cannot move the check they anchor — it is
     * the anti-phishing binding accounts_passkey.h relies on, and a caller does not get to weaken it.
     * */
    NYA_ConstCString passkey_rp_id;
    NYA_ConstCString passkey_origin;
};

// FUNCTIONS

/**
 * Opens the second-factor table, stores `config`, and answers the router to merge — or null on a
 * failure it has already logged.
 *
 * `nya_accounts_open` must have run first: these routes are that module wired to HTTP, not a second copy
 * of it. Null when the arena or database is missing, when the seal secret is absent or too short, when
 * the issuer is empty, or when the table cannot be opened. Refuses a second call while a mount is live,
 * because the route table and the config are one per process; see the file note.
 *
 * The returned router has static storage and outlives the call, so it is merged directly with
 * nya_http_server_merge. Its close is nya_http_accounts_close.
 * */
NYA_API const NYA_HttpRouter* nya_http_accounts_open(NYA_HttpAccountsConfig config) __attr_no_discard;

/** Closes the second-factor table and forgets the config. The mirror of nya_http_accounts_open; the database is the caller's. */
NYA_API void nya_http_accounts_close(void);

/**
 * The account this request's `__Host-session` cookie names, or false with the 401 already the caller's
 * to return.
 *
 * The one piece an application's own routes need from the login flow: a notes route or a settings route
 * reads the caller this way, so "who is asking" is answered by the same validation the session route
 * uses rather than by each route copying the cookie dance. A revoked, expired or forged cookie is
 * nobody. Answers false when no mount is live.
 * */
NYA_API b8 nya_http_accounts_caller(NYA_HttpExchange* exchange, OUT NYA_AccountUser* out_user) __attr_no_discard;
