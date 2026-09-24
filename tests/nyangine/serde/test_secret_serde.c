/**
 * Encrypted `@secret` fields: a reflected struct whose secret fields are written as ciphertext and
 * read back as themselves, transparent to the caller.
 *
 * What is proved here, deterministically: a `@secret` field round-trips through `.nya` and JSON; its
 * plaintext is nowhere in the saved file; a `@redact` field beside it is written plainly, which is the
 * whole difference between the two tags; a tampered value, a wrong key and a missing key all fail
 * rather than reading a secret wrong; and the reflection layer actually carries the `@secret` bit.
 *
 * The key is fixed rather than generated, so the run is repeatable; the nonce inside each seal is still
 * random, so two saves differ on the wire while both open.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <stdlib.h>
#include <string.h>

#define FIXTURE_DIRECTORY "./.cache/test_secret_serde"

/** Fixed so the test is repeatable; nothing anybody would ship. */
static const NYA_CryptoKey32 KEY       = { .bytes = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                                      17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 } };
static const NYA_CryptoKey32 OTHER_KEY = { .bytes = { 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                                      16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 } };

/** The plaintext a secret field should never leak. */
#define PASSWORD  "hunter2-is-not-secure"
#define API_TOKEN "bearer-abc123-plain"

static b8 contains(const NYA_String* haystack, NYA_ConstCString needle) {
    return nya_string_contains(haystack, needle);
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_secret_serde");
    defer      nya_arena_destroy(arena);

    (void)nya_filesystem_delete_recursive(FIXTURE_DIRECTORY);
    NYA_EXPECT(nya_filesystem_create_directory(FIXTURE_DIRECTORY));

    NYA_SerdeSecret       secret       = { .seal = nya_crypto_seal, .unseal = nya_crypto_unseal, .user = (void*)&KEY };
    NYA_SerdeSecret       wrong_secret = { .seal = nya_crypto_seal, .unseal = nya_crypto_unseal, .user = (void*)&OTHER_KEY };
    const NYA_SerdeFormat formats[]    = { NYA_SERDE_FORMAT_NYA, NYA_SERDE_FORMAT_JSON };
    NYA_ConstCString      extensions[] = { "nya", "json" };

    // TEST: the reflection layer recognises @secret and tells @secret from @redact
    printf("TEST: @secret surfaces in the field's reflection info\n");
    {
        const NYA_TypeReflection* type = nya_reflect_of(NYA_SerdeSecretExample);

        const NYA_ReflectField* label     = nya_reflect_field(type, "label");
        const NYA_ReflectField* password  = nya_reflect_field(type, "password");
        const NYA_ReflectField* pin        = nya_reflect_field(type, "pin");
        const NYA_ReflectField* api_token = nya_reflect_field(type, "api_token");

        nya_check(password != nullptr && password->is_secret, "password should be @secret");
        nya_check(pin != nullptr && pin->is_secret, "pin should be @secret");
        nya_check(api_token != nullptr && api_token->is_redacted && !api_token->is_secret, "api_token should be @redact, not @secret");
        nya_check(label != nullptr && !label->is_secret && !label->is_redacted, "label should carry neither tag");
        printf("  PASSED\n");
    }

    // TEST: crypto_seal round-trips, and fails closed on tamper, wrong key, wrong field
    printf("TEST: crypto_seal / crypto_unseal directly\n");
    {
        u8          plaintext[] = "the quick brown fox";
        u8          copy[sizeof(plaintext)];
        nya_memcpy(copy, plaintext, sizeof(plaintext));

        NYA_String* sealed = nullptr;
        NYA_EXPECT(nya_crypto_seal((void*)&KEY, arena, "field", copy, sizeof(plaintext), &sealed));
        nya_check(sealed != nullptr && sealed->length > 0, "a value seals to a non-empty string");
        nya_check(!contains(sealed, "quick"), "and the plaintext is not readable in it");

        NYA_String* opened = nullptr;
        NYA_EXPECT(nya_crypto_unseal((void*)&KEY, arena, "field", (const char*)sealed->items, sealed->length, &opened));
        nya_check(opened->length == sizeof(plaintext) && nya_memcmp(opened->items, plaintext, sizeof(plaintext)) == 0,
                  "and unseals to the same bytes");

        // A single altered character does not open under the same key: the tag no longer matches.
        NYA_String* tampered = nya_string_from(arena, (NYA_ConstCString)sealed->items);
        u64         mid       = tampered->length / 2;
        tampered->items[mid]  = tampered->items[mid] == 'A' ? 'B' : 'A';

        NYA_String* not_opened = nullptr;
        nya_check(!nya_crypto_unseal((void*)&KEY, arena, "field", (const char*)tampered->items, tampered->length, &not_opened).ok,
                  "a tampered value fails to open");

        // A wrong key does not open it.
        nya_check(!nya_crypto_unseal((void*)&OTHER_KEY, arena, "field", (const char*)sealed->items, sealed->length, &not_opened).ok,
                  "a wrong key fails to open");

        // Nor a wrong field: the field name is authenticated, so the value cannot be moved.
        nya_check(!nya_crypto_unseal((void*)&KEY, arena, "other_field", (const char*)sealed->items, sealed->length, &not_opened).ok,
                  "a value sealed for one field does not open for another");
        printf("  PASSED\n");
    }

    // TEST: a @secret struct round-trips through a sealed save, in .nya and JSON
    for (u32 f = 0; f < nya_carray_length(formats); f++) {
        printf("TEST: sealed round trip (%s)\n", extensions[f]);

        NYA_String* path = nya_string_sprintf(arena, "%s/creds.%s", FIXTURE_DIRECTORY, extensions[f]);
        NYA_CString cpath = nya_string_to_cstring(arena, path);

        NYA_SerdeSecretExample written = { 0 };
        (void)snprintf(written.label, sizeof(written.label), "%s", "production");
        (void)snprintf(written.password, sizeof(written.password), "%s", PASSWORD);
        (void)snprintf(written.api_token, sizeof(written.api_token), "%s", API_TOKEN);
        written.pin = 90210;

        NYA_EXPECT(nya_reflect_save_file_secret(nya_reflect_of(NYA_SerdeSecretExample), &written, cpath, NYA_SERDE_PRETTY, secret));

        // The plaintext of a @secret field is nowhere in the file; the plaintext of the @redact field
        // beside it is, because @redact is a log rule and not a write rule. That is the whole contrast.
        NYA_String* on_disk = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(cpath, on_disk));
        nya_check(!contains(on_disk, PASSWORD), "the @secret password is not in the file");
        nya_check(!contains(on_disk, "90210"), "the @secret pin is not in the file");
        nya_check(contains(on_disk, API_TOKEN), "the @redact api_token is written plainly");
        nya_check(contains(on_disk, "production"), "and a plain field is untouched");

        // Not zeroed first: a real load must overwrite whatever was there.
        NYA_SerdeSecretExample read = { .pin = 1 };
        (void)snprintf(read.label, sizeof(read.label), "%s", "stale");

        NYA_EXPECT(nya_reflect_load_file_secret(nya_reflect_of(NYA_SerdeSecretExample), &read, cpath, NYA_SERDE_NONE, secret));

        nya_check(nya_memcmp(&written, &read, sizeof(written)) == 0, "and the struct comes back exactly, secrets and all");
        printf("  PASSED\n");
    }

    // TEST: without a key, a save refuses rather than writing the secret in the clear
    printf("TEST: a keyless save of a @secret type refuses\n");
    {
        NYA_SerdeSecretExample record = { 0 };
        (void)snprintf(record.password, sizeof(record.password), "%s", PASSWORD);

        NYA_ConstCString path = FIXTURE_DIRECTORY "/refused.nya";

        NYA_Error err = nya_reflect_save_file(nya_reflect_of(NYA_SerdeSecretExample), &record, path, NYA_SERDE_PRETTY);
        nya_check(!err.ok, "nya_reflect_save_file must refuse a @secret type");

        // And nothing was written: the refusal is before the file, not after.
        NYA_String* leaked = nya_string_create(arena);
        nya_check(!nya_file_read(path, leaked).ok, "and no file was left behind");
        printf("  PASSED\n");
    }

    // TEST: a load with the wrong key, or none, fails closed
    printf("TEST: a load without the right key fails\n");
    {
        NYA_ConstCString path = FIXTURE_DIRECTORY "/wrongkey.nya";

        NYA_SerdeSecretExample written = { 0 };
        (void)snprintf(written.password, sizeof(written.password), "%s", PASSWORD);
        NYA_EXPECT(nya_reflect_save_file_secret(nya_reflect_of(NYA_SerdeSecretExample), &written, path, NYA_SERDE_PRETTY, secret));

        NYA_SerdeSecretExample read = { 0 };

        nya_check(!nya_reflect_load_file_secret(nya_reflect_of(NYA_SerdeSecretExample), &read, path, NYA_SERDE_NONE, wrong_secret).ok,
                  "a wrong key must not open the file");

        nya_check(!nya_reflect_load_file(nya_reflect_of(NYA_SerdeSecretExample), &read, path, NYA_SERDE_NONE).ok,
                  "and no key at all must refuse rather than read the ciphertext as text");
        printf("  PASSED\n");
    }

    // TEST: a tampered file does not load
    printf("TEST: a tampered sealed value in the file fails to load\n");
    {
        NYA_ConstCString path = FIXTURE_DIRECTORY "/tampered.nya";

        NYA_SerdeSecretExample written = { 0 };
        (void)snprintf(written.password, sizeof(written.password), "%s", PASSWORD);
        NYA_EXPECT(nya_reflect_save_file_secret(nya_reflect_of(NYA_SerdeSecretExample), &written, path, NYA_SERDE_PRETTY, secret));

        NYA_String* on_disk = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(path, on_disk));

        // A terminated copy to walk with strstr; the raw string's bytes past its length are not ours.
        char* text = nya_string_to_cstring(arena, on_disk);

        // Flip one character well inside the password's sealed string, to a character still in the base64url alphabet, so the value decodes but no longer authenticates.
        char* marker = strstr(text, "password");
        nya_check(marker != nullptr, "the file names the field");

        char* quote = strchr(marker, '"');
        nya_check(quote != nullptr, "and quotes its value");

        char* victim = quote + 10;
        *victim      = (*victim == 'A') ? 'B' : 'A';

        NYA_EXPECT(nya_file_write(path, (NYA_ConstCString)text));

        NYA_SerdeSecretExample read = { 0 };
        nya_check(!nya_reflect_load_file_secret(nya_reflect_of(NYA_SerdeSecretExample), &read, path, NYA_SERDE_NONE, secret).ok,
                  "a tampered sealed value must fail closed");
        printf("  PASSED\n");
    }

    // TEST: @redact behaviour is unchanged, and @secret is masked in a log too
    printf("TEST: the redacting walk masks both @redact and @secret\n");
    {
        NYA_SerdeSecretExample record = { 0 };
        (void)snprintf(record.label, sizeof(record.label), "%s", "production");
        (void)snprintf(record.password, sizeof(record.password), "%s", PASSWORD);
        (void)snprintf(record.api_token, sizeof(record.api_token), "%s", API_TOKEN);

        NYA_Object* redacted = nya_reflect_to_object_redacted(arena, nya_reflect_of(NYA_SerdeSecretExample), &record);

        NYA_Value* password  = nya_object_get(redacted, "password");
        NYA_Value* api_token = nya_object_get(redacted, "api_token");
        NYA_Value* label     = nya_object_get(redacted, "label");

        nya_check(api_token != nullptr && api_token->type == NYA_TYPE_STRING && nya_string_equals(api_token->as_string, NYA_REFLECT_REDACTED),
                  "@redact is masked in a log, as before");
        nya_check(password != nullptr && password->type == NYA_TYPE_STRING && nya_string_equals(password->as_string, NYA_REFLECT_REDACTED),
                  "@secret is masked in a log too");
        nya_check(label != nullptr && nya_string_equals(label->as_string, "production"), "a plain field is left alone");

        // The plain (non-redacting) object still holds the real values: redacting is a log rule only.
        NYA_Object* plain = nya_reflect_to_object(arena, nya_reflect_of(NYA_SerdeSecretExample), &record);
        NYA_Value*  api_plain = nya_object_get(plain, "api_token");
        nya_check(api_plain != nullptr && nya_string_equals(api_plain->as_string, API_TOKEN), "nya_reflect_to_object still writes @redact plainly");
        printf("  PASSED\n");
    }

    (void)nya_filesystem_delete_recursive(FIXTURE_DIRECTORY);

    printf(nya_check_failures() == 0 ? "PASSED: test_secret_serde\n" : "FAILED: test_secret_serde\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
