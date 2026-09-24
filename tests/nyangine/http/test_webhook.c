/**
 * Webhook verification, both schemes.
 *
 * The signatures are computed here with the same primitives the verifier uses, which proves the shape
 * rather than the primitive: SHA-256, HMAC and Ed25519 each have their own test against published
 * vectors. What this pins is everything around them — that the message is the bytes that arrived, that
 * a changed body or a changed id fails, that a signature of the wrong length fails rather than being
 * padded, and that a request signed correctly but sent yesterday is refused as a replay.
 **/

#include <string.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define SECRET_TEXT "a secret the sender and this server share"
#define BODY        "{\"event\":\"channel.follow\",\"user\":\"someone\"}"

#define MESSAGE_ID "3fb6cf36-bd1e-4f5f-a9a0-8e8ce88e7cba"
#define NOW_S      1758553200ULL

static const u8 SECRET[] = SECRET_TEXT;
#define SECRET_SIZE (sizeof(SECRET) - 1)

/** A request as the parser would have left it: headers, and the body exactly as it arrived. */
static void request_build(OUT NYA_HttpRequest* request, NYA_ConstCString body, const NYA_ConstCString headers[][2], u32 header_count) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_POST, .media_type = NYA_HTTP_MEDIA_JSON };

    (void)snprintf(request->path, sizeof(request->path), "%s", "/hooks/twitch");

    for (u32 index = 0; index < header_count && index < NYA_HTTP_MAX_HEADERS; index++) {
        (void)snprintf(request->headers[index].name, sizeof(request->headers[index].name), "%s", headers[index][0]);
        (void)snprintf(request->headers[index].value, sizeof(request->headers[index].value), "%s", headers[index][1]);
    }

    request->header_count = header_count;
    request->body_size    = strlen(body);

    memcpy(request->body, body, request->body_size);
}

/** Bytes as lower case hex, which is how every sender here spells a signature. */
static void to_hex(const u8* bytes, u64 size, OUT char* out) {
    for (u64 index = 0; index < size; index++) (void)snprintf(out + (index * 2), 3, "%02x", bytes[index]);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_webhook");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    char timestamp[32] = { 0 };
    (void)snprintf(timestamp, sizeof(timestamp), "%llu", (unsigned long long)NOW_S);

    // TEST: HMAC-SHA256, as Twitch EventSub and GitHub send it.
    {
        // the sender's message: the id, the timestamp and the body, in that order.
        char signed_over[512] = { 0 };
        (void)snprintf(signed_over, sizeof(signed_over), "%s%s%s", MESSAGE_ID, timestamp, BODY);

        NYA_CryptoSha256Digest tag = { 0 };
        nya_crypto_hmac_sha256(SECRET, SECRET_SIZE, (const u8*)signed_over, strlen(signed_over), &tag);

        char signature[NYA_HTTP_WEBHOOK_MAX_SIGNATURE] = { 0 };
        (void)snprintf(signature, sizeof(signature), "sha256=");
        to_hex(tag.bytes, sizeof(tag.bytes), signature + strlen("sha256="));

        const NYA_ConstCString headers[][2] = {
            { "twitch-eventsub-message-id",        MESSAGE_ID },
            { "twitch-eventsub-message-timestamp", timestamp  },
            { "twitch-eventsub-message-signature", signature  },
        };

        request_build(request, BODY, headers, nya_carray_length(headers));

        NYA_HttpExchange exchange = { .request = request, .arena = arena, .now_s = NOW_S };

        NYA_HttpWebhook twitch = {
            .scheme           = NYA_HTTP_WEBHOOK_HMAC_SHA256,
            .secret           = SECRET,
            .secret_size      = SECRET_SIZE,
            .signature_header = "Twitch-Eventsub-Message-Signature",
            .signature_prefix = "sha256=",
            .id_header        = "Twitch-Eventsub-Message-Id",
            .timestamp_header = "Twitch-Eventsub-Message-Timestamp",
        };

        nya_check(nya_http_webhook_verify(&exchange, &twitch) == NYA_HTTP_WEBHOOK_ACCEPTED, "a signed request is accepted");

        // every verdict says what it is, since a log line about a webhook is the only trace of one.
        nya_check(strcmp(nya_http_webhook_verdict_text(nya_http_webhook_verify(&exchange, &twitch)), "accepted") == 0, "and says so");
        nya_check(strcmp(nya_http_webhook_verdict_text(NYA_HTTP_WEBHOOK_REFUSED), "refused") == 0, "a refusal too");
        nya_check(strcmp(nya_http_webhook_verdict_text(NYA_HTTP_WEBHOOK_REPLAYED), "replayed") == 0, "and a replay");
        nya_check(strcmp(nya_http_webhook_verdict_text(NYA_HTTP_WEBHOOK_UNCONFIGURED), "unconfigured") == 0, "and a server that was not set up");

        char id[64] = { 0 };
        nya_check(nya_http_webhook_id(&exchange, &twitch, id, sizeof(id)) && strcmp(id, MESSAGE_ID) == 0, "and its id is readable, got '%s'", id);

        // one byte of the body, which is the whole point of verifying over what arrived.
        request->body[0] = '[';
        nya_check(nya_http_webhook_verify(&exchange, &twitch) == NYA_HTTP_WEBHOOK_REFUSED, "a changed body is refused");
        request->body[0] = '{';

        // the id is part of the message, so replaying a body under a different id fails too.
        (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "%s", "another-id");
        nya_check(nya_http_webhook_verify(&exchange, &twitch) == NYA_HTTP_WEBHOOK_REFUSED, "a changed id is refused");
        (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "%s", MESSAGE_ID);

        // the secret, which is the one thing a forger does not have.
        NYA_HttpWebhook wrong = twitch;
        wrong.secret          = (const u8*)"not the secret";
        wrong.secret_size     = strlen("not the secret");

        nya_check(nya_http_webhook_verify(&exchange, &wrong) == NYA_HTTP_WEBHOOK_REFUSED, "another secret is refused");

        // signed correctly, and sent a day ago: a capture being played back.
        NYA_HttpExchange later = exchange;
        later.now_s            = NOW_S + 86400;

        nya_check(nya_http_webhook_verify(&later, &twitch) == NYA_HTTP_WEBHOOK_REPLAYED, "a captured request is refused as a replay");

        NYA_HttpExchange edge = exchange;
        edge.now_s            = NOW_S + NYA_HTTP_WEBHOOK_TOLERANCE_S;

        nya_check(nya_http_webhook_verify(&edge, &twitch) == NYA_HTTP_WEBHOOK_ACCEPTED, "the window's own edge is still inside it");

        edge.now_s = NOW_S + NYA_HTTP_WEBHOOK_TOLERANCE_S + 1;
        nya_check(nya_http_webhook_verify(&edge, &twitch) == NYA_HTTP_WEBHOOK_REPLAYED, "and one second past it is not");

        // a clock that runs ahead of the sender is the same question the other way round.
        edge.now_s = NOW_S - NYA_HTTP_WEBHOOK_TOLERANCE_S - 1;
        nya_check(nya_http_webhook_verify(&edge, &twitch) == NYA_HTTP_WEBHOOK_REPLAYED, "and so is a timestamp from the future");
    }

    // TEST: every way a signature header can be wrong.
    {
        char signed_over[512] = { 0 };
        (void)snprintf(signed_over, sizeof(signed_over), "%s%s", timestamp, BODY);

        NYA_CryptoSha256Digest tag = { 0 };
        nya_crypto_hmac_sha256(SECRET, SECRET_SIZE, (const u8*)signed_over, strlen(signed_over), &tag);

        char hex[NYA_HTTP_WEBHOOK_MAX_SIGNATURE] = { 0 };
        to_hex(tag.bytes, sizeof(tag.bytes), hex);

        NYA_HttpWebhook sender = {
            .scheme           = NYA_HTTP_WEBHOOK_HMAC_SHA256,
            .secret           = SECRET,
            .secret_size      = SECRET_SIZE,
            .signature_header = "x-signature",
            .timestamp_header = "x-timestamp",
        };

        // no prefix and no id this time: the plainest shape a sender can have.
        const NYA_ConstCString headers[][2] = {
            { "x-timestamp", timestamp },
            { "x-signature", hex       },
        };

        request_build(request, BODY, headers, nya_carray_length(headers));

        NYA_HttpExchange exchange = { .request = request, .arena = arena, .now_s = NOW_S };
        nya_check(nya_http_webhook_verify(&exchange, &sender) == NYA_HTTP_WEBHOOK_ACCEPTED, "the plain shape verifies");

        static const NYA_ConstCString BROKEN[] = {
            "",                                                                   // nothing at all
            "sha256=",                                                            // a prefix and no signature
            "0011",                                                               // too short: not padded out
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00", // too long
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeef",    // an odd number of digits
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeefg",   // not hex
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddee f",   // a space in it
        };

        for (u32 index = 0; index < nya_carray_length(BROKEN); index++) {
            (void)snprintf(request->headers[1].value, sizeof(request->headers[1].value), "%s", BROKEN[index]);

            nya_check(nya_http_webhook_verify(&exchange, &sender) == NYA_HTTP_WEBHOOK_REFUSED, "'%s' was accepted", BROKEN[index]);
        }

        // and a signature that is right, under a prefix the sender did not send.
        (void)snprintf(request->headers[1].value, sizeof(request->headers[1].value), "%s", hex);

        NYA_HttpWebhook prefixed  = sender;
        prefixed.signature_prefix = "sha256=";

        nya_check(nya_http_webhook_verify(&exchange, &prefixed) == NYA_HTTP_WEBHOOK_REFUSED, "a missing prefix is refused");

        // a server that was never configured says so rather than refusing, which is a different bug.
        NYA_HttpWebhook unconfigured = sender;
        unconfigured.secret          = nullptr;
        unconfigured.secret_size     = 0;

        nya_check(nya_http_webhook_verify(&exchange, &unconfigured) == NYA_HTTP_WEBHOOK_UNCONFIGURED, "no secret is not a refusal");

        NYA_HttpWebhook headerless    = sender;
        headerless.timestamp_header   = nullptr;

        nya_check(nya_http_webhook_verify(&exchange, &headerless) == NYA_HTTP_WEBHOOK_UNCONFIGURED, "and neither is naming no timestamp header");

        // a request missing the headers the sender promised is a refusal, not a misconfiguration.
        request_build(request, BODY, headers, 1);
        nya_check(nya_http_webhook_verify(&exchange, &sender) == NYA_HTTP_WEBHOOK_REFUSED, "a request with no signature is refused");
    }

    // TEST: Ed25519 over the timestamp and the body, as Discord sends it.
    {
        NYA_CryptoKey32       seed = { 0 };
        NYA_CryptoSignKeyPair pair = { 0 };

        nya_check(nya_crypto_key_create(&seed).ok, "a seed came from the system");
        nya_crypto_sign_key_pair_from_seed(&seed, &pair);

        char signed_over[512] = { 0 };
        (void)snprintf(signed_over, sizeof(signed_over), "%s%s", timestamp, BODY);

        NYA_CryptoSignature signature = { 0 };
        nya_crypto_sign(&pair.secret_key, (const u8*)signed_over, strlen(signed_over), &signature);

        char hex[NYA_HTTP_WEBHOOK_MAX_SIGNATURE] = { 0 };
        to_hex(signature.bytes, sizeof(signature.bytes), hex);

        const NYA_ConstCString headers[][2] = {
            { "x-signature-timestamp", timestamp },
            { "x-signature-ed25519",   hex       },
        };

        request_build(request, BODY, headers, nya_carray_length(headers));

        NYA_HttpExchange exchange = { .request = request, .arena = arena, .now_s = NOW_S };

        NYA_HttpWebhook discord = {
            .scheme           = NYA_HTTP_WEBHOOK_ED25519,
            .public_key       = &pair.public_key,
            .signature_header = "X-Signature-Ed25519",
            .timestamp_header = "X-Signature-Timestamp",
        };

        nya_check(nya_http_webhook_verify(&exchange, &discord) == NYA_HTTP_WEBHOOK_ACCEPTED, "an interaction is accepted");

        // upper case hex, which is what some senders write.
        char upper[NYA_HTTP_WEBHOOK_MAX_SIGNATURE] = { 0 };
        for (u64 i = 0; hex[i] != '\0'; i++) upper[i] = (char)((hex[i] >= 'a' && hex[i] <= 'f') ? hex[i] - 32 : hex[i]);

        (void)snprintf(request->headers[1].value, sizeof(request->headers[1].value), "%s", upper);
        nya_check(nya_http_webhook_verify(&exchange, &discord) == NYA_HTTP_WEBHOOK_ACCEPTED, "in either case");

        (void)snprintf(request->headers[1].value, sizeof(request->headers[1].value), "%s", hex);

        // a signature from another key, which is the forgery this exists to refuse.
        NYA_CryptoKey32       other_seed = { 0 };
        NYA_CryptoSignKeyPair other      = { 0 };

        nya_check(nya_crypto_key_create(&other_seed).ok, "another seed");
        nya_crypto_sign_key_pair_from_seed(&other_seed, &other);

        NYA_HttpWebhook impostor = discord;
        impostor.public_key      = &other.public_key;

        nya_check(nya_http_webhook_verify(&exchange, &impostor) == NYA_HTTP_WEBHOOK_REFUSED, "another key is refused");

        // the timestamp is part of what was signed, so moving it breaks the signature rather than
        // sliding the window.
        char ahead[32] = { 0 };
        (void)snprintf(ahead, sizeof(ahead), "%llu", (unsigned long long)(NOW_S + 10));
        (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "%s", ahead);

        nya_check(nya_http_webhook_verify(&exchange, &discord) == NYA_HTTP_WEBHOOK_REFUSED, "a moved timestamp is refused");
    }

    // TEST: an RFC 3339 timestamp, which is what Twitch actually sends.
    {
        u8  dated[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
        u32 written                       = nya_instant_to_rfc3339((NYA_Instant){ .ns = (s64)NOW_S * NYA_NS_PER_SECOND }, dated, sizeof(dated));

        nya_check(written > 0, "the timestamp rendered");

        char signed_over[512] = { 0 };
        (void)snprintf(signed_over, sizeof(signed_over), "%s%s%s", MESSAGE_ID, (const char*)dated, BODY);

        NYA_CryptoSha256Digest tag = { 0 };
        nya_crypto_hmac_sha256(SECRET, SECRET_SIZE, (const u8*)signed_over, strlen(signed_over), &tag);

        char signature[NYA_HTTP_WEBHOOK_MAX_SIGNATURE] = { 0 };
        (void)snprintf(signature, sizeof(signature), "sha256=");
        to_hex(tag.bytes, sizeof(tag.bytes), signature + strlen("sha256="));

        const NYA_ConstCString headers[][2] = {
            { "twitch-eventsub-message-id",        MESSAGE_ID          },
            { "twitch-eventsub-message-timestamp", (const char*)dated  },
            { "twitch-eventsub-message-signature", signature           },
        };

        request_build(request, BODY, headers, nya_carray_length(headers));

        NYA_HttpExchange exchange = { .request = request, .arena = arena, .now_s = NOW_S };

        NYA_HttpWebhook twitch = {
            .scheme           = NYA_HTTP_WEBHOOK_HMAC_SHA256,
            .secret           = SECRET,
            .secret_size      = SECRET_SIZE,
            .signature_header = "Twitch-Eventsub-Message-Signature",
            .signature_prefix = "sha256=",
            .id_header        = "Twitch-Eventsub-Message-Id",
            .timestamp_header = "Twitch-Eventsub-Message-Timestamp",
        };

        nya_check(nya_http_webhook_verify(&exchange, &twitch) == NYA_HTTP_WEBHOOK_ACCEPTED, "a dated timestamp is read as one");

        NYA_HttpExchange later = exchange;
        later.now_s            = NOW_S + 86400;

        nya_check(nya_http_webhook_verify(&later, &twitch) == NYA_HTTP_WEBHOOK_REPLAYED, "and is still what bounds a replay");
    }

    printf("PASSED: http webhook\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
