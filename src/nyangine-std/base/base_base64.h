#pragma once

#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"

// FUNCTIONS AND MACROS

NYA_API void nya_base64_encode(NYA_String* base64, const u8* data, u64 len);
NYA_API void nya_base64_decode(NYA_String* base64, const u8* encoded, u64 len);
