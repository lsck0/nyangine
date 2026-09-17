/**
 * @file net_crypto.h
 *
 * ```c
 * NYA_NetKeyPair identity = { 0 };
 * NYA_EXPECT(nya_net_key_pair_create(&identity));
 *
 * char hex[NYA_NET_KEY_HEX_SIZE];
 * nya_net_key_to_hex(identity.public_key, hex);   // what a player pins with --server-key
 * ```
 *
 * Every UDP connection is encrypted and authenticated. The handshake is an X25519 exchange between the
 * client's ephemeral key, the server's long term key (its identity), the server's ephemeral key and, when the
 * player has one, the player's long term key. Packets are sealed with XChaCha20-Poly1305 under a key per
 * direction, with the packet's sequence number as the nonce, so a replayed or altered packet never reaches
 * the layers above. A client that is given the server's public key refuses any other server; one that is not
 * trusts the first key it sees.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NetKeyPair NYA_NetKeyPair;

#define NYA_NET_KEY_SIZE 32

/** Two hex digits per byte and a terminator. */
#define NYA_NET_KEY_HEX_SIZE ((NYA_NET_KEY_SIZE * 2) + 1)

/** A long term X25519 key pair: who a server is, or who a player is. All zero means none. */
struct NYA_NetKeyPair {
    u8 secret_key[NYA_NET_KEY_SIZE];
    u8 public_key[NYA_NET_KEY_SIZE];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A fresh key pair from the operating system's random source. Fails only when that source does. */
NYA_API NYA_Error nya_net_key_pair_create(OUT NYA_NetKeyPair* out_key_pair) __attr_no_discard;

/** The pair a stored secret key belongs to. */
NYA_API NYA_NetKeyPair nya_net_key_pair_from_secret(const u8* secret_key) __attr_no_discard;

/** Whether any byte of a key is set. */
NYA_API b8 nya_net_key_is_set(const u8* key) __attr_no_discard;

NYA_API void nya_net_key_to_hex(const u8* key, OUT char* out_hex);

/** Parses exactly 64 hex digits. False, with `out_key` zeroed, for anything else. */
NYA_API b8 nya_net_key_from_hex(NYA_ConstCString hex, OUT u8* out_key) __attr_no_discard;
