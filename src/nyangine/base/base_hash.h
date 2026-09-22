#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** FNV-1a's 64 bit offset basis and prime, as the reference (draft-eastlake-fnv) gives them. */
#define NYA_HASH_FNV1A_OFFSET_BASIS 14695981039346656037ULL
#define NYA_HASH_FNV1A_PRIME        1099511628211ULL

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API u64 nya_hash_fnv1a(const void* data, u64 size) __attr_overloaded;

/**
 * Folds `size` more bytes into a running FNV-1a `hash`, for input that arrives in pieces. Starting from
 * NYA_HASH_FNV1A_OFFSET_BASIS gives exactly what nya_hash_fnv1a gives for the pieces laid end to end.
 * */
NYA_API u64 nya_hash_fnv1a_continue(u64 hash, const void* data, u64 size) __attr_no_discard;
NYA_API u64 nya_hash_fnv1a(NYA_ConstCString string) __attr_overloaded;
NYA_API u64 nya_hash_fnv1a(NYA_String string) __attr_overloaded;

/**
 * wyhash (final version 4, default seed and secret), eight bytes at a time. For in-memory tables on a hot
 * path: a 26 byte asset path costs about a fifth of FNV-1a. Little endian only and not keyed, so never
 * persist it or feed it attacker controlled keys.
 * */
NYA_API u64 nya_hash_wyhash(const void* data, u64 size) __attr_no_discard;

/**
 * SipHash-2-4: a keyed hash, i.e. a MAC. Unlike nya_crc64, a matching hash cannot be produced for
 * altered data without `key`. Not a replacement for HMAC-SHA256 where real cryptographic strength
 * is needed; for short inputs checked frequently against a non-cryptanalyst attacker, it is enough.
 * */
NYA_API u64 nya_siphash(const void* data, u64 size, u64 key_low, u64 key_high) __attr_no_discard;

NYA_API f32 nya_ihash2(s32 x, s32 y, u32 seed);
NYA_API f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed);

/*
 * ─────────────────────────────────────────────────────────
 * SHA-256
 * ─────────────────────────────────────────────────────────
 */

/**
 * The digest, in bytes. FIPS 180-4 fixes it at 32 and nothing about it is configurable.
 * */
#define NYA_SHA256_BYTES 32

/** One SHA-256 block, which is also the key length HMAC pads to. */
#define NYA_SHA256_BLOCK_BYTES 64

/**
 * SHA-256 of `size` bytes.
 *
 * The one hash here with real collision resistance, and the one to reach for when something outside
 * this process has to agree on the answer: an authentication challenge, a token signature, a content
 * address. Everything above it is for in-memory tables and is not that.
 *
 * ```c
 * u8 digest[NYA_SHA256_BYTES] = { 0 };
 * nya_sha256((const u8*)text, strlen(text), digest);
 * ```
 * */
NYA_API void nya_sha256(const u8* data, u64 size, OUT u8 out_digest[NYA_SHA256_BYTES]);

/**
 * HMAC-SHA256, RFC 2104. What a signed token is signed with.
 *
 * A key longer than NYA_SHA256_BLOCK_BYTES is hashed first, as the RFC requires, so any key length is
 * accepted and no caller has to know that rule.
 *
 * ```c
 * u8 tag[NYA_SHA256_BYTES] = { 0 };
 * nya_hmac_sha256(secret, secret_size, (const u8*)message, message_size, tag);
 * ```
 * */
NYA_API void nya_hmac_sha256(const u8* key, u64 key_size, const u8* data, u64 size, OUT u8 out_tag[NYA_SHA256_BYTES]);

/**
 * Whether two tags are equal, in time that does not depend on where they first differ.
 *
 * A plain memcmp returns early, and how early is a measurable fact about the secret. Use this and
 * nothing else to check a signature or an authentication tag.
 * */
NYA_API b8 nya_hash_equals_constant_time(const u8* a, const u8* b, u64 size) __attr_no_discard;
