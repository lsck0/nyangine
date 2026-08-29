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

        default: nya_log_panic("Parsing not implemented for type %s.", NYA_TYPE_NAME_MAP[target]); nya_unreachable();
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

/**
 * `*accumulator = *accumulator * base + digit`, or false if that would not fit in a u128.
 *
 * The three loops below used to accumulate unchecked, which is wrong twice over. Under
 * FLAGS_SANITIZE the build names unsigned-integer-overflow together with -fno-sanitize-recover=all,
 * so an over long literal aborted the process; without sanitizers it wrapped, and the range check
 * the integer cases in nya_type_parse apply afterwards then ran on the wrapped value. A literal that
 * wrapped back into the target's range was therefore *accepted* as a completely different number —
 * `nya_type_parse(NYA_TYPE_S64, "340282366920938463463374607431768211499")` returned true with 43.
 *
 * That matters because this is the parse primitive behind command line arguments, JSON numbers and
 * the .nya save format, so the input is user supplied on every path into it.
 *
 * The predicate is written as a division rather than as a multiply that is checked afterwards,
 * because the multiply is the thing that must not happen.
 * */
NYA_INTERNAL b8 _nya_type_accumulate_digit(u128* accumulator, u128 base, u8 digit) {
    if (*accumulator > (U128_MAX - digit) / base) return false;

    *accumulator = (*accumulator * base) + digit;
    return true;
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
            if (!_nya_type_accumulate_digit(out_value, 2, (u8)(c - '0'))) return false;
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
            if (!_nya_type_accumulate_digit(out_value, 16, digit)) return false;
        }
        return true;
    }

    // decimal
    for (u64 i = 0; i < length; i++) {
        u8 c = data[i];
        if (!('0' <= c && c <= '9')) return false;

        if (!_nya_type_accumulate_digit(out_value, 10, (u8)(c - '0'))) return false;
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

    /*
     * Bounded before the conversion, not after.
     *
     * A magnitude above S128_MAX has no value in s128 to be range checked, and negating S128_MIN
     * afterwards is signed overflow — which FLAGS_SANITIZE names, so it aborts rather than wrapping.
     * The negative side is allowed one more than the positive one, which is what lets S128_MIN
     * itself parse.
     */
    u128 limit = is_negative ? (u128)S128_MAX + 1 : (u128)S128_MAX;
    if (magnitude > limit) return false;

    // Negated as unsigned and converted afterwards, so S128_MIN never exists as a positive s128 on
    // the way to itself.
    *out_value = is_negative ? (s128)(~magnitude + 1) : (s128)magnitude;

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

    /*
     * A hexadecimal float, which is what the nya format writes: 0x1.91eb86p+1.
     *
     * Exact in both directions, and that is the whole point. Every part of it is a power of two — a
     * hex digit is four bits and the exponent scales by two — so the mantissa accumulates into an
     * integer with no rounding and the scaling is a shift rather than a division. The decimal path
     * below cannot promise that: it accumulates digits and then divides, and the error from the
     * division lands in the result. For f32 and f64 the f128 intermediate absorbs it, but for f128
     * there is no headroom and nearly a third of values came back changed — enough to fail the
     * document's own checksum on the next read.
     * */
    if (length > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) {
        u128 mantissa            = 0;
        s32  binary_exponent     = 0;
        b8   seen_dot            = false;
        b8   seen_digit          = false;
        u64  i                   = 2;

        for (; i < length; i++) {
            u8 c = data[i];

            if (c == '.') {
                if (seen_dot) return false;
                seen_dot = true;
                continue;
            }

            u32 digit;
            if ('0' <= c && c <= '9') {
                digit = (u32)(c - '0');
            } else if ('a' <= c && c <= 'f') {
                digit = (u32)(c - 'a') + 10;
            } else if ('A' <= c && c <= 'F') {
                digit = (u32)(c - 'A') + 10;
            } else {
                break;
            }

            seen_digit = true;
            mantissa   = (mantissa << 4) | digit;

            // Each digit after the point is four bits further down.
            if (seen_dot) binary_exponent -= 4;
        }

        if (!seen_digit) return false;

        // The p exponent. Required by C for a hexadecimal float, and required here too rather than
        // guessed at, so a plain 0x1F stays the integer it looks like.
        if (i >= length || (data[i] != 'p' && data[i] != 'P')) return false;
        i++;

        b8 exponent_negative = false;
        if (i < length && (data[i] == '+' || data[i] == '-')) {
            exponent_negative = data[i] == '-';
            i++;
        }

        if (i >= length) return false;

        s32 exponent = 0;
        for (; i < length; i++) {
            if (!('0' <= data[i] && data[i] <= '9')) return false;
            if (exponent < 100000) exponent = (exponent * 10) + (data[i] - '0');
        }

        binary_exponent += exponent_negative ? -exponent : exponent;

        *out_value = ldexpl((f128)mantissa, binary_exponent);
        if (is_negative) *out_value = -*out_value;

        return true;
    }

    /*
     * One mantissa and a decimal exponent, rather than an integer part over a fractional divisor.
     *
     * The pair form accumulated into u128 unchecked — the mistake _nya_type_accumulate_digit above
     * documents and fixes for the integer paths, and that the real path was simply missed by.
     *
     * Digits past what the accumulator holds are counted rather than accumulated, which is what a
     * strtod does and costs nothing: an f128 carries 113 bits of significand either way.
     * */
    u128 mantissa            = 0;
    s64  decimal_exponent    = 0;
    b8   mantissa_saturated  = false;
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

        // _nya_type_accumulate_digit leaves the accumulator untouched when the multiply would
        // overflow, which is exactly the saturation wanted here, so the guard is not spelled out a
        // fourth time. The flag is sticky: once a digit has been dropped a later, smaller one must
        // not sneak back in under the limit and land in the wrong place value.
        if (mantissa_saturated || !_nya_type_accumulate_digit(&mantissa, 10, digit)) {
            // Past what the accumulator holds. The digit's *place* still counts; its value does not.
            mantissa_saturated = true;
            if (!has_fractional_part) decimal_exponent++;
        } else if (has_fractional_part) {
            decimal_exponent--;
        }
    }

    if (!has_mantissa_digits) return false;
    if (has_exponent && !has_exponent_digits) return false;

    *out_value = (f128)mantissa;

    if (has_exponent) decimal_exponent += exponent_negative ? -exponent : exponent;

    /*
     * Zero is left alone, because scaling it would not leave it zero.
     *
     * `exponent` is clamped at six digits, so powl saturates to infinity long before the end of that
     * range, and 0.0 * infinity is NaN. NaN then passes the range checks in nya_type_parse, which
     * compare with < and >, so "0e999999" came back as a successfully parsed NaN.
     *
     * Scaling by a positive power and dividing, rather than by a negative one and multiplying, so a
     * fraction is divided by the exact 10^k it was accumulated over — which is what the fractional
     * divisor did before and what keeps the round trip through the nya format exact.
     * */
    if (mantissa != 0 && decimal_exponent != 0) {
        // Bounded before negating, so the magnitude below cannot overflow and powl is asked for
        // something it can answer with an infinity rather than with undefined behaviour.
        if (decimal_exponent > 1000000) decimal_exponent = 1000000;
        if (decimal_exponent < -1000000) decimal_exponent = -1000000;

        f128 scale = powl(10.0L, (f128)(decimal_exponent < 0 ? -decimal_exponent : decimal_exponent));

        if (decimal_exponent < 0)
            *out_value /= scale;
        else
            *out_value *= scale;
    }

    if (is_negative) *out_value = -*out_value;

    return true;
}
