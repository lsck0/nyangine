#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/crypto/crypto_kdf.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "monocypher.h"

// PRIVATE API DECLARATION

/** An Argon2 block is 1 KiB, so the memory cost in KiB is the block count. */
#define _NYA_CRYPTO_ARGON2_BLOCK_BYTES 1024

/** RFC 9106 section 3.1: at least eight blocks per lane, and a lane count under 2^24. */
#define _NYA_CRYPTO_ARGON2_BLOCKS_PER_LANE_MIN 8
#define _NYA_CRYPTO_ARGON2_LANES_MAX           ((1U << 24) - 1)

// PUBLIC API IMPLEMENTATION

NYA_Error _nya_crypto_argon2id(NYA_Arena* arena, OUT u8* out_hash, u64 hash_size, NYA_CryptoArgon2idOptions options) {
    nya_assert(arena != nullptr);
    nya_assert(out_hash != nullptr);

    if (hash_size < NYA_CRYPTO_ARGON2ID_HASH_BYTES_MIN || hash_size > U32_MAX) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an Argon2id hash is 4 bytes or more, not " FMTu64, hash_size);
    }

    nya_memset(out_hash, 0, hash_size);

    if (options.password == nullptr && options.password_size > 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a password size without a password");
    if (options.salt == nullptr || options.salt_size < NYA_CRYPTO_ARGON2ID_SALT_BYTES_MIN) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an Argon2id salt is at least %d bytes", NYA_CRYPTO_ARGON2ID_SALT_BYTES_MIN);
    }
    if (options.key == nullptr && options.key_size > 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a key size without a key");
    if (options.associated == nullptr && options.associated_size > 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an associated size without data");

    // monocypher takes every size as a u32; a password's length is an input, so this errors rather than asserts.
    if (options.password_size > U32_MAX || options.salt_size > U32_MAX || options.key_size > U32_MAX || options.associated_size > U32_MAX) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an Argon2id input past 4 GiB");
    }

    if (options.passes < 1) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "Argon2id takes at least one pass");
    if (options.lanes < 1 || options.lanes > _NYA_CRYPTO_ARGON2_LANES_MAX) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "Argon2id takes 1 to 2^24 - 1 lanes, not %u", options.lanes);
    }
    if ((u64)options.memory_kib < (u64)_NYA_CRYPTO_ARGON2_BLOCKS_PER_LANE_MIN * options.lanes) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "Argon2id needs at least 8 KiB per lane, and %u KiB is under that", options.memory_kib);
    }

    u64 work_size = (u64)options.memory_kib * _NYA_CRYPTO_ARGON2_BLOCK_BYTES;
    u8* work      = nya_arena_alloc(arena, work_size);
    if (work == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for a %u KiB Argon2id work area", options.memory_kib);

    // monocypher reads the blocks as u64, and the arena's alignment is what makes that legal.
    nya_assert(((uintptr_t)work % sizeof(u64)) == 0);

    crypto_argon2_config config = {
        .algorithm = CRYPTO_ARGON2_ID,
        .nb_blocks = options.memory_kib,
        .nb_passes = options.passes,
        .nb_lanes  = options.lanes,
    };
    crypto_argon2_inputs inputs = {
        .pass      = options.password,
        .salt      = options.salt,
        .pass_size = (u32)options.password_size,
        .salt_size = (u32)options.salt_size,
    };
    crypto_argon2_extras extras = {
        .key      = options.key,
        .ad       = options.associated,
        .key_size = (u32)options.key_size,
        .ad_size  = (u32)options.associated_size,
    };

    crypto_argon2(out_hash, (u32)hash_size, work, config, inputs, extras);

    // the blocks are the password stretched, and monocypher leaves them where they were computed.
    nya_crypto_wipe(work, work_size);
    nya_arena_free(arena, work, work_size);

    return NYA_OK;
}
