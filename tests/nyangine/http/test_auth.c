/**
 * JWT over HMAC-SHA256, and the second factor challenge.
 *
 * The round trip is one test; the rest are the ways a token can be wrong. `alg: none`, a swapped
 * algorithm, a flipped signature bit, a token signed with a different secret, an expired one and one
 * from the future all have to be refused, and refused without the claims ever being believed.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Long enough to be accepted, and obviously not a real secret. */
static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
static const u8 OTHER[]  = "fedcba9876543210fedcba9876543210";

#define SECRET_SIZE (sizeof(SECRET) - 1)
#define OTHER_SIZE  (sizeof(OTHER) - 1)

/** A fixed "now", so nothing here depends on the wall clock. */
#define NOW_S 1700000000ULL

/** A request carrying `authorization`, for the bearer extractor. */
static void request_with_authorization(OUT NYA_HttpRequest* request, NYA_ConstCString value) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET, .path = "/", .header_count = 1 };

    (void)snprintf(request->headers[0].name, sizeof(request->headers[0].name), "authorization");
    (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "%s", value);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_auth");
    defer      nya_arena_destroy(arena);

    char token[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a token round trips, claims and all.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpIdentity signed_in = {
            .scope        = NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE,
            .issued_at_s  = NOW_S,
            .expires_at_s = NOW_S + 3600,
        };

        (void)snprintf(signed_in.subject, sizeof(signed_in.subject), "luca");

        nya_assert(nya_http_jwt_encode(&signed_in, SECRET, SECRET_SIZE, token, sizeof(token)).ok);

        // three parts, and the alphabet is base64url: no padding and no '+' or '/'.
        nya_assert(nya_string_count(nya_string_from(arena, token), ".") == 2);
        nya_assert(!nya_string_contains(token, "="));
        nya_assert(!nya_string_contains(token, "+"));
        nya_assert(!nya_string_contains(token, "/"));

        NYA_HttpIdentity verified = { 0 };

        nya_assert(nya_http_jwt_decode(arena, token, strlen(token), SECRET, SECRET_SIZE, NOW_S, &verified).ok);

        nya_assert(nya_string_equals(verified.subject, "luca"));
        nya_assert(verified.scope == (NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE));
        nya_assert(verified.issued_at_s == NOW_S);
        nya_assert(verified.expires_at_s == NOW_S + 3600);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a scope is carried in full or not at all.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpIdentity identity = { .scope = NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE };

        nya_assert(nya_http_scope_contains(&identity, NYA_HTTP_SCOPE_NONE), "everyone carries the empty scope");
        nya_assert(nya_http_scope_contains(&identity, NYA_HTTP_SCOPE_READ));
        nya_assert(nya_http_scope_contains(&identity, NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_WRITE));

        nya_assert(!nya_http_scope_contains(&identity, NYA_HTTP_SCOPE_ADMIN));
        nya_assert(!nya_http_scope_contains(&identity, NYA_HTTP_SCOPE_READ | NYA_HTTP_SCOPE_ADMIN), "half a scope is not the scope");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: every way a token can fail to be this server's.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpIdentity identity = { .issued_at_s = NOW_S, .expires_at_s = NOW_S + 60, .scope = NYA_HTTP_SCOPE_READ };
        (void)snprintf(identity.subject, sizeof(identity.subject), "luca");

        nya_assert(nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, token, sizeof(token)).ok);

        NYA_HttpIdentity verified = { 0 };

        // a different secret.
        NYA_Error wrong_secret = nya_http_jwt_decode(arena, token, strlen(token), OTHER, OTHER_SIZE, NOW_S, &verified);
        nya_assert(wrong_secret.kind == NYA_ERROR_PERMISSION_DENIED);
        nya_assert(verified.subject[0] == '\0', "a refused token leaves no claims behind");

        // one character of the signature.
        NYA_String* tampered = nya_string_from(arena, token);
        tampered->items[tampered->length - 5] = tampered->items[tampered->length - 5] == 'A' ? 'B' : 'A';

        nya_assert(nya_http_jwt_decode(arena, (const char*)tampered->items, tampered->length, SECRET, SECRET_SIZE, NOW_S, &verified).kind
                   == NYA_ERROR_PERMISSION_DENIED);

        /*
         * And the last character, which is the malleability case: its low bits encode nothing, so an
         * encoder that left them set would give one signature two spellings. Refused rather than
         * ignored, so a token has exactly one form.
         */
        NYA_String* spare = nya_string_from(arena, token);
        spare->items[spare->length - 1] = spare->items[spare->length - 1] == 'A' ? 'B' : 'A';

        nya_assert(!nya_http_jwt_decode(arena, (const char*)spare->items, spare->length, SECRET, SECRET_SIZE, NOW_S, &verified).ok);

        // one bit of the payload, which the signature covers.
        NYA_String* edited = nya_string_from(arena, token);
        edited->items[20]  = edited->items[20] == 'A' ? 'B' : 'A';

        nya_assert(!nya_http_jwt_decode(arena, (const char*)edited->items, edited->length, SECRET, SECRET_SIZE, NOW_S, &verified).ok);

        // expired, and not yet valid.
        nya_assert(nya_http_jwt_decode(arena, token, strlen(token), SECRET, SECRET_SIZE, NOW_S + 61, &verified).kind == NYA_ERROR_TIMEOUT);
        nya_assert(nya_http_jwt_decode(arena, token, strlen(token), SECRET, SECRET_SIZE, NOW_S - 600, &verified).kind == NYA_ERROR_TIMEOUT);

        // and inside the allowance for a clock a little behind.
        nya_assert(nya_http_jwt_decode(arena, token, strlen(token), SECRET, SECRET_SIZE, NOW_S - 30, &verified).ok);

        // shapes that are not tokens.
        nya_assert(!nya_http_jwt_decode(arena, "", 0, SECRET, SECRET_SIZE, NOW_S, &verified).ok);
        nya_assert(!nya_http_jwt_decode(arena, "a.b", 3, SECRET, SECRET_SIZE, NOW_S, &verified).ok);
        nya_assert(!nya_http_jwt_decode(arena, "a.b.c.d", 7, SECRET, SECRET_SIZE, NOW_S, &verified).ok);
        nya_assert(!nya_http_jwt_decode(arena, "..", 2, SECRET, SECRET_SIZE, NOW_S, &verified).ok);
        nya_assert(!nya_http_jwt_decode(arena, "!!.!!.!!", 8, SECRET, SECRET_SIZE, NOW_S, &verified).ok);

        // a secret this server will not sign with is refused before anything is read.
        nya_assert(nya_http_jwt_decode(arena, token, strlen(token), SECRET, 4, NOW_S, &verified).kind == NYA_ERROR_INVALID_ARGUMENT);
        nya_assert(nya_http_jwt_encode(&identity, SECRET, 4, token, sizeof(token)).kind == NYA_ERROR_INVALID_ARGUMENT);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: `alg: none` and the downgrade family.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpIdentity verified = { 0 };

        /*
         * The classic: a header claiming no algorithm and an empty signature. It is refused by the
         * signature check, before `alg` is even looked at, because an empty signature is not thirty
         * two bytes.
         */
        NYA_ConstCString none = "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJzdWIiOiJsdWNhIiwic2NwIjo3LCJpYXQiOjAsImV4cCI6OTk5OTk5OTk5OX0.";

        nya_assert(!nya_http_jwt_decode(arena, none, strlen(none), SECRET, SECRET_SIZE, NOW_S, &verified).ok);
        nya_assert(verified.subject[0] == '\0');

        /*
         * And the subtler one: a header that is not ours, signed correctly with our secret. It still
         * fails, because the header is compared whole rather than parsed and asked what it wants.
         */
        NYA_ConstCString header  = "{\"alg\":\"HS512\",\"typ\":\"JWT\"}";
        NYA_ConstCString payload = "{\"sub\":\"luca\",\"scp\":7,\"iat\":0,\"exp\":9999999999}";

        NYA_String* encoded_header  = nya_string_create(arena);
        NYA_String* encoded_payload = nya_string_create(arena);

        nya_base64_encode(encoded_header, (const u8*)header, strlen(header));
        nya_base64_encode(encoded_payload, (const u8*)payload, strlen(payload));

        // base64 to base64url, which is the alphabet a JWS uses.
        nya_string_remove(encoded_header, "=");
        nya_string_remove(encoded_payload, "=");
        nya_string_replace(encoded_header, "+", "-");
        nya_string_replace(encoded_payload, "+", "-");
        nya_string_replace(encoded_header, "/", "_");
        nya_string_replace(encoded_payload, "/", "_");

        NYA_String* signing_input = nya_string_sprintf(arena, "%s.%s", nya_string_to_cstring(arena, encoded_header),
                                                       nya_string_to_cstring(arena, encoded_payload));

        u8 tag[NYA_SHA256_BYTES] = { 0 };
        nya_hmac_sha256(SECRET, SECRET_SIZE, (const u8*)signing_input->items, signing_input->length, tag);

        NYA_String* encoded_tag = nya_string_create(arena);
        nya_base64_encode(encoded_tag, tag, sizeof(tag));
        nya_string_remove(encoded_tag, "=");
        nya_string_replace(encoded_tag, "+", "-");
        nya_string_replace(encoded_tag, "/", "_");

        NYA_String* forged = nya_string_sprintf(arena, "%s.%s", nya_string_to_cstring(arena, signing_input), nya_string_to_cstring(arena, encoded_tag));

        NYA_Error refused = nya_http_jwt_decode(arena, (const char*)forged->items, forged->length, SECRET, SECRET_SIZE, NOW_S, &verified);

        nya_assert(refused.kind == NYA_ERROR_PERMISSION_DENIED, "a correctly signed token with a header we did not write is still not ours");
        nya_assert(verified.subject[0] == '\0');
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a subject that could inject into the payload is not a subject.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpIdentity identity = { .issued_at_s = NOW_S, .expires_at_s = NOW_S + 60 };

        (void)snprintf(identity.subject, sizeof(identity.subject), "a\",\"scp\":7,\"x\":\"");
        nya_assert(!nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, token, sizeof(token)).ok, "a quote in a subject would be a JSON injection");

        (void)snprintf(identity.subject, sizeof(identity.subject), "%s", "");
        nya_assert(!nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, token, sizeof(token)).ok, "a token needs a subject");

        (void)snprintf(identity.subject, sizeof(identity.subject), "tool.one_two-three@host");
        nya_assert(nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, token, sizeof(token)).ok);

        // an expiry that is not after the issue time is a token that was never valid.
        identity.expires_at_s = identity.issued_at_s;
        nya_assert(!nya_http_jwt_encode(&identity, SECRET, SECRET_SIZE, token, sizeof(token)).ok);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: pulling a bearer token out of a request.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
        nya_assert(request != nullptr);

        const char* found = nullptr;
        u64         size  = 0;

        request_with_authorization(request, "Bearer abc.def.ghi");
        nya_assert(nya_http_bearer_token(request, &found, &size));
        nya_assert(size == 11 && memcmp(found, "abc.def.ghi", size) == 0);

        // the scheme is matched without regard to case, as the grammar says.
        request_with_authorization(request, "bearer abc.def.ghi");
        nya_assert(nya_http_bearer_token(request, &found, &size));

        request_with_authorization(request, "Basic dXNlcjpwYXNz");
        nya_assert(!nya_http_bearer_token(request, &found, &size), "a scheme this server does not speak is not a token");

        request_with_authorization(request, "Bearer");
        nya_assert(!nya_http_bearer_token(request, &found, &size));

        request_with_authorization(request, "Bearer   ");
        nya_assert(!nya_http_bearer_token(request, &found, &size), "a bearer of nothing is not a bearer");

        *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET };
        nya_assert(!nya_http_bearer_token(request, &found, &size));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the second factor challenge, which is stateless and windowed.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u8 challenge[NYA_SHA256_BYTES] = { 0 };

        nya_assert(nya_http_challenge_create("luca", SECRET, SECRET_SIZE, NOW_S, challenge).ok);

        nya_assert(nya_http_challenge_verify("luca", SECRET, SECRET_SIZE, NOW_S, challenge));

        // one window later it is still accepted, which is what lets a challenge issued just before a
        // boundary be answered just after one.
        nya_assert(nya_http_challenge_verify("luca", SECRET, SECRET_SIZE, NOW_S + NYA_HTTP_CHALLENGE_WINDOW_S, challenge));

        // two windows later it is not.
        nya_assert(!nya_http_challenge_verify("luca", SECRET, SECRET_SIZE, NOW_S + NYA_HTTP_CHALLENGE_WINDOW_S * 3, challenge));

        // and it is one subject's, under one secret.
        nya_assert(!nya_http_challenge_verify("other", SECRET, SECRET_SIZE, NOW_S, challenge));
        nya_assert(!nya_http_challenge_verify("luca", OTHER, OTHER_SIZE, NOW_S, challenge));

        nya_assert(!nya_http_challenge_create("luca", SECRET, 4, NOW_S, challenge).ok);
        nya_assert(!nya_http_challenge_create("", SECRET, SECRET_SIZE, NOW_S, challenge).ok);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the verifier seam is empty until something installs one.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(nya_http_second_factor() == nullptr, "the engine ships with no signature verifier");

        nya_http_second_factor_set(nullptr);
        nya_assert(nya_http_second_factor() == nullptr);
    }

    printf("PASSED: http auth\n");

    return EXIT_SUCCESS;
}
