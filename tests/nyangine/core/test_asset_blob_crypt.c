/**
 * The framing behind optional at-rest asset obfuscation (core_asset_crypt.h): the exact functions the build's
 * bundler frames every entry with and the loader unframes each entry with. A round trip returns the bytes
 * unchanged; any altered byte, a wrong path or a wrong key is refused; and framing is reproducible, which is
 * what lets an encrypted blob be a deterministic build output.
 *
 * This is the anti-casual-extraction bar the TODO asks for, tested at the codec. It is honestly obfuscation:
 * the key is derived and shipped in the binary, so it stops a hex editor, not a determined attacker.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#include "nyangine-core/core/core_asset_crypt.h"

/** The key the bundler derives: SHA-256 of the passphrase. */
static NYA_CryptoKey32 key_of(NYA_ConstCString passphrase) {
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256((const u8*)passphrase, strlen(passphrase), &digest);

    NYA_CryptoKey32 key = { 0 };
    nya_memcpy(key.bytes, digest.bytes, sizeof(key.bytes));
    return key;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_asset_blob_crypt");
    defer nya_arena_destroy(arena);

    NYA_CryptoKey32  key  = key_of("nyangine-default-asset-obfuscation-key");
    NYA_ConstCString path = "./assets/textures/hero.png";

    // Sizes that exercise the empty entry, a tiny one and one past a cipher block.
    u64 sizes[] = { 0, 1, 37, 4096 };

    printf("TEST: framing round-trips to identical bytes\n");
    {
        for (u32 i = 0; i < nya_carray_length(sizes); i++) {
            u64 size  = sizes[i];
            u8* plain = nya_arena_alloc(arena, nya_max(size, (u64)1));
            for (u64 b = 0; b < size; b++) plain[b] = (u8)(b * 31 + 7);

            u8* framed      = nya_arena_alloc(arena, size + NYA_ASSET_BLOB_FRAME_OVERHEAD);
            u64 framed_size = nya_asset_blob_frame(&key, plain, size, path, framed);
            nya_check(framed_size == size + NYA_ASSET_BLOB_FRAME_OVERHEAD, "a frame is the plaintext plus nonce and tag, " FMTu64, framed_size);

            // The ciphertext is not the plaintext (nothing left in the clear), for a non-empty entry.
            if (size > 0) nya_check(nya_memcmp(framed + NYA_CRYPTO_NONCE_BYTES, plain, size) != 0, "the stored bytes are not the plaintext");

            u8* out = nya_arena_alloc(arena, nya_max(size, (u64)1));
            nya_check(nya_asset_blob_unframe(&key, framed, framed_size, path, out), "the frame opens under the right key and path, size " FMTu64, size);
            nya_check(nya_memcmp(out, plain, size) == 0, "and gives back exactly what went in, size " FMTu64, size);
        }
        printf("  PASSED\n");
    }

    printf("TEST: framing the same input twice is byte-identical (reproducible builds)\n");
    {
        u8 plain[64];
        for (u64 b = 0; b < sizeof(plain); b++) plain[b] = (u8)(b * 5 + 1);

        u8 a[sizeof(plain) + NYA_ASSET_BLOB_FRAME_OVERHEAD];
        u8 c[sizeof(plain) + NYA_ASSET_BLOB_FRAME_OVERHEAD];
        nya_asset_blob_frame(&key, plain, sizeof(plain), path, a);
        nya_asset_blob_frame(&key, plain, sizeof(plain), path, c);
        nya_check(nya_memcmp(a, c, sizeof(a)) == 0, "the same key, path and bytes frame identically");
        printf("  PASSED\n");
    }

    printf("TEST: any tampering, a wrong path or a wrong key is refused\n");
    {
        u8 plain[100];
        for (u64 b = 0; b < sizeof(plain); b++) plain[b] = (u8)(b + 3);

        u64 framed_size = sizeof(plain) + NYA_ASSET_BLOB_FRAME_OVERHEAD;
        u8  framed[sizeof(plain) + NYA_ASSET_BLOB_FRAME_OVERHEAD];
        nya_asset_blob_frame(&key, plain, sizeof(plain), path, framed);

        u8 out[sizeof(plain)];

        // A flipped bit in the nonce, the ciphertext and the tag: each end of the frame and its middle.
        u64 spots[] = { 0, NYA_CRYPTO_NONCE_BYTES + sizeof(plain) / 2, framed_size - 1 };
        for (u32 i = 0; i < nya_carray_length(spots); i++) {
            u8 tampered[sizeof(framed)];
            nya_memcpy(tampered, framed, framed_size);
            tampered[spots[i]] ^= 0x01U;
            nya_check(!nya_asset_blob_unframe(&key, tampered, framed_size, path, out), "a flipped byte at " FMTu64 " is refused", spots[i]);
        }

        // A truncated frame, shorter than even the overhead.
        nya_check(!nya_asset_blob_unframe(&key, framed, NYA_ASSET_BLOB_FRAME_OVERHEAD - 1, path, out), "a frame too short to hold nonce and tag is refused");

        // The right key and bytes but the wrong path: the associated data binds an entry to where it was baked.
        nya_check(!nya_asset_blob_unframe(&key, framed, framed_size, "./assets/textures/other.png", out), "another path is refused");

        // The wrong key.
        NYA_CryptoKey32 wrong = key_of("some-other-passphrase");
        nya_check(!nya_asset_blob_unframe(&wrong, framed, framed_size, path, out), "another key is refused");
        printf("  PASSED\n");
    }

    nya_log_info("PASSED: test_asset_blob_crypt");

    return nya_check_failures() == 0 ? 0 : 1;
}
