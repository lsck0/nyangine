#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API u8  nya_crc8(const u8* data, u64 len);
NYA_API u16 nya_crc16(const u8* data, u64 len);
NYA_API u32 nya_crc32(const u8* data, u64 len);
NYA_API u64 nya_crc64(const u8* data, u64 len);
