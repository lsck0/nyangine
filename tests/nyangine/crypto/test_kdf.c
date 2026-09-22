/**
 * Argon2id: crypto_kdf.h. RFC 9106 section 5.3's vector, which exercises the secret and the associated
 * data as well as the password and salt; then every input the call must refuse rather than hash weakly;
 * then one call at the default costs, which is what a login will actually run.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TESTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_kdf");
    defer      nya_arena_destroy(arena);

    // RFC 9106 section 5.3: the inputs are runs of one byte each.
    u8 password[32];
    u8 salt[16];
    u8 secret[8];
    u8 associated[12];
    nya_memset(password, 0x01, sizeof(password));
    nya_memset(salt, 0x02, sizeof(salt));
    nya_memset(secret, 0x03, sizeof(secret));
    nya_memset(associated, 0x04, sizeof(associated));

    printf("TEST: the Argon2id vector RFC 9106 prints\n");
    {
        const u8 expected[32] = {
            0x0D, 0x64, 0x0D, 0xF5, 0x8D, 0x78, 0x76, 0x6C, 0x08, 0xC0, 0x37, 0xA3, 0x4A, 0x8B, 0x53, 0xC9,
            0xD0, 0x1E, 0xF0, 0x45, 0x2D, 0x75, 0xB6, 0x5E, 0xB5, 0x25, 0x20, 0xE9, 0x6B, 0x01, 0xE6, 0x59,
        };

        u8        hash[32] = { 0 };
        NYA_Error hashed   = nya_crypto_argon2id(
            arena,
            hash,
            sizeof(hash),
            .password        = password,
            .password_size   = sizeof(password),
            .salt            = salt,
            .salt_size       = sizeof(salt),
            .key             = secret,
            .key_size        = sizeof(secret),
            .associated      = associated,
            .associated_size = sizeof(associated),
            .memory_kib      = 32,
            .passes          = 3,
            .lanes           = 4
        );

        nya_assert(hashed.ok, "%s", (const char*)hashed.message);
        nya_assert(nya_memcmp(hash, expected, sizeof(expected)) == 0, "the tag is not the published one");

        // without the secret it is another hash, which is what makes a pepper worth keeping out of the database.
        u8        unpeppered[32] = { 0 };
        NYA_Error again          = nya_crypto_argon2id(
            arena,
            unpeppered,
            sizeof(unpeppered),
            .password        = password,
            .password_size   = sizeof(password),
            .salt            = salt,
            .salt_size       = sizeof(salt),
            .associated      = associated,
            .associated_size = sizeof(associated),
            .memory_kib      = 32,
            .passes          = 3,
            .lanes           = 4
        );
        nya_assert(again.ok);
        nya_assert(nya_memcmp(hash, unpeppered, sizeof(hash)) != 0, "the secret changed nothing");

        printf("  the tag matched, and dropping the secret changes it\n");
    }

    printf("TEST: inputs that would hash weakly are refused\n");
    {
        u8 hash[32] = { 0 };

        // a salt under 64 bits.
        NYA_Error short_salt =
            nya_crypto_argon2id(arena, hash, sizeof(hash), .password = password, .password_size = sizeof(password), .salt = salt, .salt_size = 7);
        nya_assert(short_salt.kind == NYA_ERROR_INVALID_ARGUMENT);

        // no salt at all.
        NYA_Error no_salt = nya_crypto_argon2id(arena, hash, sizeof(hash), .password = password, .password_size = sizeof(password));
        nya_assert(no_salt.kind == NYA_ERROR_INVALID_ARGUMENT);

        // under eight blocks a lane, no passes, no lanes.
        NYA_Error little_memory = nya_crypto_argon2id(
            arena,
            hash,
            sizeof(hash),
            .password      = password,
            .password_size = sizeof(password),
            .salt          = salt,
            .salt_size     = sizeof(salt),
            .memory_kib    = 31,
            .lanes         = 4
        );
        nya_assert(little_memory.kind == NYA_ERROR_INVALID_ARGUMENT);

        NYA_Error no_passes = nya_crypto_argon2id(
            arena,
            hash,
            sizeof(hash),
            .password      = password,
            .password_size = sizeof(password),
            .salt          = salt,
            .salt_size     = sizeof(salt),
            .passes        = 0
        );
        nya_assert(no_passes.kind == NYA_ERROR_INVALID_ARGUMENT);

        NYA_Error no_lanes = nya_crypto_argon2id(
            arena,
            hash,
            sizeof(hash),
            .password      = password,
            .password_size = sizeof(password),
            .salt          = salt,
            .salt_size     = sizeof(salt),
            .lanes         = 0
        );
        nya_assert(no_lanes.kind == NYA_ERROR_INVALID_ARGUMENT);

        // a tag under four bytes.
        NYA_Error short_hash =
            nya_crypto_argon2id(arena, hash, 3, .password = password, .password_size = sizeof(password), .salt = salt, .salt_size = sizeof(salt));
        nya_assert(short_hash.kind == NYA_ERROR_INVALID_ARGUMENT);

        // a size with no bytes behind it.
        NYA_Error phantom =
            nya_crypto_argon2id(arena, hash, sizeof(hash), .password = nullptr, .password_size = 4, .salt = salt, .salt_size = sizeof(salt));
        nya_assert(phantom.kind == NYA_ERROR_INVALID_ARGUMENT);

        // a password past monocypher's u32, which a caller could be handed; refused before it is read.
        NYA_Error huge = nya_crypto_argon2id(
            arena,
            hash,
            sizeof(hash),
            .password      = password,
            .password_size = (u64)U32_MAX + 1,
            .salt          = salt,
            .salt_size     = sizeof(salt)
        );
        nya_assert(huge.kind == NYA_ERROR_INVALID_ARGUMENT);

        printf("  short salt, no salt, little memory, no passes, no lanes, short tag, phantom and huge sizes\n");
    }

    printf("TEST: the default costs\n");
    {
        u8 first[32]  = { 0 };
        u8 second[32] = { 0 };

        // a scratch arena per call, as the header asks, so the 64 MiB work area is given back to the system.
        {
            NYA_Arena* scratch = nya_arena_create(.name = "test_kdf_scratch");
            defer      nya_arena_destroy(scratch);

            NYA_Error hashed = nya_crypto_argon2id(
                scratch,
                first,
                sizeof(first),
                .password      = (const u8*)"correct horse",
                .password_size = 13,
                .salt          = salt,
                .salt_size     = sizeof(salt)
            );
            nya_assert(hashed.ok, "%s", (const char*)hashed.message);
        }
        {
            NYA_Arena* scratch = nya_arena_create(.name = "test_kdf_scratch");
            defer      nya_arena_destroy(scratch);

            NYA_Error hashed = nya_crypto_argon2id(
                scratch,
                second,
                sizeof(second),
                .password      = (const u8*)"correct horsf",
                .password_size = 13,
                .salt          = salt,
                .salt_size     = sizeof(salt)
            );
            nya_assert(hashed.ok, "%s", (const char*)hashed.message);
        }

        nya_assert(nya_memcmp(first, second, sizeof(first)) != 0, "two passwords hashed alike");

        printf(
            "  %d KiB, %d passes, %d lanes: two passwords one letter apart hash apart\n",
            NYA_CRYPTO_ARGON2ID_MEMORY_KIB,
            NYA_CRYPTO_ARGON2ID_PASSES,
            NYA_CRYPTO_ARGON2ID_LANES
        );
    }

    printf("PASSED: test_kdf (0 failures)\n");

    return EXIT_SUCCESS;
}
