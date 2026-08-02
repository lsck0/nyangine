#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_type_try_parse_bool(const u8* data, u64 length, OUT u128* out_value);
NYA_INTERNAL b8 _nya_type_try_parse_u128(const u8* data, u64 length, OUT u128* out_value);
NYA_INTERNAL b8 _nya_type_try_parse_s128(const u8* data, u64 length, OUT s128* out_value);
NYA_INTERNAL b8 _nya_type_try_parse_f128(const u8* data, u64 length, OUT f128* out_value);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_type_parse(NYA_Type target, const u8* data, u64 length, OUT void* out_value) {
    nya_assert(data != nullptr);
    nya_assert(out_value != nullptr);

    switch (target) {
        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128: return _nya_type_try_parse_bool(data, length, out_value);

        case NYA_TYPE_U8:   {
            u128 value;
            if (!_nya_type_try_parse_u128(data, length, &value)) return false;

            if (value > U8_MAX) return false;
            *(u8*)out_value = value;

            return true;
        } break;

        case NYA_TYPE_U16: {
            u128 value;
            if (!_nya_type_try_parse_u128(data, length, &value)) return false;

            if (value > U16_MAX) return false;
            *(u16*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_U32: {
            u128 value;
            if (!_nya_type_try_parse_u128(data, length, &value)) return false;

            if (value > U32_MAX) return false;
            *(u32*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_U64: {
            u128 value;
            if (!_nya_type_try_parse_u128(data, length, &value)) return false;

            if (value > U64_MAX) return false;
            *(u64*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_U128: return _nya_type_try_parse_u128(data, length, (u128*)out_value);

        case NYA_TYPE_S8:   {
            s128 value;
            if (!_nya_type_try_parse_s128(data, length, &value)) return false;

            if (value < S8_MIN || value > S8_MAX) return false;
            *(s8*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_S16: {
            s128 value;
            if (!_nya_type_try_parse_s128(data, length, &value)) return false;

            if (value < S16_MIN || value > S16_MAX) return false;
            *(s16*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_S32: {
            s128 value;
            if (!_nya_type_try_parse_s128(data, length, &value)) return false;

            if (value < S32_MIN || value > S32_MAX) return false;
            *(s32*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_S64: {
            s128 value;
            if (!_nya_type_try_parse_s128(data, length, &value)) return false;

            if (value < S64_MIN || value > S64_MAX) return false;
            *(s64*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_S128: return _nya_type_try_parse_s128(data, length, (s128*)out_value);

        case NYA_TYPE_F16:  {
            f128 value;
            if (!_nya_type_try_parse_f128(data, length, &value)) return false;

            if (value < F16_MIN || value > F16_MAX) return false;
            *(f16*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_F32: {
            f128 value;
            if (!_nya_type_try_parse_f128(data, length, &value)) return false;

            if (value < F32_MIN || value > F32_MAX) return false;
            *(f32*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_F64: {
            f128 value;
            if (!_nya_type_try_parse_f128(data, length, &value)) return false;

            if (value < F64_MIN || value > F64_MAX) return false;
            *(f64*)out_value = value;
            return true;
        } break;

        case NYA_TYPE_F128: return _nya_type_try_parse_f128(data, length, (f128*)out_value);

        case NYA_TYPE_CHAR: {
            if (length == 1) {
                *(char*)out_value = (char)data[0];
                return true;
            }
            return false;
        } break;

        default: nya_panic("Parsing not implemented for type %s.", NYA_TYPE_NAME_MAP[target]); nya_unreachable();
    }
}

b8 nya_type_name_parse(const u8* data, u64 length, OUT NYA_Type* out_type, OUT NYA_ConstCString* out_type_name) {
    nya_assert(data != nullptr);
    nya_assert(out_type != nullptr);
    nya_assert(out_type_name != nullptr);

    for (u32 i = 0; i < NYA_TYPE_COUNT; i++) {
        const char* type_name = NYA_TYPE_NAME_MAP[i];
        if (strncmp((const char*)data, type_name, length) == 0 && strlen(type_name) == length) {
            *out_type      = (NYA_Type)i;
            *out_type_name = type_name;
            return true;
        }
    }

    return false;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_type_try_parse_bool(const u8* data, u64 length, OUT u128* out_value) {
    nya_assert(data != nullptr);
    nya_assert(out_value != nullptr);

    *out_value = 0;

    if (length == 0) return false;

    if (length == 1 && strncmp((const char*)data, "1", 1) == 0) {
        *out_value = true;
        return true;
    }
    if (length == 4 && strncmp((const char*)data, "true", 4) == 0) {
        *out_value = true;
        return true;
    }
    if (length == 4 && strncmp((const char*)data, "True", 4) == 0) {
        *out_value = true;
        return true;
    }

    if (length == 1 && strncmp((const char*)data, "0", 1) == 0) {
        *out_value = false;
        return true;
    }
    if (length == 5 && strncmp((const char*)data, "false", 5) == 0) {
        *out_value = false;
        return true;
    }
    if (length == 5 && strncmp((const char*)data, "False", 5) == 0) {
        *out_value = false;
        return true;
    }

    return false;
}

NYA_INTERNAL b8 _nya_type_try_parse_u128(const u8* data, u64 length, OUT u128* out_value) {
    nya_assert(data != nullptr);
    nya_assert(out_value != nullptr);

    *out_value = 0;

    if (length == 0) return false;

    // binary
    if (length > 2 && data[0] == '0' && (data[1] == 'b' || data[1] == 'B')) {
        for (u64 i = 2; i < length; i++) {
            u8 c = data[i];
            if (c != '0' && c != '1') return false;
            *out_value = (*out_value * 2) + (c - '0');
        }
        return true;
    }

    // hex
    if (length > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) {
        for (u64 i = 2; i < length; i++) {
            u8 c = data[i];
            u8 digit;
            if ('0' <= c && c <= '9') {
                digit = c - '0';
            } else if ('a' <= c && c <= 'f') {
                digit = 10 + (c - 'a');
            } else if ('A' <= c && c <= 'F') {
                digit = 10 + (c - 'A');
            } else {
                return false;
            }
            *out_value = (*out_value * 16) + digit;
        }
        return true;
    }

    // decimal
    for (u64 i = 0; i < length; i++) {
        u8 c = data[i];
        if (!('0' <= c && c <= '9')) return false;

        u8 digit   = c - '0';
        *out_value = (*out_value * 10) + digit;
    }

    return true;
}

NYA_INTERNAL b8 _nya_type_try_parse_s128(const u8* data, u64 length, OUT s128* out_value) {
    nya_assert(data != nullptr);
    nya_assert(out_value != nullptr);

    *out_value = 0;

    if (length == 0) return false;

    b8 is_negative = false;
    if (data[0] == '-') {
        is_negative = true;

        data++;
        length--;

        if (length == 0) return false;
    }

    u128 magnitude = 0;
    if (!_nya_type_try_parse_u128(data, length, &magnitude)) return false;
    *out_value = (s128)magnitude;

    if (is_negative) *out_value = -*out_value;

    return true;
}

NYA_INTERNAL b8 _nya_type_try_parse_f128(const u8* data, u64 length, OUT f128* out_value) {
    nya_assert(data != nullptr);
    nya_assert(out_value != nullptr);

    *out_value = 0.0;

    if (length == 0) return false;

    b8 is_negative = false;
    if (data[0] == '-') {
        is_negative = true;

        data++;
        length--;

        if (length == 0) return false;
    }

    u128 integer_part        = 0;
    u128 fractional_part     = 0;
    u128 fractional_divisor  = 1;
    b8   has_fractional_part = false;

    // Scientific notation. The mantissa is scanned first and the exponent applied at the end, so
    // 1.5e3 and 1500 produce the same value. Without this every JSON document containing an
    // exponent, which is how most encoders write large or small reals, would fail to parse.
    s32 exponent            = 0;
    b8  has_exponent        = false;
    b8  exponent_negative   = false;
    b8  has_exponent_digits = false;
    b8  has_mantissa_digits = false;

    for (u64 i = 0; i < length; i++) {
        u8 c = data[i];

        if (has_exponent) {
            if ((c == '+' || c == '-') && !has_exponent_digits && exponent == 0) {
                // A sign is only valid immediately after the 'e'.
                if (i == 0 || (data[i - 1] != 'e' && data[i - 1] != 'E')) return false;
                exponent_negative = c == '-';
                continue;
            }

            if (!('0' <= c && c <= '9')) return false;

            has_exponent_digits = true;

            // Clamped, because anything past this is already infinity or zero and letting it run
            // would overflow the counter itself.
            if (exponent < 100000) exponent = (exponent * 10) + (c - '0');
            continue;
        }

        if (c == '.') {
            if (has_fractional_part) return false;
            has_fractional_part = true;
            continue;
        }

        if (c == 'e' || c == 'E') {
            if (!has_mantissa_digits) return false;
            has_exponent = true;
            continue;
        }

        if (!('0' <= c && c <= '9')) return false;

        has_mantissa_digits = true;

        u8 digit = c - '0';
        if (!has_fractional_part) {
            integer_part = (integer_part * 10) + digit;
        } else {
            fractional_part     = (fractional_part * 10) + digit;
            fractional_divisor *= 10;
        }
    }

    if (!has_mantissa_digits) return false;
    if (has_exponent && !has_exponent_digits) return false;

    *out_value = (f128)integer_part + ((f128)fractional_part / (f128)fractional_divisor);

    if (has_exponent && exponent != 0) {
        f128 scale = powl(10.0L, (f128)exponent);
        if (exponent_negative)
            *out_value /= scale;
        else
            *out_value *= scale;
    }

    if (is_negative) *out_value = -*out_value;

    return true;
}
