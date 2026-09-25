#include <string.h>

#include "nyangine-plugins/acme/acme.h"
#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-core/crypto/crypto_encoding.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/crypto/crypto_sign.h"
#include "nyangine-std/os/os_time.h"
#include "nyangine-std/serde/serde_json.h"

#ifdef NYA_MODULE_TLS
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif // NYA_MODULE_TLS

// PRIVATE TYPES

/** An account key: EdDSA in `eddsa` (the engine's own) or ES256 in `es256` (OpenSSL), with the ES256 public point cached beside it. */
struct NYA_AcmeAccountKey {
    NYA_AcmeAlgorithm algorithm;
    NYA_Arena*        arena;

    /** The EdDSA pair, filled for NYA_ACME_ALGORITHM_EDDSA. */
    NYA_CryptoSignKeyPair eddsa;

    /** The ES256 public point, big-endian, filled for NYA_ACME_ALGORITHM_ES256. */
    u8 es256_x[NYA_CRYPTO_ECDSA_COORDINATE_BYTES];
    u8 es256_y[NYA_CRYPTO_ECDSA_COORDINATE_BYTES];

#ifdef NYA_MODULE_TLS
    /** The ES256 private key, OpenSSL's. Null for an EdDSA key. Freed by nya_acme_account_key_destroy. */
    EVP_PKEY* es256;
#endif
};

/** The token to key-authorization map an HTTP-01 check reads, a small string dictionary over an arena. */
struct NYA_AcmeChallengeStore {
    NYA_Arena*  arena;
    NYA_Object* map;
};

/** What one obtain threads through its steps: the directory it discovered, the live nonce, and the account it is. */
typedef struct {
    NYA_Arena*            arena;
    const NYA_AcmeConfig* config;

    /** The URLs read from the directory, each an arena copy. */
    NYA_ConstCString new_nonce;
    NYA_ConstCString new_account;
    NYA_ConstCString new_order;

    /** The nonce the next POST must carry; replaced from every reply's Replay-Nonce. */
    NYA_ConstCString nonce;

    /** The account URL the CA returned from newAccount, the `kid` every later request names. */
    NYA_ConstCString kid;
} _NYA_AcmeSession;

// PRIVATE API DECLARATION

/** `size` bytes as base64url text in the arena, terminated. Null on an allocation that cannot hold it, which cannot happen here. */
NYA_INTERNAL NYA_ConstCString _nya_acme_b64url(NYA_Arena* arena, const u8* data, u64 size) __attr_no_discard;

/** The canonical RFC 7638 thumbprint input for the key, the exact bytes SHA-256 is taken over. */
NYA_INTERNAL NYA_ConstCString _nya_acme_thumbprint_input(NYA_Arena* arena, const NYA_AcmeAccountKey* key) __attr_no_discard;

/** The `alg` header value for the key's algorithm. */
NYA_INTERNAL NYA_ConstCString _nya_acme_alg_name(NYA_AcmeAlgorithm algorithm) __attr_no_discard;

/** Signs `message` with the account key, writing the JWS-form signature (raw r||s, or the Ed25519 signature). */
NYA_INTERNAL NYA_Error _nya_acme_sign(const NYA_AcmeAccountKey* key, const u8* message, u64 size, OUT u8* out_signature, u64 capacity, OUT u64* out_size)
    __attr_no_discard;

/** The token a challenge path names, or null when the path is not under the prefix. Points into `path`. */
NYA_INTERNAL NYA_ConstCString _nya_acme_challenge_token(NYA_ConstCString path) __attr_no_discard;

/** No control byte, no whitespace, not empty: what a domain must be before it goes in a CSR or an order. */
NYA_INTERNAL b8 _nya_acme_domain_ok(NYA_ConstCString domain) __attr_no_discard;

#ifdef NYA_MODULE_TLS
/** A fresh P-256 certificate key and the DER of a PKCS#10 CSR over it and `domains`, both base64url and PEM. */
NYA_INTERNAL NYA_Error _nya_acme_make_csr(NYA_Arena* arena, const NYA_ConstCString* domains, u64 domain_count, OUT NYA_ConstCString* out_csr_b64url,
                                          OUT NYA_String** out_key_pem) __attr_no_discard;
#endif

// SMALL HELPERS

NYA_ConstCString _nya_acme_b64url(NYA_Arena* arena, const u8* data, u64 size) {
    // three bytes become four characters; a couple of bytes of slack and a terminator past that.
    u64   capacity = ((size + 2) / 3) * 4 + 4;
    char* text     = nya_arena_alloc(arena, capacity);

    u64 written = 0;
    if (!nya_crypto_base64url_encode(data, size, text, capacity, &written)) return nullptr;

    return (NYA_ConstCString)text;
}

NYA_ConstCString _nya_acme_alg_name(NYA_AcmeAlgorithm algorithm) {
    return algorithm == NYA_ACME_ALGORITHM_EDDSA ? "EdDSA" : "ES256";
}

NYA_ConstCString _nya_acme_thumbprint_input(NYA_Arena* arena, const NYA_AcmeAccountKey* key) {
    // RFC 7638: required members, lexicographically ordered, no whitespace; built by hand so the byte order is not a serializer detail.
    if (key->algorithm == NYA_ACME_ALGORITHM_EDDSA) {
        NYA_ConstCString x = _nya_acme_b64url(arena, key->eddsa.public_key.bytes, sizeof(key->eddsa.public_key.bytes));
        return nya_string_to_cstring(arena, nya_string_sprintf(arena, "{\"crv\":\"Ed25519\",\"kty\":\"OKP\",\"x\":\"%s\"}", x));
    }

    NYA_ConstCString x = _nya_acme_b64url(arena, key->es256_x, sizeof(key->es256_x));
    NYA_ConstCString y = _nya_acme_b64url(arena, key->es256_y, sizeof(key->es256_y));
    return nya_string_to_cstring(arena, nya_string_sprintf(arena, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x, y));
}

b8 _nya_acme_domain_ok(NYA_ConstCString domain) {
    if (domain == nullptr || domain[0] == '\0') return false;
    for (const char* c = domain; *c != '\0'; c++) {
        if ((u8)*c <= ' ' || *c == '"' || *c == '\\') return false;
    }
    return true;
}

NYA_ConstCString _nya_acme_challenge_token(NYA_ConstCString path) {
    if (path == nullptr) return nullptr;

    u64 prefix = strlen(NYA_ACME_HTTP01_PREFIX);
    if (strncmp(path, NYA_ACME_HTTP01_PREFIX, prefix) != 0) return nullptr;

    NYA_ConstCString token = path + prefix;
    if (token[0] == '\0') return nullptr;

    // a token is one path segment; a further slash is a different resource, not this one's.
    if (strchr(token, '/') != nullptr) return nullptr;

    return token;
}

// ACCOUNT KEYS

#ifdef NYA_MODULE_TLS
/** Fills a key's cached ES256 public coordinates from its OpenSSL key. */
NYA_INTERNAL NYA_Error _nya_acme_es256_public(NYA_AcmeAccountKey* key) {
    BIGNUM* x = nullptr;
    BIGNUM* y = nullptr;

    if (EVP_PKEY_get_bn_param(key->es256, OSSL_PKEY_PARAM_EC_PUB_X, &x) != 1 || EVP_PKEY_get_bn_param(key->es256, OSSL_PKEY_PARAM_EC_PUB_Y, &y) != 1) {
        BN_free(x);
        BN_free(y);
        return nya_error(NYA_ERROR_NOT_OK, "reading the ES256 public point failed");
    }

    // BN_bn2binpad left-pads to exactly the coordinate size, which is what a JWK and a thumbprint want.
    b8 ok = BN_bn2binpad(x, key->es256_x, (int)sizeof(key->es256_x)) == (int)sizeof(key->es256_x) &&
            BN_bn2binpad(y, key->es256_y, (int)sizeof(key->es256_y)) == (int)sizeof(key->es256_y);

    BN_free(x);
    BN_free(y);

    return ok ? NYA_OK : nya_error(NYA_ERROR_NOT_OK, "the ES256 public point is the wrong size");
}
#endif

NYA_Error nya_acme_account_key_create(NYA_Arena* arena, NYA_AcmeAlgorithm algorithm, OUT NYA_AcmeAccountKey** out_key) {
    nya_assert(arena != nullptr);
    nya_assert(out_key != nullptr);
    *out_key = nullptr;

    NYA_AcmeAccountKey* key = nya_arena_alloc(arena, sizeof(NYA_AcmeAccountKey));
    nya_memset(key, 0, sizeof(*key));
    key->arena     = arena;
    key->algorithm = algorithm;

    if (algorithm == NYA_ACME_ALGORITHM_EDDSA) {
        NYA_TRY(nya_crypto_sign_key_pair_create(&key->eddsa));
        *out_key = key;
        return NYA_OK;
    }

#ifdef NYA_MODULE_TLS
    key->es256 = EVP_EC_gen("P-256");
    if (key->es256 == nullptr) return nya_error(NYA_ERROR_NOT_OK, "generating a P-256 account key failed");

    NYA_Error published = _nya_acme_es256_public(key);
    if (!published.ok) {
        EVP_PKEY_free(key->es256);
        key->es256 = nullptr;
        return published;
    }

    *out_key = key;
    return NYA_OK;
#else
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "an ES256 account key needs OpenSSL, which this build has none of; use EdDSA");
#endif
}

NYA_Error nya_acme_account_key_from_eddsa_seed(NYA_Arena* arena, const u8 seed[32], OUT NYA_AcmeAccountKey** out_key) {
    nya_assert(arena != nullptr);
    nya_assert(seed != nullptr);
    nya_assert(out_key != nullptr);
    *out_key = nullptr;

    NYA_AcmeAccountKey* key = nya_arena_alloc(arena, sizeof(NYA_AcmeAccountKey));
    nya_memset(key, 0, sizeof(*key));
    key->arena     = arena;
    key->algorithm = NYA_ACME_ALGORITHM_EDDSA;

    NYA_CryptoKey32 seed32 = { 0 };
    nya_memcpy(seed32.bytes, seed, sizeof(seed32.bytes));
    nya_crypto_sign_key_pair_from_seed(&seed32, &key->eddsa);
    nya_crypto_wipe(&seed32, sizeof(seed32));

    *out_key = key;
    return NYA_OK;
}

void nya_acme_account_key_destroy(NYA_AcmeAccountKey* key) {
    if (key == nullptr) return;

    nya_crypto_sign_key_pair_destroy(&key->eddsa);
#ifdef NYA_MODULE_TLS
    if (key->es256 != nullptr) {
        EVP_PKEY_free(key->es256);
        key->es256 = nullptr;
    }
#endif
    // the public coordinates are not secret, but wiping the whole struct is one call and leaves nothing.
    nya_crypto_wipe(&key->eddsa, sizeof(key->eddsa));
}

NYA_AcmeAlgorithm nya_acme_account_key_algorithm(const NYA_AcmeAccountKey* key) {
    nya_assert(key != nullptr);
    return key->algorithm;
}

// JWK, THUMBPRINT AND JWS

NYA_Error nya_acme_jwk(NYA_Arena* arena, const NYA_AcmeAccountKey* key, OUT NYA_Object** out_jwk) {
    nya_assert(arena != nullptr);
    nya_assert(key != nullptr);
    nya_assert(out_jwk != nullptr);

    NYA_Object* jwk = nya_object_create(arena);

    // added in the RFC 7638 order, so this is readable and the thumbprint input at once.
    if (key->algorithm == NYA_ACME_ALGORITHM_EDDSA) {
        NYA_ConstCString x = _nya_acme_b64url(arena, key->eddsa.public_key.bytes, sizeof(key->eddsa.public_key.bytes));
        nya_object_add(jwk, "crv", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"Ed25519" });
        nya_object_add(jwk, "kty", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"OKP" });
        nya_object_add(jwk, "x", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)x });
    } else {
        NYA_ConstCString x = _nya_acme_b64url(arena, key->es256_x, sizeof(key->es256_x));
        NYA_ConstCString y = _nya_acme_b64url(arena, key->es256_y, sizeof(key->es256_y));
        nya_object_add(jwk, "crv", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"P-256" });
        nya_object_add(jwk, "kty", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"EC" });
        nya_object_add(jwk, "x", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)x });
        nya_object_add(jwk, "y", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)y });
    }

    *out_jwk = jwk;
    return NYA_OK;
}

NYA_Error nya_acme_jwk_thumbprint(const NYA_AcmeAccountKey* key, OUT NYA_CryptoSha256Digest* out_thumbprint) {
    nya_assert(key != nullptr);
    nya_assert(out_thumbprint != nullptr);

    // a scratch arena for the input text, so the thumbprint of a key does not need one passed in.
    NYA_Arena* scratch = nya_arena_create(.name = "acme-thumbprint");
    defer      nya_arena_destroy(scratch);

    NYA_ConstCString input = _nya_acme_thumbprint_input(scratch, key);
    nya_crypto_sha256((const u8*)input, strlen(input), out_thumbprint);

    return NYA_OK;
}

NYA_Error _nya_acme_sign(const NYA_AcmeAccountKey* key, const u8* message, u64 size, OUT u8* out_signature, u64 capacity, OUT u64* out_size) {
    *out_size = 0;

    if (key->algorithm == NYA_ACME_ALGORITHM_EDDSA) {
        if (capacity < NYA_CRYPTO_SIGNATURE_BYTES) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the EdDSA signature does not fit");

        NYA_CryptoSignature signature = { 0 };
        nya_crypto_sign(&key->eddsa.secret_key, message, size, &signature);
        nya_memcpy(out_signature, signature.bytes, sizeof(signature.bytes));
        *out_size = sizeof(signature.bytes);
        return NYA_OK;
    }

#ifdef NYA_MODULE_TLS
    if (capacity < NYA_CRYPTO_ECDSA_SIGNATURE_BYTES) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the ES256 signature does not fit");

    // OpenSSL returns DER; the JWS form is raw r||s each padded to the coordinate size, so the DER is decoded and rewritten.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no signing context");

    NYA_Error result = NYA_OK;
    u8*       der     = nullptr;
    size_t    der_len = 0;

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key->es256) != 1 || EVP_DigestSign(ctx, nullptr, &der_len, message, size) != 1) {
        result = nya_error(NYA_ERROR_NOT_OK, "ES256 signing setup failed");
        goto done;
    }

    der = nya_arena_alloc(key->arena, der_len);
    if (EVP_DigestSign(ctx, der, &der_len, message, size) != 1) {
        result = nya_error(NYA_ERROR_NOT_OK, "ES256 signing failed");
        goto done;
    }

    {
        const unsigned char* p   = der;
        ECDSA_SIG*           sig = d2i_ECDSA_SIG(nullptr, &p, (long)der_len);
        if (sig == nullptr) {
            result = nya_error(NYA_ERROR_NOT_OK, "the ES256 signature did not decode");
            goto done;
        }

        const BIGNUM* r = nullptr;
        const BIGNUM* s = nullptr;
        ECDSA_SIG_get0(sig, &r, &s);

        nya_memset(out_signature, 0, NYA_CRYPTO_ECDSA_SIGNATURE_BYTES);
        b8 ok = BN_bn2binpad(r, out_signature, NYA_CRYPTO_ECDSA_COORDINATE_BYTES) == NYA_CRYPTO_ECDSA_COORDINATE_BYTES &&
                BN_bn2binpad(s, out_signature + NYA_CRYPTO_ECDSA_COORDINATE_BYTES, NYA_CRYPTO_ECDSA_COORDINATE_BYTES) == NYA_CRYPTO_ECDSA_COORDINATE_BYTES;
        ECDSA_SIG_free(sig);

        if (!ok) {
            result = nya_error(NYA_ERROR_NOT_OK, "the ES256 signature is the wrong size");
            goto done;
        }
        *out_size = NYA_CRYPTO_ECDSA_SIGNATURE_BYTES;
    }

done:
    EVP_MD_CTX_free(ctx);
    return result;
#else
    (void)message;
    (void)size;
    (void)out_signature;
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "ES256 needs OpenSSL");
#endif
}

NYA_Error nya_acme_jws_sign(NYA_Arena* arena, const NYA_AcmeAccountKey* key, NYA_ConstCString url, NYA_ConstCString nonce, NYA_ConstCString kid,
                            const NYA_Object* payload, OUT NYA_Object** out_jws) {
    nya_assert(arena != nullptr);
    nya_assert(key != nullptr);
    nya_assert(url != nullptr);
    nya_assert(out_jws != nullptr);

    // the protected header: alg and url always, then either the embedded jwk (newAccount) or the kid, then the nonce.
    NYA_Object* header = nya_object_create(arena);
    nya_object_add(header, "alg", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)_nya_acme_alg_name(key->algorithm) });

    if (kid != nullptr) {
        nya_object_add(header, "kid", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)kid });
    } else {
        NYA_Object* jwk = nullptr;
        NYA_TRY(nya_acme_jwk(arena, key, &jwk));
        nya_object_add(header, "jwk", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *jwk });
    }

    if (nonce != nullptr) nya_object_add(header, "nonce", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)nonce });
    nya_object_add(header, "url", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)url });

    NYA_String*      header_json = nya_serde_json_serialize(arena, header, 0);
    NYA_ConstCString protected_b64 = _nya_acme_b64url(arena, header_json->items, header_json->length);

    // a null payload is a POST-as-GET, whose signed payload is the empty string, not the text "null".
    NYA_ConstCString payload_b64 = "";
    if (payload != nullptr) {
        NYA_String* payload_json = nya_serde_json_serialize(arena, payload, 0);
        payload_b64              = _nya_acme_b64url(arena, payload_json->items, payload_json->length);
    }

    // the signing input is exactly these two base64url strings joined by a dot, RFC 7515 section 5.1.
    NYA_String* signing_input = nya_string_sprintf(arena, "%s.%s", protected_b64, payload_b64);

    u8        signature[NYA_CRYPTO_SIGNATURE_BYTES > NYA_CRYPTO_ECDSA_SIGNATURE_BYTES ? NYA_CRYPTO_SIGNATURE_BYTES : NYA_CRYPTO_ECDSA_SIGNATURE_BYTES] = { 0 };
    u64       signature_size = 0;
    NYA_TRY(_nya_acme_sign(key, signing_input->items, signing_input->length, signature, sizeof(signature), &signature_size));

    NYA_ConstCString signature_b64 = _nya_acme_b64url(arena, signature, signature_size);

    NYA_Object* jws = nya_object_create(arena);
    nya_object_add(jws, "protected", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)protected_b64 });
    nya_object_add(jws, "payload", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)payload_b64 });
    nya_object_add(jws, "signature", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)signature_b64 });

    *out_jws = jws;
    return NYA_OK;
}

// HTTP-01 CHALLENGE

NYA_Error nya_acme_key_authorization(NYA_Arena* arena, NYA_ConstCString token, const NYA_AcmeAccountKey* key, OUT NYA_ConstCString* out_key_authorization) {
    nya_assert(arena != nullptr);
    nya_assert(token != nullptr);
    nya_assert(key != nullptr);
    nya_assert(out_key_authorization != nullptr);

    NYA_CryptoSha256Digest thumbprint = { 0 };
    NYA_TRY(nya_acme_jwk_thumbprint(key, &thumbprint));

    NYA_ConstCString thumbprint_b64 = _nya_acme_b64url(arena, thumbprint.bytes, sizeof(thumbprint.bytes));

    *out_key_authorization = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s.%s", token, thumbprint_b64));
    return NYA_OK;
}

NYA_AcmeChallengeStore* nya_acme_challenge_store_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_AcmeChallengeStore* store = nya_arena_alloc(arena, sizeof(NYA_AcmeChallengeStore));
    store->arena                  = arena;
    store->map                    = nya_object_create(arena);
    return store;
}

void nya_acme_challenge_store_destroy(NYA_AcmeChallengeStore* store) {
    if (store == nullptr) return;
    // the store lives in an arena, so clearing the map is the whole of it: nothing here owns heap memory.
    nya_object_reset(store->map);
}

NYA_Error nya_acme_challenge_store_add(NYA_AcmeChallengeStore* store, NYA_ConstCString token, NYA_ConstCString key_authorization) {
    nya_assert(store != nullptr);
    nya_assert(token != nullptr);
    nya_assert(key_authorization != nullptr);

    if (_nya_acme_challenge_token(nya_string_to_cstring(store->arena, nya_string_sprintf(store->arena, NYA_ACME_HTTP01_PREFIX "%s", token))) == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a challenge token cannot be empty or carry a slash");
    }

    // copied into the store's arena, so the caller's buffers can go: the store outlives the request.
    NYA_ConstCString token_copy = nya_string_to_cstring(store->arena, nya_string_from(store->arena, token));
    NYA_ConstCString auth_copy  = nya_string_to_cstring(store->arena, nya_string_from(store->arena, key_authorization));

    nya_object_add(store->map, (NYA_CString)token_copy, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)auth_copy });
    return NYA_OK;
}

void nya_acme_challenge_store_remove(NYA_AcmeChallengeStore* store, NYA_ConstCString token) {
    nya_assert(store != nullptr);
    if (token == nullptr) return;
    nya_object_remove(store->map, (NYA_CString)token);
}

b8 nya_acme_challenge_response(const NYA_AcmeChallengeStore* store, NYA_ConstCString request_path, OUT NYA_ConstCString* out_body) {
    nya_assert(store != nullptr);
    nya_assert(out_body != nullptr);
    *out_body = nullptr;

    NYA_ConstCString token = _nya_acme_challenge_token(request_path);
    if (token == nullptr) return false;

    NYA_Value* value = nya_object_get(store->map, (NYA_CString)token);
    if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return false;

    *out_body = (NYA_ConstCString)value->as_string;
    return true;
}

// CSR AND CERTIFICATE KEY

#ifdef NYA_MODULE_TLS
NYA_Error _nya_acme_make_csr(NYA_Arena* arena, const NYA_ConstCString* domains, u64 domain_count, OUT NYA_ConstCString* out_csr_b64url, OUT NYA_String** out_key_pem) {
    *out_csr_b64url = nullptr;
    *out_key_pem    = nullptr;

    EVP_PKEY*             pkey       = nullptr;
    X509_REQ*             req        = nullptr;
    STACK_OF(X509_EXTENSION)* exts   = nullptr;
    X509_EXTENSION*       san        = nullptr;
    u8*                   der        = nullptr;
    NYA_Error             result     = NYA_OK;

    pkey = EVP_EC_gen("P-256");
    if (pkey == nullptr) { result = nya_error(NYA_ERROR_NOT_OK, "generating the certificate key failed"); goto done; }

    req = X509_REQ_new();
    if (req == nullptr || X509_REQ_set_version(req, 0L) != 1 || X509_REQ_set_pubkey(req, pkey) != 1) {
        result = nya_error(NYA_ERROR_NOT_OK, "building the CSR failed");
        goto done;
    }

    // the subject is empty and the domains go in the subjectAltName (`DNS:a,DNS:b`); each is validated first so it cannot be steered.
    {
        NYA_String* san_value = nya_string_create(arena);
        for (u64 i = 0; i < domain_count; i++) {
            if (!_nya_acme_domain_ok(domains[i])) { result = nya_error(NYA_ERROR_INVALID_ARGUMENT, "a domain is empty or has a forbidden byte"); goto done; }
            if (i > 0) nya_string_extend(san_value, ",");
            nya_string_extend_sprintf(san_value, "DNS:%s", domains[i]);
        }

        exts = sk_X509_EXTENSION_new_null();
        san  = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, nya_string_to_cstring(arena, san_value));
        if (exts == nullptr || san == nullptr || sk_X509_EXTENSION_push(exts, san) <= 0) {
            result = nya_error(NYA_ERROR_NOT_OK, "adding the subjectAltName failed");
            goto done;
        }
        san = nullptr; // owned by the stack now.

        if (X509_REQ_add_extensions(req, exts) != 1) { result = nya_error(NYA_ERROR_NOT_OK, "attaching the CSR extensions failed"); goto done; }
    }

    if (X509_REQ_sign(req, pkey, EVP_sha256()) == 0) { result = nya_error(NYA_ERROR_NOT_OK, "signing the CSR failed"); goto done; }

    {
        int der_len = i2d_X509_REQ(req, &der);
        if (der_len <= 0) { result = nya_error(NYA_ERROR_NOT_OK, "encoding the CSR failed"); goto done; }
        *out_csr_b64url = _nya_acme_b64url(arena, der, (u64)der_len);
    }

    // the private key as PEM, for the caller to write beside the chain.
    {
        BIO* bio = BIO_new(BIO_s_mem());
        if (bio == nullptr || PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            BIO_free(bio);
            result = nya_error(NYA_ERROR_NOT_OK, "writing the certificate key failed");
            goto done;
        }
        char* data = nullptr;
        long  len  = BIO_get_mem_data(bio, &data);
        NYA_String* pem = nya_string_create_with_capacity(arena, (u64)len + 1);
        for (long i = 0; i < len; i++) nya_string_push_back(pem, (u8)data[i]);
        *out_key_pem = pem;
        BIO_free(bio);
    }

done:
    if (san != nullptr) X509_EXTENSION_free(san);
    if (exts != nullptr) sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    if (req != nullptr) X509_REQ_free(req);
    if (pkey != nullptr) EVP_PKEY_free(pkey);
    OPENSSL_free(der);
    return result;
}
#endif // NYA_MODULE_TLS

// THE FLOW

// Everything from here to nya_acme_obtain is the order flow, reached only from that function, which
// needs OpenSSL for the CSR. Compiled only with TLS; otherwise these are unused functions and
// -Werror,-Wunused-function fails the NYA_NO_TLS (Windows) cross-build.
#ifdef NYA_MODULE_TLS

/** Reads a string member of an object, or null. Points into the object's own storage. */
NYA_INTERNAL NYA_ConstCString _nya_acme_string(const NYA_Object* object, NYA_ConstCString key) {
    if (object == nullptr) return nullptr;
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;
    return (NYA_ConstCString)value->as_string;
}

/** Parses the reply body as a JSON object, or an error naming the step when it is not JSON. */
NYA_INTERNAL NYA_Error _nya_acme_parse(NYA_Arena* arena, const NYA_AcmeHttpResponse* response, NYA_ConstCString what, OUT NYA_Object** out_object) {
    *out_object = nullptr;
    if (response->body == nullptr || response->body->length == 0) return nya_error(NYA_ERROR_PARSE, "%s: the reply had no body", what);
    return nya_serde_json_deserialize(arena, response->body->items, response->body->length, 0, out_object);
}

/** The CA's problem detail turned into an error, or a plain one when the body is not a problem document. */
NYA_INTERNAL NYA_Error _nya_acme_problem(NYA_Arena* arena, const NYA_AcmeHttpResponse* response, NYA_ConstCString what) {
    NYA_Object* problem = nullptr;
    if (_nya_acme_parse(arena, response, what, &problem).ok) {
        NYA_ConstCString detail = _nya_acme_string(problem, "detail");
        NYA_ConstCString type   = _nya_acme_string(problem, "type");
        if (detail != nullptr) return nya_error(NYA_ERROR_NOT_OK, "%s: the CA refused it (%u): %s", what, response->status, detail);
        if (type != nullptr) return nya_error(NYA_ERROR_NOT_OK, "%s: the CA refused it (%u): %s", what, response->status, type);
    }
    return nya_error(NYA_ERROR_NOT_OK, "%s: the CA refused it with status %u", what, response->status);
}

/** GET or HEAD, no body and no signing. */
NYA_INTERNAL NYA_Error _nya_acme_plain(_NYA_AcmeSession* session, NYA_AcmeMethod method, NYA_ConstCString url, OUT NYA_AcmeHttpResponse* out_response) {
    NYA_AcmeHttpRequest request = { .method = method, .url = url };
    nya_memset(out_response, 0, sizeof(*out_response));
    return session->config->transport.perform(session->config->transport.userdata, session->arena, &request, out_response);
}

/**
 * Signs `payload` (null for POST-as-GET) for `url` with the session's live nonce, POSTs it, and threads
 * the reply's Replay-Nonce back into the session. A reply with no nonce leaves the session to fetch a
 * fresh one before the next POST, so a missing header is a slower request rather than a wrong one.
 * */
NYA_INTERNAL NYA_Error _nya_acme_post(_NYA_AcmeSession* session, NYA_ConstCString url, NYA_ConstCString kid, const NYA_Object* payload,
                                      OUT NYA_AcmeHttpResponse* out_response) {
    if (session->nonce == nullptr) {
        NYA_AcmeHttpResponse nonce_reply = { 0 };
        NYA_TRY(_nya_acme_plain(session, NYA_ACME_METHOD_HEAD, session->new_nonce, &nonce_reply));
        if (nonce_reply.replay_nonce == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the CA's newNonce returned no Replay-Nonce");
        session->nonce = nonce_reply.replay_nonce;
    }

    NYA_Object* jws = nullptr;
    NYA_TRY(nya_acme_jws_sign(session->arena, session->config->account_key, url, session->nonce, kid, payload, &jws));

    NYA_String* body = nya_serde_json_serialize(session->arena, jws, 0);

    NYA_AcmeHttpRequest request = {
        .method    = NYA_ACME_METHOD_POST,
        .url       = url,
        .body      = body->items,
        .body_size = body->length,
    };
    nya_memset(out_response, 0, sizeof(*out_response));
    NYA_TRY(session->config->transport.perform(session->config->transport.userdata, session->arena, &request, out_response));

    // consumed: whatever the reply carried is the next request's, and the old one is spent either way.
    session->nonce = out_response->replay_nonce;
    return NYA_OK;
}

/** Fetches the directory and copies its URLs into the session. */
NYA_INTERNAL NYA_Error _nya_acme_directory(_NYA_AcmeSession* session) {
    NYA_AcmeHttpResponse response = { 0 };
    NYA_TRY(_nya_acme_plain(session, NYA_ACME_METHOD_GET, session->config->directory_url, &response));
    if (response.status != 200) return _nya_acme_problem(session->arena, &response, "fetching the directory");

    NYA_Object* directory = nullptr;
    NYA_TRY(_nya_acme_parse(session->arena, &response, "the directory", &directory));

    session->new_nonce   = _nya_acme_string(directory, "newNonce");
    session->new_account = _nya_acme_string(directory, "newAccount");
    session->new_order   = _nya_acme_string(directory, "newOrder");

    if (session->new_nonce == nullptr || session->new_account == nullptr || session->new_order == nullptr) {
        return nya_error(NYA_ERROR_PARSE, "the directory is missing newNonce, newAccount or newOrder");
    }
    return NYA_OK;
}

/** newAccount, which registers the key and gives back the account URL every later request names as kid. */
NYA_INTERNAL NYA_Error _nya_acme_account(_NYA_AcmeSession* session) {
    NYA_Object* payload = nya_object_create(session->arena);
    nya_object_add(payload, "termsOfServiceAgreed", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    if (session->config->contact_email != nullptr && session->config->contact_email[0] != '\0') {
        NYA_ArrayᐸNYA_Valueᐳ* contact = nya_array_create(session->arena, NYA_Value);
        NYA_ConstCString      mailto  = nya_string_to_cstring(session->arena, nya_string_sprintf(session->arena, "mailto:%s", session->config->contact_email));
        nya_array_add(contact, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)mailto }));
        nya_object_add(payload, "contact", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *contact });
    }

    NYA_AcmeHttpResponse response = { 0 };
    NYA_TRY(_nya_acme_post(session, session->new_account, nullptr, payload, &response));

    // 200 is an existing account, 201 a new one; both hand back the account URL in Location.
    if (response.status != 200 && response.status != 201) return _nya_acme_problem(session->arena, &response, "creating the account");
    if (response.location == nullptr) return nya_error(NYA_ERROR_NOT_OK, "newAccount returned no account URL");

    session->kid = response.location;
    return NYA_OK;
}

/** Registers and triggers the HTTP-01 challenge of one authorization, then polls it to valid. */
NYA_INTERNAL NYA_Error _nya_acme_authorization(_NYA_AcmeSession* session, NYA_ConstCString authorization_url) {
    NYA_AcmeHttpResponse response = { 0 };
    NYA_TRY(_nya_acme_post(session, authorization_url, session->kid, nullptr, &response)); // POST-as-GET
    if (response.status != 200) return _nya_acme_problem(session->arena, &response, "fetching an authorization");

    NYA_Object* authorization = nullptr;
    NYA_TRY(_nya_acme_parse(session->arena, &response, "an authorization", &authorization));

    // find the http-01 challenge among the offered ones.
    NYA_Value* challenges = nya_object_get(authorization, "challenges");
    if (challenges == nullptr || challenges->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_PARSE, "the authorization has no challenges");

    NYA_ConstCString challenge_url = nullptr;
    NYA_ConstCString token         = nullptr;
    for (u64 i = 0; i < challenges->as_array.length; i++) {
        NYA_Value* entry = &challenges->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;
        NYA_ConstCString type = _nya_acme_string(&entry->as_object, "type");
        if (type == nullptr || !nya_string_equals(type, "http-01")) continue;
        challenge_url = _nya_acme_string(&entry->as_object, "url");
        token         = _nya_acme_string(&entry->as_object, "token");
        break;
    }
    if (challenge_url == nullptr || token == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the CA offered no http-01 challenge");

    // serve the key-authorization, then tell the CA to check it (an empty object triggers the challenge).
    NYA_ConstCString key_authorization = nullptr;
    NYA_TRY(nya_acme_key_authorization(session->arena, token, session->config->account_key, &key_authorization));
    NYA_TRY(nya_acme_challenge_store_add(session->config->challenges, token, key_authorization));

    NYA_Object* trigger = nya_object_create(session->arena);
    NYA_TRY(_nya_acme_post(session, challenge_url, session->kid, trigger, &response));
    if (response.status != 200 && response.status != 202) return _nya_acme_problem(session->arena, &response, "triggering the challenge");

    // poll the authorization until it is valid or invalid.
    for (u32 poll = 0; poll < NYA_ACME_MAX_POLLS; poll++) {
        NYA_TRY(_nya_acme_post(session, authorization_url, session->kid, nullptr, &response));
        if (response.status != 200) return _nya_acme_problem(session->arena, &response, "polling an authorization");

        NYA_Object* state = nullptr;
        NYA_TRY(_nya_acme_parse(session->arena, &response, "an authorization", &state));
        NYA_ConstCString status = _nya_acme_string(state, "status");

        if (status != nullptr && nya_string_equals(status, "valid")) {
            nya_acme_challenge_store_remove(session->config->challenges, token);
            return NYA_OK;
        }
        if (status != nullptr && nya_string_equals(status, "invalid")) {
            nya_acme_challenge_store_remove(session->config->challenges, token);
            return nya_error(NYA_ERROR_NOT_OK, "the CA could not validate a domain (authorization invalid)");
        }

        u32 wait_ms = response.retry_after_seconds != 0 ? response.retry_after_seconds * 1000U : NYA_ACME_POLL_INTERVAL_MS;
        nya_os_time_sleep_ms(wait_ms);
    }

    nya_acme_challenge_store_remove(session->config->challenges, token);
    return nya_error(NYA_ERROR_TIMEOUT, "an authorization did not become valid in time");
}

#endif // NYA_MODULE_TLS

NYA_Error nya_acme_obtain(NYA_Arena* arena, const NYA_AcmeConfig* config, OUT NYA_AcmeCertificate* out_certificate) {
    nya_assert(arena != nullptr);
    nya_assert(config != nullptr);
    nya_assert(out_certificate != nullptr);
    nya_memset(out_certificate, 0, sizeof(*out_certificate));

    if (config->directory_url == nullptr || config->account_key == nullptr || config->challenges == nullptr || config->transport.perform == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a directory URL, an account key, a challenge store and a transport are all required");
    }
    if (config->domains == nullptr || config->domain_count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "at least one domain is required");
    if (config->domain_count > NYA_ACME_MAX_DOMAINS) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "too many domains for one order");
    for (u64 i = 0; i < config->domain_count; i++) {
        if (!_nya_acme_domain_ok(config->domains[i])) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a domain is empty or carries a forbidden byte");
    }

#ifndef NYA_MODULE_TLS
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "obtaining a certificate needs OpenSSL for the CSR, which this build has none of");
#else
    _NYA_AcmeSession session = { .arena = arena, .config = config };

    NYA_TRY(_nya_acme_directory(&session));
    NYA_TRY(_nya_acme_account(&session));

    // newOrder: the identifiers, each a dns type carrying one domain.
    NYA_Object*           order_payload = nya_object_create(arena);
    NYA_ArrayᐸNYA_Valueᐳ* identifiers   = nya_array_create(arena, NYA_Value);
    for (u64 i = 0; i < config->domain_count; i++) {
        NYA_Object* identifier = nya_object_create(arena);
        nya_object_add(identifier, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"dns" });
        nya_object_add(identifier, "value", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)config->domains[i] });
        nya_array_add(identifiers, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *identifier }));
    }
    nya_object_add(order_payload, "identifiers", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *identifiers });

    NYA_AcmeHttpResponse response = { 0 };
    NYA_TRY(_nya_acme_post(&session, session.new_order, session.kid, order_payload, &response));
    if (response.status != 201 && response.status != 200) return _nya_acme_problem(arena, &response, "creating the order");

    NYA_ConstCString order_url = response.location;
    if (order_url == nullptr) return nya_error(NYA_ERROR_NOT_OK, "newOrder returned no order URL");

    NYA_Object* order = nullptr;
    NYA_TRY(_nya_acme_parse(arena, &response, "the order", &order));

    NYA_ConstCString finalize_url = _nya_acme_string(order, "finalize");
    if (finalize_url == nullptr) return nya_error(NYA_ERROR_PARSE, "the order has no finalize URL");

    // each authorization, one domain at a time.
    NYA_Value* authorizations = nya_object_get(order, "authorizations");
    if (authorizations == nullptr || authorizations->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_PARSE, "the order has no authorizations");
    for (u64 i = 0; i < authorizations->as_array.length; i++) {
        NYA_Value* entry = &authorizations->as_array.items[i];
        if (entry->type != NYA_TYPE_STRING) continue;
        NYA_TRY(_nya_acme_authorization(&session, (NYA_ConstCString)entry->as_string));
    }

    // finalize: a fresh key's CSR. The key PEM is kept for the result.
    NYA_ConstCString csr_b64url = nullptr;
    NYA_String*      key_pem    = nullptr;
    NYA_TRY(_nya_acme_make_csr(arena, config->domains, config->domain_count, &csr_b64url, &key_pem));

    NYA_Object* finalize_payload = nya_object_create(arena);
    nya_object_add(finalize_payload, "csr", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)csr_b64url });
    NYA_TRY(_nya_acme_post(&session, finalize_url, session.kid, finalize_payload, &response));
    if (response.status != 200) return _nya_acme_problem(arena, &response, "finalizing the order");

    // poll the order until it is valid and names a certificate URL.
    NYA_ConstCString certificate_url = nullptr;
    for (u32 poll = 0; poll < NYA_ACME_MAX_POLLS; poll++) {
        NYA_TRY(_nya_acme_post(&session, order_url, session.kid, nullptr, &response));
        if (response.status != 200) return _nya_acme_problem(arena, &response, "polling the order");

        NYA_Object* state = nullptr;
        NYA_TRY(_nya_acme_parse(arena, &response, "the order", &state));
        NYA_ConstCString status = _nya_acme_string(state, "status");

        if (status != nullptr && nya_string_equals(status, "valid")) {
            certificate_url = _nya_acme_string(state, "certificate");
            break;
        }
        if (status != nullptr && nya_string_equals(status, "invalid")) return nya_error(NYA_ERROR_NOT_OK, "the order became invalid at finalize");

        u32 wait_ms = response.retry_after_seconds != 0 ? response.retry_after_seconds * 1000U : NYA_ACME_POLL_INTERVAL_MS;
        nya_os_time_sleep_ms(wait_ms);
    }
    if (certificate_url == nullptr) return nya_error(NYA_ERROR_TIMEOUT, "the order did not become valid in time");

    // download the chain, PEM, leaf first.
    NYA_TRY(_nya_acme_post(&session, certificate_url, session.kid, nullptr, &response));
    if (response.status != 200) return _nya_acme_problem(arena, &response, "downloading the certificate");

    out_certificate->chain_pem       = response.body;
    out_certificate->private_key_pem = key_pem;
    return NYA_OK;
#endif // NYA_MODULE_TLS
}

// RENEWAL

NYA_Error nya_acme_needs_renewal(NYA_ConstCString chain_pem, u32 renew_before_days, OUT b8* out_needs_renewal) {
    nya_assert(chain_pem != nullptr);
    nya_assert(out_needs_renewal != nullptr);
    *out_needs_renewal = false;

    if (renew_before_days == 0) renew_before_days = NYA_ACME_RENEW_BEFORE_DAYS;

#ifndef NYA_MODULE_TLS
    (void)chain_pem;
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "reading a certificate's expiry needs OpenSSL, which this build has none of");
#else
    BIO* bio = BIO_new_mem_buf(chain_pem, -1);
    if (bio == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no BIO for the certificate");

    // only the leaf, the first certificate in the chain, since that is the one that expires.
    X509* leaf = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (leaf == nullptr) return nya_error(NYA_ERROR_PARSE, "the PEM held no certificate");

    // renew when notAfter is before now + the window; X509_cmp_time returns -1 when the time is earlier.
    time_t deadline    = time(nullptr) + (time_t)renew_before_days * 24 * 60 * 60;
    int    comparison  = X509_cmp_time(X509_get0_notAfter(leaf), &deadline);
    *out_needs_renewal = comparison < 0;

    X509_free(leaf);
    return NYA_OK;
#endif // NYA_MODULE_TLS
}
