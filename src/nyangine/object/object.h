#pragma once

#include "nyangine/base/base.h"

NYA_API NYA_EXTERN b8 nya_type_parse(NYA_Type target, const u8* data, u64 length, OUT void* out_value);
NYA_API NYA_EXTERN b8 nya_type_parse_guess(const u8* data, u64 length, OUT NYA_Type* out_type, OUT void* out_value);
NYA_API NYA_EXTERN b8 nya_type_name_parse(const u8* data, u64 length, OUT NYA_Type* out_type, OUT NYA_ConstCString* out_type_name);
