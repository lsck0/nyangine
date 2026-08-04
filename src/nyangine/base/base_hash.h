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
 * SipHash-2-4: a keyed hash, i.e. a MAC.
 *
 * The difference from nya_crc64 that matters: a checksum can be recomputed by anyone, so it detects
 * accidental corruption but not deliberate modification — change the data, recompute, done. Without
 * `key` you cannot produce a matching SipHash for altered data.
 *
 * Not a replacement for HMAC-SHA256 where real cryptographic strength is needed; it is the standard
 * choice where the input is short, the check is frequent, and the attacker is a person with a hex
 * editor rather than a cryptanalyst.
 * */
NYA_API u64 nya_siphash(const void* data, u64 size, u64 key_low, u64 key_high) __attr_no_discard;

NYA_API f32 nya_ihash2(s32 x, s32 y, u32 seed);
NYA_API f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed);
