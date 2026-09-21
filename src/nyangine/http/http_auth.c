#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_object.h"
#include "nyangine/http/http_auth.h"
#include "nyangine/serde/serde.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The only header this server signs and the only one it accepts. Compared whole; see the file note. */
#define _NYA_HTTP_JWT_HEADER "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"

/**
 * Seconds a token may claim to have been issued in the future.
 *
 * Zero would make a token minted on a machine whose clock is a second ahead unusable, which is a
 * support ticket rather than a security property. A minute is the usual allowance.
 * */
#define _NYA_HTTP_JWT_SKEW_S 60

/** Longest JSON payload this encodes or will decode. The claims are four short fields. */
#define _NYA_HTTP_JWT_PAYLOAD_BYTES 256

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What nya_http_second_factor_set installed. Null is the shipped state; see the file note. */
NYA_INTERNAL NYA_HttpSecondFactorFn _NYA_HTTP_SECOND_FACTOR = nullptr;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * base64url without padding, which is what a JWS uses.
 *
 * Its own rather than base_base64.c's: that one is the padded, '+' and '/' alphabet, and a token in
 * that alphabet is not a token. Two alphabets in one function would be a flag nobody remembers to set.
 * */
NYA_INTERNAL b8 _nya_http_base64url_encode(const u8* data, u64 size, OUT char* out_text, u64 capacity, OUT u64* out_size);

/** The inverse. False for a character outside the alphabet, for padding, and for a length that cannot be one. */
NYA_INTERNAL b8 _nya_http_base64url_decode(const char* text, u64 size, OUT u8* out_data, u64 capacity, OUT u64* out_size);

/** The value of one base64url character, or 64 for anything else. */
NYA_INTERNAL u8 _nya_http_base64url_value(char character) __attr_no_discard;

/**
 * Whether `subject` is one this server will put in a token: one to NYA_HTTP_MAX_SUBJECT - 1 characters
 * of `A-Z a-z 0-9 . _ - @`.
 *
 * Narrow on purpose. Nothing here escapes JSON, because nothing here ever has to: a subject that could
 * contain a quote would be a JSON injection into the payload, and the fix is a subject that cannot.
 * */
NYA_INTERNAL b8 _nya_http_subject_is_valid(NYA_ConstCString subject, u64 size) __attr_no_discard;

/** One field of a parsed payload, as a non-negative integer. False when it is absent or is not one. */
NYA_INTERNAL b8 _nya_http_claim_u64(const NYA_Object* payload, NYA_ConstCString key, OUT u64* out_value);

/** The HMAC of `subject` and the window `now_s` falls in, which is what a challenge is. */
NYA_INTERNAL void
_nya_http_challenge_for_window(NYA_ConstCString subject, const u8* secret, u64 secret_size, u64 window, OUT u8 out_challenge[NYA_SHA256_BYTES]);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * TOKENS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_http_jwt_encode(const NYA_HttpIdentity* identity, const u8* secret, u64 secret_size, char* out_token, u64 capacity) {
    nya_assert(identity != nullptr);
    nya_assert(out_token != nullptr);
    nya_assert(capacity > 0);

    out_token[0] = '\0';

    if (secret == nullptr || secret_size < NYA_HTTP_MIN_SECRET_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a signing secret is at least %d bytes", NYA_HTTP_MIN_SECRET_BYTES);
    }

    u64 subject_size = 0;
    while (subject_size < NYA_HTTP_MAX_SUBJECT && identity->subject[subject_size] != '\0') subject_size++;

    if (!_nya_http_subject_is_valid(identity->subject, subject_size)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a subject is 1 to %d characters of A-Z a-z 0-9 . _ - @", NYA_HTTP_MAX_SUBJECT - 1);
    }

    if (identity->expires_at_s <= identity->issued_at_s) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a token has to expire after it was issued");
    }

    char payload[_NYA_HTTP_JWT_PAYLOAD_BYTES] = { 0 };

    s32 payload_size = snprintf(
        payload,
        sizeof(payload),
        "{\"sub\":\"%s\",\"scp\":%u,\"iat\":%llu,\"exp\":%llu}",
        identity->subject,
        (u32)identity->scope,
        (unsigned long long)identity->issued_at_s,
        (unsigned long long)identity->expires_at_s
    );

    if (payload_size < 0 || (u64)payload_size >= sizeof(payload)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the claims do not fit a payload");

    char signing_input[NYA_HTTP_MAX_TOKEN_BYTES] = { 0 };
    u64  signing_size                            = 0;

    if (!_nya_http_base64url_encode(
            (const u8*)_NYA_HTTP_JWT_HEADER,
            sizeof(_NYA_HTTP_JWT_HEADER) - 1,
            signing_input,
            sizeof(signing_input),
            &signing_size
        )) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the token does not fit");
    }

    if (signing_size + 1 >= sizeof(signing_input)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the token does not fit");

    signing_input[signing_size++] = '.';

    u64 encoded = 0;
    if (!_nya_http_base64url_encode(
            (const u8*)payload,
            (u64)payload_size,
            signing_input + signing_size,
            sizeof(signing_input) - signing_size,
            &encoded
        )) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the token does not fit");
    }

    signing_size += encoded;

    u8 tag[NYA_SHA256_BYTES] = { 0 };
    nya_hmac_sha256(secret, secret_size, (const u8*)signing_input, signing_size, tag);

    if (signing_size + 1 >= capacity) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the token does not fit the caller's buffer");

    memcpy(out_token, signing_input, signing_size);
    out_token[signing_size] = '.';

    u64 tag_size = 0;
    if (!_nya_http_base64url_encode(tag, sizeof(tag), out_token + signing_size + 1, capacity - signing_size - 1, &tag_size)) {
        out_token[0] = '\0';
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the token does not fit the caller's buffer");
    }

    out_token[signing_size + 1 + tag_size] = '\0';

    return NYA_OK;
}

NYA_Error
nya_http_jwt_decode(NYA_Arena* arena, const char* token, u64 size, const u8* secret, u64 secret_size, u64 now_s, NYA_HttpIdentity* out_identity) {
    nya_assert(arena != nullptr);
    nya_assert(out_identity != nullptr);

    *out_identity = (NYA_HttpIdentity){ 0 };

    if (secret == nullptr || secret_size < NYA_HTTP_MIN_SECRET_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a signing secret is at least %d bytes", NYA_HTTP_MIN_SECRET_BYTES);
    }

    // the length bound first, before anything walks the bytes.
    if (token == nullptr || size == 0 || size >= NYA_HTTP_MAX_TOKEN_BYTES) return nya_error(NYA_ERROR_PARSE, "not a token this server will read");

    u64 first = 0;
    while (first < size && token[first] != '.') first++;

    if (first == 0 || first >= size) return nya_error(NYA_ERROR_PARSE, "a token has three parts");

    u64 second = first + 1;
    while (second < size && token[second] != '.') second++;

    if (second >= size || second == first + 1 || second + 1 >= size) return nya_error(NYA_ERROR_PARSE, "a token has three parts");

    for (u64 index = second + 1; index < size; index++) {
        if (token[index] == '.') return nya_error(NYA_ERROR_PARSE, "a token has three parts");
    }

    /*
     * The signature, before the header and before the payload. A token that does not verify never has
     * its claims parsed, so a forged one cannot reach a JSON parser at all.
     */
    u8  signature[NYA_SHA256_BYTES + 4] = { 0 };
    u64 signature_size                  = 0;

    if (!_nya_http_base64url_decode(token + second + 1, size - second - 1, signature, sizeof(signature), &signature_size)) {
        return nya_error(NYA_ERROR_PARSE, "the signature is not base64url");
    }

    if (signature_size != NYA_SHA256_BYTES) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the signature is the wrong length");

    u8 expected[NYA_SHA256_BYTES] = { 0 };
    nya_hmac_sha256(secret, secret_size, (const u8*)token, second, expected);

    if (!nya_hash_equals_constant_time(signature, expected, NYA_SHA256_BYTES)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the signature does not match");
    }

    /*
     * `alg`, whole and compared against one value. Reading the algorithm out of the token and then
     * trusting it is the `alg: none` bug, and accepting anything but HS256 here is the downgrade one.
     */
    u8  header[_NYA_HTTP_JWT_PAYLOAD_BYTES] = { 0 };
    u64 header_size                         = 0;

    if (!_nya_http_base64url_decode(token, first, header, sizeof(header), &header_size)) {
        return nya_error(NYA_ERROR_PARSE, "the header is not base64url");
    }

    if (header_size != sizeof(_NYA_HTTP_JWT_HEADER) - 1 || memcmp(header, _NYA_HTTP_JWT_HEADER, header_size) != 0) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "this server only accepts HS256 tokens");
    }

    u8  payload[_NYA_HTTP_JWT_PAYLOAD_BYTES] = { 0 };
    u64 payload_size                         = 0;

    if (!_nya_http_base64url_decode(token + first + 1, second - first - 1, payload, sizeof(payload), &payload_size)) {
        return nya_error(NYA_ERROR_PARSE, "the payload is not base64url");
    }

    NYA_Object* claims = nullptr;
    NYA_TRY(nya_deserialize(arena, payload, payload_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &claims));

    if (claims == nullptr) return nya_error(NYA_ERROR_PARSE, "the payload is not a JSON object");

    NYA_Value* subject = nya_object_get(claims, "sub");

    if (subject == nullptr || subject->type != NYA_TYPE_STRING || subject->as_string == nullptr) {
        return nya_error(NYA_ERROR_PARSE, "the payload has no 'sub'");
    }

    u64 subject_size = strlen(subject->as_string);

    if (!_nya_http_subject_is_valid(subject->as_string, subject_size)) return nya_error(NYA_ERROR_PARSE, "'sub' is not a subject");

    u64 scope        = 0;
    u64 issued_at_s  = 0;
    u64 expires_at_s = 0;

    if (!_nya_http_claim_u64(claims, "scp", &scope) || !_nya_http_claim_u64(claims, "iat", &issued_at_s) ||
        !_nya_http_claim_u64(claims, "exp", &expires_at_s)) {
        return nya_error(NYA_ERROR_PARSE, "the payload needs 'scp', 'iat' and 'exp'");
    }

    if (scope > U32_MAX) return nya_error(NYA_ERROR_PARSE, "'scp' is not a scope");

    if (expires_at_s <= now_s) return nya_error(NYA_ERROR_TIMEOUT, "the token has expired");

    if (issued_at_s > now_s + _NYA_HTTP_JWT_SKEW_S) return nya_error(NYA_ERROR_TIMEOUT, "the token is not valid yet");

    memcpy(out_identity->subject, subject->as_string, subject_size);
    out_identity->subject[subject_size] = '\0';

    out_identity->scope        = (NYA_HttpScope)scope;
    out_identity->issued_at_s  = issued_at_s;
    out_identity->expires_at_s = expires_at_s;

    return NYA_OK;
}

b8 nya_http_bearer_token(const NYA_HttpRequest* request, const char** out_token, u64* out_size) {
    nya_assert(out_token != nullptr);
    nya_assert(out_size != nullptr);

    *out_token = nullptr;
    *out_size  = 0;

    NYA_ConstCString authorization = nya_http_request_header(request, "authorization");
    if (authorization == nullptr) return false;

    u64 size = strlen(authorization);

    if (size < 7 || !_nya_http_equals_ignore_case(authorization, 6, "Bearer") || authorization[6] != ' ') return false;

    u64 start = 7;
    while (start < size && authorization[start] == ' ') start++;

    if (start >= size || size - start >= NYA_HTTP_MAX_TOKEN_BYTES) return false;

    *out_token = authorization + start;
    *out_size  = size - start;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────
 * SCOPES
 * ─────────────────────────────────────────────────────────
 */

b8 nya_http_scope_contains(const NYA_HttpIdentity* identity, NYA_HttpScope required) {
    nya_assert(identity != nullptr);

    return ((u32)identity->scope & (u32)required) == (u32)required;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE SECOND FACTOR
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_http_challenge_create(NYA_ConstCString subject, const u8* secret, u64 secret_size, u64 now_s, u8 out_challenge[NYA_SHA256_BYTES]) {
    nya_assert(out_challenge != nullptr);

    memset(out_challenge, 0, NYA_SHA256_BYTES);

    if (secret == nullptr || secret_size < NYA_HTTP_MIN_SECRET_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a challenge secret is at least %d bytes", NYA_HTTP_MIN_SECRET_BYTES);
    }

    if (subject == nullptr || !_nya_http_subject_is_valid(subject, strlen(subject))) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a challenge needs a subject");
    }

    _nya_http_challenge_for_window(subject, secret, secret_size, now_s / NYA_HTTP_CHALLENGE_WINDOW_S, out_challenge);

    return NYA_OK;
}

b8 nya_http_challenge_verify(NYA_ConstCString subject, const u8* secret, u64 secret_size, u64 now_s, const u8 challenge[NYA_SHA256_BYTES]) {
    if (challenge == nullptr || secret == nullptr || secret_size < NYA_HTTP_MIN_SECRET_BYTES) return false;
    if (subject == nullptr || !_nya_http_subject_is_valid(subject, strlen(subject))) return false;

    u64 window = now_s / NYA_HTTP_CHALLENGE_WINDOW_S;

    /*
     * This window and the one before it. Both are always computed and both comparisons always run, so
     * how long this takes says nothing about which one matched or whether either did.
     */
    u8 current[NYA_SHA256_BYTES]  = { 0 };
    u8 previous[NYA_SHA256_BYTES] = { 0 };

    _nya_http_challenge_for_window(subject, secret, secret_size, window, current);
    _nya_http_challenge_for_window(subject, secret, secret_size, window > 0 ? window - 1 : window, previous);

    b8 matches_current  = nya_hash_equals_constant_time(challenge, current, NYA_SHA256_BYTES);
    b8 matches_previous = nya_hash_equals_constant_time(challenge, previous, NYA_SHA256_BYTES);

    return matches_current || matches_previous;
}

void nya_http_second_factor_set(NYA_HttpSecondFactorFn verify) {
    _NYA_HTTP_SECOND_FACTOR = verify;
}

NYA_HttpSecondFactorFn nya_http_second_factor(void) {
    return _NYA_HTTP_SECOND_FACTOR;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_http_base64url_encode(const u8* data, u64 size, char* out_text, u64 capacity, u64* out_size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    nya_assert(capacity > 0);

    *out_size   = 0;
    out_text[0] = '\0';

    // three bytes become four characters, and a trailing one or two become two or three.
    u64 encoded = (size / 3) * 4 + (size % 3 == 0 ? 0 : size % 3 + 1);

    if (encoded + 1 > capacity) return false;

    u64 written = 0;

    for (u64 index = 0; index < size; index += 3) {
        u64 remaining = size - index;

        u32 chunk = (u32)data[index] << 16;
        if (remaining > 1) chunk |= (u32)data[index + 1] << 8;
        if (remaining > 2) chunk |= (u32)data[index + 2];

        out_text[written++] = alphabet[(chunk >> 18) & 0x3fu];
        out_text[written++] = alphabet[(chunk >> 12) & 0x3fu];

        if (remaining > 1) out_text[written++] = alphabet[(chunk >> 6) & 0x3fu];
        if (remaining > 2) out_text[written++] = alphabet[chunk & 0x3fu];
    }

    out_text[written] = '\0';
    *out_size         = written;

    return true;
}

b8 _nya_http_base64url_decode(const char* text, u64 size, u8* out_data, u64 capacity, u64* out_size) {
    *out_size = 0;

    // a base64 group is two, three or four characters; one leftover character encodes nothing and is
    // the shape a truncated token has.
    if (size == 0 || size % 4 == 1) return false;

    u64 decoded = (size / 4) * 3 + (size % 4 == 0 ? 0 : size % 4 - 1);

    if (decoded > capacity) return false;

    u64 written = 0;

    for (u64 index = 0; index < size; index += 4) {
        u64 remaining = size - index;

        u8  values[4] = { 0, 0, 0, 0 };
        u64 group     = remaining < 4 ? remaining : 4;

        for (u64 offset = 0; offset < group; offset++) {
            values[offset] = _nya_http_base64url_value(text[index + offset]);
            if (values[offset] == 64) return false;
        }

        u32 chunk = ((u32)values[0] << 18) | ((u32)values[1] << 12) | ((u32)values[2] << 6) | (u32)values[3];

        out_data[written++] = (u8)((chunk >> 16) & 0xffu);
        if (group > 2) out_data[written++] = (u8)((chunk >> 8) & 0xffu);
        if (group > 3) out_data[written++] = (u8)(chunk & 0xffu);
    }

    *out_size = written;

    return true;
}

u8 _nya_http_base64url_value(char character) {
    if (character >= 'A' && character <= 'Z') return (u8)(character - 'A');
    if (character >= 'a' && character <= 'z') return (u8)(character - 'a' + 26);
    if (character >= '0' && character <= '9') return (u8)(character - '0' + 52);
    if (character == '-') return 62;
    if (character == '_') return 63;

    return 64;
}

b8 _nya_http_subject_is_valid(NYA_ConstCString subject, u64 size) {
    if (subject == nullptr || size == 0 || size >= NYA_HTTP_MAX_SUBJECT) return false;

    for (u64 index = 0; index < size; index++) {
        char character = subject[index];

        if (character >= 'a' && character <= 'z') continue;
        if (character >= 'A' && character <= 'Z') continue;
        if (character >= '0' && character <= '9') continue;
        if (character == '.' || character == '_' || character == '-' || character == '@') continue;

        return false;
    }

    return true;
}

b8 _nya_http_claim_u64(const NYA_Object* payload, NYA_ConstCString key, u64* out_value) {
    *out_value = 0;

    NYA_Value* value = nya_object_get(payload, (NYA_CString)key);
    if (value == nullptr) return false;

    // serde reads every JSON integer as s64, so a negative claim is a number that parsed and a value
    // this has no meaning for. Refused rather than cast, which would make -1 an enormous scope.
    if (value->type != NYA_TYPE_S64 || value->as_s64 < 0) return false;

    *out_value = (u64)value->as_s64;

    return true;
}

void _nya_http_challenge_for_window(NYA_ConstCString subject, const u8* secret, u64 secret_size, u64 window, u8 out_challenge[NYA_SHA256_BYTES]) {
    // the window as text beside the subject, separated by a byte the subject alphabet excludes, so no
    // pair of (subject, window) can be written two ways.
    char message[NYA_HTTP_MAX_SUBJECT + 32] = { 0 };

    s32 size = snprintf(message, sizeof(message), "%s:%llu", subject, (unsigned long long)window);

    nya_assert(size > 0 && (u64)size < sizeof(message), "a challenge message is bounded by the subject's own bound");

    nya_hmac_sha256(secret, secret_size, (const u8*)message, (u64)size, out_challenge);
}
