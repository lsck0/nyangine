#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_url.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/os/os_random.h"
#include "nyangine/plugins/oidc/oidc.h"
#include "nyangine/serde/serde.h"

// ───────────────────────────────────── PRIVATE TYPES ─────────────────────────────────────

/**
 * One cached key: the `kid` it answers to, which family it is, and the key itself.
 *
 * The family is kept because it is half of the check that matters: a token whose header says RS256
 * must be verified with an RSA key and an ES256 one with an EC key, and a verifier that looked up a
 * key by `kid` alone would be one a provider's own key rotation could confuse.
 * */
typedef struct {
    char kid[NYA_OIDC_MAX_KID];

    NYA_OidcAlgorithm algorithm;

    NYA_CryptoRsaPublicKey   rsa;
    NYA_CryptoEcdsaPublicKey ec;
} _NYA_OidcKey;

struct NYA_OidcProvider {
    NYA_Arena* arena;

    char issuer[NYA_OIDC_MAX_URL];
    char client_id[NYA_OIDC_MAX_CLIENT_ID];
    char client_secret[NYA_OIDC_MAX_CLIENT_SECRET];
    char redirect_uri[NYA_OIDC_MAX_REDIRECT_URI];
    char scopes[NYA_OIDC_MAX_SCOPES];

    /** Filled by nya_oidc_discover; every other call but that one refuses until this is true. */
    b8   discovered;
    char authorization_endpoint[NYA_OIDC_MAX_URL];
    char token_endpoint[NYA_OIDC_MAX_URL];
    char jwks_uri[NYA_OIDC_MAX_URL];
    /** Optional in the discovery document; empty means nya_oidc_userinfo is NYA_ERROR_NOT_SUPPORTED. */
    char userinfo_endpoint[NYA_OIDC_MAX_URL];

    _NYA_OidcKey keys[NYA_OIDC_MAX_KEYS];
    u32          key_count;

    /** When the jwks was last fetched, successfully or not. Zero means never, which has no cooldown. */
    u64 jwks_fetched_at_ms;

    u64 timeout_ms;

    NYA_Error (*perform)(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response);
    u64 (*now_ms)(void* user);
    u64 (*now_s)(void* user);
    void* user;
};

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/* The default transport: nya_request_perform, and the engine's clocks. */
NYA_INTERNAL NYA_Error _nya_oidc_perform(void* user, NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) __attr_no_discard;
NYA_INTERNAL u64       _nya_oidc_now_ms(void* user) __attr_no_discard;
NYA_INTERNAL u64       _nya_oidc_now_s(void* user) __attr_no_discard;

/** Copies `text` into `destination`, truncating rather than running over. Callers bound-check first. */
NYA_INTERNAL void _nya_oidc_copy(char* destination, u64 capacity, NYA_ConstCString text);

/** Whether the space separated `scopes` names "openid" as one whole scope, not as a substring of another. */
NYA_INTERNAL b8 _nya_oidc_scopes_contain_openid(NYA_ConstCString scopes) __attr_no_discard;

/** 32 random bytes as base64url text: what backs `state`, `nonce` and the PKCE verifier alike. */
NYA_INTERNAL b8 _nya_oidc_random_secret(OUT char out[NYA_OIDC_SECRET_TEXT_BYTES]) __attr_no_discard;

/** A string claim, bounded. False when it is absent, is not a string, or does not fit `capacity`. */
NYA_INTERNAL b8 _nya_oidc_claim_string(const NYA_Object* object, NYA_ConstCString key, OUT char* out_value, u64 capacity);

/** A non-negative integer claim. False when it is absent or is not one; mirrors http_auth.c's own. */
NYA_INTERNAL b8 _nya_oidc_claim_u64(const NYA_Object* object, NYA_ConstCString key, OUT u64* out_value);

/** A boolean claim, defaulting to false when absent or of another type: what "email_verified" wants. */
NYA_INTERNAL b8 _nya_oidc_claim_bool(const NYA_Object* object, NYA_ConstCString key) __attr_no_discard;

/** Whether `aud` is `client_id`, in either the single string form or the array of strings one. */
NYA_INTERNAL b8 _nya_oidc_claim_audience_contains(const NYA_Object* payload, NYA_ConstCString client_id) __attr_no_discard;

/** Fetches `provider->jwks_uri` and replaces the cached keys wholesale, which is how a rotation drops the old ones. */
NYA_INTERNAL NYA_Error _nya_oidc_jwks_fetch(NYA_OidcProvider* provider, NYA_Arena* arena) __attr_no_discard;

/** The cached key named `kid`, or null. */
NYA_INTERNAL const _NYA_OidcKey* _nya_oidc_jwks_find(const NYA_OidcProvider* provider, NYA_ConstCString kid) __attr_no_discard;

/**
 * The key named `kid`, fetching once if it is not held. A second miss right after a fetch is refused
 * rather than fetched again: see NYA_OIDC_JWKS_REFETCH_COOLDOWN_MS.
 * */
NYA_INTERNAL NYA_Error
_nya_oidc_jwks_ensure(NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString kid, OUT const _NYA_OidcKey** out_key) __attr_no_discard;

/**
 * Decodes and verifies `token` against `provider`'s issuer, client id and jwks, checks `nonce` against
 * `expected_nonce`, and fills `out_claims`. See the file note on the order this trusts things in.
 * */
NYA_INTERNAL NYA_Error _nya_oidc_id_token_verify(
    NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString token, u64 token_size, NYA_ConstCString expected_nonce, OUT NYA_OidcClaims* out_claims
) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_oidc_create(NYA_Arena* arena, NYA_OidcOptions options, NYA_OidcProvider** out_provider) {
    nya_assert(arena != nullptr && out_provider != nullptr);

    *out_provider = nullptr;

    if (options.issuer == nullptr || options.issuer[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a provider needs an issuer");
    if (strlen(options.issuer) >= NYA_OIDC_MAX_URL) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not an issuer");

    if (options.client_id == nullptr || options.client_id[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a provider needs a client id");
    if (strlen(options.client_id) >= NYA_OIDC_MAX_CLIENT_ID) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a client id");

    if (options.client_secret != nullptr && strlen(options.client_secret) >= NYA_OIDC_MAX_CLIENT_SECRET) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a client secret");
    }

    if (options.redirect_uri == nullptr || options.redirect_uri[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a provider needs a redirect uri");
    if (strlen(options.redirect_uri) >= NYA_OIDC_MAX_REDIRECT_URI) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a redirect uri");

    if (options.scopes == nullptr || strlen(options.scopes) >= NYA_OIDC_MAX_SCOPES || !_nya_oidc_scopes_contain_openid(options.scopes)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the scope string must carry 'openid', which the spec this implements requires");
    }

    NYA_OidcProvider* provider = nya_arena_alloc(arena, sizeof(NYA_OidcProvider));
    nya_memset(provider, 0, sizeof(NYA_OidcProvider));

    provider->arena = arena;

    _nya_oidc_copy(provider->issuer, sizeof(provider->issuer), options.issuer);
    _nya_oidc_copy(provider->client_id, sizeof(provider->client_id), options.client_id);
    _nya_oidc_copy(provider->client_secret, sizeof(provider->client_secret), options.client_secret != nullptr ? options.client_secret : "");
    _nya_oidc_copy(provider->redirect_uri, sizeof(provider->redirect_uri), options.redirect_uri);
    _nya_oidc_copy(provider->scopes, sizeof(provider->scopes), options.scopes);

    provider->timeout_ms = options.timeout_ms > 0 ? options.timeout_ms : NYA_REQUEST_DEFAULT_TIMEOUT_MS;

    provider->perform = options.perform != nullptr ? options.perform : _nya_oidc_perform;
    provider->now_ms   = options.now_ms != nullptr ? options.now_ms : _nya_oidc_now_ms;
    provider->now_s    = options.now_s != nullptr ? options.now_s : _nya_oidc_now_s;
    provider->user     = options.user;

    *out_provider = provider;

    return NYA_OK;
}

void nya_oidc_destroy(NYA_OidcProvider* provider) {
    if (provider == nullptr) return;

    nya_crypto_wipe(provider->client_secret, sizeof(provider->client_secret));
    nya_crypto_wipe(provider->keys, sizeof(provider->keys));
}

NYA_Error nya_oidc_discover(NYA_OidcProvider* provider, NYA_Arena* arena) {
    nya_assert(provider != nullptr && arena != nullptr);

    char url[NYA_OIDC_MAX_URL + 40] = { 0 };
    int  written                    = snprintf(url, sizeof(url), "%s/.well-known/openid-configuration", provider->issuer);
    if (written < 0 || (u64)written >= sizeof(url)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the discovery url does not fit");

    NYA_Response response  = { 0 };
    NYA_Error    performed = provider->perform(
        provider->user, arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = url, .timeout_ms = provider->timeout_ms }, &response
    );

    if (!performed.ok) return nya_error(NYA_ERROR_NOT_OK, "fetching the discovery document failed: %s", (NYA_ConstCString)performed.message);
    if (response.body == nullptr) return nya_error(NYA_ERROR_PARSE, "the discovery document is not JSON");

    char issuer[NYA_OIDC_MAX_URL] = { 0 };
    if (!_nya_oidc_claim_string(response.body, "issuer", issuer, sizeof(issuer))) {
        return nya_error(NYA_ERROR_PARSE, "the discovery document has no usable 'issuer'");
    }

    // A discovery document naming a different issuer is what a misrouted proxy hands back; trusting its endpoints would post a code somewhere the provider never was.
    if (!nya_string_equals((NYA_ConstCString)issuer, provider->issuer)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the discovery document's issuer is not the one this provider was created with");
    }

    char authorization_endpoint[NYA_OIDC_MAX_URL] = { 0 };
    char token_endpoint[NYA_OIDC_MAX_URL]          = { 0 };
    char jwks_uri[NYA_OIDC_MAX_URL]                = { 0 };

    if (!_nya_oidc_claim_string(response.body, "authorization_endpoint", authorization_endpoint, sizeof(authorization_endpoint))) {
        return nya_error(NYA_ERROR_PARSE, "the discovery document has no usable 'authorization_endpoint'");
    }
    if (!_nya_oidc_claim_string(response.body, "token_endpoint", token_endpoint, sizeof(token_endpoint))) {
        return nya_error(NYA_ERROR_PARSE, "the discovery document has no usable 'token_endpoint'");
    }
    if (!_nya_oidc_claim_string(response.body, "jwks_uri", jwks_uri, sizeof(jwks_uri))) {
        return nya_error(NYA_ERROR_PARSE, "the discovery document has no usable 'jwks_uri'");
    }

    _nya_oidc_copy(provider->authorization_endpoint, sizeof(provider->authorization_endpoint), authorization_endpoint);
    _nya_oidc_copy(provider->token_endpoint, sizeof(provider->token_endpoint), token_endpoint);
    _nya_oidc_copy(provider->jwks_uri, sizeof(provider->jwks_uri), jwks_uri);

    // Optional: absent is a provider with no userinfo endpoint, not a broken discovery document.
    char userinfo_endpoint[NYA_OIDC_MAX_URL] = { 0 };
    if (_nya_oidc_claim_string(response.body, "userinfo_endpoint", userinfo_endpoint, sizeof(userinfo_endpoint))) {
        _nya_oidc_copy(provider->userinfo_endpoint, sizeof(provider->userinfo_endpoint), userinfo_endpoint);
    }

    provider->discovered = true;

    return NYA_OK;
}

NYA_Error nya_oidc_authorize_url(const NYA_OidcProvider* provider, char* out_url, u64 capacity, NYA_OidcAuthorizeState* out_state) {
    nya_assert(provider != nullptr && out_url != nullptr && capacity > 0 && out_state != nullptr);

    out_url[0]  = '\0';
    *out_state = (NYA_OidcAuthorizeState){ 0 };

    if (!provider->discovered) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "call nya_oidc_discover before building an authorize url");

    if (!_nya_oidc_random_secret(out_state->state) || !_nya_oidc_random_secret(out_state->nonce) ||
        !_nya_oidc_random_secret(out_state->code_verifier)) {
        *out_state = (NYA_OidcAuthorizeState){ 0 };
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed; refusing to start a login with no entropy behind it");
    }

    NYA_CryptoSha256Digest verifier_hash = { 0 };
    nya_crypto_sha256((const u8*)out_state->code_verifier, strlen(out_state->code_verifier), &verifier_hash);

    char code_challenge[NYA_OIDC_SECRET_TEXT_BYTES] = { 0 };
    u64  challenge_length                            = 0;
    if (!nya_crypto_base64url_encode(verifier_hash.bytes, sizeof(verifier_hash.bytes), code_challenge, sizeof(code_challenge), &challenge_length)) {
        *out_state = (NYA_OidcAuthorizeState){ 0 };
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the code challenge does not fit");
    }

    // client_id, redirect_uri and scope are caller config that may need escaping; state, nonce and the challenge are our own base64url, already unreserved, so they are not encoded.
    char client_id_encoded[NYA_OIDC_MAX_CLIENT_ID * 3]    = { 0 };
    char redirect_uri_encoded[NYA_OIDC_MAX_REDIRECT_URI * 3] = { 0 };
    char scope_encoded[NYA_OIDC_MAX_SCOPES * 3]           = { 0 };
    u64  encoded_length                                    = 0;

    NYA_TRY(nya_percent_encode((const u8*)provider->client_id, strlen(provider->client_id), client_id_encoded, sizeof(client_id_encoded), &encoded_length));
    NYA_TRY(nya_percent_encode(
        (const u8*)provider->redirect_uri, strlen(provider->redirect_uri), redirect_uri_encoded, sizeof(redirect_uri_encoded), &encoded_length
    ));
    NYA_TRY(nya_percent_encode((const u8*)provider->scopes, strlen(provider->scopes), scope_encoded, sizeof(scope_encoded), &encoded_length));

    int written = snprintf(
        out_url, capacity,
        "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s&state=%s&nonce=%s&code_challenge=%s&code_challenge_method=S256",
        provider->authorization_endpoint, client_id_encoded, redirect_uri_encoded, scope_encoded, out_state->state, out_state->nonce, code_challenge
    );

    if (written < 0 || (u64)written >= capacity) {
        out_url[0]  = '\0';
        *out_state = (NYA_OidcAuthorizeState){ 0 };
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the authorize url does not fit the caller's buffer");
    }

    return NYA_OK;
}

NYA_Error nya_oidc_exchange(NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString code, const NYA_OidcAuthorizeState* state, NYA_OidcClaims* out_claims) {
    nya_assert(provider != nullptr && arena != nullptr && out_claims != nullptr);

    *out_claims = (NYA_OidcClaims){ 0 };

    if (!provider->discovered) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "call nya_oidc_discover before exchanging a code");
    if (code == nullptr || code[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a login callback needs a code");
    if (state == nullptr || state->state[0] == '\0') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "exchanging a code needs the state nya_oidc_authorize_url handed out");
    }

    NYA_Object* body = nya_object_create(arena);
    nya_object_add(body, "grant_type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"authorization_code" });
    nya_object_add(body, "code", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)code });
    nya_object_add(body, "redirect_uri", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = provider->redirect_uri });
    nya_object_add(body, "client_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = provider->client_id });
    nya_object_add(body, "code_verifier", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)state->code_verifier });

    // A public client sends none; a confidential one authenticates via the body here, since this engine's request module cannot express the basic-auth form (see request.h).
    if (provider->client_secret[0] != '\0') {
        nya_object_add(body, "client_secret", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = provider->client_secret });
    }

    NYA_Response response  = { 0 };
    NYA_Error    performed = provider->perform(
        provider->user, arena,
        (NYA_Request){
            .method = NYA_REQUEST_METHOD_POST,
            .url    = provider->token_endpoint,
            .body   = body,

            // What RFC 6749 4.1.3 asks for, and the one encoding every provider takes.
            .body_kind  = NYA_REQUEST_BODY_FORM,
            .timeout_ms = provider->timeout_ms,
        },
        &response
    );

    if (!performed.ok) {
        char error_code[64]        = { 0 };
        char error_description[128] = { 0 };

        if (response.body != nullptr && _nya_oidc_claim_string(response.body, "error", error_code, sizeof(error_code))) {
            (void)_nya_oidc_claim_string(response.body, "error_description", error_description, sizeof(error_description));
            return nya_error(
                NYA_ERROR_PERMISSION_DENIED, "the token endpoint refused the code: %s%s%s", error_code, error_description[0] != '\0' ? ": " : "",
                error_description
            );
        }

        return nya_error(performed.kind, "redeeming the code failed: %s", (NYA_ConstCString)performed.message);
    }

    if (response.body == nullptr) return nya_error(NYA_ERROR_PARSE, "the token endpoint's reply is not JSON");

    char id_token[NYA_OIDC_MAX_ID_TOKEN_BYTES] = { 0 };
    if (!_nya_oidc_claim_string(response.body, "id_token", id_token, sizeof(id_token))) {
        return nya_error(NYA_ERROR_PARSE, "the token endpoint's reply has no usable 'id_token'");
    }

    // Optional in the strict sense, but every real provider returns one and nya_oidc_userinfo wants it.
    (void)_nya_oidc_claim_string(response.body, "access_token", out_claims->access_token, sizeof(out_claims->access_token));

    // A token failing any single check below is refused whole, so a failure here clears the entire struct rather than leaving it half filled.
    NYA_Error verified = _nya_oidc_id_token_verify(provider, arena, id_token, strlen(id_token), state->nonce, out_claims);
    if (!verified.ok) {
        *out_claims = (NYA_OidcClaims){ 0 };
        return verified;
    }

    return NYA_OK;
}

NYA_Error nya_oidc_userinfo(NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString access_token, NYA_Object** out_claims) {
    nya_assert(provider != nullptr && arena != nullptr && out_claims != nullptr);

    *out_claims = nullptr;

    if (provider->userinfo_endpoint[0] == '\0') {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "this provider's discovery document named no userinfo_endpoint");
    }

    if (access_token == nullptr || access_token[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "userinfo needs an access token");

    NYA_Response response  = { 0 };
    NYA_Error    performed = provider->perform(
        provider->user, arena,
        (NYA_Request){
            .method       = NYA_REQUEST_METHOD_GET,
            .url          = provider->userinfo_endpoint,
            .bearer_token = access_token,
            .timeout_ms   = provider->timeout_ms,
        },
        &response
    );

    if (!performed.ok) return nya_error(performed.kind, "the userinfo endpoint refused the token: %s", (NYA_ConstCString)performed.message);
    if (response.body == nullptr) return nya_error(NYA_ERROR_PARSE, "the userinfo endpoint's reply is not JSON");

    *out_claims = response.body;

    return NYA_OK;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_oidc_perform(void* user, NYA_Arena* arena, NYA_Request request, NYA_Response* out_response) {
    (void)user;

    return nya_request_perform(arena, request, out_response);
}

u64 _nya_oidc_now_ms(void* user) {
    (void)user;

    return nya_clock_get_monotonic_ms();
}

u64 _nya_oidc_now_s(void* user) {
    (void)user;

    return nya_clock_get_timestamp_s();
}

void _nya_oidc_copy(char* destination, u64 capacity, NYA_ConstCString text) {
    nya_assert(destination != nullptr && capacity > 0);

    destination[0] = '\0';

    if (text == nullptr) return;

    u64 length = strlen(text);
    if (length > capacity - 1) length = capacity - 1;

    nya_memcpy(destination, text, length);
    destination[length] = '\0';
}

b8 _nya_oidc_scopes_contain_openid(NYA_ConstCString scopes) {
    if (scopes == nullptr) return false;

    u64 length = strlen(scopes);
    u64 start  = 0;

    for (u64 i = 0; i <= length; i++) {
        if (i == length || scopes[i] == ' ') {
            if (i - start == 6 && memcmp(scopes + start, "openid", 6) == 0) return true;
            start = i + 1;
        }
    }

    return false;
}

b8 _nya_oidc_random_secret(char out[NYA_OIDC_SECRET_TEXT_BYTES]) {
    u8 bytes[NYA_OIDC_SECRET_BYTES] = { 0 };
    if (!nya_os_random_bytes(bytes, sizeof(bytes))) return false;

    u64 length  = 0;
    b8  encoded = nya_crypto_base64url_encode(bytes, sizeof(bytes), out, NYA_OIDC_SECRET_TEXT_BYTES, &length);

    nya_crypto_wipe(bytes, sizeof(bytes));

    return encoded;
}

b8 _nya_oidc_claim_string(const NYA_Object* object, NYA_ConstCString key, char* out_value, u64 capacity) {
    out_value[0] = '\0';

    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return false;

    u64 length = strlen(value->as_string);
    if (length >= capacity) return false;

    nya_memcpy(out_value, value->as_string, length);
    out_value[length] = '\0';

    return true;
}

b8 _nya_oidc_claim_u64(const NYA_Object* object, NYA_ConstCString key, u64* out_value) {
    *out_value = 0;

    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr) return false;

    // serde reads every JSON integer as s64, so a negative claim parsed but has no meaning here.
    if (value->type != NYA_TYPE_S64 || value->as_s64 < 0) return false;

    *out_value = (u64)value->as_s64;

    return true;
}

b8 _nya_oidc_claim_bool(const NYA_Object* object, NYA_ConstCString key) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_B8) return false;

    return value->as_b8;
}

b8 _nya_oidc_claim_audience_contains(const NYA_Object* payload, NYA_ConstCString client_id) {
    NYA_Value* aud = nya_object_get(payload, "aud");
    if (aud == nullptr) return false;

    if (aud->type == NYA_TYPE_STRING) return aud->as_string != nullptr && nya_string_equals((NYA_ConstCString)aud->as_string, client_id);

    if (aud->type == NYA_TYPE_ARRAY) {
        for (u64 i = 0; i < aud->as_array.length; i++) {
            NYA_Value* item = &aud->as_array.items[i];
            if (item->type == NYA_TYPE_STRING && item->as_string != nullptr && nya_string_equals((NYA_ConstCString)item->as_string, client_id)) {
                return true;
            }
        }
    }

    return false;
}

NYA_Error _nya_oidc_jwks_fetch(NYA_OidcProvider* provider, NYA_Arena* arena) {
    NYA_Response response  = { 0 };
    NYA_Error    performed = provider->perform(
        provider->user, arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = provider->jwks_uri, .timeout_ms = provider->timeout_ms }, &response
    );

    // Stamped whether or not this succeeded: a briefly-down jwks endpoint should not become one refetch per token.
    provider->jwks_fetched_at_ms = provider->now_ms(provider->user);

    if (!performed.ok) return nya_error(NYA_ERROR_NOT_OK, "fetching the provider's jwks failed: %s", (NYA_ConstCString)performed.message);
    if (response.body == nullptr) return nya_error(NYA_ERROR_PARSE, "the provider's jwks is not JSON");

    NYA_Value* keys = nya_object_get(response.body, "keys");
    if (keys == nullptr || keys->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_PARSE, "the provider's jwks has no 'keys' array");

    // A fresh fetch replaces the cache wholesale rather than merging, which is what drops a key the provider has retired.
    u32 stored = 0;

    for (u64 i = 0; i < keys->as_array.length && stored < NYA_OIDC_MAX_KEYS; i++) {
        NYA_Value* entry = &keys->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;

        char kty[16] = { 0 };
        if (!_nya_oidc_claim_string(&entry->as_object, "kty", kty, sizeof(kty))) continue;

        char kid[NYA_OIDC_MAX_KID] = { 0 };
        if (!_nya_oidc_claim_string(&entry->as_object, "kid", kid, sizeof(kid))) continue;

        _NYA_OidcKey held = { 0 };

        if (nya_string_equals((NYA_ConstCString)kty, "RSA")) {
            // A 4096-bit modulus is 683 base64url characters; this leaves slack rather than sizing exactly.
            char n_text[700] = { 0 };
            char e_text[32]  = { 0 };
            if (!_nya_oidc_claim_string(&entry->as_object, "n", n_text, sizeof(n_text))) continue;
            if (!_nya_oidc_claim_string(&entry->as_object, "e", e_text, sizeof(e_text))) continue;

            u8  modulus[NYA_CRYPTO_RSA_MAX_BYTES] = { 0 };
            u64 modulus_size                      = 0;
            if (!nya_crypto_base64url_decode(n_text, strlen(n_text), modulus, sizeof(modulus), &modulus_size)) continue;

            u8  exponent[8]   = { 0 };
            u64 exponent_size = 0;
            if (!nya_crypto_base64url_decode(e_text, strlen(e_text), exponent, sizeof(exponent), &exponent_size)) continue;

            if (!nya_crypto_rsa_public_key_from_parts(modulus, modulus_size, exponent, exponent_size, &held.rsa).ok) continue;

            held.algorithm = NYA_OIDC_ALGORITHM_RS256;
        } else if (nya_string_equals((NYA_ConstCString)kty, "EC")) {
            // P-256 only, because ES256 names exactly that curve; a P-384 key for ES384 is skipped rather than stored under an algorithm this cannot verify.
            char crv[16] = { 0 };
            if (!_nya_oidc_claim_string(&entry->as_object, "crv", crv, sizeof(crv)) || !nya_string_equals((NYA_ConstCString)crv, "P-256")) continue;

            char x_text[64] = { 0 };
            char y_text[64] = { 0 };
            if (!_nya_oidc_claim_string(&entry->as_object, "x", x_text, sizeof(x_text))) continue;
            if (!_nya_oidc_claim_string(&entry->as_object, "y", y_text, sizeof(y_text))) continue;

            u8  x[NYA_CRYPTO_ECDSA_COORDINATE_BYTES] = { 0 };
            u8  y[NYA_CRYPTO_ECDSA_COORDINATE_BYTES] = { 0 };
            u64 x_size                               = 0;
            u64 y_size                               = 0;

            if (!nya_crypto_base64url_decode(x_text, strlen(x_text), x, sizeof(x), &x_size)) continue;
            if (!nya_crypto_base64url_decode(y_text, strlen(y_text), y, sizeof(y), &y_size)) continue;

            if (!nya_crypto_ecdsa_public_key_from_xy(x, x_size, y, y_size, &held.ec).ok) continue;

            held.algorithm = NYA_OIDC_ALGORITHM_ES256;
        } else {
            continue;
        }

        _nya_oidc_copy(held.kid, sizeof(held.kid), kid);

        provider->keys[stored] = held;
        stored++;
    }

    provider->key_count = stored;

    return NYA_OK;
}

const _NYA_OidcKey* _nya_oidc_jwks_find(const NYA_OidcProvider* provider, NYA_ConstCString kid) {
    for (u32 i = 0; i < provider->key_count; i++) {
        if (nya_string_equals((NYA_ConstCString)provider->keys[i].kid, kid)) return &provider->keys[i];
    }

    return nullptr;
}

NYA_Error _nya_oidc_jwks_ensure(NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString kid, const _NYA_OidcKey** out_key) {
    *out_key = _nya_oidc_jwks_find(provider, kid);
    if (*out_key != nullptr) return NYA_OK;

    u64 now_ms = provider->now_ms(provider->user);

    // An id_token naming an unknown kid is a rotation or an attacker's free refetch; inside the cooldown it is treated as the latter and not fetched again.
    if (provider->jwks_fetched_at_ms != 0 && now_ms - provider->jwks_fetched_at_ms < NYA_OIDC_JWKS_REFETCH_COOLDOWN_MS) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's kid does not name a key this provider's jwks holds");
    }

    NYA_TRY(_nya_oidc_jwks_fetch(provider, arena));

    *out_key = _nya_oidc_jwks_find(provider, kid);
    if (*out_key == nullptr) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's kid does not name a key this provider's jwks holds");

    return NYA_OK;
}

NYA_Error _nya_oidc_id_token_verify(
    NYA_OidcProvider* provider, NYA_Arena* arena, NYA_ConstCString token, u64 token_size, NYA_ConstCString expected_nonce, NYA_OidcClaims* out_claims
) {
    if (token_size == 0 || token_size >= NYA_OIDC_MAX_ID_TOKEN_BYTES) return nya_error(NYA_ERROR_PARSE, "the id_token is longer than this client accepts");

    u64 first = 0;
    while (first < token_size && token[first] != '.') first++;
    if (first == 0 || first >= token_size) return nya_error(NYA_ERROR_PARSE, "the id_token does not have three parts");

    u64 second = first + 1;
    while (second < token_size && token[second] != '.') second++;
    if (second >= token_size || second == first + 1 || second + 1 >= token_size) {
        return nya_error(NYA_ERROR_PARSE, "the id_token does not have three parts");
    }

    for (u64 i = second + 1; i < token_size; i++) {
        if (token[i] == '.') return nya_error(NYA_ERROR_PARSE, "the id_token does not have three parts");
    }

    // The header is trusted for exactly two things — `alg`, to refuse anything but RS256/ES256, and `kid`, to pick a key — and nothing else until the signature over these bytes verifies below.
    u8  header_bytes[512] = { 0 };
    u64 header_size        = 0;
    if (!nya_crypto_base64url_decode(token, first, header_bytes, sizeof(header_bytes), &header_size)) {
        return nya_error(NYA_ERROR_PARSE, "the id_token's header is not base64url");
    }

    NYA_Object* header = nullptr;
    NYA_TRY(nya_deserialize(arena, header_bytes, header_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &header));
    if (header == nullptr) return nya_error(NYA_ERROR_PARSE, "the id_token's header is not a JSON object");

    char alg[16] = { 0 };
    if (!_nya_oidc_claim_string(header, "alg", alg, sizeof(alg))) return nya_error(NYA_ERROR_PARSE, "the id_token's header carries no alg");

    NYA_OidcAlgorithm algorithm = NYA_OIDC_ALGORITHM_NONE;

    if (nya_string_equals((NYA_ConstCString)alg, "RS256")) algorithm = NYA_OIDC_ALGORITHM_RS256;
    if (nya_string_equals((NYA_ConstCString)alg, "ES256")) algorithm = NYA_OIDC_ALGORITHM_ES256;

    if (algorithm == NYA_OIDC_ALGORITHM_NONE) {
        // Covers "none", "HS256" and everything else in one refusal: this module never computes an HMAC, so there is no downgrade path to fall into.
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's alg is neither RS256 nor ES256");
    }

    char kid[NYA_OIDC_MAX_KID] = { 0 };
    if (!_nya_oidc_claim_string(header, "kid", kid, sizeof(kid))) return nya_error(NYA_ERROR_PARSE, "the id_token's header carries no kid");

    const _NYA_OidcKey* key = nullptr;
    NYA_TRY(_nya_oidc_jwks_ensure(provider, arena, kid, &key));

    // The key's family must be the one `alg` named: an RSA key verifying an ES256 token is exactly the confusion this refuses before any arithmetic.
    if (key->algorithm != algorithm) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's alg is not what its kid names a key for");

    u8  signature[NYA_CRYPTO_RSA_MAX_BYTES] = { 0 };
    u64 signature_size                      = 0;
    if (!nya_crypto_base64url_decode(token + second + 1, token_size - second - 1, signature, sizeof(signature), &signature_size)) {
        return nya_error(NYA_ERROR_PARSE, "the id_token's signature is not base64url");
    }

    // over "header.payload" exactly as they arrived, before either half is parsed as anything but bytes.
    b8 verified = algorithm == NYA_OIDC_ALGORITHM_RS256 ? nya_crypto_rsa_verify_sha256(&key->rsa, (const u8*)token, second, signature, signature_size)
                                                        : nya_crypto_ecdsa_verify_sha256(&key->ec, (const u8*)token, second, signature, signature_size);

    if (!verified) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's signature does not verify");

    // Everything from here on is trusted, since the signature proved this provider's key signed exactly these bytes.
    u8  payload_bytes[NYA_OIDC_MAX_ID_TOKEN_BYTES] = { 0 };
    u64 payload_size                                = 0;
    if (!nya_crypto_base64url_decode(token + first + 1, second - first - 1, payload_bytes, sizeof(payload_bytes), &payload_size)) {
        return nya_error(NYA_ERROR_PARSE, "the id_token's payload is not base64url");
    }

    NYA_Object* payload = nullptr;
    NYA_TRY(nya_deserialize(arena, payload_bytes, payload_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &payload));
    if (payload == nullptr) return nya_error(NYA_ERROR_PARSE, "the id_token's payload is not a JSON object");

    // out_claims->issuer is filled here whether or not the check passes; a caller sees it only on NYA_OK, since nya_oidc_exchange clears the struct on any error.
    if (!_nya_oidc_claim_string(payload, "iss", out_claims->issuer, sizeof(out_claims->issuer)) ||
        !nya_string_equals((NYA_ConstCString)out_claims->issuer, provider->issuer)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's iss is not this provider's issuer");
    }

    if (!_nya_oidc_claim_audience_contains(payload, provider->client_id)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's aud does not include this client");
    }

    u64 now_s = provider->now_s(provider->user);

    u64 exp = 0;
    if (!_nya_oidc_claim_u64(payload, "exp", &exp)) return nya_error(NYA_ERROR_PARSE, "the id_token has no usable 'exp'");
    if (exp <= now_s) return nya_error(NYA_ERROR_TIMEOUT, "the id_token has expired");

    u64 iat = 0;
    if (!_nya_oidc_claim_u64(payload, "iat", &iat)) return nya_error(NYA_ERROR_PARSE, "the id_token has no usable 'iat'");
    if (iat > now_s + NYA_OIDC_CLOCK_SKEW_S) return nya_error(NYA_ERROR_TIMEOUT, "the id_token's iat is in the future");

    char nonce[NYA_OIDC_SECRET_TEXT_BYTES] = { 0 };
    if (!_nya_oidc_claim_string(payload, "nonce", nonce, sizeof(nonce)) || expected_nonce == nullptr ||
        !nya_string_equals((NYA_ConstCString)nonce, expected_nonce)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's nonce does not match what this login started with");
    }

    char azp[NYA_OIDC_MAX_CLIENT_ID] = { 0 };
    if (_nya_oidc_claim_string(payload, "azp", azp, sizeof(azp)) && !nya_string_equals((NYA_ConstCString)azp, provider->client_id)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the id_token's azp is not this client");
    }

    if (!_nya_oidc_claim_string(payload, "sub", out_claims->subject, sizeof(out_claims->subject))) {
        return nya_error(NYA_ERROR_PARSE, "the id_token has no usable 'sub'");
    }

    // everything past this point is optional per the spec, so its absence is not a refusal.
    (void)_nya_oidc_claim_string(payload, "email", out_claims->email, sizeof(out_claims->email));
    out_claims->email_verified = _nya_oidc_claim_bool(payload, "email_verified");
    (void)_nya_oidc_claim_string(payload, "name", out_claims->name, sizeof(out_claims->name));
    (void)_nya_oidc_claim_string(payload, "picture", out_claims->picture, sizeof(out_claims->picture));

    out_claims->raw = payload;

    return NYA_OK;
}
