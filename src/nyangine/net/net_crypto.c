#include "nyangine/nyangine.h"

#include "monocypher.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Poly1305 tag length, appended to every sealed packet. */
#define _NYA_NET_MAC_SIZE 16

/**
 * X25519 of a secret and a public key. False when the result is all zero, which is what a low order public key
 * produces and what a peer would send to force a known shared secret.
 * */
NYA_INTERNAL b8 _nya_net_crypto_exchange(OUT u8* shared, const u8* secret_key, const u8* public_key) __attr_no_discard;

/**
 * What the client can compute before it has heard the server's ephemeral key: both exchanges against the server's
 * identity, bound to the three public keys involved. `dh_static_static` and `client_static` are zero for an anonymous
 * player.
 * */
NYA_INTERNAL void _nya_net_crypto_premaster(
    OUT u8* premaster, const u8* dh_ephemeral_static, const u8* dh_static_static, const u8* server_static, const u8* client_ephemeral,
    const u8* client_static
);

/** The key the client's RESPONSE is tagged with, proving it holds what the premaster needs. */
NYA_INTERNAL void _nya_net_crypto_response_key(OUT u8* key, const u8* premaster);

/** The two session keys, once the ephemeral exchange is done. */
NYA_INTERNAL void _nya_net_crypto_session(
    OUT u8* client_to_server, OUT u8* server_to_client, const u8* premaster, const u8* dh_ephemeral, const u8* server_ephemeral
);

/** Encrypts `text` in place and writes its tag. `counter` must never repeat under one key. */
NYA_INTERNAL void _nya_net_crypto_seal(const u8* key, u64 counter, const u8* ad, u64 ad_size, u8* text, u64 size, OUT u8* mac);

/** Decrypts `text` in place, or leaves it and returns false when the tag does not match. */
NYA_INTERNAL b8 _nya_net_crypto_open(const u8* key, u64 counter, const u8* ad, u64 ad_size, u8* text, u64 size, const u8* mac)
    __attr_no_discard;

NYA_INTERNAL void _nya_net_crypto_wipe(void* secret, u64 size);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_key_pair_create(OUT NYA_NetKeyPair* out_key_pair) {
    nya_assert(out_key_pair != nullptr);

    *out_key_pair = (NYA_NetKeyPair){ 0 };

    u8 secret[NYA_NET_KEY_SIZE] = { 0 };
    if (!nya_random_bytes(secret, sizeof(secret))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    *out_key_pair = nya_net_key_pair_from_secret(secret);
    _nya_net_crypto_wipe(secret, sizeof(secret));

    return NYA_OK;
}

NYA_NetKeyPair nya_net_key_pair_from_secret(const u8* secret_key) {
    nya_assert(secret_key != nullptr);

    NYA_NetKeyPair pair = { 0 };

    nya_memcpy(pair.secret_key, secret_key, NYA_NET_KEY_SIZE);
    crypto_x25519_public_key(pair.public_key, pair.secret_key);

    return pair;
}

b8 nya_net_key_is_set(const u8* key) {
    nya_assert(key != nullptr);

    u8 any = 0;
    for (u32 i = 0; i < NYA_NET_KEY_SIZE; i++) any |= key[i];

    return any != 0;
}

void nya_net_key_to_hex(const u8* key, OUT char* out_hex) {
    nya_assert(key != nullptr);
    nya_assert(out_hex != nullptr);

    NYA_ConstCString digits = "0123456789abcdef";

    for (u64 i = 0; i < NYA_NET_KEY_SIZE; i++) {
        out_hex[i * 2]       = digits[key[i] >> 4];
        out_hex[(i * 2) + 1] = digits[key[i] & 0x0F];
    }

    out_hex[NYA_NET_KEY_HEX_SIZE - 1] = '\0';
}

b8 nya_net_key_from_hex(NYA_ConstCString hex, OUT u8* out_key) {
    nya_assert(out_key != nullptr);

    nya_memset(out_key, 0, NYA_NET_KEY_SIZE);

    if (hex == nullptr) return false;

    u8 key[NYA_NET_KEY_SIZE] = { 0 };

    for (u32 i = 0; i < NYA_NET_KEY_HEX_SIZE - 1; i++) {
        char c = hex[i];
        u8   nibble = 0;

        if (c >= '0' && c <= '9') nibble = (u8)(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = (u8)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = (u8)(c - 'A' + 10);
        else return false;

        key[i / 2] = (u8)((key[i / 2] << 4) | nibble);
    }

    if (hex[NYA_NET_KEY_HEX_SIZE - 1] != '\0') return false;

    nya_memcpy(out_key, key, NYA_NET_KEY_SIZE);

    return true;
}

NYA_Error nya_net_key_pair_load(NYA_ConstCString relative, OUT NYA_NetKeyPair* out_key_pair) {
    nya_assert(relative != nullptr);
    nya_assert(out_key_pair != nullptr);

    *out_key_pair = (NYA_NetKeyPair){ 0 };

    NYA_Arena* scratch = nya_arena_create(.name = "net_key_pair_load");
    defer      nya_arena_destroy(scratch);

    NYA_Object* saved = nullptr;
    NYA_Error   read  = nya_save_read(scratch, relative, NYA_SERDE_NONE, &saved);

    if (read.ok) {
        NYA_Value* secret = nya_object_get(saved, "secret_key");
        u8         key[NYA_NET_KEY_SIZE] = { 0 };

        if (secret != nullptr && secret->type == NYA_TYPE_STRING && nya_net_key_from_hex(secret->as_string, key) && nya_net_key_is_set(key)) {
            *out_key_pair = nya_net_key_pair_from_secret(key);
            _nya_net_crypto_wipe(key, sizeof(key));
            return NYA_OK;
        }

        // a damaged file is replaced rather than trusted, which gives the endpoint a new identity. Said out loud, since players who pinned the old one will be refused.
        nya_log_warn("The key pair in '%s' is unreadable; making a new one.", relative);
    } else if (read.kind != NYA_ERROR_NOT_FOUND) {
        return read;
    }

    NYA_TRY(nya_net_key_pair_create(out_key_pair));

    char hex[NYA_NET_KEY_HEX_SIZE];
    nya_net_key_to_hex(out_key_pair->secret_key, hex);

    NYA_Object* fresh = nya_object_create(scratch);
    nya_object_set(fresh, "secret_key", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = hex });

    NYA_Error written = nya_save_write(relative, fresh, NYA_SERDE_NONE);
    _nya_net_crypto_wipe(hex, sizeof(hex));

    return written;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_net_crypto_exchange(OUT u8* shared, const u8* secret_key, const u8* public_key) {
    crypto_x25519(shared, secret_key, public_key);

    return nya_net_key_is_set(shared);
}

void _nya_net_crypto_premaster(
    OUT u8* premaster, const u8* dh_ephemeral_static, const u8* dh_static_static, const u8* server_static, const u8* client_ephemeral,
    const u8* client_static
) {
    crypto_blake2b_ctx context;
    crypto_blake2b_init(&context, NYA_NET_KEY_SIZE);

    // the protocol name first, so these bytes can never be mistaken for another protocol's.
    crypto_blake2b_update(&context, (const u8*)"nyangine udp 6", 14);
    crypto_blake2b_update(&context, dh_ephemeral_static, NYA_NET_KEY_SIZE);
    crypto_blake2b_update(&context, dh_static_static, NYA_NET_KEY_SIZE);
    crypto_blake2b_update(&context, server_static, NYA_NET_KEY_SIZE);
    crypto_blake2b_update(&context, client_ephemeral, NYA_NET_KEY_SIZE);
    crypto_blake2b_update(&context, client_static, NYA_NET_KEY_SIZE);
    crypto_blake2b_final(&context, premaster);
}

void _nya_net_crypto_response_key(OUT u8* key, const u8* premaster) {
    crypto_blake2b_keyed(key, NYA_NET_KEY_SIZE, premaster, NYA_NET_KEY_SIZE, (const u8*)"response", 8);
}

void _nya_net_crypto_session(OUT u8* client_to_server, OUT u8* server_to_client, const u8* premaster, const u8* dh_ephemeral, const u8* server_ephemeral) {
    u8 material[NYA_NET_KEY_SIZE * 2] = { 0 };
    nya_memcpy(material, dh_ephemeral, NYA_NET_KEY_SIZE);
    nya_memcpy(material + NYA_NET_KEY_SIZE, server_ephemeral, NYA_NET_KEY_SIZE);

    u8 keys[NYA_NET_KEY_SIZE * 2] = { 0 };
    crypto_blake2b_keyed(keys, sizeof(keys), premaster, NYA_NET_KEY_SIZE, material, sizeof(material));

    nya_memcpy(client_to_server, keys, NYA_NET_KEY_SIZE);
    nya_memcpy(server_to_client, keys + NYA_NET_KEY_SIZE, NYA_NET_KEY_SIZE);

    _nya_net_crypto_wipe(material, sizeof(material));
    _nya_net_crypto_wipe(keys, sizeof(keys));
}

void _nya_net_crypto_seal(const u8* key, u64 counter, const u8* ad, u64 ad_size, u8* text, u64 size, OUT u8* mac) {
    u8 nonce[24] = { 0 };
    for (u32 i = 0; i < 8; i++) nonce[i] = (u8)((counter >> (i * 8)) & 0xFF);

    // a tag over the header alone still needs somewhere to point the text.
    u8 none = 0;
    if (text == nullptr) text = &none;

    crypto_aead_lock(text, mac, key, nonce, ad, ad_size, text, size);
}

b8 _nya_net_crypto_open(const u8* key, u64 counter, const u8* ad, u64 ad_size, u8* text, u64 size, const u8* mac) {
    u8 nonce[24] = { 0 };
    for (u32 i = 0; i < 8; i++) nonce[i] = (u8)((counter >> (i * 8)) & 0xFF);

    u8 none = 0;
    if (text == nullptr) text = &none;

    return crypto_aead_unlock(text, mac, key, nonce, ad, ad_size, text, size) == 0;
}

void _nya_net_crypto_wipe(void* secret, u64 size) {
    crypto_wipe(secret, size);
}
