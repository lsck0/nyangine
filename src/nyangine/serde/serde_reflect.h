/**
 * @file serde_reflect.h
 *
 * A reflected struct straight to and from a file. The low-level pair is
 * nya_reflect_to_object / nya_reflect_from_object in base_reflection.h plus nya_serialize /
 * nya_deserialize in serde.h; this is the one call that is almost always what a caller wants, built
 * on top of both and exporting neither away.
 *
 * ```c
 * NYA_TRY(nya_reflect_save_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, "graphics.nya", NYA_SERDE_PRETTY));
 * NYA_TRY(nya_reflect_load_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, "graphics.nya", NYA_SERDE_NO_CHECKSUM));
 * ```
 *
 * Here rather than beside the reflection tables because the direction of the dependency matters: the
 * base layer knows nothing about formats or files, and a description of a type is useful without
 * either. This is the module that already owns both.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_string.h"
#include "nyangine/serde/serde_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENCRYPTED FIELDS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A field tagged `@secret` in its header (see base_reflection.h) is written encrypted and read back
 * decrypted, so a save file holds ciphertext where its plaintext would be. This is the opposite end
 * from `@redact`: `@redact` masks a value in a log and still writes it plainly to a file, while
 * `@secret` round-trips through a cipher and never touches a log in the clear.
 *
 * serde owns the reflection and turning a field's value to and from bytes, and knows nothing of keys
 * or ciphers. The key and the cipher come in as a NYA_SerdeSecret: two functions and an opaque `user`
 * that the seal and unseal carry (a key, in practice). crypto_seal.h has a pair shaped exactly for it.
 * A `@secret` field with no NYA_SerdeSecret refuses rather than writing plaintext, and an unseal that
 * does not authenticate fails closed, the same discipline the keyed database holds to.
 *
 * The value is first encoded to the compact binary `.nya` form, then handed to `seal`, so a secret of
 * any shape — a string, a number, a whole nested struct — travels the same way. On the wire it is one
 * base64 string; the plaintext bytes never appear in the document, which is the property the tests
 * assert.
 */

typedef struct NYA_SerdeSecret NYA_SerdeSecret;

/**
 * Encrypts `plaintext_size` bytes and writes the sealed value as text into `out_text`, from `arena`.
 * `field` is the name of the field being written, authenticated with the ciphertext so a value cannot
 * be moved to another field. `user` is whatever the NYA_SerdeSecret carried. May consume and wipe
 * `plaintext`. Returns an error to refuse the write.
 * */
typedef NYA_Error (*NYA_SerdeSecretSeal)(void* user, NYA_Arena* arena, NYA_ConstCString field, u8* plaintext, u64 plaintext_size,
                                         OUT NYA_String** out_text);

/**
 * The inverse: opens `text_size` characters of a sealed value into `out_plaintext`, from `arena`.
 * Returns an error — and writes nothing — when the value does not authenticate under `user` and
 * `field`, which is how a wrong key, a moved value or a tampered byte all fail closed.
 * */
typedef NYA_Error (*NYA_SerdeSecretUnseal)(void* user, NYA_Arena* arena, NYA_ConstCString field, const char* text, u64 text_size,
                                           OUT NYA_String** out_plaintext);

/**
 * The cipher and key threaded through a reflected save or load for the sake of `@secret` fields. Wire
 * it from crypto: `{ .seal = nya_crypto_seal, .unseal = nya_crypto_unseal, .user = &key }`.
 * */
struct NYA_SerdeSecret {
    NYA_SerdeSecretSeal   seal;
    NYA_SerdeSecretUnseal unseal;
    void*                 user;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes `instance` to `path` as the format the extension names, through `type`'s description.
 *
 * Allocates from a scratch arena of its own and releases it before returning, so nothing of the
 * document outlives the call.
 * */
NYA_API NYA_Error nya_reflect_save_file(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags)
    __attr_no_discard;

/**
 * Reads `path` over `instance`, in place. A field the file does not mention keeps the value it had,
 * which is what lets a file written by an older build load into a struct that has since grown.
 *
 * Every problem in the document is logged as a warning naming the key, what was found and what was
 * expected, and then skipped; see nya_reflect_check. `instance` is only touched once the whole file
 * has parsed, so a syntax error leaves it exactly as it was.
 *
 * Pass NYA_SERDE_NO_CHECKSUM for a file a person is invited to edit: the native format's checksum is
 * over the contents, and an honest edit changes it.
 * */
NYA_API NYA_Error nya_reflect_load_file(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags)
    __attr_no_discard;

/**
 * nya_reflect_save_file, with a cipher for the type's `@secret` fields. A `@secret` field is sealed
 * with `secret` before it is written, so the file holds ciphertext in its place; a type with no
 * `@secret` field is written exactly as nya_reflect_save_file would write it.
 *
 * nya_reflect_save_file itself carries no cipher, so it refuses a type that has a `@secret` field
 * rather than write the secret in the clear: pass this and a key for those.
 * */
NYA_API NYA_Error
nya_reflect_save_file_secret(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, NYA_SerdeSecret secret)
    __attr_no_discard;

/**
 * nya_reflect_load_file, with the cipher that opens the type's `@secret` fields. Each is unsealed with
 * `secret` before the document is checked or applied, so a wrong key, a tampered value or a missing
 * cipher stops the load rather than reading a secret wrong; see NYA_SerdeSecret.
 * */
NYA_API NYA_Error
nya_reflect_load_file_secret(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, NYA_SerdeSecret secret)
    __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TEST FIXTURE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The worked example of `@secret`, and what test_secret_serde round-trips. A reflected struct that
 * mixes the three dispositions a field can have, so the difference between them is one type to read:
 *
 * - `label` is ordinary: written and read as itself.
 * - `password` and `pin` are `@secret`: written encrypted, read back decrypted, and refused outright
 *   when no key is given — the plaintext is never in the file.
 * - `api_token` is `@redact`: written to the file as itself, and masked only where the struct is
 *   logged. It is the field that shows `@secret` is the stronger tag.
 * */
typedef struct NYA_SerdeSecretExample NYA_SerdeSecretExample;

// @reflect
struct NYA_SerdeSecretExample {
    /** Not a secret: an ordinary field, present to prove the sealed ones do not disturb the plain ones. */
    char label[32];

    /** A password kept encrypted at rest. */
    char password[64]; // @secret

    /** A numeric secret, to show `@secret` is not only for strings. */
    u32 pin; // @secret

    /** A bearer token: written to disk plainly, masked in a log. The `@redact` half of the contrast. */
    char api_token[64]; // @redact
};
