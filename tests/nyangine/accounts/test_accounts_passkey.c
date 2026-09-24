/**
 * Passkeys (WebAuthn) as a second factor: a credential enrolled, an assertion verified, and every way
 * an assertion is not one.
 *
 * There is no browser or authenticator here, so the test is its own: it generates an Ed25519 key with
 * monocypher, hand-builds the exact clientDataJSON, authenticatorData and attestationObject a real
 * authenticator would, and signs the assertion with the key it made. That is enough to drive the whole
 * relying-party path — the CBOR reader, the COSE key parse, the origin and RP id hash checks, the
 * signature verify and the counter — over inputs it controls to the byte, including malformed ones the
 * parser must refuse rather than read past.
 *
 * Everything runs against an in-memory database of its own.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <string.h>

#define PASSWORD "a correct horse battery staple"

#define RP_ID  "example.com"
#define ORIGIN "https://example.com"

/* A TINY CBOR WRITER, ENOUGH TO BUILD WHAT AN AUTHENTICATOR SENDS */

typedef struct {
  u8 *bytes;
  u64 length;
  u64 capacity;
} Buffer;

static void put_byte(Buffer *buffer, u8 byte) {
  nya_assert(buffer->length < buffer->capacity);
  buffer->bytes[buffer->length++] = byte;
}

static void put_bytes(Buffer *buffer, const u8 *data, u64 size) {
  for (u64 index = 0; index < size; index++) put_byte(buffer, data[index]);
}

/** A CBOR item head: the major type, and its argument in the smallest width that holds it. */
static void cbor_head(Buffer *buffer, u8 major, u64 argument) {
  u8 tag = (u8)(major << 5);

  if (argument < 24) {
    put_byte(buffer, (u8)(tag | (u8)argument));
  } else if (argument <= 0xFF) {
    put_byte(buffer, (u8)(tag | 24));
    put_byte(buffer, (u8)argument);
  } else if (argument <= 0xFFFF) {
    put_byte(buffer, (u8)(tag | 25));
    put_byte(buffer, (u8)(argument >> 8));
    put_byte(buffer, (u8)argument);
  } else {
    put_byte(buffer, (u8)(tag | 26));
    put_byte(buffer, (u8)(argument >> 24));
    put_byte(buffer, (u8)(argument >> 16));
    put_byte(buffer, (u8)(argument >> 8));
    put_byte(buffer, (u8)argument);
  }
}

static void cbor_uint(Buffer *buffer, u64 value) { cbor_head(buffer, 0, value); }

/** A negative integer of the given positive magnitude: -8 is written as cbor_nint(buffer, 8). */
static void cbor_nint(Buffer *buffer, u64 magnitude) { cbor_head(buffer, 1, magnitude - 1); }

static void cbor_bytes(Buffer *buffer, const u8 *data, u64 size) {
  cbor_head(buffer, 2, size);
  put_bytes(buffer, data, size);
}

static void cbor_text(Buffer *buffer, const char *text) {
  u64 size = strlen(text);
  cbor_head(buffer, 3, size);
  put_bytes(buffer, (const u8 *)text, size);
}

static void cbor_map(Buffer *buffer, u64 pairs) { cbor_head(buffer, 5, pairs); }

/* BUILDERS FOR THE THREE STRUCTURES A RELYING PARTY IS HANDED */

/** A COSE_Key map for an Ed25519 public key: kty OKP, alg EdDSA, crv Ed25519, x the 32 key bytes. */
static void put_cose_ed25519(Buffer *buffer, const u8 public_key[32]) {
  cbor_map(buffer, 4);
  cbor_uint(buffer, 1); cbor_uint(buffer, 1);   // kty = OKP
  cbor_uint(buffer, 3); cbor_nint(buffer, 8);   // alg = -8 (EdDSA)
  cbor_nint(buffer, 1); cbor_uint(buffer, 6);   // crv (label -1) = Ed25519
  cbor_nint(buffer, 2); cbor_bytes(buffer, public_key, 32); // x (label -2)
}

/** A COSE_Key map for an ES256 key, which this build must refuse: kty EC2, alg ES256, crv P-256, x and y. */
static void put_cose_es256(Buffer *buffer, const u8 xy[64]) {
  cbor_map(buffer, 5);
  cbor_uint(buffer, 1); cbor_uint(buffer, 2);   // kty = EC2
  cbor_uint(buffer, 3); cbor_nint(buffer, 7);   // alg = -7 (ES256)
  cbor_nint(buffer, 1); cbor_uint(buffer, 1);   // crv (label -1) = P-256
  cbor_nint(buffer, 2); cbor_bytes(buffer, xy, 32);       // x (label -2)
  cbor_nint(buffer, 3); cbor_bytes(buffer, xy + 32, 32);  // y (label -3)
}

/**
 * The authenticator data: the RP id hash, a flags byte, a big-endian counter, and — when a credential is
 * carried — the AAGUID, the credential id, and the COSE public key.
 *
 * `rp_id_for_hash` is what the hash is taken over, kept separate so a test can sign over a hash of the
 * wrong domain. `cose` may be null for an assertion, which carries no credential.
 */
static u64 build_authenticator_data(
    Buffer *out, const char *rp_id_for_hash, u8 flags, u32 sign_count, const u8 *credential_id, u64 credential_id_length, const Buffer *cose
) {
  NYA_CryptoSha256Digest hash = {0};
  nya_crypto_sha256((const u8 *)rp_id_for_hash, strlen(rp_id_for_hash), &hash);
  put_bytes(out, hash.bytes, 32);

  put_byte(out, flags);

  put_byte(out, (u8)(sign_count >> 24));
  put_byte(out, (u8)(sign_count >> 16));
  put_byte(out, (u8)(sign_count >> 8));
  put_byte(out, (u8)sign_count);

  if (cose != nullptr) {
    u8 aaguid[16] = {0};
    put_bytes(out, aaguid, sizeof(aaguid));

    put_byte(out, (u8)(credential_id_length >> 8));
    put_byte(out, (u8)credential_id_length);
    put_bytes(out, credential_id, credential_id_length);

    put_bytes(out, cose->bytes, cose->length);
  }

  return out->length;
}

/** An attestation object with the "none" format: fmt, the authenticator data, and an empty statement. */
static void build_attestation_object(Buffer *out, const Buffer *authenticator_data) {
  cbor_map(out, 3);
  cbor_text(out, "fmt");     cbor_text(out, "none");
  cbor_text(out, "authData"); cbor_bytes(out, authenticator_data->bytes, authenticator_data->length);
  cbor_text(out, "attStmt"); cbor_map(out, 0);
}

/** A clientDataJSON. The challenge and origin have no JSON-special characters, so no escaping is needed. */
static u64 build_client_data(char *out, u64 capacity, const char *type, const char *challenge, const char *origin) {
  s32 written = snprintf(out, capacity, "{\"type\":\"%s\",\"challenge\":\"%s\",\"origin\":\"%s\"}", type, challenge, origin);
  nya_assert(written > 0 && (u64)written < capacity);
  return (u64)written;
}


static NYA_Database *open_accounts(NYA_Arena *arena) {
  nya_account_throttle_reset();

  NYA_Database *database = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &database));
  NYA_EXPECT(nya_accounts_open(arena, database));
  return database;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena *arena = nya_arena_create(.name = "test_accounts_passkey");
  defer nya_arena_destroy(arena);

  // A fixed seed makes the whole test deterministic: the same key, the same public bytes, every run.
  NYA_CryptoKey32 seed = {0};
  for (u32 index = 0; index < 32; index++) seed.bytes[index] = (u8)(index + 1);

  NYA_CryptoSignKeyPair key_pair = {0};
  nya_crypto_sign_key_pair_from_seed(&seed, &key_pair);

  // A credential id an authenticator might mint; its exact value does not matter, only that it round-trips.
  u8 credential_id[16] = {0};
  for (u32 index = 0; index < sizeof(credential_id); index++) credential_id[index] = (u8)(0xA0 + index);

  char credential_id_b64[NYA_ACCOUNTS_PASSKEY_CRED_ID_TEXT] = {0};
  {
    u64 length = 0;
    nya_check(nya_crypto_base64url_encode(credential_id, sizeof(credential_id), credential_id_b64, sizeof(credential_id_b64), &length), "the credential id encodes");
  }

  Buffer cose = { .bytes = (u8[512]){0}, .length = 0, .capacity = 512 };
  put_cose_ed25519(&cose, key_pair.public_key.bytes);

  // TEST: everything answers cleanly with the tables closed
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    nya_check(!nya_account_passkey_register_begin(arena, 1, &challenge).ok, "no challenge without the tables");
  }

  NYA_Database *database = open_accounts(arena);
  defer nya_accounts_close();
  defer nya_sql_close(database);

  NYA_AccountUser user = {0};
  NYA_EXPECT(nya_account_create(arena, "ada", PASSWORD, &user));

  // TEST: registration stores the Ed25519 public key
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_register_begin(arena, user.id, &challenge));
    nya_check(strlen(challenge.challenge) > 0, "a registration challenge is minted");

    Buffer authenticator_data = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x41 /* UP | AT */, 0, credential_id, sizeof(credential_id), &cose);

    Buffer attestation = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_attestation_object(&attestation, &authenticator_data);

    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.create", challenge.challenge, ORIGIN);

    NYA_AccountPasskey stored = {0};
    NYA_AccountPasskeyRegistration request = {
        .rp_id = RP_ID, .origin = ORIGIN,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .attestation_object = attestation.bytes, .attestation_object_size = attestation.length,
        .name = "a security key",
    };
    NYA_Error result = nya_account_passkey_register_finish(arena, user.id, &request, &stored);
    nya_check(result.ok, "a well-formed registration is accepted");
    nya_check(stored.algorithm == NYA_ACCOUNTS_PASSKEY_COSE_ALG_EDDSA, "the stored algorithm is EdDSA");
    nya_check(nya_string_equals(stored.credential_id, credential_id_b64), "the credential id round-trips");
    nya_check(stored.sign_count == 0, "the initial counter is what the authenticator reported");

    b8 has = false;
    NYA_EXPECT(nya_account_passkey_has(arena, user.id, &has));
    nya_check(has, "the user now has a passkey to require");

    NYA_AccountPasskey *list = nullptr;
    u32 count = 0;
    NYA_EXPECT(nya_account_passkey_list(arena, user.id, &list, &count));
    nya_check(count == 1, "the credential is listed once");
  }

  // TEST: a re-registration of the same credential id is refused
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_register_begin(arena, user.id, &challenge));

    Buffer authenticator_data = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x41, 0, credential_id, sizeof(credential_id), &cose);
    Buffer attestation = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_attestation_object(&attestation, &authenticator_data);
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.create", challenge.challenge, ORIGIN);

    NYA_AccountPasskeyRegistration request = {
        .rp_id = RP_ID, .origin = ORIGIN, .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .attestation_object = attestation.bytes, .attestation_object_size = attestation.length,
    };
    NYA_Error result = nya_account_passkey_register_finish(arena, user.id, &request, nullptr);
    nya_check(result.kind == NYA_ERROR_ALREADY_EXISTS, "a duplicate credential id is refused");
  }

  // TEST: a valid assertion verifies, and the counter climbs
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x01 /* UP */, 1, nullptr, 0, nullptr);

    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, ORIGIN);

    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);

    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);

    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    NYA_AccountPasskey verified = {0};
    NYA_Error result = nya_account_passkey_assert_finish(arena, user.id, &request, &verified);
    nya_check(result.ok, "a valid assertion verifies");
    nya_check(verified.sign_count == 1, "the counter moved to what the authenticator reported");
  }

  // TEST: a wrong signature is refused (and the counter does not move)
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x01, 5, nullptr, 0, nullptr);

    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, ORIGIN);

    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);
    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);

    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);
    signature.bytes[0] ^= 0xFF; // one flipped byte

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    NYA_Error result = nya_account_passkey_assert_finish(arena, user.id, &request, nullptr);
    nya_check(!result.ok, "a wrong signature is refused");

    NYA_AccountPasskey *list = nullptr;
    u32 count = 0;
    NYA_EXPECT(nya_account_passkey_list(arena, user.id, &list, &count));
    nya_check(count == 1 && list[0].sign_count == 1, "a refused assertion left the counter where it was");
  }

  // TEST: a mismatched challenge, origin, and RP id hash are each refused
  {
    // A syntactically valid but never-issued challenge.
    const char *bogus = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x01, 9, nullptr, 0, nullptr);

    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", bogus, ORIGIN);
    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);
    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);
    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    nya_check(!nya_account_passkey_assert_finish(arena, user.id, &request, nullptr).ok, "a challenge this server never issued is refused");
  }
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x01, 9, nullptr, 0, nullptr);
    // The clientDataJSON claims an origin that is not the relying party's.
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, "https://evil.example");
    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);
    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);
    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    nya_check(!nya_account_passkey_assert_finish(arena, user.id, &request, nullptr).ok, "a mismatched origin is refused");
  }
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    // The authenticator data hashes the wrong domain, so its RP id hash will not match example.com's.
    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, "evil.example", 0x01, 9, nullptr, 0, nullptr);
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, ORIGIN);
    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);
    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);
    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    nya_check(!nya_account_passkey_assert_finish(arena, user.id, &request, nullptr).ok, "a mismatched RP id hash is refused");
  }

  // TEST: a clear user-present flag is refused, even with a valid signature
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x00 /* no UP */, 9, nullptr, 0, nullptr);
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, ORIGIN);
    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);
    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);
    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    nya_check(!nya_account_passkey_assert_finish(arena, user.id, &request, nullptr).ok, "an assertion with no user present is refused");
  }

  // TEST: a counter that did not climb is refused as a clone
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    // The stored counter is 1 from the valid assertion above; reporting 1 again is a clone or a replay.
    Buffer authenticator_data = { .bytes = (u8[256]){0}, .length = 0, .capacity = 256 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x01, 1, nullptr, 0, nullptr);
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, ORIGIN);
    NYA_CryptoSha256Digest client_hash = {0};
    nya_crypto_sha256((const u8 *)client_data, client_data_size, &client_hash);
    u8 message[512] = {0};
    nya_memcpy(message, authenticator_data.bytes, authenticator_data.length);
    nya_memcpy(message + authenticator_data.length, client_hash.bytes, 32);
    NYA_CryptoSignature signature = {0};
    nya_crypto_sign(&key_pair.secret_key, message, authenticator_data.length + 32, &signature);

    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = authenticator_data.bytes, .authenticator_data_size = authenticator_data.length,
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    nya_check(!nya_account_passkey_assert_finish(arena, user.id, &request, nullptr).ok, "a non-incrementing counter is refused");
  }

  // TEST: an ES256 (-7) credential is refused as unsupported, not mis-verified
  {
    NYA_AccountUser bob = {0};
    NYA_EXPECT(nya_account_create(arena, "bob", PASSWORD, &bob));

    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_register_begin(arena, bob.id, &challenge));

    u8 xy[64] = {0};
    for (u32 index = 0; index < sizeof(xy); index++) xy[index] = (u8)(index + 3);

    Buffer es256 = { .bytes = (u8[512]){0}, .length = 0, .capacity = 512 };
    put_cose_es256(&es256, xy);

    u8 other_id[16] = {0};
    for (u32 index = 0; index < sizeof(other_id); index++) other_id[index] = (u8)(0x10 + index);

    Buffer authenticator_data = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x41, 0, other_id, sizeof(other_id), &es256);
    Buffer attestation = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_attestation_object(&attestation, &authenticator_data);
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.create", challenge.challenge, ORIGIN);

    NYA_AccountPasskeyRegistration request = {
        .rp_id = RP_ID, .origin = ORIGIN, .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .attestation_object = attestation.bytes, .attestation_object_size = attestation.length,
    };
    NYA_Error result = nya_account_passkey_register_finish(arena, bob.id, &request, nullptr);
    nya_check(result.kind == NYA_ERROR_NOT_SUPPORTED, "an ES256 credential is refused as unsupported, got kind %d", (s32)result.kind);

    b8 has = false;
    NYA_EXPECT(nya_account_passkey_has(arena, bob.id, &has));
    nya_check(!has, "and nothing was stored for it");
  }

  // TEST: malformed CBOR and short buffers are refused without reading out of bounds (the sanitizer build is what proves the "without reading out of bounds" half)
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_register_begin(arena, user.id, &challenge));
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.create", challenge.challenge, ORIGIN);

    // Not a CBOR map at all — a reserved initial byte.
    u8 garbage[] = {0xFF, 0x00, 0x11};
    NYA_AccountPasskeyRegistration request = {
        .rp_id = RP_ID, .origin = ORIGIN, .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .attestation_object = garbage, .attestation_object_size = sizeof(garbage),
    };
    nya_check(!nya_account_passkey_register_finish(arena, user.id, &request, nullptr).ok, "garbage CBOR is refused");
  }
  {
    // A map that claims a huge byte-string authData whose length runs off the end of the buffer.
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_register_begin(arena, user.id, &challenge));
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.create", challenge.challenge, ORIGIN);

    Buffer attestation = { .bytes = (u8[64]){0}, .length = 0, .capacity = 64 };
    cbor_map(&attestation, 1);
    cbor_text(&attestation, "authData");
    cbor_head(&attestation, 2, 60000); // claims 60000 bytes; only a few follow

    NYA_AccountPasskeyRegistration request = {
        .rp_id = RP_ID, .origin = ORIGIN, .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .attestation_object = attestation.bytes, .attestation_object_size = attestation.length,
    };
    nya_check(!nya_account_passkey_register_finish(arena, user.id, &request, nullptr).ok, "a byte string longer than the buffer is refused");
  }
  {
    // An assertion whose authenticator data is shorter than a header.
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));
    char client_data[512] = {0};
    u64 client_data_size = build_client_data(client_data, sizeof(client_data), "webauthn.get", challenge.challenge, ORIGIN);

    u8 tiny[8] = {0};
    NYA_CryptoSignature signature = {0};
    NYA_AccountPasskeyAssertion request = {
        .rp_id = RP_ID, .origin = ORIGIN, .credential_id = credential_id_b64,
        .client_data_json = (const u8 *)client_data, .client_data_json_size = client_data_size,
        .authenticator_data = tiny, .authenticator_data_size = sizeof(tiny),
        .signature = signature.bytes, .signature_size = sizeof(signature.bytes),
    };
    nya_check(!nya_account_passkey_assert_finish(arena, user.id, &request, nullptr).ok, "a too-short authenticator data is refused");
  }
  {
    // clientDataJSON that is not JSON.
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_register_begin(arena, user.id, &challenge));
    const char *not_json = "this is not json at all";
    Buffer authenticator_data = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_authenticator_data(&authenticator_data, RP_ID, 0x41, 0, credential_id, sizeof(credential_id), &cose);
    Buffer attestation = { .bytes = (u8[1024]){0}, .length = 0, .capacity = 1024 };
    build_attestation_object(&attestation, &authenticator_data);

    NYA_AccountPasskeyRegistration request = {
        .rp_id = RP_ID, .origin = ORIGIN, .client_data_json = (const u8 *)not_json, .client_data_json_size = strlen(not_json),
        .attestation_object = attestation.bytes, .attestation_object_size = attestation.length,
    };
    nya_check(!nya_account_passkey_register_finish(arena, user.id, &request, nullptr).ok, "a non-JSON clientDataJSON is refused");
  }

  // TEST: a credential can be removed, and only by its owner
  {
    NYA_AccountPasskey *list = nullptr;
    u32 count = 0;
    NYA_EXPECT(nya_account_passkey_list(arena, user.id, &list, &count));
    nya_check(count == 1, "ada still has her one credential");

    NYA_AccountUser bob = {0};
    NYA_EXPECT(nya_account_find(arena, "bob", &bob));
    nya_check(!nya_account_passkey_remove(arena, bob.id, list[0].id).ok, "nobody removes another's credential");

    NYA_EXPECT(nya_account_passkey_remove(arena, user.id, list[0].id));

    b8 has = true;
    NYA_EXPECT(nya_account_passkey_has(arena, user.id, &has));
    nya_check(!has, "and once removed it is gone");
  }

  // TEST: expired challenges are pruned on demand
  {
    NYA_AccountPasskeyChallenge challenge = {0};
    NYA_EXPECT(nya_account_passkey_assert_begin(arena, user.id, &challenge));

    u32 removed = 0;
    // keep_for far in the future keeps everything; keep_for of zero drops everything already expired, and a fresh challenge is not yet expired, so nothing goes.
    NYA_EXPECT(nya_account_passkey_challenge_prune(arena, 0, &removed));
    nya_check(removed == 0, "a live challenge is not pruned");
  }

  (void)database;

  return nya_check_failures() == 0 ? 0 : 1;
}
