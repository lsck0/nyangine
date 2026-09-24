#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/** FNV-1a's 64 bit offset basis and prime, as the reference (draft-eastlake-fnv) gives them. */
#define NYA_HASH_FNV1A_OFFSET_BASIS 14695981039346656037ULL
#define NYA_HASH_FNV1A_PRIME        1099511628211ULL

// FUNCTIONS AND MACROS

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
