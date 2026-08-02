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

NYA_API f32 nya_ihash2(s32 x, s32 y, u32 seed);
NYA_API f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed);
