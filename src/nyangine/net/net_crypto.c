#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Poly1305 tag length, appended to every sealed packet. */
#define _NYA_NET_MAC_SIZE NYA_CRYPTO_TAG_BYTES

/** The protocol's name, hashed first into the premaster so these bytes can never be another protocol's. */
#define _NYA_NET_CRYPTO_PROTOCOL "nyangine udp 6"

/** The premaster hashes the protocol's name and five keys: two exchanges and three public keys. */
#define _NYA_NET_CRYPTO_PREMASTER_KEYS  5
#define _NYA_NET_CRYPTO_PREMASTER_BYTES ((sizeof(_NYA_NET_CRYPTO_PROTOCOL) - 1) + ((u64)NYA_NET_KEY_SIZE * _NYA_NET_CRYPTO_PREMASTER_KEYS))

/** What the RESPONSE key is derived for, as the BLAKE2b message under the premaster. */
#define _NYA_NET_CRYPTO_RESPONSE "response"

static_assert(NYA_NET_KEY_SIZE == NYA_CRYPTO_EXCHANGE_KEY_BYTES, "a net key is an X25519 key");
static_assert(NYA_NET_KEY_SIZE == NYA_CRYPTO_KEY_BYTES, "a session key is an XChaCha20-Poly1305 key");

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_key_pair_create(OUT NYA_NetKeyPair* out_key_pair) {
    nya_assert(out_key_pair != nullptr);

    *out_key_pair = (NYA_NetKeyPair){ 0 };

    NYA_CryptoExchangeKeyPair pair = { 0 };
    defer nya_crypto_exchange_key_pair_destroy(&pair);

    NYA_TRY(nya_crypto_exchange_key_pair_create(&pair));

    nya_memcpy(out_key_pair->secret_key, pair.secret_key.bytes, NYA_NET_KEY_SIZE);
    nya_memcpy(out_key_pair->public_key, pair.public_key.bytes, NYA_NET_KEY_SIZE);

    return NYA_OK;
}

NYA_NetKeyPair nya_net_key_pair_from_secret(const u8* secret_key) {
    nya_assert(secret_key != nullptr);

    NYA_CryptoExchangeSecretKey secret = { 0 };
    nya_memcpy(secret.bytes, secret_key, NYA_NET_KEY_SIZE);

    NYA_CryptoExchangeKeyPair derived = { 0 };
    nya_crypto_exchange_key_pair_from_secret(&secret, &derived);

    NYA_NetKeyPair pair = { 0 };
    nya_memcpy(pair.secret_key, derived.secret_key.bytes, NYA_NET_KEY_SIZE);
    nya_memcpy(pair.public_key, derived.public_key.bytes, NYA_NET_KEY_SIZE);

    nya_crypto_wipe(&secret, sizeof(secret));
    nya_crypto_exchange_key_pair_destroy(&derived);

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

NYA_Error nya_net_key_pair_load(NYA_ConstCString path, OUT NYA_NetKeyPair* out_key_pair) {
    nya_assert(path != nullptr);
    nya_assert(out_key_pair != nullptr);

    *out_key_pair = (NYA_NetKeyPair){ 0 };

    NYA_Arena* scratch = nya_arena_create(.name = "net_key_pair_load");
    defer      nya_arena_destroy(scratch);

    if (nya_filesystem_is_file(path)) {
        NYA_Object* saved = nullptr;
        NYA_Error   read  = nya_serde_load_file(scratch, path, NYA_SERDE_NONE, &saved);

        if (read.ok) {
            NYA_Value* secret = nya_object_get(saved, "secret_key");
            u8         key[NYA_NET_KEY_SIZE] = { 0 };

            if (secret != nullptr && secret->type == NYA_TYPE_STRING && nya_net_key_from_hex(secret->as_string, key) && nya_net_key_is_set(key)) {
                *out_key_pair = nya_net_key_pair_from_secret(key);
                nya_crypto_wipe(key, sizeof(key));
                return NYA_OK;
            }
        }

        // a damaged file is replaced rather than trusted, which gives the endpoint a new identity. Said out loud, since players who pinned the old one will be refused.
        nya_log_warn("The key pair in '%s' is unreadable; making a new one.", path);
    }

    NYA_TRY(nya_net_key_pair_create(out_key_pair));

    char hex[NYA_NET_KEY_HEX_SIZE];
    nya_net_key_to_hex(out_key_pair->secret_key, hex);

    NYA_Object* fresh = nya_object_create(scratch);
    nya_object_add(fresh, "secret_key", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = hex });

    NYA_String* directory = nya_path_dirname(scratch, path);

    NYA_Error written = directory == nullptr || directory->length == 0
                            ? NYA_OK
                            : nya_filesystem_create_directory(nya_string_to_cstring(scratch, directory));

    if (written.ok) written = nya_serde_save_file(fresh, path, NYA_SERDE_NONE);

    nya_crypto_wipe(hex, sizeof(hex));

    return written;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_net_crypto_exchange(OUT u8* shared, const u8* secret_key, const u8* public_key) {
    NYA_CryptoExchangeSecretKey secret = { 0 };
    NYA_CryptoExchangePublicKey public = { 0 };
    NYA_CryptoSharedSecret      result = { 0 };

    nya_memcpy(secret.bytes, secret_key, NYA_NET_KEY_SIZE);
    nya_memcpy(public.bytes, public_key, NYA_NET_KEY_SIZE);

    b8 accepted = nya_crypto_exchange(&secret, &public, &result);
    nya_memcpy(shared, result.bytes, NYA_NET_KEY_SIZE);

    nya_crypto_wipe(&secret, sizeof(secret));
    nya_crypto_wipe(&result, sizeof(result));

    return accepted;
}

void _nya_net_crypto_premaster(
    OUT u8* premaster, const u8* dh_ephemeral_static, const u8* dh_static_static, const u8* server_static, const u8* client_ephemeral,
    const u8* client_static
) {
    // one buffer hashed once is the same BLAKE2b as the pieces fed in turn, so the bytes on the wire are
    // what they were when this streamed.
    u8  transcript[_NYA_NET_CRYPTO_PREMASTER_BYTES] = { 0 };
    u64 at                                          = 0;

    nya_memcpy(transcript + at, _NYA_NET_CRYPTO_PROTOCOL, sizeof(_NYA_NET_CRYPTO_PROTOCOL) - 1);
    at += sizeof(_NYA_NET_CRYPTO_PROTOCOL) - 1;

    const u8* parts[_NYA_NET_CRYPTO_PREMASTER_KEYS] = { dh_ephemeral_static, dh_static_static, server_static, client_ephemeral, client_static };
    for (u32 i = 0; i < nya_carray_length(parts); i++) {
        nya_memcpy(transcript + at, parts[i], NYA_NET_KEY_SIZE);
        at += NYA_NET_KEY_SIZE;
    }

    nya_assert(at == sizeof(transcript));

    nya_crypto_blake2b(transcript, sizeof(transcript), premaster, NYA_NET_KEY_SIZE);
    nya_crypto_wipe(transcript, sizeof(transcript));
}

void _nya_net_crypto_response_key(OUT u8* key, const u8* premaster) {
    nya_crypto_blake2b_keyed(premaster, NYA_NET_KEY_SIZE, (const u8*)_NYA_NET_CRYPTO_RESPONSE, sizeof(_NYA_NET_CRYPTO_RESPONSE) - 1, key, NYA_NET_KEY_SIZE);
}

void _nya_net_crypto_session(OUT u8* client_to_server, OUT u8* server_to_client, const u8* premaster, const u8* dh_ephemeral, const u8* server_ephemeral) {
    u8 material[NYA_NET_KEY_SIZE * 2] = { 0 };
    nya_memcpy(material, dh_ephemeral, NYA_NET_KEY_SIZE);
    nya_memcpy(material + NYA_NET_KEY_SIZE, server_ephemeral, NYA_NET_KEY_SIZE);

    u8 keys[NYA_NET_KEY_SIZE * 2] = { 0 };
    nya_crypto_blake2b_keyed(premaster, NYA_NET_KEY_SIZE, material, sizeof(material), keys, sizeof(keys));

    nya_memcpy(client_to_server, keys, NYA_NET_KEY_SIZE);
    nya_memcpy(server_to_client, keys + NYA_NET_KEY_SIZE, NYA_NET_KEY_SIZE);

    nya_crypto_wipe(material, sizeof(material));
    nya_crypto_wipe(keys, sizeof(keys));
}

void _nya_net_crypto_seal(const u8* key, u64 counter, const u8* ad, u64 ad_size, u8* text, u64 size, OUT u8* mac) {
    NYA_CryptoKey32   session = { 0 };
    NYA_CryptoNonce24 nonce   = nya_crypto_nonce_from_counter(counter);
    NYA_CryptoTag16   tag     = { 0 };

    nya_memcpy(session.bytes, key, NYA_NET_KEY_SIZE);

    nya_crypto_aead_encrypt(&session, &nonce, (NYA_CryptoAeadMessage){ .text = text, .text_size = size, .associated = ad, .associated_size = ad_size }, &tag);
    nya_memcpy(mac, tag.bytes, NYA_CRYPTO_TAG_BYTES);

    nya_crypto_key_destroy(&session);
}

b8 _nya_net_crypto_open(const u8* key, u64 counter, const u8* ad, u64 ad_size, u8* text, u64 size, const u8* mac) {
    NYA_CryptoKey32   session = { 0 };
    NYA_CryptoNonce24 nonce   = nya_crypto_nonce_from_counter(counter);
    NYA_CryptoTag16   tag     = { 0 };

    nya_memcpy(session.bytes, key, NYA_NET_KEY_SIZE);
    nya_memcpy(tag.bytes, mac, NYA_CRYPTO_TAG_BYTES);

    b8 opened = nya_crypto_aead_decrypt(
        &session, &nonce, (NYA_CryptoAeadMessage){ .text = text, .text_size = size, .associated = ad, .associated_size = ad_size }, &tag
    );

    nya_crypto_key_destroy(&session);

    return opened;
}
