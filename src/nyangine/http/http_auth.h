/**
 * @file http_auth.h
 *
 * Who is calling, proved by a signed token rather than asserted by a header.
 *
 * ```
 * nya_http_jwt_encode         an identity -> a compact JWS, signed HS256 with the server secret
 * nya_http_jwt_decode         the inverse: bytes -> an NYA_HttpIdentity, or an error saying why not
 *
 * nya_http_bearer_token       the token out of an Authorization header, bounded, without copying it
 *
 * nya_http_challenge_create   a stateless second factor challenge for a subject
 * nya_http_challenge_verify   the pair: whether a challenge is one we issued and is still in date
 * nya_http_second_factor_set  installs the verifier that checks a signature over that challenge
 * nya_http_second_factor      what that last set, null when there is none
 *
 * nya_http_scope_contains     whether an identity carries every bit a route demands
 * ```
 *
 * ```c
 * NYA_HttpIdentity identity = { .scope = NYA_HTTP_SCOPE_READ, .issued_at_s = now, .expires_at_s = now + 3600 };
 * (void)snprintf(identity.subject, sizeof(identity.subject), "luca");
 *
 * char token[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };
 * NYA_TRY(nya_http_jwt_encode(&identity, secret, secret_size, token, sizeof(token)));
 * ```
 *
 * ── what is real and what is a seam ──
 *
 * The JWT is real: HS256 over the crypto module's HMAC-SHA256, with the signature checked in constant time
 * and checked *before* the payload is parsed, so a forged token never reaches a JSON parser. `alg` is
 * compared against "HS256" and nothing else, which is the whole of the `alg: none` and the
 * RS256-downgrade family.
 *
 * The second factor is half real. The challenge is real and stateless: an HMAC of the subject and a
 * coarse timestamp under the same secret, so a server that restarts still recognises the challenges
 * it issued, and nothing is stored per user. What is a seam is the signature check: OpenPGP parsing
 * is not in this engine and is not going into it as a side effect of an HTTP server, so
 * nya_http_second_factor_set installs the verifier and a route that demands a second factor with none
 * installed answers 501 rather than passing. See TODO.md, "Web".
 *
 * ── the secret ──
 *
 * From the environment, never from a file in the tree and never a default. A server started with no
 * secret refuses to register any route that needs one; see NYA_HttpConfig.secret.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/http/http_types.h"

// CONSTANTS

/** Longest subject, terminator included. A subject is a user name or a tool name, not a sentence. */
#define NYA_HTTP_MAX_SUBJECT 64

/**
 * Longest token this server will encode or look at, terminator included.
 *
 * A token of these claims is about two hundred bytes; the rest is headroom. A bearer header longer
 * than this is refused without being decoded, which is the bound that keeps a hostile Authorization
 * header from being work.
 * */
#define NYA_HTTP_MAX_TOKEN_BYTES 512

/** Longest signing secret. Longer keys are hashed down by HMAC anyway; see nya_crypto_hmac_sha256. */
#define NYA_HTTP_MAX_SECRET_BYTES 64

/** Shortest secret that will be accepted. Under this a token is guessable, so it is refused at startup. */
#define NYA_HTTP_MIN_SECRET_BYTES 32

/**
 * How long a second factor challenge stays valid, and the granularity it is minted at.
 *
 * One window either side is accepted, so a challenge issued just before a boundary still verifies.
 * Thirty seconds twice over is long enough to find a hardware key and short enough that a captured
 * challenge is worth nothing by the time it is replayed.
 * */
#define NYA_HTTP_CHALLENGE_WINDOW_S 30

// TYPES

typedef enum NYA_HttpScope      NYA_HttpScope;
typedef struct NYA_HttpIdentity NYA_HttpIdentity;

// @reflect
/**
 * What a caller is allowed to do. Bits rather than levels, because "may read metrics" and "may change
 * a setting" are not points on one line.
 * */
enum NYA_HttpScope {
    NYA_HTTP_SCOPE_NONE = 0,

    /** Read anything the server exposes. What a metrics page needs and nothing more. */
    NYA_HTTP_SCOPE_READ = 1 << 0,

    /** Change something: a setting, a system's enabled flag. */
    NYA_HTTP_SCOPE_WRITE = 1 << 1,

    /** Everything above, plus whatever a future route marks as dangerous. */
    NYA_HTTP_SCOPE_ADMIN = 1 << 2,

    /**
     * A second factor was presented and verified for this token. A route that carries this bit cannot
     * be reached with a password-only token however wide its other scopes are.
     * */
    NYA_HTTP_SCOPE_SECOND_FACTOR = 1 << 3,
};

// @reflect
/**
 * A caller whose token verified. There is no way to make one but nya_http_jwt_decode, which is the
 * point: a handler that takes one cannot be reached by an unauthenticated request.
 * */
struct NYA_HttpIdentity {
    /** Who. Null terminated, and never empty in a verified identity. */
    char subject[NYA_HTTP_MAX_SUBJECT];

    NYA_HttpScope scope; // @flags(NYA_HttpScope)

    /** Seconds since the epoch, as the token claimed and as the decoder checked against `now_s`. */
    u64 issued_at_s;
    u64 expires_at_s;
};

/**
 * Verifies a detached signature over `challenge` for `subject`. The PGP seam; see the file note.
 *
 * Returns NYA_OK when the signature is that subject's and covers exactly those bytes. Anything else
 * is a failure, and a failure is never a reason to let the request through.
 * */
typedef NYA_Error (*NYA_HttpSecondFactorFn)(
    NYA_ConstCString subject,
    const u8*        challenge,
    u64              challenge_size,
    const u8*        signature,
    u64              signature_size
);

// FUNCTIONS

// TOKENS

/**
 * Signs `identity` into `out_token` as a compact JWS.
 *
 * NYA_ERROR_INVALID_ARGUMENT for an empty subject, a secret under NYA_HTTP_MIN_SECRET_BYTES, or an
 * expiry that is not after the issue time; NYA_ERROR_OUT_OF_MEMORY when the token does not fit
 * `capacity`. Allocates nothing: the claims and the signature both fit fixed buffers, which is what
 * lets a token be minted from anywhere. Only the decoder needs an arena, because only it parses JSON.
 * */
NYA_API NYA_Error nya_http_jwt_encode(const NYA_HttpIdentity* identity, const u8* secret, u64 secret_size, OUT char* out_token, u64 capacity)
    __attr_no_discard;

/**
 * Verifies `token` and parses what it claims.
 *
 * In this order, and the order is the security property: the length bound, then the three-part shape,
 * then the signature in constant time, then the header's `alg`, and only then the payload. A token
 * that fails any of those has not had its claims parsed at all.
 *
 * NYA_ERROR_PERMISSION_DENIED for a bad signature or an `alg` that is not HS256, NYA_ERROR_TIMEOUT for
 * one that has expired or is not yet valid, NYA_ERROR_PARSE for anything malformed, and
 * NYA_ERROR_INVALID_ARGUMENT for a secret this server will not sign with.
 * */
NYA_API NYA_Error
nya_http_jwt_decode(NYA_Arena* arena, const char* token, u64 size, const u8* secret, u64 secret_size, u64 now_s, OUT NYA_HttpIdentity* out_identity)
    __attr_no_discard;

/**
 * The token out of `Authorization: Bearer <token>`, pointing into the request and copying nothing.
 *
 * False when there is no such header, when the scheme is not `Bearer`, or when what follows is empty
 * or longer than NYA_HTTP_MAX_TOKEN_BYTES.
 * */
NYA_API b8 nya_http_bearer_token(const NYA_HttpRequest* request, OUT const char** out_token, OUT u64* out_size);

/** The name of the cookie an access token travels in, which carries a prefix a browser enforces. */
#define NYA_HTTP_SESSION_COOKIE "__Host-session"

/**
 * The access token out of `Authorization: Bearer`, or out of the NYA_HTTP_SESSION_COOKIE cookie when
 * there is no such header. Points into the request and copies nothing.
 *
 * A cookie is sent by the browser whether or not the page meant to send it, which is what CSRF is, so a
 * token that arrives this way is only safe behind the two defences this server already has: dispatch
 * refuses a cross site request that changes anything before any handler runs, and the cookie itself is
 * written `SameSite=Strict`, so a browser does not attach it to a cross site request in the first place.
 * A route that wants neither of those takes the header form and nothing else.
 * */
NYA_API b8 nya_http_access_token(const NYA_HttpRequest* request, OUT const char** out_token, OUT u64* out_size);

// SCOPES

/** Whether `identity` carries every bit in `required`. NYA_HTTP_SCOPE_NONE is carried by everyone. */
NYA_API b8 nya_http_scope_contains(const NYA_HttpIdentity* identity, NYA_HttpScope required) __attr_no_discard;

// THE SECOND FACTOR

/**
 * A challenge for `subject`, valid for the window `now_s` falls in.
 *
 * Stateless on purpose: it is an HMAC of the subject and the window number under the server secret, so
 * there is nothing to store, nothing to expire and nothing to leak, and a restarted server still
 * recognises what it issued.
 * */
NYA_API NYA_Error
nya_http_challenge_create(NYA_ConstCString subject, const u8* secret, u64 secret_size, u64 now_s, OUT u8 out_challenge[NYA_CRYPTO_SHA256_BYTES])
    __attr_no_discard;

/**
 * Whether `challenge` is one this server issued for `subject`, in the current window or the one
 * before it. Constant time, so a near miss does not say how near.
 * */
NYA_API b8 nya_http_challenge_verify(NYA_ConstCString subject, const u8* secret, u64 secret_size, u64 now_s, const u8 challenge[NYA_CRYPTO_SHA256_BYTES])
    __attr_no_discard;

/** Installs the signature verifier. Null removes it, which is what the engine ships with. */
NYA_API void nya_http_second_factor_set(NYA_HttpSecondFactorFn verify);

/** What nya_http_second_factor_set last installed, or null. */
NYA_API NYA_HttpSecondFactorFn nya_http_second_factor(void) __attr_no_discard;
