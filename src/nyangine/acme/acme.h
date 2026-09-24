/**
 * @file acme.h
 *
 * ── the acme module ──
 *
 * A single executable that gets and renews its own TLS certificate, so "one binary" is the whole of it
 * and there is no certbot beside it and no proxy in front of it. This is an ACME v2 client, RFC 8555, and
 * it speaks to Let's Encrypt or any other conforming certificate authority.
 *
 * ```c
 * // one account key, kept for the life of the program and never logged
 * NYA_AcmeAccountKey* account = nullptr;
 * NYA_TRY(nya_acme_account_key_create(arena, NYA_ACME_ALGORITHM_ES256, &account));
 * defer nya_acme_account_key_destroy(account);
 *
 * // a place a served challenge is looked up, filled while the order runs
 * NYA_AcmeChallengeStore* challenges = nya_acme_challenge_store_create(arena);
 *
 * NYA_ConstCString domains[] = { "example.com", "www.example.com" };
 * NYA_AcmeConfig   config    = {
 *     .directory_url = "https://acme-v02.api.letsencrypt.org/directory",
 *     .domains       = domains,
 *     .domain_count  = nya_carray_length(domains),
 *     .account_key   = account,
 *     .contact_email = "admin@example.com",
 *     .challenges    = challenges,
 *     .transport     = { .perform = my_curl_transport, .userdata = handle },
 * };
 *
 * NYA_AcmeCertificate certificate = { 0 };
 * NYA_TRY(nya_acme_obtain(arena, &config, &certificate));
 * // certificate.chain_pem and certificate.private_key_pem are what nya_tls_context_create is handed
 * ```
 *
 * ── the transport is a seam, and so is serving the challenge ──
 *
 * This module never opens a socket. Every request to the CA is handed to a NYA_AcmeTransport the program
 * supplies — a program with the curl plugin wires one over nya_request_perform, a test wires one that
 * plays a scripted CA with no network at all. That is the only way an ACME client is testable in a
 * sandbox where a real CA cannot be reached: the flow, the nonce handling and the state machine run over
 * canned replies, and the crypto underneath is checked directly. See the tests.
 *
 * Serving the HTTP-01 challenge is the mirror of the same idea. This module holds the token to
 * key-authorization map (a NYA_AcmeChallengeStore) and answers nya_acme_challenge_response for a request
 * path; mounting that at `/.well-known/acme-challenge/` on an HTTP server is the program's one line, so
 * this module stays below http and does not reach up into it. See nya_acme_challenge_response.
 *
 * ── the crypto, and why it is the real test ──
 *
 * Every POST to the CA is a JWS (RFC 7515) the CA verifies before it does anything, so a signature built
 * wrong is rejected at the door — that is the property the tests pin down. The account key signs it,
 * ES256 (ECDSA over P-256, RFC 7518) or EdDSA (Ed25519, RFC 8037). The first newAccount carries the
 * public key inline as a JWK; every request after it names the account by the `kid` URL the CA returned,
 * because the CA told us to. The signed input is `base64url(protected) || "." || base64url(payload)` and
 * nothing else, the protected header carries the fresh `nonce` and the request `url`, and each reply's
 * `Replay-Nonce` is the next request's — a nonce is used once and a stale one is a rejected request.
 *
 * The HTTP-01 key-authorization is `token || "." || base64url(SHA-256(JWK-thumbprint-input))`, RFC 8555
 * section 8.1 over the RFC 7638 thumbprint, and nya_acme_key_authorization computes exactly that.
 *
 * ── keys, monocypher and OpenSSL ──
 *
 * The account key is monocypher's when it is EdDSA (crypto_sign.h, the engine's own signature) and
 * OpenSSL's when it is ES256, because the engine verifies ECDSA but does not sign it — see crypto_ecdsa.h
 * on why signing ECDSA safely is OpenSSL's job. The certificate key is a fresh P-256 key and the
 * finalize step needs a PKCS#10 CSR carrying it and the domains as subjectAltNames; both are OpenSSL's,
 * the same OpenSSL tls.h links. A build with no OpenSSL (NYA_NO_TLS, which today is Windows) can still
 * build an EdDSA JWS and compute a key-authorization, but nya_acme_obtain returns NYA_ERROR_NOT_SUPPORTED
 * there because it cannot make a CSR.
 *
 * ── the account key is a secret ──
 *
 * Its private half never reaches a log, an error message or a backtrace, exactly as smtp treats a
 * password. What this module logs of a run is the order state and the CA's own problem documents, never a
 * key.
 *
 * ── what a live run needs ──
 *
 * An end-to-end obtain cannot run in the test sandbox: it needs a reachable CA and a domain that resolves
 * to this host on port 80 for the HTTP-01 check. Point `directory_url` at a local Pebble
 * (https://github.com/letsencrypt/pebble) or the Let's Encrypt staging directory to exercise the whole
 * path against a real server. The unit tests cover everything a mock can: the JWS, the thumbprint, the
 * key-authorization, the challenge route, and the directory and order parsing.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"
#include "nyangine/crypto/crypto_hash.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The path prefix an HTTP-01 challenge is served under, RFC 8555 section 8.3. */
#define NYA_ACME_HTTP01_PREFIX "/.well-known/acme-challenge/"

/** The most domains one order may carry, which bounds the CSR and the authorization loop. */
#ifndef NYA_ACME_MAX_DOMAINS
#define NYA_ACME_MAX_DOMAINS 64
#endif

/** How many times an authorization or an order is polled before the obtain gives up. */
#ifndef NYA_ACME_MAX_POLLS
#define NYA_ACME_MAX_POLLS 30
#endif

/** How long a poll waits when the CA sends no Retry-After, in milliseconds. */
#ifndef NYA_ACME_POLL_INTERVAL_MS
#define NYA_ACME_POLL_INTERVAL_MS 2000
#endif

/** Renew this many days before a certificate expires, when a caller names no other window. */
#ifndef NYA_ACME_RENEW_BEFORE_DAYS
#define NYA_ACME_RENEW_BEFORE_DAYS 30
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_AcmeAccountKey    NYA_AcmeAccountKey;
typedef struct NYA_AcmeChallengeStore NYA_AcmeChallengeStore;

/** Which signature the account key carries, and which `alg` its JWS protected header names. */
typedef enum {
    /** ECDSA over P-256 with SHA-256, `ES256`. OpenSSL's, and what a CA is likeliest to accept. */
    NYA_ACME_ALGORITHM_ES256 = 0,

    /** Ed25519, `EdDSA` with `crv` Ed25519. The engine's own signature (crypto_sign.h), no OpenSSL needed. */
    NYA_ACME_ALGORITHM_EDDSA,
} NYA_AcmeAlgorithm;

/** How a request reaches the CA. HEAD is newNonce, GET is the directory, POST is everything else (POST-as-GET included). */
typedef enum {
    NYA_ACME_METHOD_GET = 0,
    NYA_ACME_METHOD_HEAD,
    NYA_ACME_METHOD_POST,
} NYA_AcmeMethod;

/** One request for the transport to make: a method, a URL, and for a POST the JWS bytes. */
typedef struct {
    NYA_AcmeMethod method;

    /** The whole URL. Never null. */
    NYA_ConstCString url;

    /** The `application/jose+json` body for a POST, null for a GET or a HEAD. Not terminated. */
    const u8* body;
    u64       body_size;
} NYA_AcmeHttpRequest;

/**
 * What the transport fills in from the CA's reply.
 *
 * The headers here are the four an ACME client reads. A transport that cannot find one leaves it null (or
 * zero for `retry_after_seconds`); the flow treats an absent nonce or Location as the protocol error it
 * is.
 * */
typedef struct {
    /** The HTTP status. Zero means the transport got no reply at all, which it must report as an error. */
    u16 status;

    /** The body exactly as it arrived, never null and empty when the body was. */
    NYA_String* body;

    /** `Replay-Nonce`: the nonce the next request must carry. Null when the reply had none. */
    NYA_ConstCString replay_nonce;

    /** `Location`: the account URL after newAccount, the order URL after newOrder. Null when absent. */
    NYA_ConstCString location;

    /** `Content-Type`, so a `application/problem+json` error is told from a certificate. Null when absent. */
    NYA_ConstCString content_type;

    /** `Retry-After` in seconds, or zero when the CA sent none. A poll waits this long when it is set. */
    u32 retry_after_seconds;
} NYA_AcmeHttpResponse;

/**
 * Performs one request and fills `out_response`. `arena` is scratch the reply may be allocated from.
 *
 * Returns an error only for a transport failure that produced no reply — a refused connection, a timeout,
 * a TLS verification failure. A reply that arrived is a success here whatever its status; the flow reads
 * the status and decides. A real transport verifies the CA's certificate (see tls.h): an ACME client that
 * talked to an unverified CA would take a certificate from an attacker.
 * */
typedef NYA_Error (*NYA_AcmeTransportFn)(void* userdata, NYA_Arena* arena, const NYA_AcmeHttpRequest* request, OUT NYA_AcmeHttpResponse* out_response);

/** The transport the program supplies; see NYA_AcmeTransportFn. */
typedef struct {
    NYA_AcmeTransportFn perform;

    /** Handed back to `perform` untouched: a curl handle, a scripted CA, whatever it needs. */
    void* userdata;
} NYA_AcmeTransport;

/** What an obtain produces: the chain to serve and the private key it was issued against. */
typedef struct {
    /** The certificate chain, leaf first, PEM. What nya_tls_context_create takes as `certificate_path` once written. */
    NYA_String* chain_pem;

    /** The freshly generated private key for the leaf, PEM. The `key_path` half of the pair. A secret. */
    NYA_String* private_key_pem;
} NYA_AcmeCertificate;

/** Everything one obtain needs: where the CA is, what to certify, who to sign as, and how to reach it. */
typedef struct {
    /** The CA's directory URL, the one document every other URL is discovered from. Required. */
    NYA_ConstCString directory_url;

    /** The domains to certify, the first of which is the certificate's common name. Required, at least one. */
    const NYA_ConstCString* domains;
    u64                     domain_count;

    /** The account key, which signs every request. Required; generate it once and keep it. */
    const NYA_AcmeAccountKey* account_key;

    /** An optional contact for the account, carried as `mailto:`. The CA emails expiry warnings here. */
    NYA_ConstCString contact_email;

    /** Where a served challenge is registered so the CA's HTTP-01 check finds it. Required. */
    NYA_AcmeChallengeStore* challenges;

    /** How requests reach the CA. Required. */
    NYA_AcmeTransport transport;
} NYA_AcmeConfig;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCOUNT KEYS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A fresh account key of `algorithm`, allocated from `arena`.
 *
 * NYA_ERROR_NOT_SUPPORTED for NYA_ACME_ALGORITHM_ES256 on a build with no OpenSSL; EdDSA works everywhere,
 * since it is the engine's own signature. The private half is never logged. Freed with
 * nya_acme_account_key_destroy, which wipes it.
 * */
NYA_API NYA_Error nya_acme_account_key_create(NYA_Arena* arena, NYA_AcmeAlgorithm algorithm, OUT NYA_AcmeAccountKey** out_key) __attr_no_discard;

/**
 * The EdDSA account key a stored 32 byte seed belongs to, so a program keeps one seed rather than a key
 * file. NYA_ACME_ALGORITHM_ES256 has no seed form here; store its PEM. Deterministic, which is what lets a
 * test pin a JWS to known bytes.
 * */
NYA_API NYA_Error nya_acme_account_key_from_eddsa_seed(NYA_Arena* arena, const u8 seed[32], OUT NYA_AcmeAccountKey** out_key) __attr_no_discard;

/** Wipes the private half and releases the key. Safe on null. */
NYA_API void nya_acme_account_key_destroy(NYA_AcmeAccountKey* key);

/** Which algorithm the key carries. */
NYA_API NYA_AcmeAlgorithm nya_acme_account_key_algorithm(const NYA_AcmeAccountKey* key) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * JWK, THUMBPRINT AND JWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The public JWK of the account key, as the object a newAccount request embeds.
 *
 * `{ "crv":..., "kty":..., "x":... }` for EdDSA, with `"y"` besides for ES256 — the public numbers and
 * nothing private. The members are added in the RFC 7638 required order, so serializing this compact is
 * also the thumbprint input.
 * */
NYA_API NYA_Error nya_acme_jwk(NYA_Arena* arena, const NYA_AcmeAccountKey* key, OUT NYA_Object** out_jwk) __attr_no_discard;

/**
 * The RFC 7638 thumbprint of the account key: SHA-256 of the canonical JWK, the lexicographically ordered
 * members with no whitespace. This is what a key-authorization and an account are identified by.
 * */
NYA_API NYA_Error nya_acme_jwk_thumbprint(const NYA_AcmeAccountKey* key, OUT NYA_CryptoSha256Digest* out_thumbprint) __attr_no_discard;

/**
 * Builds the signed JWS object a POST carries, RFC 7515 flattened JSON.
 *
 * `url` is the request URL and goes in the protected header; `nonce` is the fresh Replay-Nonce. Exactly
 * one of `kid` and the embedded JWK identifies the account: pass `kid` (the account URL) for every
 * request after newAccount, and null for newAccount itself, which embeds the JWK. `payload` is the
 * request body object, or null for a POST-as-GET, whose payload is the empty string. The result is the
 * `{ "protected":..., "payload":..., "signature":... }` object to serialize as the body.
 * */
NYA_API NYA_Error nya_acme_jws_sign(NYA_Arena* arena, const NYA_AcmeAccountKey* key, NYA_ConstCString url, NYA_ConstCString nonce, NYA_ConstCString kid,
                                    const NYA_Object* payload, OUT NYA_Object** out_jws) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HTTP-01 CHALLENGE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The key-authorization for `token`: `token || "." || base64url(SHA-256(thumbprint input))`, RFC 8555
 * section 8.1. This is the exact body served at the challenge URL, and the string the CA recomputes.
 * */
NYA_API NYA_Error nya_acme_key_authorization(NYA_Arena* arena, NYA_ConstCString token, const NYA_AcmeAccountKey* key, OUT NYA_ConstCString* out_key_authorization)
    __attr_no_discard;

/** A challenge store, empty. Holds the token to key-authorization map an HTTP-01 check reads. */
NYA_API NYA_AcmeChallengeStore* nya_acme_challenge_store_create(NYA_Arena* arena) __attr_no_discard;

/**
 * Forgets every challenge, leaving the store empty and reusable.
 *
 * The store's memory is the arena's, so this frees nothing a caller must; it is the create's pair and the
 * one call that clears a store between orders. Safe on null.
 * */
NYA_API void nya_acme_challenge_store_destroy(NYA_AcmeChallengeStore* store);

/**
 * Registers `key_authorization` under `token`, so a request for the token's challenge path is answered.
 *
 * Called by the flow when a challenge is about to be triggered, and safe to call directly from a program
 * that drives its own order. Overwrites an existing entry for the same token.
 * */
NYA_API NYA_Error nya_acme_challenge_store_add(NYA_AcmeChallengeStore* store, NYA_ConstCString token, NYA_ConstCString key_authorization) __attr_no_discard;

/** Forgets the token, once its order is done. Safe on a token that is not there. */
NYA_API void nya_acme_challenge_store_remove(NYA_AcmeChallengeStore* store, NYA_ConstCString token);

/**
 * The body to serve for `request_path`, or false when the path is not a live challenge.
 *
 * `request_path` is the request's path, `/.well-known/acme-challenge/<token>`; a path outside the prefix,
 * or one whose token is not in the store, is not this module's to answer and returns false. On a match,
 * `out_body` points at the key-authorization to write as `text/plain` with a 200. This is the whole of
 * the HTTP-01 route, split out so a test checks it without an HTTP server and a program mounts it in one
 * line.
 * */
NYA_API b8 nya_acme_challenge_response(const NYA_AcmeChallengeStore* store, NYA_ConstCString request_path, OUT NYA_ConstCString* out_body) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * OBTAIN AND RENEW
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Runs the whole RFC 8555 flow and fills `out_certificate`.
 *
 * directory → newNonce → newAccount → newOrder → for each authorization, register and trigger its
 * HTTP-01 challenge and poll it valid → finalize with a fresh key's CSR → download the chain. `arena`
 * owns the result. The account key must already exist; the certificate key is generated here and returned
 * beside the chain.
 *
 * NYA_ERROR_NOT_SUPPORTED on a build with no OpenSSL (no CSR is possible); NYA_ERROR_INVALID_ARGUMENT for a
 * config missing a required field; NYA_ERROR_IO or NYA_ERROR_NOT_OK when the CA refuses a step, carrying
 * the CA's own problem detail; NYA_ERROR_TIMEOUT when an authorization or the order does not become valid
 * within NYA_ACME_MAX_POLLS. The account key never appears in any of these.
 * */
NYA_API NYA_Error nya_acme_obtain(NYA_Arena* arena, const NYA_AcmeConfig* config, OUT NYA_AcmeCertificate* out_certificate) __attr_no_discard;

/**
 * Whether `chain_pem`'s leaf certificate expires within `renew_before_days` (or NYA_ACME_RENEW_BEFORE_DAYS
 * when that is zero), which is the whole of the renew decision: obtain again when it says true.
 *
 * NYA_ERROR_NOT_SUPPORTED with no OpenSSL, NYA_ERROR_PARSE when the PEM holds no certificate. Reads only the
 * leaf, the first certificate in the chain, since that is the one that expires.
 * */
NYA_API NYA_Error nya_acme_needs_renewal(NYA_ConstCString chain_pem, u32 renew_before_days, OUT b8* out_needs_renewal) __attr_no_discard;
