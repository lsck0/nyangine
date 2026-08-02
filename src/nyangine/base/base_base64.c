#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

NYA_INTERNAL u8 _nya_base64_char_to_value(u8 c);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_base64_encode(NYA_String* base64, const u8* data, u64 len) {
    nya_assert(base64 != nullptr);
    nya_assert(data != nullptr);

    if (len == 0) return;

    u64 encoded_len = ((len + 2) / 3) * 4;
    nya_string_reserve(base64, encoded_len + 1);

    u64 i = 0, j = 0;
    while (i + 2 < len) {
        u32 triple = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8) | (u32)data[i + 2];

        base64->items[j++] = BASE64_CHARS[(triple >> 18) & 0x3F];
        base64->items[j++] = BASE64_CHARS[(triple >> 12) & 0x3F];
        base64->items[j++] = BASE64_CHARS[(triple >> 6) & 0x3F];
        base64->items[j++] = BASE64_CHARS[triple & 0x3F];

        i += 3;
    }

    // remaining bytes
    if (i < len) {
        u32 triple = (u32)data[i] << 16;
        if (i + 1 < len) { triple |= (u32)data[i + 1] << 8; }

        base64->items[j++] = BASE64_CHARS[(triple >> 18) & 0x3F];
        base64->items[j++] = BASE64_CHARS[(triple >> 12) & 0x3F];
        base64->items[j++] = (i + 1 < len) ? BASE64_CHARS[(triple >> 6) & 0x3F] : '=';
        base64->items[j++] = '=';
    }

    base64->items[j] = '\0';
    base64->length   = j;
}

void nya_base64_decode(NYA_String* base64, const u8* encoded, u64 len) {
    nya_assert(base64 != nullptr);
    nya_assert(encoded != nullptr);

    if (len == 0) return;

    // skip padding
    while (len > 0 && encoded[len - 1] == '=') len--;

    u64 decoded_len = (len * 3) / 4;
    nya_string_reserve(base64, decoded_len + 1);

    u64 i = 0, j = 0;
    while (i + 3 < len) {
        u32 triple = (_nya_base64_char_to_value(encoded[i]) << 18) | (_nya_base64_char_to_value(encoded[i + 1]) << 12) |
                     (_nya_base64_char_to_value(encoded[i + 2]) << 6) | _nya_base64_char_to_value(encoded[i + 3]);

        base64->items[j++] = (u8)((triple >> 16) & 0xFF);
        base64->items[j++] = (u8)((triple >> 8) & 0xFF);
        base64->items[j++] = (u8)(triple & 0xFF);

        i += 4;
    }

    // remaining chars
    if (i < len) {
        u32 triple = _nya_base64_char_to_value(encoded[i]) << 18;
        if (i + 1 < len) {
            triple             |= _nya_base64_char_to_value(encoded[i + 1]) << 12;
            base64->items[j++]  = (u8)((triple >> 16) & 0xFF);

            if (i + 2 < len) {
                triple             |= _nya_base64_char_to_value(encoded[i + 2]) << 6;
                base64->items[j++]  = (u8)((triple >> 8) & 0xFF);
            }
        }
    }

    base64->items[j] = '\0';
    base64->length   = j;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL u8 _nya_base64_char_to_value(u8 c) {
    if (c >= 'A' && c <= 'Z') return (u8)(c - 'A');
    if (c >= 'a' && c <= 'z') return (u8)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (u8)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;

    return 0; // '=' padding or invalid
}
