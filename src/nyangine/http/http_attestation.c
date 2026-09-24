#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/http/http_attestation.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Appends `value` as eight little-endian bytes at `cursor` and returns the advanced cursor. */
NYA_INTERNAL u8* _nya_http_attestation_put_u64(u8* cursor, u64 value);

/** A copy of `text` in `arena`, so a value stored in an object outlives the stack buffer it was built in. */
NYA_INTERNAL char* _nya_http_attestation_dup(NYA_Arena* arena, NYA_ConstCString text);

/** Reads one string field, base64url-decodes it into `out`, and fails unless it was exactly `expected` bytes. */
NYA_INTERNAL NYA_Error _nya_http_attestation_field(const NYA_Object* object, NYA_CString key, OUT u8* out, u64 expected) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_http_attestation_bundle_digest(const NYA_HttpAttestationFile* files, u64 count, OUT NYA_CryptoSha256Digest* out_digest) {
    nya_assert(out_digest != nullptr);
    nya_assert(count == 0 || files != nullptr);

    NYA_CryptoSha256 sha256 = { 0 };
    nya_crypto_sha256_begin(&sha256);

    for (u64 index = 0; index < count; index++) {
        const NYA_HttpAttestationFile* file = &files[index];

        // Path and bytes, each behind its own length, so a file renamed, a byte changed, or two files
        // whose contents would otherwise run together are all a different digest. Little-endian lengths,
        // the same order everywhere, so an origin and a verifier fold the identical stream.
        u64 path_size = file->path != nullptr ? strlen(file->path) : 0;

        u8 length[sizeof(u64)] = { 0 };

        for (u64 byte = 0; byte < sizeof(u64); byte++) length[byte] = (u8)(path_size >> (byte * 8));
        nya_crypto_sha256_update(&sha256, length, sizeof(length));
        nya_crypto_sha256_update(&sha256, (const u8*)file->path, path_size);

        for (u64 byte = 0; byte < sizeof(u64); byte++) length[byte] = (u8)(file->size >> (byte * 8));
        nya_crypto_sha256_update(&sha256, length, sizeof(length));
        nya_crypto_sha256_update(&sha256, file->data, file->size);
    }

    nya_crypto_sha256_end(&sha256, out_digest);
}

NYA_Error nya_http_attestation_encode(const NYA_HttpAttestationManifest* manifest, OUT u8* out_message, u64 capacity, OUT u64* out_size) {
    nya_assert(manifest != nullptr && out_message != nullptr && out_size != nullptr);

    if (capacity < NYA_HTTP_ATTESTATION_MAX_MESSAGE) return nya_error(NYA_ERROR_NOT_OK, "the attestation message buffer is too small");

    // The origin must be NUL terminated inside its bound: a field that runs to the end of the buffer has
    // no honest length, and a length is what the whole layout leans on.
    u64 origin_size = strnlen(manifest->origin, NYA_HTTP_ATTESTATION_MAX_ORIGIN);
    if (origin_size >= NYA_HTTP_ATTESTATION_MAX_ORIGIN) return nya_error(NYA_ERROR_NOT_OK, "the attestation origin is not terminated inside its bound");

    u8* cursor = out_message;

    // The magic and version, then each variable field behind its length. See the header note on why the
    // lengths are not optional: without them two different manifests could serialize to the same bytes.
    nya_memcpy(cursor, NYA_HTTP_ATTESTATION_MAGIC, NYA_HTTP_ATTESTATION_MAGIC_BYTES);
    cursor += NYA_HTTP_ATTESTATION_MAGIC_BYTES;

    cursor = _nya_http_attestation_put_u64(cursor, origin_size);
    nya_memcpy(cursor, manifest->origin, origin_size);
    cursor += origin_size;

    cursor = _nya_http_attestation_put_u64(cursor, manifest->issued_at_s);

    cursor = _nya_http_attestation_put_u64(cursor, NYA_CRYPTO_SHA256_BYTES);
    nya_memcpy(cursor, manifest->content.bytes, NYA_CRYPTO_SHA256_BYTES);
    cursor += NYA_CRYPTO_SHA256_BYTES;

    *out_size = (u64)(cursor - out_message);

    return NYA_OK;
}

NYA_Error nya_http_attestation_sign(const NYA_CryptoSignSecretKey* secret_key, const NYA_HttpAttestationManifest* manifest, OUT NYA_CryptoSignature* out_signature) {
    nya_assert(secret_key != nullptr && manifest != nullptr && out_signature != nullptr);

    u8  message[NYA_HTTP_ATTESTATION_MAX_MESSAGE] = { 0 };
    u64 message_size                              = 0;

    NYA_TRY(nya_http_attestation_encode(manifest, message, sizeof(message), &message_size));

    nya_crypto_sign(secret_key, message, message_size, out_signature);

    return NYA_OK;
}

b8 nya_http_attestation_verify(const NYA_CryptoSignPublicKey* public_key, const NYA_HttpAttestationManifest* manifest, const NYA_CryptoSignature* signature) {
    nya_assert(public_key != nullptr && manifest != nullptr && signature != nullptr);

    u8  message[NYA_HTTP_ATTESTATION_MAX_MESSAGE] = { 0 };
    u64 message_size                              = 0;

    // A manifest that will not encode cannot have been signed either, so it verifies as false rather than
    // being read past. The one early return, and it is on the manifest's own shape, not on the signature.
    if (!nya_http_attestation_encode(manifest, message, sizeof(message), &message_size).ok) return false;

    return nya_crypto_sign_verify(public_key, message, message_size, signature);
}

NYA_Error nya_http_attestation_to_json(
    NYA_Arena* arena, const NYA_HttpAttestationManifest* manifest, const NYA_CryptoSignPublicKey* public_key, const NYA_CryptoSignature* signature, OUT NYA_Object** out_object
) {
    nya_assert(arena != nullptr && manifest != nullptr && public_key != nullptr && signature != nullptr && out_object != nullptr);

    char content_b64[64]   = { 0 };
    char public_b64[64]    = { 0 };
    char signature_b64[128] = { 0 };
    u64  size              = 0;

    if (!nya_crypto_base64url_encode(manifest->content.bytes, NYA_CRYPTO_SHA256_BYTES, content_b64, sizeof(content_b64), &size)
        || !nya_crypto_base64url_encode(public_key->bytes, NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES, public_b64, sizeof(public_b64), &size)
        || !nya_crypto_base64url_encode(signature->bytes, NYA_CRYPTO_SIGNATURE_BYTES, signature_b64, sizeof(signature_b64), &size)) {
        return nya_error(NYA_ERROR_NOT_OK, "an attestation field could not be base64url-encoded");
    }

    NYA_Object* object = nya_object_create(arena);

    // Every string is copied into the arena: nya_object_add keeps the pointer it is handed, and the base64
    // buffers and the origin are the caller's stack, gone the moment this returns. The arena outlives the
    // object, so the copies are what the serializer reads later.
    nya_object_add(object, "version", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_HTTP_ATTESTATION_VERSION });
    nya_object_add(object, "origin", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _nya_http_attestation_dup(arena, manifest->origin) });
    nya_object_add(object, "issued_at_s", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = manifest->issued_at_s });
    nya_object_add(object, "content_sha256", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _nya_http_attestation_dup(arena, content_b64) });
    nya_object_add(object, "public_key", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _nya_http_attestation_dup(arena, public_b64) });
    nya_object_add(object, "signature", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _nya_http_attestation_dup(arena, signature_b64) });

    *out_object = object;

    return NYA_OK;
}

NYA_Error nya_http_attestation_from_json(
    const NYA_Object* object, OUT NYA_HttpAttestationManifest* out_manifest, OUT NYA_CryptoSignPublicKey* out_public_key, OUT NYA_CryptoSignature* out_signature
) {
    nya_assert(object != nullptr && out_manifest != nullptr && out_public_key != nullptr && out_signature != nullptr);

    *out_manifest   = (NYA_HttpAttestationManifest){ 0 };
    *out_public_key = (NYA_CryptoSignPublicKey){ 0 };
    *out_signature  = (NYA_CryptoSignature){ 0 };

    NYA_Value* origin = nya_object_get(object, "origin");
    if (origin == nullptr || origin->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_NOT_OK, "the attestation has no origin");
    if (strnlen(origin->as_string, NYA_HTTP_ATTESTATION_MAX_ORIGIN) >= NYA_HTTP_ATTESTATION_MAX_ORIGIN) return nya_error(NYA_ERROR_NOT_OK, "the attestation origin is too long");
    (void)snprintf(out_manifest->origin, sizeof(out_manifest->origin), "%s", origin->as_string);

    // JSON parses every integer as signed, and a timestamp is never negative: an S64 is what serde hands
    // back, and a U64 is accepted too for a producer that annotated it. Anything else is a malformed field.
    NYA_Value* issued = nya_object_get(object, "issued_at_s");
    if (issued == nullptr || (issued->type != NYA_TYPE_S64 && issued->type != NYA_TYPE_U64)) return nya_error(NYA_ERROR_NOT_OK, "the attestation has no issued_at_s");
    if (issued->type == NYA_TYPE_S64 && issued->as_s64 < 0) return nya_error(NYA_ERROR_NOT_OK, "the attestation issued_at_s is negative");
    out_manifest->issued_at_s = issued->type == NYA_TYPE_S64 ? (u64)issued->as_s64 : issued->as_u64;

    NYA_TRY(_nya_http_attestation_field(object, "content_sha256", out_manifest->content.bytes, NYA_CRYPTO_SHA256_BYTES));
    NYA_TRY(_nya_http_attestation_field(object, "public_key", out_public_key->bytes, NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES));
    NYA_TRY(_nya_http_attestation_field(object, "signature", out_signature->bytes, NYA_CRYPTO_SIGNATURE_BYTES));

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u8* _nya_http_attestation_put_u64(u8* cursor, u64 value) {
    for (u64 byte = 0; byte < sizeof(u64); byte++) cursor[byte] = (u8)(value >> (byte * 8));

    return cursor + sizeof(u64);
}

char* _nya_http_attestation_dup(NYA_Arena* arena, NYA_ConstCString text) {
    u64   size = strlen(text);
    char* copy = nya_arena_alloc(arena, size + 1);

    nya_memcpy(copy, text, size);
    copy[size] = '\0';

    return copy;
}

NYA_Error _nya_http_attestation_field(const NYA_Object* object, NYA_CString key, OUT u8* out, u64 expected) {
    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_NOT_OK, "an attestation field is missing or not a string");

    u64 decoded = 0;

    // The decode fails a spelling that is not base64url; the length check fails one that is, but of the
    // wrong size — a 31-byte digest, a truncated signature — so a field is either exactly right or refused.
    if (!nya_crypto_base64url_decode(value->as_string, strlen(value->as_string), out, expected, &decoded) || decoded != expected) {
        return nya_error(NYA_ERROR_NOT_OK, "an attestation field is not base64url of the expected length");
    }

    return NYA_OK;
}
