#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API u64 nya_hash_fnv1a(const void* data, u64 size) __attr_overloaded;
NYA_API u64 nya_hash_fnv1a(NYA_ConstCString string) __attr_overloaded;
NYA_API u64 nya_hash_fnv1a(NYA_String string) __attr_overloaded;

/**
 * SipHash-2-4: a keyed hash, i.e. a MAC. Unlike nya_crc64, a matching hash cannot be produced for
 * altered data without `key`. Not a replacement for HMAC-SHA256 where real cryptographic strength
 * is needed; for short inputs checked frequently against a non-cryptanalyst attacker, it is enough.
 * */
NYA_API u64 nya_siphash(const void* data, u64 size, u64 key_low, u64 key_high) __attr_no_discard;

NYA_API f32 nya_ihash2(s32 x, s32 y, u32 seed);
NYA_API f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed);
