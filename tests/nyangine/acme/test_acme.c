/**
 * The ACME client, without a certificate authority.
 *
 * A live CA cannot run in the sandbox, so everything a mock can pin down is pinned here. The crypto core
 * is checked directly: a JWS is signed and then verified with the engine's own verifier — ES256 through
 * nya_crypto_ecdsa_verify_sha256, EdDSA through nya_crypto_sign_verify — which is the property a real CA
 * enforces, that a signature built wrong is rejected. The RFC 8037 Ed25519 key, its JWK and its thumbprint
 * are the published test vectors. The key-authorization, the challenge route and the directory and order
 * parsing are asserted against known inputs. Finally the whole obtain flow runs against a scripted CA that
 * *verifies every JWS it receives* — so the flow only completes if the nonce threading, the kid handling
 * and the signatures are all correct — and a tampered signature is shown to be refused.
 *
 * An end-to-end run against a real server (Pebble or Let's Encrypt staging) is what acme.h documents; it
 * needs a reachable CA and a resolvable domain, neither of which a unit test has.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

/** A self-signed P-256 certificate valid for `days_valid` days, PEM, for the renewal check. */
static NYA_String* self_signed_cert(NYA_Arena* arena, s32 days_valid) {
  EVP_PKEY* pkey = EVP_EC_gen("P-256");
  X509*     cert = X509_new();
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), (long)days_valid * 24 * 60 * 60);
  X509_set_pubkey(cert, pkey);

  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"example.com", -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_sign(cert, pkey, EVP_sha256());

  BIO*  bio  = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, cert);
  char* data = nullptr;
  long  len  = BIO_get_mem_data(bio, &data);
  NYA_String* pem = nya_string_create_with_capacity(arena, (u64)len + 1);
  for (long i = 0; i < len; i++) nya_string_push_back(pem, (u8)data[i]);

  BIO_free(bio);
  X509_free(cert);
  EVP_PKEY_free(pkey);
  return pem;
}

/* JWS HELPERS SHARED BY THE MOCK CA AND THE UNIT TESTS */

/** Decodes a base64url member of an object into `out`, returning how many bytes it held (0 on absence). */
static u64 b64url_member(const NYA_Object* object, NYA_ConstCString key, u8* out, u64 capacity) {
  if (object == nullptr) return 0;
  NYA_Value* value = nya_object_get(object, (NYA_CString)key);
  if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return 0;
  u64 size = 0;
  if (!nya_crypto_base64url_decode(value->as_string, strlen(value->as_string), out, capacity, &size)) return 0;
  return size;
}

/** The string member, or null. */
static NYA_ConstCString member(const NYA_Object* object, NYA_ConstCString key) {
  if (object == nullptr) return nullptr;
  NYA_Value* value = nya_object_get(object, (NYA_CString)key);
  if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;
  return (NYA_ConstCString)value->as_string;
}

/** The ES256 public key an account key carries, read out of its JWK: what a kid-signed JWS is verified against. */
static NYA_CryptoEcdsaPublicKey account_es256_pubkey(NYA_Arena* arena, const NYA_AcmeAccountKey* key) {
  NYA_Object* jwk = nullptr;
  nya_check(nya_acme_jwk(arena, key, &jwk).ok, "the account jwk builds");
  u8  x[64] = { 0 };
  u8  y[64] = { 0 };
  u64 xs    = b64url_member(jwk, "x", x, sizeof(x));
  u64 ys    = b64url_member(jwk, "y", y, sizeof(y));
  NYA_CryptoEcdsaPublicKey pub = { 0 };
  nya_check(nya_crypto_ecdsa_public_key_from_xy(x, xs, y, ys, &pub).ok, "the account public key reads");
  return pub;
}

/**
 * Verifies one flattened JWS as a CA would: the ES256 signature over `protected.payload`, and that the
 * protected header carries `expected_url` and `expected_nonce`. The key is the embedded JWK when the header
 * has one (newAccount), or `account` otherwise. Fills `out_header` and `out_payload` for the caller to read.
 * Returns false on any mismatch, which is what makes the flow test meaningful.
 * */
static b8 verify_jws(NYA_Arena* arena, const NYA_String* body, NYA_ConstCString expected_url, NYA_ConstCString expected_nonce,
                     const NYA_CryptoEcdsaPublicKey* account, NYA_Object** out_header, NYA_Object** out_payload) {
  *out_header  = nullptr;
  *out_payload = nullptr;

  NYA_Object* jws = nullptr;
  if (!nya_serde_json_deserialize(arena, body->items, body->length, 0, &jws).ok) return false;

  NYA_ConstCString protected_b64 = member(jws, "protected");
  NYA_ConstCString payload_b64   = member(jws, "payload");
  NYA_ConstCString signature_b64 = member(jws, "signature");
  if (protected_b64 == nullptr || payload_b64 == nullptr || signature_b64 == nullptr) return false;

  // decode the protected header.
  u8  header_bytes[2048] = { 0 };
  u64 header_size        = 0;
  if (!nya_crypto_base64url_decode(protected_b64, strlen(protected_b64), header_bytes, sizeof(header_bytes), &header_size)) return false;
  NYA_Object* header = nullptr;
  if (!nya_serde_json_deserialize(arena, header_bytes, header_size, 0, &header).ok) return false;

  // the header must name this request's url and the nonce we last issued.
  NYA_ConstCString url   = member(header, "url");
  NYA_ConstCString nonce = member(header, "nonce");
  if (url == nullptr || !nya_string_equals(url, expected_url)) return false;
  if (expected_nonce != nullptr && (nonce == nullptr || !nya_string_equals(nonce, expected_nonce))) return false;

  // the verifying key: the embedded JWK for newAccount, or the account key set from it before.
  NYA_CryptoEcdsaPublicKey key = { 0 };
  NYA_Value*               jwk = nya_object_get(header, "jwk");
  if (jwk != nullptr && jwk->type == NYA_TYPE_OBJECT) {
    u8  x[64] = { 0 };
    u8  y[64] = { 0 };
    u64 xs    = b64url_member(&jwk->as_object, "x", x, sizeof(x));
    u64 ys    = b64url_member(&jwk->as_object, "y", y, sizeof(y));
    if (xs == 0 || ys == 0) return false;
    if (!nya_crypto_ecdsa_public_key_from_xy(x, xs, y, ys, &key).ok) return false;
  } else if (account != nullptr) {
    key = *account;
  } else {
    return false;
  }

  // the signing input is exactly the two base64url strings joined by a dot.
  NYA_String* signing_input = nya_string_sprintf(arena, "%s.%s", protected_b64, payload_b64);

  u8  signature[64] = { 0 };
  u64 signature_size = 0;
  if (!nya_crypto_base64url_decode(signature_b64, strlen(signature_b64), signature, sizeof(signature), &signature_size)) return false;

  if (!nya_crypto_ecdsa_verify_sha256(&key, signing_input->items, signing_input->length, signature, signature_size)) return false;

  // the payload, empty for a POST-as-GET.
  if (strlen(payload_b64) == 0) {
    *out_payload = nya_object_create(arena);
  } else {
    u8  payload_bytes[4096] = { 0 };
    u64 payload_size        = 0;
    if (!nya_crypto_base64url_decode(payload_b64, strlen(payload_b64), payload_bytes, sizeof(payload_bytes), &payload_size)) return false;
    if (!nya_serde_json_deserialize(arena, payload_bytes, payload_size, 0, out_payload).ok) return false;
  }

  *out_header = header;
  return true;
}

/* A SCRIPTED CA THAT VERIFIES EVERY SIGNATURE */

#define CA_DIR      "https://ca.test/dir"
#define CA_NONCE    "https://ca.test/new-nonce"
#define CA_ACCOUNT  "https://ca.test/new-account"
#define CA_ORDER    "https://ca.test/new-order"
#define CA_ACCT_URL "https://ca.test/acct/1"
#define CA_ORDER_URL "https://ca.test/order/1"
#define CA_AUTHZ    "https://ca.test/authz/1"
#define CA_CHALLENGE "https://ca.test/chall/1"
#define CA_FINALIZE "https://ca.test/finalize/1"
#define CA_CERT     "https://ca.test/cert/1"

typedef struct {
  NYA_Arena* arena;

  u32              nonce_counter;
  NYA_ConstCString issued_nonce; // the last nonce handed out; the next POST must carry it.

  b8                       has_account;
  NYA_CryptoEcdsaPublicKey account; // set from the newAccount JWK, reused for kid requests.

  u32 authz_polls; // so the first poll is pending and the second valid, exercising the poll loop.
  u32 order_polls;

  b8 saw_bad_signature; // set if any JWS failed to verify: the flow must never succeed then.
  u32 requests;
} MockCa;

/** Hands out a fresh nonce and records it as the one the next POST must carry. */
static NYA_ConstCString issue_nonce(MockCa* ca) {
  ca->issued_nonce = nya_string_to_cstring(ca->arena, nya_string_sprintf(ca->arena, "nonce-%u", ca->nonce_counter++));
  return ca->issued_nonce;
}

/** A JSON reply with a status, a fresh nonce, and optional Location. */
static NYA_Error reply(MockCa* ca, NYA_AcmeHttpResponse* out, u16 status, NYA_ConstCString json, NYA_ConstCString location) {
  nya_memset(out, 0, sizeof(*out));
  out->status       = status;
  out->replay_nonce = issue_nonce(ca);
  out->location     = location;
  out->content_type = "application/json";
  out->body         = nya_string_create(ca->arena);
  if (json != nullptr) nya_string_extend(out->body, json);
  return NYA_OK;
}

static NYA_Error mock_perform(void* userdata, NYA_Arena* arena, const NYA_AcmeHttpRequest* request, NYA_AcmeHttpResponse* out) {
  MockCa* ca = (MockCa*)userdata;
  ca->requests++;

  // the directory: unsigned GET.
  if (request->method == NYA_ACME_METHOD_GET && nya_string_equals(request->url, CA_DIR)) {
    return reply(ca, out, 200,
                 "{\"newNonce\":\"" CA_NONCE "\",\"newAccount\":\"" CA_ACCOUNT "\",\"newOrder\":\"" CA_ORDER "\"}", nullptr);
  }

  // newNonce: unsigned HEAD.
  if (request->method == NYA_ACME_METHOD_HEAD && nya_string_equals(request->url, CA_NONCE)) {
    return reply(ca, out, 200, nullptr, nullptr);
  }

  // every other request is a signed POST: verify it before answering.
  NYA_String* body = nya_string_create(arena);
  for (u64 i = 0; i < request->body_size; i++) nya_string_push_back(body, request->body[i]);

  NYA_Object* header  = nullptr;
  NYA_Object* payload = nullptr;
  b8          ok      = verify_jws(arena, body, request->url, ca->issued_nonce, ca->has_account ? &ca->account : nullptr, &header, &payload);
  if (!ok) {
    ca->saw_bad_signature = true;
    return reply(ca, out, 400, "{\"type\":\"urn:ietf:params:acme:error:malformed\",\"detail\":\"bad JWS\"}", nullptr);
  }

  // newAccount: remember the JWK's key for the kid requests that follow.
  if (nya_string_equals(request->url, CA_ACCOUNT)) {
    NYA_Value* jwk = nya_object_get(header, "jwk");
    if (jwk != nullptr && jwk->type == NYA_TYPE_OBJECT) {
      u8  x[64] = { 0 };
      u8  y[64] = { 0 };
      u64 xs    = b64url_member(&jwk->as_object, "x", x, sizeof(x));
      u64 ys    = b64url_member(&jwk->as_object, "y", y, sizeof(y));
      if (nya_crypto_ecdsa_public_key_from_xy(x, xs, y, ys, &ca->account).ok) ca->has_account = true;
    }
    return reply(ca, out, 201, "{\"status\":\"valid\"}", CA_ACCT_URL);
  }

  if (nya_string_equals(request->url, CA_ORDER)) {
    return reply(ca, out, 201,
                 "{\"status\":\"pending\",\"authorizations\":[\"" CA_AUTHZ "\"],\"finalize\":\"" CA_FINALIZE "\"}", CA_ORDER_URL);
  }

  if (nya_string_equals(request->url, CA_AUTHZ)) {
    // first fetch describes the challenge; later fetches are the poll, pending then valid.
    if (ca->authz_polls++ == 0) {
      return reply(ca, out, 200,
                   "{\"status\":\"pending\",\"challenges\":[{\"type\":\"http-01\",\"url\":\"" CA_CHALLENGE "\",\"token\":\"tok-abc\"}]}", nullptr);
    }
    NYA_ConstCString status = ca->authz_polls >= 2 ? "valid" : "pending";
    return reply(ca, out, 200, nya_string_to_cstring(ca->arena, nya_string_sprintf(ca->arena, "{\"status\":\"%s\"}", status)), nullptr);
  }

  if (nya_string_equals(request->url, CA_CHALLENGE)) {
    return reply(ca, out, 200, "{\"status\":\"pending\"}", nullptr);
  }

  if (nya_string_equals(request->url, CA_FINALIZE)) {
    // the payload must carry a CSR.
    if (member(payload, "csr") == nullptr) { ca->saw_bad_signature = true; return reply(ca, out, 400, "{\"detail\":\"no csr\"}", nullptr); }
    return reply(ca, out, 200, "{\"status\":\"processing\"}", CA_ORDER_URL);
  }

  if (nya_string_equals(request->url, CA_ORDER_URL)) {
    NYA_ConstCString json = ca->order_polls++ == 0 ? "{\"status\":\"processing\"}" : "{\"status\":\"valid\",\"certificate\":\"" CA_CERT "\"}";
    return reply(ca, out, 200, json, nullptr);
  }

  if (nya_string_equals(request->url, CA_CERT)) {
    reply(ca, out, 200, "-----BEGIN CERTIFICATE-----\nMIIB...fake...chain\n-----END CERTIFICATE-----\n", nullptr);
    return NYA_OK;
  }

  return reply(ca, out, 404, "{\"detail\":\"no such resource\"}", nullptr);
}

/* MAIN */

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_acme");
  defer      nya_arena_destroy(arena);

  // TEST: an ES256 JWS signs, and its signature verifies with the engine's verifier.
  {
    NYA_AcmeAccountKey* key = nullptr;
    nya_check(nya_acme_account_key_create(arena, NYA_ACME_ALGORITHM_ES256, &key).ok, "an ES256 account key generates");
    defer nya_acme_account_key_destroy(key);

    NYA_Object* payload = nya_object_create(arena);
    nya_object_add(payload, "hello", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)"world" });

    NYA_Object* jws = nullptr;
    nya_check(nya_acme_jws_sign(arena, key, "https://ca.test/new-order", "abc123", "https://ca.test/acct/1", payload, &jws).ok, "a JWS signs");

    NYA_String* body = nya_serde_json_serialize(arena, jws, 0);

    NYA_CryptoEcdsaPublicKey account = account_es256_pubkey(arena, key);
    NYA_Object* header  = nullptr;
    NYA_Object* decoded = nullptr;
    b8 verified = verify_jws(arena, body, "https://ca.test/new-order", "abc123", &account, &header, &decoded);
    nya_check(verified, "the ES256 JWS verifies with nya_crypto_ecdsa_verify_sha256");

    nya_check(nya_string_equals(member(header, "alg"), "ES256"), "the protected header names ES256");
    nya_check(nya_string_equals(member(header, "kid"), "https://ca.test/acct/1"), "a kid request carries the kid and no jwk");
    nya_check(nya_object_get(header, "jwk") == nullptr, "a kid request does not embed the jwk");
    nya_check(nya_string_equals(member(decoded, "hello"), "world"), "the payload round-trips");

    // a newAccount JWS embeds the jwk instead of a kid.
    NYA_Object* jws2 = nullptr;
    nya_check(nya_acme_jws_sign(arena, key, "https://ca.test/new-account", "n2", nullptr, payload, &jws2).ok, "a newAccount JWS signs");
    NYA_String* body2 = nya_serde_json_serialize(arena, jws2, 0);
    NYA_Object* header2 = nullptr;
    NYA_Object* decoded2 = nullptr;
    nya_check(verify_jws(arena, body2, "https://ca.test/new-account", "n2", nullptr, &header2, &decoded2), "the newAccount JWS verifies via its embedded jwk");
    nya_check(nya_object_get(header2, "jwk") != nullptr, "newAccount embeds the jwk");
    nya_check(member(header2, "kid") == nullptr, "newAccount carries no kid");
  }

  // TEST: a tampered ES256 signature is refused — the property a CA relies on.
  {
    NYA_AcmeAccountKey* key = nullptr;
    nya_check(nya_acme_account_key_create(arena, NYA_ACME_ALGORITHM_ES256, &key).ok, "a key generates");
    defer nya_acme_account_key_destroy(key);

    // a newAccount-style JWS (kid null) so the embedded jwk is the verifying key and the flipped bit is the only thing wrong.
    NYA_Object* jws = nullptr;
    nya_check(nya_acme_jws_sign(arena, key, "https://ca.test/x", "nonce", nullptr, nullptr, &jws).ok, "a JWS signs");

    // flip one bit of the signature and show it no longer verifies.
    NYA_Value* signature = nya_object_get(jws, "signature");
    nya_check(signature != nullptr && signature->type == NYA_TYPE_STRING, "the JWS has a signature");
    char* s = signature->as_string;
    s[0]    = (s[0] == 'A') ? 'B' : 'A';

    NYA_String* body    = nya_serde_json_serialize(arena, jws, 0);
    NYA_Object* header  = nullptr;
    NYA_Object* decoded = nullptr;
    nya_check(!verify_jws(arena, body, "https://ca.test/x", "nonce", nullptr, &header, &decoded), "a tampered signature is refused");
  }

  // TEST: an EdDSA JWS signs and verifies with the engine's own Ed25519 verifier.
  {
    NYA_AcmeAccountKey* key = nullptr;
    nya_check(nya_acme_account_key_create(arena, NYA_ACME_ALGORITHM_EDDSA, &key).ok, "an EdDSA account key generates");
    defer nya_acme_account_key_destroy(key);

    // kid null, so the jwk is embedded and is the key we verify against.
    NYA_Object* jws = nullptr;
    nya_check(nya_acme_jws_sign(arena, key, "https://ca.test/o", "eddsa-nonce", nullptr, nullptr, &jws).ok, "an EdDSA JWS signs");

    // reconstruct the signing input and verify with crypto_sign against the public key from the jwk.
    NYA_ConstCString protected_b64 = member(jws, "protected");
    NYA_ConstCString payload_b64   = member(jws, "payload");
    NYA_ConstCString signature_b64 = member(jws, "signature");
    NYA_String*      signing_input = nya_string_sprintf(arena, "%s.%s", protected_b64, payload_b64);

    u8  header_bytes[1024] = { 0 };
    u64 header_size        = 0;
    nya_check(nya_crypto_base64url_decode(protected_b64, strlen(protected_b64), header_bytes, sizeof(header_bytes), &header_size), "the header decodes");
    NYA_Object* header = nullptr;
    nya_check(nya_serde_json_deserialize(arena, header_bytes, header_size, 0, &header).ok, "the header parses");
    nya_check(nya_string_equals(member(header, "alg"), "EdDSA"), "the alg is EdDSA");

    NYA_Value* jwk = nya_object_get(header, "jwk");
    nya_check(jwk != nullptr && jwk->type == NYA_TYPE_OBJECT, "the jwk is embedded");
    nya_check(nya_string_equals(member(&jwk->as_object, "crv"), "Ed25519"), "the curve is Ed25519");
    nya_check(nya_string_equals(member(&jwk->as_object, "kty"), "OKP"), "the key type is OKP");

    NYA_CryptoSignPublicKey pub = { 0 };
    nya_check(b64url_member(&jwk->as_object, "x", pub.bytes, sizeof(pub.bytes)) == sizeof(pub.bytes), "the public key decodes to 32 bytes");

    NYA_CryptoSignature signature = { 0 };
    nya_check(b64url_member(jws, "signature", signature.bytes, sizeof(signature.bytes)) == sizeof(signature.bytes), "the signature decodes to 64 bytes");
    (void)signature_b64;

    nya_check(nya_crypto_sign_verify(&pub, signing_input->items, signing_input->length, &signature), "the EdDSA JWS verifies with nya_crypto_sign_verify");
  }

  // TEST: the RFC 8037 Ed25519 key, its JWK and its thumbprint (known-answer vectors).
  {
    // RFC 8037 Appendix A.1: the private seed d and the public key x.
    NYA_ConstCString d_b64 = "nWGxne_9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A";
    NYA_ConstCString x_b64 = "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo";
    // RFC 8037 Appendix A.3: the JWK thumbprint of that key.
    NYA_ConstCString thumbprint_b64 = "kPrK_qmxVWaYVA9wwBF6Iuo3vVzz7TxHCTwXBygrS4k";

    u8  seed[32] = { 0 };
    u64 seed_size = 0;
    nya_check(nya_crypto_base64url_decode(d_b64, strlen(d_b64), seed, sizeof(seed), &seed_size) && seed_size == 32, "the seed decodes");

    NYA_AcmeAccountKey* key = nullptr;
    nya_check(nya_acme_account_key_from_eddsa_seed(arena, seed, &key).ok, "the key builds from the seed");
    defer nya_acme_account_key_destroy(key);

    NYA_Object* jwk = nullptr;
    nya_check(nya_acme_jwk(arena, key, &jwk).ok, "the jwk builds");
    nya_check(nya_string_equals(member(jwk, "x"), x_b64), "the jwk x matches the RFC 8037 public key");

    NYA_CryptoSha256Digest thumbprint = { 0 };
    nya_check(nya_acme_jwk_thumbprint(key, &thumbprint).ok, "the thumbprint computes");

    char b64[64] = { 0 };
    u64  b64_size = 0;
    nya_check(nya_crypto_base64url_encode(thumbprint.bytes, sizeof(thumbprint.bytes), b64, sizeof(b64), &b64_size), "the thumbprint encodes");
    nya_check(nya_string_equals((NYA_ConstCString)b64, thumbprint_b64), "the thumbprint matches the RFC 8037 vector");
  }

  // TEST: the HTTP-01 key-authorization is token "." base64url(SHA-256(thumbprint input)).
  {
    NYA_AcmeAccountKey* key = nullptr;
    nya_check(nya_acme_account_key_create(arena, NYA_ACME_ALGORITHM_EDDSA, &key).ok, "a key generates");
    defer nya_acme_account_key_destroy(key);

    NYA_ConstCString key_auth = nullptr;
    nya_check(nya_acme_key_authorization(arena, "tok-abc", key, &key_auth).ok, "a key-authorization computes");

    // it is exactly token, a dot, then the base64url of the thumbprint digest.
    NYA_CryptoSha256Digest thumbprint = { 0 };
    nya_check(nya_acme_jwk_thumbprint(key, &thumbprint).ok, "the thumbprint computes");
    char digest_b64[64] = { 0 };
    u64  n = 0;
    nya_crypto_base64url_encode(thumbprint.bytes, sizeof(thumbprint.bytes), digest_b64, sizeof(digest_b64), &n);
    NYA_ConstCString expected = nya_string_to_cstring(arena, nya_string_sprintf(arena, "tok-abc.%s", digest_b64));
    nya_check(nya_string_equals(key_auth, expected), "the key-authorization is token.base64url(SHA-256(thumbprint))");
    nya_check(nya_string_count(nya_string_from(arena, key_auth), ".") == 1, "it has exactly one dot");

    // a different token gives a different authorization.
    NYA_ConstCString other = nullptr;
    nya_check(nya_acme_key_authorization(arena, "tok-xyz", key, &other).ok, "a second key-authorization computes");
    nya_check(!nya_string_equals(key_auth, other), "a different token gives a different key-authorization");
  }

  // TEST: the challenge route serves the key-authorization for the right path only.
  {
    NYA_AcmeChallengeStore* store = nya_acme_challenge_store_create(arena);
    nya_check(nya_acme_challenge_store_add(store, "tok-abc", "tok-abc.keyauth").ok, "a challenge registers");

    NYA_ConstCString body = nullptr;
    nya_check(nya_acme_challenge_response(store, "/.well-known/acme-challenge/tok-abc", &body), "the right path is answered");
    nya_check(nya_string_equals(body, "tok-abc.keyauth"), "and the body is the key-authorization");

    nya_check(!nya_acme_challenge_response(store, "/.well-known/acme-challenge/tok-unknown", &body), "an unknown token is not answered");
    nya_check(!nya_acme_challenge_response(store, "/index.html", &body), "a path outside the prefix is not answered");
    nya_check(!nya_acme_challenge_response(store, "/.well-known/acme-challenge/tok-abc/extra", &body), "a token with a further segment is not answered");

    nya_acme_challenge_store_remove(store, "tok-abc");
    nya_check(!nya_acme_challenge_response(store, "/.well-known/acme-challenge/tok-abc", &body), "a removed challenge is not answered");

    nya_check(nya_acme_challenge_store_add(store, "tok-1", "tok-1.auth").ok, "another challenge registers");
    nya_acme_challenge_store_destroy(store);
    nya_check(!nya_acme_challenge_response(store, "/.well-known/acme-challenge/tok-1", &body), "destroying the store forgets every challenge");
  }

  // TEST: the renewal check reads the leaf's expiry against the window.
  {
    b8 needs = false;

    // a certificate with 10 days left needs renewal at the 30 day window; one with a year does not.
    NYA_String* soon = self_signed_cert(arena, 10);
    nya_check(nya_acme_needs_renewal(nya_string_to_cstring(arena, soon), 30, &needs).ok, "the soon-to-expire cert parses");
    nya_check(needs, "a certificate inside the renewal window needs renewal");

    NYA_String* later = self_signed_cert(arena, 365);
    nya_check(nya_acme_needs_renewal(nya_string_to_cstring(arena, later), 30, &needs).ok, "the long-lived cert parses");
    nya_check(!needs, "a certificate outside the window does not need renewal");

    // a default window (0 => NYA_ACME_RENEW_BEFORE_DAYS) still reads.
    nya_check(nya_acme_needs_renewal(nya_string_to_cstring(arena, later), 0, &needs).ok, "the default window reads");
    nya_check(!needs, "and a long-lived cert is fine under the default window");

    // garbage is a parse error, not a crash.
    NYA_Error parse = nya_acme_needs_renewal("not a certificate", 30, &needs);
    nya_check(!parse.ok && parse.kind == NYA_ERROR_PARSE, "a non-certificate is a parse error");
  }

  // TEST: the whole obtain flow against a CA that verifies every signature.
  {
    NYA_AcmeAccountKey* key = nullptr;
    nya_check(nya_acme_account_key_create(arena, NYA_ACME_ALGORITHM_ES256, &key).ok, "the account key generates");
    defer nya_acme_account_key_destroy(key);
    nya_check(nya_acme_account_key_algorithm(key) == NYA_ACME_ALGORITHM_ES256, "the key reports its algorithm");

    MockCa ca = { .arena = arena };

    NYA_AcmeChallengeStore* store = nya_acme_challenge_store_create(arena);

    NYA_ConstCString domains[] = { "example.com", "www.example.com" };
    NYA_AcmeConfig   config    = {
      .directory_url = CA_DIR,
      .domains       = domains,
      .domain_count  = nya_carray_length(domains),
      .account_key   = key,
      .contact_email = "admin@example.com",
      .challenges    = store,
      .transport     = { .perform = mock_perform, .userdata = &ca },
    };

    NYA_AcmeCertificate certificate = { 0 };
    NYA_Error           result      = nya_acme_obtain(arena, &config, &certificate);
    nya_check(result.ok, "the obtain flow completes: %s", (NYA_ConstCString)result.message);
    nya_check(!ca.saw_bad_signature, "every JWS the CA received verified");
    nya_check(certificate.chain_pem != nullptr && nya_string_contains(certificate.chain_pem, "BEGIN CERTIFICATE"), "a certificate chain came back");
    nya_check(certificate.private_key_pem != nullptr && nya_string_contains(certificate.private_key_pem, "PRIVATE KEY"), "a private key came back");
    nya_check(ca.has_account, "the CA registered the account from the embedded jwk");
  }

  // TEST: obtain refuses a config missing a required field before any request.
  {
    NYA_AcmeCertificate certificate = { 0 };
    NYA_AcmeConfig      config      = { .directory_url = CA_DIR }; // no domains, key, store or transport
    NYA_Error           refused     = nya_acme_obtain(arena, &config, &certificate);
    nya_check(!refused.ok && refused.kind == NYA_ERROR_INVALID_ARGUMENT, "a config missing fields is refused");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
