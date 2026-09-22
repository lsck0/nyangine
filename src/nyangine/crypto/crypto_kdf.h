/**
 * @file crypto_kdf.h
 *
 * Argon2id, RFC 9106, for anything a person types.
 *
 * Stretches a password into a hash or a key, at a memory and time cost that makes guessing it offline
 * expensive.
 *
 * Overview:
 *   nya_crypto_argon2id   hash `password` under `salt` with the RFC's second recommended costs,
 *                         any of which a call may override
 *
 * ```c
 * u8 salt[NYA_CRYPTO_ARGON2ID_SALT_BYTES] = { 0 };
 * if (!nya_os_random_bytes(salt, sizeof(salt))) return nya_error(NYA_ERROR_NOT_OK, "no entropy");
 *
 * NYA_Arena* scratch = nya_arena_create(.name = "login");
 * defer      nya_arena_destroy(scratch);
 *
 * u8 hash[32] = { 0 };
 * NYA_TRY(nya_crypto_argon2id(scratch, hash, sizeof(hash), .password = typed, .password_size = typed_size,
 *                             .salt = salt, .salt_size = sizeof(salt)));
 * ```
 *
 * It allocates: the work area is `memory_kib` KiB, 64 MiB by default, from `arena`, wiped and freed back
 * to it before the call returns. An arena serves a block that large from a region of its own and does not
 * hand it out again until it is reset, so pass a scratch arena and destroy it, as above, rather than one
 * that lives for the program.
 *
 * Argon2id rather than bcrypt or scrypt: bcrypt caps a password at 72 bytes and costs no memory, so a
 * GPU guesses it cheaply, and scrypt's memory cost can be traded for time. Argon2id is the RFC's
 * recommendation and the one that has both defences.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * RFC 9106 section 4's second recommended option: t=3, p=4, m=2^16 KiB, a 128 bit salt. The first needs
 * 2 GiB per call, which one server logging several people in at once cannot spend. monocypher runs the
 * lanes one after another, so four cost the time one would; they stay four so the costs are the RFC's.
 * */
#ifndef NYA_CRYPTO_ARGON2ID_MEMORY_KIB
#define NYA_CRYPTO_ARGON2ID_MEMORY_KIB 65536
#endif
#ifndef NYA_CRYPTO_ARGON2ID_PASSES
#define NYA_CRYPTO_ARGON2ID_PASSES 3
#endif
#ifndef NYA_CRYPTO_ARGON2ID_LANES
#define NYA_CRYPTO_ARGON2ID_LANES 4
#endif

/** RFC 9106 section 3.1: 128 bits are enough for every use, and 64 is the floor this refuses below. */
#define NYA_CRYPTO_ARGON2ID_SALT_BYTES     16
#define NYA_CRYPTO_ARGON2ID_SALT_BYTES_MIN 8

/** RFC 9106 section 3.1: a tag is at least four bytes. */
#define NYA_CRYPTO_ARGON2ID_HASH_BYTES_MIN 4

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_CryptoArgon2idOptions NYA_CryptoArgon2idOptions;

/**
 * The inputs and costs of one Argon2id call. `key` is RFC 9106's optional secret, a pepper kept out of
 * the database; `associated` is its optional associated data. Either may be null with a zero size.
 * */
struct NYA_CryptoArgon2idOptions {
    const u8* password;
    u64       password_size;
    const u8* salt;
    u64       salt_size;
    const u8* key;
    u64       key_size;
    const u8* associated;
    u64       associated_size;

    u32 memory_kib;
    u32 passes;
    u32 lanes;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** nya_crypto_argon2id with every option spelled out. */
NYA_API NYA_Error _nya_crypto_argon2id(NYA_Arena* arena, OUT u8* out_hash, u64 hash_size, NYA_CryptoArgon2idOptions options) __attr_no_discard;

/** The costs every call starts from, before its own options override them. */
#define _NYA_CRYPTO_ARGON2ID_DEFAULT_OPTIONS                                                                                                         \
    .memory_kib = NYA_CRYPTO_ARGON2ID_MEMORY_KIB, .passes = NYA_CRYPTO_ARGON2ID_PASSES, .lanes = NYA_CRYPTO_ARGON2ID_LANES

/**
 * Hashes `.password` under `.salt` into `hash_size` bytes. Refuses a salt under
 * NYA_CRYPTO_ARGON2ID_SALT_BYTES_MIN, costs below the RFC's minimums, and inputs past monocypher's u32
 * sizes, all as NYA_ERROR_INVALID_ARGUMENT, since a password's length is the caller's user's to choose.
 * */
#define nya_crypto_argon2id(arena, out_hash, hash_size, ...)                                                                                         \
    _nya_crypto_argon2id((arena), (out_hash), (hash_size), (NYA_CryptoArgon2idOptions){ _NYA_CRYPTO_ARGON2ID_DEFAULT_OPTIONS, __VA_ARGS__ })
