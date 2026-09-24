#include <string.h>

#include "nyangine/accounts/accounts_passkey.h"
#include "nyangine/accounts/accounts_user.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/crypto/crypto_sign.h"
#include "nyangine/db/db_orm.h"
#include "nyangine/os/os_random.h"
#include "nyangine/serde/serde_cbor.h"
#include "nyangine/serde/serde_json.h"

// CONSTANTS — the fixed authenticator-data layout (WebAuthn §6.1), so bounds checks compare against constants.

/** Bytes of the RP id hash at the front of every authenticator data: a SHA-256, so 32. */
#define _NYA_PASSKEY_RP_ID_HASH_BYTES 32

/** The one flags byte that follows the hash, and the bit in it that says a person was present. */
#define _NYA_PASSKEY_FLAG_USER_PRESENT 0x01
#define _NYA_PASSKEY_FLAG_ATTESTED     0x40

/** Bytes of the big-endian signature counter that follows the flags. */
#define _NYA_PASSKEY_SIGN_COUNT_BYTES 4

/** The offset of the flags byte, of the counter, and the least an authenticator data may be: 32 + 1 + 4. */
#define _NYA_PASSKEY_FLAGS_OFFSET      _NYA_PASSKEY_RP_ID_HASH_BYTES
#define _NYA_PASSKEY_SIGN_COUNT_OFFSET (_NYA_PASSKEY_RP_ID_HASH_BYTES + 1)
#define _NYA_PASSKEY_HEADER_BYTES      (_NYA_PASSKEY_RP_ID_HASH_BYTES + 1 + _NYA_PASSKEY_SIGN_COUNT_BYTES)

/** Bytes of the AAGUID that opens the attested credential data, after the header, on a registration. */
#define _NYA_PASSKEY_AAGUID_BYTES 16

/** The COSE_Key map labels this reads (RFC 8152): the key type, the algorithm, the curve, and the public value. */
#define _NYA_PASSKEY_COSE_LABEL_KTY 1
#define _NYA_PASSKEY_COSE_LABEL_ALG 3
#define _NYA_PASSKEY_COSE_LABEL_CRV (-1)
#define _NYA_PASSKEY_COSE_LABEL_X   (-2)

/** The COSE key type and curve of an Ed25519 key: an octet key pair (OKP) on the Ed25519 curve. */
#define _NYA_PASSKEY_COSE_KTY_OKP     1
#define _NYA_PASSKEY_COSE_KTY_EC2     2
#define _NYA_PASSKEY_COSE_CRV_ED25519 6

// PRIVATE API DECLARATION

/** The string value at `key`, or null when it is absent or not a string. Used to read clientDataJSON fields. */
NYA_INTERNAL NYA_ConstCString _nya_passkey_json_string(const NYA_Object* object, NYA_ConstCString key) __attr_no_discard;

/** Parses the fixed head of authenticator data (user-present flag, counter); checks length >= header and that the RP id hash is SHA-256 of `rp_id` (anti-phishing and bounds in one read). */
NYA_INTERNAL b8 _nya_passkey_authenticator_head(
    const u8* authenticator_data, u64 size, NYA_ConstCString rp_id, OUT b8* out_user_present, OUT b8* out_attested, OUT u32* out_sign_count
) __attr_no_discard;

/** Reads the Ed25519 public key from a COSE_Key map: NYA_ERROR_NOT_SUPPORTED for ES256 (refused, not mis-read), NYA_ERROR_PARSE for a malformed or non-Ed25519 key; on success fills the 32 key bytes and the algorithm. */
NYA_INTERNAL NYA_Error _nya_passkey_cose_ed25519(
    const u8* cose, u64 size, OUT u8 out_public_key[NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES], OUT s64* out_algorithm
) __attr_no_discard;

/** Mints, stores and answers a challenge bound to `user_id` and `purpose`; supersedes an outstanding one of the same purpose. */
NYA_INTERNAL NYA_Error _nya_passkey_challenge_begin(NYA_Arena* arena, u64 user_id, s64 purpose, OUT NYA_AccountPasskeyChallenge* out_challenge)
    __attr_no_discard;

/** Looks up and *spends* the challenge a clientDataJSON carries, returning whether it was live; the row is deleted on find (spent or expired), so a challenge works once, and a wrong user/purpose isn't found. */
NYA_INTERNAL NYA_Error _nya_passkey_challenge_spend(NYA_Arena* arena, u64 user_id, s64 purpose, NYA_ConstCString challenge, OUT b8* out_live)
    __attr_no_discard;

/** Ends the oldest credentials of a user until at most `keep` are left, so enrolling a new one always fits. */
NYA_INTERNAL NYA_Error _nya_passkey_trim(NYA_Arena* arena, u64 user_id, u32 keep) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_account_passkey_register_begin(NYA_Arena* arena, u64 user_id, NYA_AccountPasskeyChallenge* out_challenge) {
    nya_assert(arena != nullptr && out_challenge != nullptr);

    return _nya_passkey_challenge_begin(arena, user_id, NYA_ACCOUNTS_PASSKEY_PURPOSE_REGISTER, out_challenge);
}

NYA_Error nya_account_passkey_assert_begin(NYA_Arena* arena, u64 user_id, NYA_AccountPasskeyChallenge* out_challenge) {
    nya_assert(arena != nullptr && out_challenge != nullptr);

    return _nya_passkey_challenge_begin(arena, user_id, NYA_ACCOUNTS_PASSKEY_PURPOSE_ASSERT, out_challenge);
}

NYA_Error nya_account_passkey_register_finish(NYA_Arena* arena, u64 user_id, const NYA_AccountPasskeyRegistration* request, NYA_AccountPasskey* out_passkey) {
    nya_assert(arena != nullptr && request != nullptr);

    if (out_passkey != nullptr) nya_memset(out_passkey, 0, sizeof(NYA_AccountPasskey));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    if (request->rp_id == nullptr || request->origin == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a passkey registration needs an rp id and an origin");

    // The bytes are untrusted and bounded before parsing, so a hostile response can't make the server work without limit; null/empty is malformed.
    if (request->client_data_json == nullptr || request->client_data_json_size == 0 || request->client_data_json_size > NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES)
        return nya_error(NYA_ERROR_PARSE, "the clientDataJSON is missing or too large");

    if (request->attestation_object == nullptr || request->attestation_object_size == 0 || request->attestation_object_size > NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES)
        return nya_error(NYA_ERROR_PARSE, "the attestation object is missing or too large");

    // The user has to be there and enabled: a credential for a disabled account is a way back into one.
    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, user_id, &user));
    if (user.disabled) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that account is disabled");

    // ── the clientDataJSON: what the browser says it did, signed over by the authenticator ──
    NYA_Object* client_data = nullptr;
    if (!nya_serde_json_deserialize(arena, request->client_data_json, request->client_data_json_size, NYA_SERDE_NONE, &client_data).ok)
        return nya_error(NYA_ERROR_PARSE, "the clientDataJSON is not valid JSON");

    NYA_ConstCString type      = _nya_passkey_json_string(client_data, "type");
    NYA_ConstCString challenge = _nya_passkey_json_string(client_data, "challenge");
    NYA_ConstCString origin    = _nya_passkey_json_string(client_data, "origin");

    if (type == nullptr || !nya_string_equals(type, "webauthn.create")) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the clientDataJSON is not a webauthn.create");
    if (origin == nullptr || !nya_string_equals(origin, request->origin)) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the origin does not match the relying party");
    if (challenge == nullptr) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the clientDataJSON carries no challenge");

    // The challenge is spent whatever happens next, so a response can't be replayed even if a later check fails; a live match is required to go on.
    b8 live = false;
    NYA_TRY(_nya_passkey_challenge_spend(arena, user_id, NYA_ACCOUNTS_PASSKEY_PURPOSE_REGISTER, challenge, &live));
    if (!live) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the challenge is not one this server is waiting on");

    // ── the attestation object: a CBOR map whose "authData" holds the authenticator data ──
    NYA_CborReader attestation = nya_cbor_reader(request->attestation_object, request->attestation_object_size);

    u64 entries = 0;
    if (!nya_cbor_read_map(&attestation, &entries)) return nya_error(NYA_ERROR_PARSE, "the attestation object is not a CBOR map");

    const u8* authenticator_data = nullptr;
    u64       authenticator_size = 0;

    for (u64 index = 0; index < entries; index++) {
        const u8* key      = nullptr;
        u64       key_size = 0;

        if (!nya_cbor_read_text(&attestation, &key, &key_size)) return nya_error(NYA_ERROR_PARSE, "the attestation object has a non-text key");

        if (key_size == strlen("authData") && memcmp(key, "authData", key_size) == 0) {
            if (!nya_cbor_read_bytes(&attestation, &authenticator_data, &authenticator_size)) return nya_error(NYA_ERROR_PARSE, "authData is not a byte string");
        } else {
            // fmt, attStmt and the rest are stepped over: this accepts "none" attestation and doesn't verify an attestation statement, which would need a cert chain not worth trusting for a second factor.
            if (!nya_cbor_skip(&attestation)) return nya_error(NYA_ERROR_PARSE, "the attestation object is malformed");
        }
    }

    if (authenticator_data == nullptr || authenticator_size > NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES) return nya_error(NYA_ERROR_PARSE, "the attestation object carries no authData");

    // ── the authenticator data: the RP id hash, the flags, and the new credential ──
    b8  user_present = false;
    b8  attested     = false;
    u32 sign_count   = 0;

    if (!_nya_passkey_authenticator_head(authenticator_data, authenticator_size, request->rp_id, &user_present, &attested, &sign_count))
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the authenticator data does not match the relying party");

    if (!user_present) return nya_error(NYA_ERROR_PERMISSION_DENIED, "the authenticator reports no user was present");
    if (!attested) return nya_error(NYA_ERROR_PARSE, "the authenticator data carries no attested credential");

    // Attested credential data follows the header: AAGUID, a 2-byte credential id length, the id, then the COSE key; every read is checked against the remaining length.
    u64 cursor = _NYA_PASSKEY_HEADER_BYTES + _NYA_PASSKEY_AAGUID_BYTES;
    if (cursor + 2 > authenticator_size) return nya_error(NYA_ERROR_PARSE, "the attested credential data is truncated");

    u64 credential_id_length = ((u64)authenticator_data[cursor] << 8) | (u64)authenticator_data[cursor + 1];
    cursor += 2;

    if (credential_id_length == 0 || credential_id_length > NYA_ACCOUNTS_PASSKEY_CRED_ID_MAX_BYTES) return nya_error(NYA_ERROR_PARSE, "the credential id is missing or too large");
    if (cursor + credential_id_length > authenticator_size) return nya_error(NYA_ERROR_PARSE, "the credential id runs past the authenticator data");

    const u8* credential_id_bytes = authenticator_data + cursor;
    cursor += credential_id_length;

    // The COSE key is the rest of the authenticator data.
    u8  public_key[NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES] = { 0 };
    s64 algorithm                                         = 0;

    NYA_TRY(_nya_passkey_cose_ed25519(authenticator_data + cursor, authenticator_size - cursor, public_key, &algorithm));

    // ── store the credential ──
    NYA_AccountPasskey stored = { 0 };

    u64 written = 0;
    if (!nya_crypto_base64url_encode(credential_id_bytes, credential_id_length, stored.credential_id, sizeof(stored.credential_id), &written))
        return nya_error(NYA_ERROR_PARSE, "the credential id could not be encoded");
    if (!nya_crypto_base64url_encode(public_key, sizeof(public_key), stored.public_key, sizeof(stored.public_key), &written))
        return nya_error(NYA_ERROR_NOT_OK, "the public key could not be encoded");

    // A credential id names one key, so a duplicate would be a second row a login could match the wrong way.
    void* clash       = nullptr;
    u32   clash_count = 0;
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.passkeys, arena, "WHERE credential_id = ? LIMIT 1", (NYA_SqlValue[]){ nya_sql_text(stored.credential_id) }, 1, &clash, &clash_count));
    if (clash_count > 0) return nya_error(NYA_ERROR_ALREADY_EXISTS, "that credential is already enrolled");

    // Room first, so the trim never drops the credential this call is about to store.
    NYA_TRY(_nya_passkey_trim(arena, user_id, NYA_ACCOUNTS_MAX_PASSKEYS_PER_USER - 1));

    u64 now_s = nya_clock_get_timestamp_s();

    stored.user_id      = user_id;
    stored.algorithm    = algorithm;
    stored.sign_count   = (s64)sign_count;
    stored.created_at_s = now_s;
    stored.used_at_s    = now_s;
    (void)snprintf(stored.name, sizeof(stored.name), "%s", request->name != nullptr ? request->name : "");

    NYA_TRY(nya_orm_insert(_NYA_ACCOUNTS.passkeys, &stored));

    if (out_passkey != nullptr) *out_passkey = stored;

    return NYA_OK;
}

NYA_Error nya_account_passkey_assert_finish(NYA_Arena* arena, u64 user_id, const NYA_AccountPasskeyAssertion* request, NYA_AccountPasskey* out_passkey) {
    nya_assert(arena != nullptr && request != nullptr);

    if (out_passkey != nullptr) nya_memset(out_passkey, 0, sizeof(NYA_AccountPasskey));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    // One refusal for the many ways an assertion is not valid, the shape accounts.h describes.
    NYA_Error refused = nya_error(NYA_ERROR_PERMISSION_DENIED, "that assertion is not valid");

    if (request->rp_id == nullptr || request->origin == nullptr || request->credential_id == nullptr) return refused;

    if (request->client_data_json == nullptr || request->client_data_json_size == 0 || request->client_data_json_size > NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES) return refused;
    if (request->authenticator_data == nullptr || request->authenticator_data_size < _NYA_PASSKEY_HEADER_BYTES || request->authenticator_data_size > NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES) return refused;

    // A raw Ed25519 signature is exactly 64 bytes; anything else does not verify and is refused before any work.
    if (request->signature == nullptr || request->signature_size != NYA_CRYPTO_SIGNATURE_BYTES) return refused;

    NYA_AccountUser user = { 0 };
    if (!nya_account_find_by_id(arena, user_id, &user).ok || user.disabled) return refused;

    // ── the stored credential this claims to be, and its public key ──
    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.passkeys, arena, "WHERE user_id = ? AND credential_id = ? LIMIT 1",
        (NYA_SqlValue[]){ nya_sql_s64((s64)user_id), nya_sql_text(request->credential_id) }, 2, &rows, &count
    ));
    if (count == 0) return refused;

    const NYA_AccountPasskey* found      = nya_orm_at(_NYA_ACCOUNTS.passkeys, rows, 0);
    NYA_AccountPasskey        credential = *found;

    NYA_CryptoSignPublicKey public_key = { 0 };
    u64                     key_size   = 0;
    if (!nya_crypto_base64url_decode(credential.public_key, strlen(credential.public_key), public_key.bytes, sizeof(public_key.bytes), &key_size) || key_size != sizeof(public_key.bytes))
        return refused;

    // ── the clientDataJSON: a webauthn.get for this server's challenge and origin ──
    NYA_Object* client_data = nullptr;
    if (!nya_serde_json_deserialize(arena, request->client_data_json, request->client_data_json_size, NYA_SERDE_NONE, &client_data).ok) return refused;

    NYA_ConstCString type      = _nya_passkey_json_string(client_data, "type");
    NYA_ConstCString challenge = _nya_passkey_json_string(client_data, "challenge");
    NYA_ConstCString origin    = _nya_passkey_json_string(client_data, "origin");

    if (type == nullptr || !nya_string_equals(type, "webauthn.get")) return refused;
    if (origin == nullptr || !nya_string_equals(origin, request->origin)) return refused;
    if (challenge == nullptr) return refused;

    // Spend the challenge before the signature is checked, so a captured response cannot be replayed.
    b8 live = false;
    NYA_TRY(_nya_passkey_challenge_spend(arena, user_id, NYA_ACCOUNTS_PASSKEY_PURPOSE_ASSERT, challenge, &live));
    if (!live) return refused;

    // ── the authenticator data: the RP id hash, and the user-present flag ──
    b8  user_present = false;
    b8  attested     = false;
    u32 sign_count   = 0;
    if (!_nya_passkey_authenticator_head(request->authenticator_data, request->authenticator_data_size, request->rp_id, &user_present, &attested, &sign_count)) return refused;
    if (!user_present) return refused;

    // ── the signature over authenticatorData || SHA-256(clientDataJSON), with the STORED key ──
    NYA_CryptoSha256Digest client_hash = { 0 };
    nya_crypto_sha256(request->client_data_json, request->client_data_json_size, &client_hash);

    // The bound above keeps authenticator data under NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES, so the signed message fits a fixed buffer with nothing to allocate.
    u8  message[NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES + NYA_CRYPTO_SHA256_BYTES] = { 0 };
    u64 message_size                                                            = request->authenticator_data_size + NYA_CRYPTO_SHA256_BYTES;

    nya_memcpy(message, request->authenticator_data, request->authenticator_data_size);
    nya_memcpy(message + request->authenticator_data_size, client_hash.bytes, sizeof(client_hash.bytes));

    NYA_CryptoSignature signature = { 0 };
    nya_memcpy(signature.bytes, request->signature, sizeof(signature.bytes));

    if (!nya_crypto_sign_verify(&public_key, message, message_size, &signature)) return refused;

    // The counter must climb, or this is a cloned authenticator: zero on both sides means it keeps none (WebAuthn allows it); anything else not increasing is a replay/clone and ends the assertion.
    s64 received = (s64)sign_count;
    if (!(received == 0 && credential.sign_count == 0) && received <= credential.sign_count) return refused;

    credential.sign_count = received;
    credential.used_at_s  = nya_clock_get_timestamp_s();

    NYA_TRY(nya_orm_update(_NYA_ACCOUNTS.passkeys, &credential));

    if (out_passkey != nullptr) *out_passkey = credential;

    return NYA_OK;
}

NYA_Error nya_account_passkey_list(NYA_Arena* arena, u64 user_id, NYA_AccountPasskey** out_passkeys, u32* out_count) {
    nya_assert(arena != nullptr && out_passkeys != nullptr && out_count != nullptr);

    *out_passkeys = nullptr;
    *out_count    = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.passkeys, arena, "WHERE user_id = ? ORDER BY created_at_s DESC", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count));

    *out_passkeys = (NYA_AccountPasskey*)rows;
    *out_count    = count;

    return NYA_OK;
}

NYA_Error nya_account_passkey_has(NYA_Arena* arena, u64 user_id, b8* out_has) {
    nya_assert(arena != nullptr && out_has != nullptr);

    *out_has = false;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.passkeys, arena, "WHERE user_id = ? LIMIT 1", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count));

    *out_has = count > 0;

    return NYA_OK;
}

NYA_Error nya_account_passkey_remove(NYA_Arena* arena, u64 user_id, u64 passkey_id) {
    nya_assert(arena != nullptr);

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    // Found by id and user together, so removing one is only ever removing one of your own.
    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.passkeys, arena, "WHERE id = ? AND user_id = ? LIMIT 1", (NYA_SqlValue[]){ nya_sql_s64((s64)passkey_id), nya_sql_s64((s64)user_id) }, 2, &rows, &count
    ));
    if (count == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no such credential for that user");

    NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.passkeys, nya_sql_s64((s64)passkey_id)));

    return NYA_OK;
}

NYA_Error nya_account_passkey_challenge_prune(NYA_Arena* arena, u64 keep_for_s, u32* out_removed) {
    nya_assert(arena != nullptr && out_removed != nullptr);

    *out_removed = 0;

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    u64 now_s   = nya_clock_get_timestamp_s();
    u64 cutoff  = now_s > keep_for_s ? now_s - keep_for_s : 0;

    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.passkey_challenges, arena, "WHERE expires_at_s < ?", (NYA_SqlValue[]){ nya_sql_s64((s64)cutoff) }, 1, &rows, &count));

    for (u32 index = 0; index < count; index++) {
        const NYA_AccountPasskeyChallenge* row = nya_orm_at(_NYA_ACCOUNTS.passkey_challenges, rows, index);
        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.passkey_challenges, nya_sql_s64((s64)row->id)));
    }

    *out_removed = count;

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

NYA_ConstCString _nya_passkey_json_string(const NYA_Object* object, NYA_ConstCString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;

    return value->as_string;
}

b8 _nya_passkey_authenticator_head(const u8* authenticator_data, u64 size, NYA_ConstCString rp_id, b8* out_user_present, b8* out_attested, u32* out_sign_count) {
    *out_user_present = false;
    *out_attested     = false;
    *out_sign_count   = 0;

    // The header has to be there whole before any byte of it is read.
    if (authenticator_data == nullptr || size < _NYA_PASSKEY_HEADER_BYTES) return false;

    // Anti-phishing: the RP id hash the authenticator signed must be SHA-256 of this server's RP id, so a signature for another site's RP id doesn't verify here.
    NYA_CryptoSha256Digest expected = { 0 };
    nya_crypto_sha256((const u8*)rp_id, rp_id != nullptr ? strlen(rp_id) : 0, &expected);

    if (nya_memcmp(authenticator_data, expected.bytes, _NYA_PASSKEY_RP_ID_HASH_BYTES) != 0) return false;

    u8 flags = authenticator_data[_NYA_PASSKEY_FLAGS_OFFSET];

    *out_user_present = (flags & _NYA_PASSKEY_FLAG_USER_PRESENT) != 0;
    *out_attested     = (flags & _NYA_PASSKEY_FLAG_ATTESTED) != 0;

    u32 sign_count = 0;
    for (u32 index = 0; index < _NYA_PASSKEY_SIGN_COUNT_BYTES; index++) {
        sign_count = (sign_count << 8) | (u32)authenticator_data[_NYA_PASSKEY_SIGN_COUNT_OFFSET + index];
    }

    *out_sign_count = sign_count;

    return true;
}

NYA_Error _nya_passkey_cose_ed25519(const u8* cose, u64 size, u8 out_public_key[NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES], s64* out_algorithm) {
    nya_memset(out_public_key, 0, NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES);
    *out_algorithm = 0;

    NYA_CborReader reader = nya_cbor_reader(cose, size);

    u64 entries = 0;
    if (!nya_cbor_read_map(&reader, &entries)) return nya_error(NYA_ERROR_PARSE, "the COSE key is not a CBOR map");

    b8        have_kty = false, have_alg = false, have_crv = false;
    s64       kty = 0, alg = 0, crv = 0;
    const u8* x      = nullptr;
    u64       x_size = 0;

    for (u64 index = 0; index < entries; index++) {
        s64 label = 0;
        if (!nya_cbor_read_int(&reader, &label)) return nya_error(NYA_ERROR_PARSE, "the COSE key has a non-integer label");

        if (label == _NYA_PASSKEY_COSE_LABEL_KTY) {
            if (!nya_cbor_read_int(&reader, &kty)) return nya_error(NYA_ERROR_PARSE, "the COSE key type is malformed");
            have_kty = true;
        } else if (label == _NYA_PASSKEY_COSE_LABEL_ALG) {
            if (!nya_cbor_read_int(&reader, &alg)) return nya_error(NYA_ERROR_PARSE, "the COSE algorithm is malformed");
            have_alg = true;
        } else if (label == _NYA_PASSKEY_COSE_LABEL_CRV) {
            if (!nya_cbor_read_int(&reader, &crv)) return nya_error(NYA_ERROR_PARSE, "the COSE curve is malformed");
            have_crv = true;
        } else if (label == _NYA_PASSKEY_COSE_LABEL_X) {
            if (!nya_cbor_read_bytes(&reader, &x, &x_size)) return nya_error(NYA_ERROR_PARSE, "the COSE public value is malformed");
        } else {
            // A -3 (y coordinate) or any other label: stepped over. An EC2 key's -3 is caught below by its algorithm and key type, so nothing to do with the value here.
            if (!nya_cbor_skip(&reader)) return nya_error(NYA_ERROR_PARSE, "the COSE key is malformed");
        }
    }

    // ES256 is refused by name (its algorithm or key type), not mis-read, since there's no P-256 verifier here; the clean refusal the header promises.
    if ((have_alg && alg == NYA_ACCOUNTS_PASSKEY_COSE_ALG_ES256) || (have_kty && kty == _NYA_PASSKEY_COSE_KTY_EC2))
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "ES256 (COSE -7) passkeys need a P-256 verifier this build does not vendor");

    // Everything else that is not exactly an Ed25519 OKP key is malformed for this purpose.
    if (!have_alg || alg != NYA_ACCOUNTS_PASSKEY_COSE_ALG_EDDSA) return nya_error(NYA_ERROR_NOT_SUPPORTED, "only Ed25519 (COSE -8) passkeys are supported");
    if (!have_kty || kty != _NYA_PASSKEY_COSE_KTY_OKP) return nya_error(NYA_ERROR_PARSE, "the COSE key is not an octet key pair");
    if (!have_crv || crv != _NYA_PASSKEY_COSE_CRV_ED25519) return nya_error(NYA_ERROR_PARSE, "the COSE key is not on the Ed25519 curve");
    if (x == nullptr || x_size != NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES) return nya_error(NYA_ERROR_PARSE, "the Ed25519 public key is the wrong size");

    nya_memcpy(out_public_key, x, NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES);
    *out_algorithm = alg;

    return NYA_OK;
}

NYA_Error _nya_passkey_challenge_begin(NYA_Arena* arena, u64 user_id, s64 purpose, NYA_AccountPasskeyChallenge* out_challenge) {
    nya_memset(out_challenge, 0, sizeof(NYA_AccountPasskeyChallenge));

    if (!nya_accounts_is_open()) return nya_error(NYA_ERROR_NOT_OK, "the accounts tables are not open");

    NYA_AccountUser user = { 0 };
    NYA_TRY(nya_account_find_by_id(arena, user_id, &user));
    if (user.disabled) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that account is disabled");

    // A new challenge of this purpose supersedes an outstanding one, so a user never accumulates them and there's only ever one to spend.
    void* existing = nullptr;
    u32   had      = 0;
    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.passkey_challenges, arena, "WHERE user_id = ? AND purpose = ?", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id), nya_sql_s64(purpose) }, 2, &existing, &had
    ));
    for (u32 index = 0; index < had; index++) {
        const NYA_AccountPasskeyChallenge* row = nya_orm_at(_NYA_ACCOUNTS.passkey_challenges, existing, index);
        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.passkey_challenges, nya_sql_s64((s64)row->id)));
    }

    u8 bytes[NYA_ACCOUNTS_PASSKEY_CHALLENGE_BYTES] = { 0 };
    if (!nya_os_random_bytes(bytes, sizeof(bytes))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    NYA_AccountPasskeyChallenge challenge = { 0 };
    u64                         length    = 0;

    if (!nya_crypto_base64url_encode(bytes, sizeof(bytes), challenge.challenge, sizeof(challenge.challenge), &length)) {
        nya_crypto_wipe(bytes, sizeof(bytes));
        return nya_error(NYA_ERROR_NOT_OK, "the challenge could not be encoded");
    }

    nya_crypto_wipe(bytes, sizeof(bytes));

    u64 now_s = nya_clock_get_timestamp_s();

    challenge.user_id      = user_id;
    challenge.purpose      = purpose;
    challenge.created_at_s = now_s;
    challenge.expires_at_s = now_s + NYA_ACCOUNTS_PASSKEY_CHALLENGE_TTL_S;

    NYA_TRY(nya_orm_insert(_NYA_ACCOUNTS.passkey_challenges, &challenge));

    *out_challenge = challenge;

    return NYA_OK;
}

NYA_Error _nya_passkey_challenge_spend(NYA_Arena* arena, u64 user_id, s64 purpose, NYA_ConstCString challenge, b8* out_live) {
    *out_live = false;

    // A challenge longer than the column holds is not one this minted; refuse it before it reaches the query.
    if (challenge == nullptr || strlen(challenge) >= NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT) return NYA_OK;

    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(
        _NYA_ACCOUNTS.passkey_challenges, arena, "WHERE user_id = ? AND purpose = ? AND challenge = ? LIMIT 1",
        (NYA_SqlValue[]){ nya_sql_s64((s64)user_id), nya_sql_s64(purpose), nya_sql_text(challenge) }, 3, &rows, &count
    ));

    if (count == 0) return NYA_OK;

    const NYA_AccountPasskeyChallenge* found = nya_orm_at(_NYA_ACCOUNTS.passkey_challenges, rows, 0);
    NYA_AccountPasskeyChallenge        row   = *found;

    // Single use: the row goes whether it was live or expired, so a challenge is spent exactly once.
    NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.passkey_challenges, nya_sql_s64((s64)row.id)));

    *out_live = nya_clock_get_timestamp_s() <= row.expires_at_s;

    return NYA_OK;
}

NYA_Error _nya_passkey_trim(NYA_Arena* arena, u64 user_id, u32 keep) {
    void* rows  = nullptr;
    u32   count = 0;
    NYA_TRY(nya_orm_select(_NYA_ACCOUNTS.passkeys, arena, "WHERE user_id = ? ORDER BY created_at_s ASC", (NYA_SqlValue[]){ nya_sql_s64((s64)user_id) }, 1, &rows, &count));

    // Oldest first, dropping only the overflow past `keep`.
    for (u32 index = 0; index + keep < count; index++) {
        const NYA_AccountPasskey* row = nya_orm_at(_NYA_ACCOUNTS.passkeys, rows, index);
        NYA_TRY(nya_orm_delete(_NYA_ACCOUNTS.passkeys, nya_sql_s64((s64)row->id)));
    }

    return NYA_OK;
}
