/**
 * The crypto hot paths: what a login pays for Argon2id, what a hash and a MAC cost per byte, what the
 * per-packet AEAD costs, and what one seal and unseal cost through the keyring.
 *
 * Two shapes of number live here and they are not comparable. Argon2id is deliberately slow — its whole
 * job is to cost a guesser — so it is reported per call, and the number worth reading is the milliseconds
 * a person waits on a login. Everything else is a throughput path a server runs per request or per
 * packet, so it is reported per byte, and dividing 1000 by the ns/item gives its MB/s.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/* A megabyte to hash: big enough that the per-call overhead vanishes and the number is the compression function's. */
#define HASH_BYTES (1024 * 1024)

/* A packet-sized body for the AEAD, which is where the per-message path actually runs. */
#define PACKET_BYTES 4096

static u32 lcg = 0x1234567u;

static u8 next_byte(void) {
    lcg = (u32)((((u64)lcg * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull);
    return (u8)(lcg >> 16);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_crypto");
    defer      nya_arena_destroy(arena);

    u8* buffer = nya_arena_alloc(arena, HASH_BYTES);
    nya_assert(buffer != nullptr);
    for (u64 i = 0; i < HASH_BYTES; i++) buffer[i] = next_byte();

    // Argon2id: the login cost, reported per call
    {
        /* A scratch arena the KDF allocates its 64 MiB work area from and frees back to; reused across rounds, since the free returns the block for the next call rather than growing the arena. */
        NYA_Arena* kdf = nya_arena_create(.name = "bench_crypto_kdf");
        defer      nya_arena_destroy(kdf);

        u8 salt[NYA_CRYPTO_ARGON2ID_SALT_BYTES];
        for (u32 i = 0; i < sizeof(salt); i++) salt[i] = next_byte();

        NYA_ConstCString password      = "correct horse battery staple";
        u64              password_size = strlen(password);

        nya_bench_begin("argon2id (per call — this is the number a person waits on)");

        // The shipped default: RFC 9106's second option, 64 MiB, t=3, p=4. What a real login runs.
        nya_bench("default cost, m=64MiB t=3 p=4", 0, {
            u8 hash[32] = { 0 };
            NYA_EXPECT(nya_crypto_argon2id(kdf, hash, sizeof(hash), .password = (const u8*)password, .password_size = password_size,
                                           .salt = salt, .salt_size = sizeof(salt)));
            nya_bench_keep(hash[0]);
        });

        // A cheaper cost, so the optimisation pass can see how the number tracks the memory parameter.
        nya_bench("light cost, m=8MiB t=3 p=4", 0, {
            u8 hash[32] = { 0 };
            NYA_EXPECT(nya_crypto_argon2id(kdf, hash, sizeof(hash), .password = (const u8*)password, .password_size = password_size,
                                           .salt = salt, .salt_size = sizeof(salt), .memory_kib = 8192));
            nya_bench_keep(hash[0]);
        });

        if (nya_bench_end() != 0) return 1;
    }

    // Hashing and MAC: throughput, reported per byte (1000 / ns-per-item = MB/s)
    {
        nya_bench_begin("hashing, 1 MiB (ns/item is ns/byte; 1000 / it = MB/s)");

        nya_bench("sha256", HASH_BYTES, {
            NYA_CryptoSha256Digest digest = { 0 };
            nya_crypto_sha256(buffer, HASH_BYTES, &digest);
            nya_bench_keep(digest.bytes[0]);
        });

        nya_bench("hmac-sha256", HASH_BYTES, {
            NYA_CryptoSha256Digest tag = { 0 };
            nya_crypto_hmac_sha256((const u8*)"a thirty two byte mac key here!!", 32, buffer, HASH_BYTES, &tag);
            nya_bench_keep(tag.bytes[0]);
        });

        // What only the engine reads: the fast MAC and key-derivation hash.
        nya_bench("blake2b", HASH_BYTES, {
            u8 hash[32] = { 0 };
            nya_crypto_blake2b(buffer, HASH_BYTES, hash, sizeof(hash));
            nya_bench_keep(hash[0]);
        });

        // Kept for TOTP and the WebSocket handshake, and measured so its cost against SHA-256 is on record.
        nya_bench("sha1", HASH_BYTES, {
            NYA_CryptoSha1Digest digest = { 0 };
            nya_crypto_sha1(buffer, HASH_BYTES, &digest);
            nya_bench_keep(digest.bytes[0]);
        });

        if (nya_bench_end() != 0) return 1;
    }

    // AEAD: the per-message path a session encrypts and opens, per byte over a packet
    {
        NYA_CryptoKey32 key = { 0 };
        for (u32 i = 0; i < sizeof(key.bytes); i++) key.bytes[i] = next_byte();

        u8* text = nya_arena_alloc(arena, PACKET_BYTES);
        nya_assert(text != nullptr);
        for (u32 i = 0; i < PACKET_BYTES; i++) text[i] = next_byte();

        nya_bench_begin("aead xchacha20-poly1305, 4 KiB packet (ns/item is ns/byte)");

        // Encrypt in place and undo it, since the body mutates its own input.
        nya_bench("encrypt", PACKET_BYTES, {
            NYA_CryptoNonce24     nonce   = nya_crypto_nonce_from_counter(1);
            NYA_CryptoTag16       tag     = { 0 };
            NYA_CryptoAeadMessage message = { .text = text, .text_size = PACKET_BYTES };
            nya_crypto_aead_encrypt(&key, &nonce, message, &tag);
            nya_bench_keep(tag.bytes[0]);
            // back to plaintext so the next round encrypts the same bytes it did.
            (void)nya_crypto_aead_decrypt(&key, &nonce, message, &tag);
        });

        // Decrypt of a genuine message: what a peer pays per packet it accepts.
        {
            NYA_CryptoNonce24     nonce  = nya_crypto_nonce_from_counter(2);
            NYA_CryptoTag16       tag    = { 0 };
            NYA_CryptoAeadMessage sealed = { .text = text, .text_size = PACKET_BYTES };
            nya_crypto_aead_encrypt(&key, &nonce, sealed, &tag);

            nya_bench("decrypt (valid tag)", PACKET_BYTES, {
                NYA_CryptoAeadMessage message = { .text = text, .text_size = PACKET_BYTES };
                b8                    ok      = nya_crypto_aead_decrypt(&key, &nonce, message, &tag);
                nya_bench_keep(ok);
                // re-encrypt, so every round opens the same ciphertext.
                nya_crypto_aead_encrypt(&key, &nonce, message, &tag);
            });
        }

        if (nya_bench_end() != 0) return 1;
    }

    // The keyring seal and unseal: what a stateless cookie costs to mint and to open, per call
    {
        NYA_HttpKeyring ring = { 0 };
        (void)nya_http_keyring_rotate(&ring);

        struct {
            u64 who;
            u64 issued;
        } payload = { .who = 0x1122334455667788ull, .issued = 1234567890ull };

        char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
        NYA_EXPECT(nya_http_keyring_seal(&ring, "session", (const u8*)&payload, sizeof(payload), 900, token, sizeof(token)));
        u64 token_size = strlen(token);

        nya_bench_begin("keyring seal / unseal (per call)");

        nya_bench("seal", 0, {
            char out[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
            NYA_EXPECT(nya_http_keyring_seal(&ring, "session", (const u8*)&payload, sizeof(payload), 900, out, sizeof(out)));
            nya_bench_keep(out[0]);
        });

        nya_bench("unseal (valid token)", 0, {
            u8  opened[sizeof(payload)] = { 0 };
            u64 opened_size             = 0;
            b8  ok = nya_http_keyring_unseal(&ring, "session", token, token_size, opened, sizeof(opened), &opened_size);
            nya_bench_keep(ok);
        });

        nya_http_keyring_wipe(&ring);

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
