/**
 * @file oidc.h
 *
 * "Log in with Google" and its relatives: the authorization code flow with PKCE, ending at an id_token
 * whose signature this actually checks rather than trusting because the transport was TLS.
 *
 * ```c
 * NYA_OidcProvider* google = nullptr;
 * NYA_EXPECT(nya_oidc_create(arena, (NYA_OidcOptions){
 *     .issuer       = "https://accounts.google.com",
 *     .client_id    = client_id,
 *     .client_secret = client_secret,
 *     .redirect_uri = "https://example.com/callback",
 *     .scopes       = "openid email profile",
 * }, &google));
 *
 * NYA_EXPECT(nya_oidc_discover(google, arena));   // once, at startup
 *
 * // sending a person to log in
 * char                   url[NYA_OIDC_MAX_URL] = { 0 };
 * NYA_OidcAuthorizeState state                  = { 0 };
 * NYA_EXPECT(nya_oidc_authorize_url(google, url, sizeof(url), &state));
 * // redirect the browser to `url`; stash `state` against the session that started this login.
 *
 * // the callback: `code` and `state` came back on the query string
 * NYA_OidcClaims claims = { 0 };
 * NYA_TRY(nya_oidc_exchange(google, arena, code, &state, &claims));
 * // claims.subject is who this token now says the caller is
 * ```
 *
 * ── why the verifier, state and nonce come back to the caller rather than staying here ──
 *
 * A login is one browser round trip against one struct returned by nya_oidc_authorize_url, but this
 * provider is a long lived thing an app makes once at startup and reuses for every login anybody
 * starts. Holding one pending login's secrets inside it would mean either a queue of them (with its own
 * capacity, its own expiry, its own eviction) or one login at a time across the whole process. Neither
 * is this module's business: the engine already has a place a login in progress belongs — the session
 * or the request that is handling it — so NYA_OidcAuthorizeState is hand luggage. Put it in a cookie,
 * a server side session keyed by `state`, or wherever else a caller already keeps per visitor data.
 *
 * ── PKCE is not optional here ──
 *
 * There is no code path that skips the challenge. A confidential client with a secret is still a
 * browser redirect away from a stolen authorization code, and the fix that closes that (RFC 7636) costs
 * nothing a client already capable of generating a nonce cannot afford.
 *
 * ── verifying an id_token cannot be "the signature first" the way this engine's own JWTs are ──
 *
 * http_auth.h's HS256 tokens check the signature before looking at anything the token claims, because
 * with one shared secret there is nothing to learn from the token before that check. An id_token is
 * signed by one of several keys a provider rotates, named by `kid` in the header, so which key to try
 * has to come from the token itself. What stays true is that the header is trusted for exactly two
 * things — `alg`, to refuse anything that is not RS256, and `kid`, to pick a key this provider's own
 * jwks already holds — and nothing else about the token is believed until nya_crypto_rsa_verify_sha256
 * says the bytes were signed by that key. `alg` is compared against "RS256" and nothing else, which is
 * the whole of the `alg: none` bug and the HS256-downgrade one: this module never computes an HMAC over
 * anything, so there is no confused-algorithm path to fall into even if it wanted to.
 *
 * ── what a caller still has to decide ──
 *
 * A verified id_token says the provider vouches for this subject, at this moment, for this client. It
 * does not say the email is still that person's, that the account still exists, or that scopes granted
 * a year ago still apply — ordinary session and revocation concerns that belong to whatever this feeds
 * into, not to a token verifier.
 *
 * ── the token endpoint sends JSON, which is not what RFC 6749 asks for ──
 *
 * The authorization code is redeemed with `plugins/curl/request.h`, and that module serializes every
 * body as `application/json`; it has no form-urlencoded body to offer. RFC 6749 section 4.1.3 wants
 * `application/x-www-form-urlencoded` here. Google's and Auth0's token endpoints accept a JSON body
 * without complaint; a provider that insists on the RFC's encoding — most default Keycloak
 * installations, for one — will refuse this exchange with an `invalid_request`, and the fix belongs in
 * request.h as a form-urlencoded body option, not as a special case grown here.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"
#include "nyangine/crypto/crypto_rsa.h"
#include "nyangine/plugins/curl/request.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** An issuer or a discovered endpoint, terminator included. Room for a path past the bare host. */
#define NYA_OIDC_MAX_URL 512

/** A client id, terminator included. Providers hand out anything from a short slug to a long opaque one. */
#define NYA_OIDC_MAX_CLIENT_ID 256

/** A client secret, terminator included. Wiped at destroy like any other secret this engine holds. */
#define NYA_OIDC_MAX_CLIENT_SECRET 256

/** A redirect uri, terminator included. Matches NYA_OIDC_MAX_URL: it is a url like any other here. */
#define NYA_OIDC_MAX_REDIRECT_URI NYA_OIDC_MAX_URL

/** The configured scope string, space separated, terminator included. "openid email profile" and its kin. */
#define NYA_OIDC_MAX_SCOPES 256

/** RFC 7519's own bound on `sub`: 255 ASCII characters, plus the terminator. */
#define NYA_OIDC_MAX_SUBJECT 256

/** RFC 5321's mailbox limit, plus the terminator. */
#define NYA_OIDC_MAX_EMAIL 255

/** A display name. Nothing in the spec bounds it; this is generous for a person's name and not a bio. */
#define NYA_OIDC_MAX_NAME 128

/** A picture claim is a url. */
#define NYA_OIDC_MAX_PICTURE NYA_OIDC_MAX_URL

/** A key id out of a JWKS `kid`, terminator included. Providers use short opaque strings or a thumbprint. */
#define NYA_OIDC_MAX_KID 128

/**
 * RSA keys a provider's jwks is cached as.
 *
 * A provider signs with one key and rotates by publishing the next one alongside it for a while — Google
 * typically has two or three live at once — so four covers an ordinary rotation with a key to spare; this
 * is not a key store for many providers, since one NYA_OidcProvider speaks for exactly one.
 * */
#define NYA_OIDC_MAX_KEYS 4

/**
 * How long a jwks fetch must be honoured before another is allowed, in milliseconds.
 *
 * An id_token naming a `kid` this provider has never seen is either an ordinary key rotation or a token
 * built by someone hoping the client will refetch on command — the same request either way, but the
 * second one is free for an attacker to repeat. One refetch a minute is fast enough to pick up a real
 * rotation on the next login after it happens and slow enough that naming a random kid costs the
 * provider nothing more than one refused login.
 * */
#define NYA_OIDC_JWKS_REFETCH_COOLDOWN_MS 60000

/**
 * Clock skew this side tolerates on `iat`, in seconds. Never applied to `exp`: a token past the expiry
 * it claimed is expired, full stop, and stretching that window is stretching how long a stolen token
 * works. What skew buys is a token whose `iat` looks a few seconds in the future only because the
 * provider's clock and this host's disagree, which is ordinary and not a reason to refuse a login.
 * */
#define NYA_OIDC_CLOCK_SKEW_S 60

/**
 * Random bytes behind `state`, `nonce` and the PKCE verifier alike: 256 bits, which is far past what
 * either RFC asks for and cheap enough that there is no reason to measure closer.
 * */
#define NYA_OIDC_SECRET_BYTES 32

/**
 * What NYA_OIDC_SECRET_BYTES becomes as base64url text, terminator included: 43 characters. RFC 7636
 * wants a verifier from 43 to 128 characters long, so this lands exactly at the floor of that range
 * while still carrying the full 256 bits.
 * */
#define NYA_OIDC_SECRET_TEXT_BYTES 44

/**
 * Longest id_token this reads, terminator included.
 *
 * A real one carrying the standard claims plus a handful of custom ones runs one to two kilobytes; this
 * leaves headroom without leaving the bound open. Longer is refused before anything about it is parsed.
 * */
#define NYA_OIDC_MAX_ID_TOKEN_BYTES 8192

/** Longest access_token this holds onto for a follow up nya_oidc_userinfo call, terminator included. */
#define NYA_OIDC_MAX_ACCESS_TOKEN_BYTES 2048

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_OidcOptions        NYA_OidcOptions;
typedef struct NYA_OidcAuthorizeState NYA_OidcAuthorizeState;
typedef struct NYA_OidcClaims         NYA_OidcClaims;
typedef struct NYA_OidcProvider       NYA_OidcProvider;

struct NYA_OidcOptions {
    /** Required. No trailing slash; this is the exact string the discovery document's own `issuer` must equal. */
    NYA_ConstCString issuer;

    /** Required. */
    NYA_ConstCString client_id;

    /**
     * The client secret, for a confidential client. Empty is a public client: PKCE is still mandatory
     * either way, so this is never the only thing standing between an intercepted code and a token.
     * Copied at create and wiped at destroy.
     * */
    NYA_ConstCString client_secret;

    /** Required. Must equal what was registered with the provider; they refuse a mismatch, not this. */
    NYA_ConstCString redirect_uri;

    /**
     * Space separated. "openid" is required by the spec this whole file implements; a scope string
     * without it is refused at create rather than sent to a provider that would refuse it anyway.
     * */
    NYA_ConstCString scopes;

    /** What one transfer is given. Zero means NYA_REQUEST_DEFAULT_TIMEOUT_MS. */
    u64 timeout_ms;

    /**
     * How a request is actually performed, and where the clocks come from.
     *
     * Left null, nya_request_perform and the engine's clocks. A test fills them with canned replies,
     * and a program that cannot afford a synchronous transfer on its calling thread fills them with its
     * own; see nya_oidc_exchange for what blocks.
     * */
    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    u64 (*now_s)(void* user);
    void* user;
};

/**
 * What a caller keeps between sending someone to nya_oidc_authorize_url and calling nya_oidc_exchange
 * with what came back. See the file note on why this is not held inside NYA_OidcProvider.
 * */
struct NYA_OidcAuthorizeState {
    /**
     * What the callback's own `state` query parameter must equal.
     *
     * nya_oidc_exchange never sees the callback's query string, only this struct, so it cannot make
     * that comparison itself: the caller's own lookup — finding the session or cookie this `state`
     * names — is where a forged or replayed callback is already refused, before nya_oidc_exchange is
     * ever called. This field exists so that lookup has something to compare against.
     * */
    char state[NYA_OIDC_SECRET_TEXT_BYTES];

    /** What the id_token's own `nonce` claim must equal, which is what ties the token to this browser round trip. */
    char nonce[NYA_OIDC_SECRET_TEXT_BYTES];

    /** RFC 7636's PKCE verifier: the secret the challenge sent up front was a hash of. */
    char code_verifier[NYA_OIDC_SECRET_TEXT_BYTES];
};

/** What a verified id_token says, plus enough to ask for more. */
struct NYA_OidcClaims {
    /** `sub`. Stable per provider per client; never empty in a value this handed back. */
    char subject[NYA_OIDC_MAX_SUBJECT];

    /** `iss`, which is provider->issuer by the time this is filled in: nya_oidc_exchange already checked it. */
    char issuer[NYA_OIDC_MAX_URL];

    /** `email`. Empty when the id_token carried none — most providers need the "email" scope for this. */
    char email[NYA_OIDC_MAX_EMAIL];
    b8   email_verified;

    /** `name` and `picture`. Empty when absent; ask for the "profile" scope to get them. */
    char name[NYA_OIDC_MAX_NAME];
    char picture[NYA_OIDC_MAX_PICTURE];

    /**
     * The access_token the same response carried, for nya_oidc_userinfo. Empty when the token endpoint
     * did not return one, which the spec allows for an id_token only request.
     * */
    char access_token[NYA_OIDC_MAX_ACCESS_TOKEN_BYTES];

    /**
     * Every claim the id_token's payload carried, parsed. Lives in the arena passed to nya_oidc_exchange,
     * so it is valid exactly as long as that arena is.
     * */
    const NYA_Object* raw;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Makes a provider. Refuses a missing issuer, client id or redirect uri, and a scope string that does
 * not carry "openid". Nothing is sent until nya_oidc_discover.
 * */
NYA_API NYA_Error nya_oidc_create(NYA_Arena* arena, NYA_OidcOptions options, OUT NYA_OidcProvider** out_provider) __attr_no_discard;

/**
 * Wipes the client secret and any cached key material.
 *
 * A provider lives entirely inside the arena it was created with rather than owning one of its own —
 * there is no per-login queue here the way twitch_helix.h and telegram.h have, so there was nothing
 * this needed a private arena for. Destroying that arena is what reclaims the memory; this call only
 * needs to exist so the secret does not wait for that to happen.
 * */
NYA_API void nya_oidc_destroy(NYA_OidcProvider* provider);

/**
 * Fetches `<issuer>/.well-known/openid-configuration` and keeps `authorization_endpoint`,
 * `token_endpoint` and `jwks_uri` from it.
 *
 * Refused, without keeping anything from the document, when its own `issuer` is not byte for byte the
 * one this provider was created with: a discovery document is exactly the kind of thing a misconfigured
 * proxy or a DNS mixup hands back for the wrong host, and believing its endpoints anyway is how a login
 * ends up posting a code to somewhere that was never the provider.
 * */
NYA_API NYA_Error nya_oidc_discover(NYA_OidcProvider* provider, NYA_Arena* arena) __attr_no_discard;

/**
 * The url to send someone to, and the state this login needs kept until the callback.
 *
 * Builds `response_type=code`, the configured scopes, a fresh `state` and `nonce`, and a PKCE
 * `code_challenge` (S256) over a fresh verifier — all from the operating system's random source, none
 * of it derived from anything guessable. Refused before nya_oidc_discover has filled in
 * `authorization_endpoint`.
 * */
NYA_API NYA_Error nya_oidc_authorize_url(const NYA_OidcProvider* provider, OUT char* out_url, u64 capacity, OUT NYA_OidcAuthorizeState* out_state)
    __attr_no_discard;

/**
 * Redeems `code` at the token endpoint and verifies the id_token that comes back.
 *
 * `state` is what nya_oidc_authorize_url handed out for this login; its `code_verifier` goes to the
 * token endpoint, and its `nonce` must equal the id_token's own claim. `arena` owns `out_claims->raw`
 * and everything else this allocates while checking the token; nothing here is kept past the call
 * except inside `provider`'s own jwks cache, which holds no arena memory.
 *
 * Every refusal names the one check that failed: a wrong `aud`, an expired token and a signature that
 * does not verify are three different errors with three different messages, because "the id_token is
 * not valid" tells nobody debugging a stuck login what to fix. A token failing any single check is
 * refused whole; there is no partially trusted result.
 * */
NYA_API NYA_Error
nya_oidc_exchange(NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString code, const NYA_OidcAuthorizeState* state, OUT NYA_OidcClaims* out_claims)
    __attr_no_discard;

/**
 * The userinfo endpoint's answer for `access_token`, parsed. Optional per the spec and here alike: a
 * caller that only needed the id_token's own claims never has to call this.
 *
 * Discovery does not require `userinfo_endpoint`; a provider that omits it makes this
 * NYA_ERROR_NOT_SUPPORTED rather than a request to an empty url.
 * */
NYA_API NYA_Error nya_oidc_userinfo(NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString access_token, OUT NYA_Object** out_claims)
    __attr_no_discard;
