#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN void nya_base64_encode(NYA_String* base64, const u8* data, u64 len);
NYA_API NYA_EXTERN void nya_base64_decode(NYA_String* base64, const u8* encoded, u64 len);
