#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock_format.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "nyangine-core/http/http_webhook.h"

// PRIVATE API DECLARATION

/** One hex digit's value, or false for anything else: a half written signature must not decode to zero. */
NYA_INTERNAL b8 _nya_http_webhook_hex(char character, OUT u8* out_value) __attr_no_discard;

/** Decodes a hex signature into `out`, refusing an odd length, a wrong length, or a byte that is not hex. */
NYA_INTERNAL b8 _nya_http_webhook_signature(NYA_ConstCString text, NYA_ConstCString prefix, OUT u8* out, u64 size) __attr_no_discard;

/** The timestamp header as seconds since the epoch: a plain number, or RFC 3339 as Twitch sends it. */
NYA_INTERNAL b8 _nya_http_webhook_timestamp(NYA_ConstCString text, OUT u64* out_seconds) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_HttpWebhookVerdict nya_http_webhook_verify(const NYA_HttpExchange* exchange, const NYA_HttpWebhook* webhook) {
    nya_assert(exchange != nullptr && exchange->request != nullptr);
    nya_assert(webhook != nullptr);

    if (webhook->signature_header == nullptr || webhook->timestamp_header == nullptr) return NYA_HTTP_WEBHOOK_UNCONFIGURED;
    if (webhook->scheme >= NYA_HTTP_WEBHOOK_SCHEME_COUNT) return NYA_HTTP_WEBHOOK_UNCONFIGURED;

    if (webhook->scheme == NYA_HTTP_WEBHOOK_HMAC_SHA256 && (webhook->secret == nullptr || webhook->secret_size == 0)) {
        return NYA_HTTP_WEBHOOK_UNCONFIGURED;
    }

    if (webhook->scheme == NYA_HTTP_WEBHOOK_ED25519 && webhook->public_key == nullptr) return NYA_HTTP_WEBHOOK_UNCONFIGURED;

    NYA_ConstCString signature_text = nya_http_request_header(exchange->request, webhook->signature_header);
    NYA_ConstCString timestamp_text = nya_http_request_header(exchange->request, webhook->timestamp_header);

    if (signature_text == nullptr || timestamp_text == nullptr) return NYA_HTTP_WEBHOOK_REFUSED;

    // The message, assembled from what the sender signs: id and timestamp before the body for HMAC, the timestamp alone for Ed25519; built from the bytes that arrived, not anything parsed out — a re-serialized body is a different body, and that difference is where a forgery lives.
    u8  message[NYA_HTTP_WEBHOOK_MAX_MESSAGE] = { 0 };
    u64 length                                = 0;

    NYA_ConstCString id = webhook->id_header != nullptr ? nya_http_request_header(exchange->request, webhook->id_header) : nullptr;

    if (webhook->scheme == NYA_HTTP_WEBHOOK_HMAC_SHA256 && id != nullptr) {
        s32 written = snprintf((char*)message + length, sizeof(message) - length, "%s", id);
        if (written < 0 || (u64)written >= sizeof(message) - length) return NYA_HTTP_WEBHOOK_REFUSED;

        length += (u64)written;
    }

    s32 stamp = snprintf((char*)message + length, sizeof(message) - length, "%s", timestamp_text);
    if (stamp < 0 || (u64)stamp >= sizeof(message) - length) return NYA_HTTP_WEBHOOK_REFUSED;

    length += (u64)stamp;

    if (length + exchange->request->body_size >= sizeof(message)) return NYA_HTTP_WEBHOOK_REFUSED;

    memcpy(message + length, exchange->request->body, exchange->request->body_size);
    length += exchange->request->body_size;

    b8 verified = false;

    switch (webhook->scheme) {
        case NYA_HTTP_WEBHOOK_HMAC_SHA256: {
            u8 claimed[NYA_CRYPTO_SHA256_BYTES] = { 0 };
            if (!_nya_http_webhook_signature(signature_text, webhook->signature_prefix, claimed, sizeof(claimed))) return NYA_HTTP_WEBHOOK_REFUSED;

            NYA_CryptoSha256Digest tag = { 0 };
            nya_crypto_hmac_sha256(webhook->secret, webhook->secret_size, message, length, &tag);

            verified = nya_crypto_equals(tag.bytes, claimed, sizeof(tag.bytes));
            break;
        }

        case NYA_HTTP_WEBHOOK_ED25519: {
            NYA_CryptoSignature claimed = { 0 };
            if (!_nya_http_webhook_signature(signature_text, webhook->signature_prefix, claimed.bytes, sizeof(claimed.bytes))) {
                return NYA_HTTP_WEBHOOK_REFUSED;
            }

            verified = nya_crypto_sign_verify(webhook->public_key, message, length, &claimed);
            break;
        }

        case NYA_HTTP_WEBHOOK_SCHEME_COUNT:
        default:                            return NYA_HTTP_WEBHOOK_UNCONFIGURED;
    }

    if (!verified) return NYA_HTTP_WEBHOOK_REFUSED;

    u64 sent_at_s = 0;
    if (!_nya_http_webhook_timestamp(timestamp_text, &sent_at_s)) return NYA_HTTP_WEBHOOK_REFUSED;

    // signed by the right key but too old to act on: a capture being replayed. Said apart from a refusal because it's the one failure a caller can act on, such as logging it.
    u64 tolerance = webhook->tolerance_s > 0 ? webhook->tolerance_s : NYA_HTTP_WEBHOOK_TOLERANCE_S;
    u64 drift     = exchange->now_s > sent_at_s ? exchange->now_s - sent_at_s : sent_at_s - exchange->now_s;

    if (drift > tolerance) return NYA_HTTP_WEBHOOK_REPLAYED;

    return NYA_HTTP_WEBHOOK_ACCEPTED;
}

b8 nya_http_webhook_id(const NYA_HttpExchange* exchange, const NYA_HttpWebhook* webhook, char* out_id, u64 capacity) {
    nya_assert(exchange != nullptr && exchange->request != nullptr);
    nya_assert(webhook != nullptr);
    nya_assert(out_id != nullptr && capacity > 0);

    out_id[0] = '\0';

    if (webhook->id_header == nullptr) return false;

    NYA_ConstCString id = nya_http_request_header(exchange->request, webhook->id_header);
    if (id == nullptr || id[0] == '\0' || strlen(id) >= capacity) return false;

    (void)snprintf(out_id, capacity, "%s", id);

    return true;
}

NYA_ConstCString nya_http_webhook_verdict_text(NYA_HttpWebhookVerdict verdict) {
    switch (verdict) {
        case NYA_HTTP_WEBHOOK_ACCEPTED:     return "accepted";
        case NYA_HTTP_WEBHOOK_REPLAYED:     return "replayed";
        case NYA_HTTP_WEBHOOK_UNCONFIGURED: return "unconfigured";

        // a verdict nobody set reads as the refusal it is: the enum's zero is REFUSED for that reason.
        case NYA_HTTP_WEBHOOK_REFUSED:
        case NYA_HTTP_WEBHOOK_VERDICT_COUNT:
        default:                            return "refused";
    }
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_webhook_hex(char character, u8* out_value) {
    if (character >= '0' && character <= '9') {
        *out_value = (u8)(character - '0');
        return true;
    }

    // lower case only where a sender has a choice, since two spellings of one signature is one more thing two readers can disagree about; upper case is accepted because Discord sends it.
    if (character >= 'a' && character <= 'f') {
        *out_value = (u8)(character - 'a' + 10);
        return true;
    }

    if (character >= 'A' && character <= 'F') {
        *out_value = (u8)(character - 'A' + 10);
        return true;
    }

    return false;
}

b8 _nya_http_webhook_signature(NYA_ConstCString text, NYA_ConstCString prefix, u8* out, u64 size) {
    if (prefix != nullptr && prefix[0] != '\0') {
        u64 prefix_size = strlen(prefix);

        if (strncmp(text, prefix, prefix_size) != 0) return false;

        text += prefix_size;
    }

    // exactly the length the primitive produces: a short signature padded with zeroes would verify against a tag that happened to start the same way.
    if (strlen(text) != size * 2) return false;

    for (u64 index = 0; index < size; index++) {
        u8 high = 0;
        u8 low  = 0;

        if (!_nya_http_webhook_hex(text[index * 2], &high) || !_nya_http_webhook_hex(text[(index * 2) + 1], &low)) return false;

        out[index] = (u8)((high << 4) | low);
    }

    return true;
}

b8 _nya_http_webhook_timestamp(NYA_ConstCString text, u64* out_seconds) {
    *out_seconds = 0;

    if (text[0] == '\0') return false;

    // a plain count of seconds, which is what Discord and Stripe send.
    b8 digits = true;
    for (u64 index = 0; text[index] != '\0'; index++) {
        if (text[index] < '0' || text[index] > '9') digits = false;
    }

    if (digits) return nya_type_parse(NYA_TYPE_U64, (const u8*)text, strlen(text), out_seconds);

    // or a date, which is what Twitch sends. Parsed rather than pattern matched, so an offset is honoured.
    NYA_Instant instant  = { 0 };
    u64         position = 0;

    if (nya_instant_from_rfc3339((const u8*)text, strlen(text), &instant, &position) != NYA_TIME_PARSE_OK) return false;
    if (instant.ns < 0) return false;

    *out_seconds = (u64)(instant.ns / NYA_NS_PER_SECOND);

    return true;
}
