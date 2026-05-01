#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN u8  nya_crc8(const u8* data, u64 len);
NYA_API NYA_EXTERN u16 nya_crc16(const u8* data, u64 len);
NYA_API NYA_EXTERN u32 nya_crc32(const u8* data, u64 len);
NYA_API NYA_EXTERN u64 nya_crc64(const u8* data, u64 len);
