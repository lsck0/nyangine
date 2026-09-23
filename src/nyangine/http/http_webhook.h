/**
 * @file http_webhook.h
 *
 * Proving that a webhook came from who it claims to, before anything acts on it.
 *
 * ```c
 * NYA_HttpWebhook twitch = {
 *     .scheme           = NYA_HTTP_WEBHOOK_HMAC_SHA256,
 *     .secret           = SECRET,
 *     .secret_size      = sizeof(SECRET),
 *     .signature_header = "Twitch-Eventsub-Message-Signature",
 *     .signature_prefix = "sha256=",
 *     .id_header        = "Twitch-Eventsub-Message-Id",
 *     .timestamp_header = "Twitch-Eventsub-Message-Timestamp",
 * };
 *
 * if (nya_http_webhook_verify(exchange, &twitch) != NYA_HTTP_WEBHOOK_ACCEPTED) return NYA_HTTP_STATUS_FORBIDDEN;
 * ```
 *
 * ── what a webhook is, and why it needs its own call ──
 *
 * A webhook is a request from a stranger that says it is from a service you trust, and the only thing
 * separating those two is a signature over the exact bytes that arrived. Everything that gets this
 * wrong gets it wrong the same way: verifying a re-serialized body rather than the bytes, comparing
 * signatures with strcmp, or accepting a request with no timestamp and replaying it forever. So this
 * verifies over `request->body` as it arrived, compares in constant time, and refuses a request whose
 * timestamp is outside a window the caller writes down.
 *
 * ── the two schemes ──
 *
 * `HMAC_SHA256` is what Twitch EventSub, GitHub and Stripe send: a shared secret, and a hex signature
 * over a message the sender defines. `ED25519` is what Discord sends to an interactions endpoint: a
 * public key, and a hex signature over the timestamp followed by the body. Both take the same shape
 * here, because the difference is which primitive verifies and what goes into the message, and a caller
 * that had to know more than that would write the dangerous parts again.
 *
 * ── what is not here ──
 *
 * No delivery, no retry and no queue: acting on a webhook is the program's business, and this answers
 * exactly one question. No replay cache either — the timestamp window bounds how long a captured
 * request stays useful, and remembering ids needs storage this module does not have. `id_header` is
 * read out for a caller that has somewhere to keep them; see NYA_HTTP_WEBHOOK_REPLAYED.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/crypto/crypto_sign.h"
#include "nyangine/http/http_router.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How far a webhook's timestamp may be from this server's clock, either way.
 *
 * Five minutes is what Twitch, Stripe and Discord all document, so a sender that is inside their own
 * tolerance is inside this one. It bounds what a captured request is worth: past it, a replay is
 * refused whether or not anything remembers having seen it.
 * */
#define NYA_HTTP_WEBHOOK_TOLERANCE_S 300

/** Longest signature this accepts, as hex. Ed25519 is 64 bytes and HMAC-SHA256 is 32, so 128 hex digits covers both. */
#define NYA_HTTP_WEBHOOK_MAX_SIGNATURE 129

/** Longest message assembled for signing: the id, the timestamp and the body, with the body's bound the real one. */
#define NYA_HTTP_WEBHOOK_MAX_MESSAGE (NYA_HTTP_MAX_BODY_BYTES + 256)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Which primitive proves the request, and what goes into the message it is proved over. */
typedef enum {
    /** A shared secret over `id + timestamp + body`, hex, as Twitch EventSub and GitHub send. */
    NYA_HTTP_WEBHOOK_HMAC_SHA256 = 0,

    /** A public key over `timestamp + body`, hex, as Discord sends to an interactions endpoint. */
    NYA_HTTP_WEBHOOK_ED25519,

    NYA_HTTP_WEBHOOK_SCHEME_COUNT,
} NYA_HttpWebhookScheme;

/** What verifying answered. Anything but ACCEPTED means the request is a stranger's. */
typedef enum {
    /**
     * Refused, and the zero of the enum so that an unset verdict fails closed: a caller that forgot to
     * look at the answer refuses rather than accepts.
     * */
    NYA_HTTP_WEBHOOK_REFUSED = 0,

    NYA_HTTP_WEBHOOK_ACCEPTED,

    /** The signature verified, and the timestamp is outside the window. A capture being replayed. */
    NYA_HTTP_WEBHOOK_REPLAYED,

    /** This server was not configured for it: no secret, no key, or no header named. */
    NYA_HTTP_WEBHOOK_UNCONFIGURED,

    NYA_HTTP_WEBHOOK_VERDICT_COUNT,
} NYA_HttpWebhookVerdict;

/** One sender's rules: which scheme, which key, and which headers carry the proof. */
typedef struct {
    NYA_HttpWebhookScheme scheme;

    /** HMAC_SHA256: the shared secret. Ignored by ED25519. */
    const u8* secret;
    u64       secret_size;

    /** ED25519: the sender's public key. Ignored by HMAC_SHA256. */
    const NYA_CryptoSignPublicKey* public_key;

    /** The header carrying the signature as hex. Required. */
    NYA_ConstCString signature_header;

    /** What the signature is prefixed with, such as "sha256=". Null or empty for a bare signature. */
    NYA_ConstCString signature_prefix;

    /** The header carrying the message id, prepended to the message when the sender does that. Optional. */
    NYA_ConstCString id_header;

    /** The header carrying the timestamp. Required: without one nothing bounds a replay. */
    NYA_ConstCString timestamp_header;

    /**
     * Seconds of clock difference to allow, or zero for NYA_HTTP_WEBHOOK_TOLERANCE_S.
     *
     * The timestamp is read as seconds since the epoch, or as RFC 3339 when it does not parse as a
     * number, which is what Twitch sends.
     * */
    u64 tolerance_s;
} NYA_HttpWebhook;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether this exchange's body really came from the holder of that secret or key.
 *
 * Verifies over the body as it arrived, compares the signature in constant time, and checks the
 * timestamp against the window before answering ACCEPTED. A refusal says nothing about which half was
 * wrong, because telling a forger that their signature was fine and their clock was not is telling them
 * where to try next.
 * */
NYA_API NYA_HttpWebhookVerdict nya_http_webhook_verify(const NYA_HttpExchange* exchange, const NYA_HttpWebhook* webhook) __attr_no_discard;

/**
 * The message id this request carried, for a caller keeping a list of what it has already acted on.
 *
 * False when the sender names no id header or the request did not carry one. Reading it says nothing
 * about whether the request verified; call this after nya_http_webhook_verify answered ACCEPTED.
 * */
NYA_API b8 nya_http_webhook_id(const NYA_HttpExchange* exchange, const NYA_HttpWebhook* webhook, OUT char* out_id, u64 capacity) __attr_no_discard;

/** "accepted", "refused", ... for a log line. Never null. */
NYA_API NYA_ConstCString nya_http_webhook_verdict_text(NYA_HttpWebhookVerdict verdict) __attr_no_discard;
