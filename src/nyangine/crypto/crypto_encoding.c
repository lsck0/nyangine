#include "nyangine/base/base_assert.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_secret.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_CRYPTO_BASE32_BITS    5
#define _NYA_CRYPTO_BASE32_PADDING '='

/** RFC 4648 section 6: `A` to `Z` are 0 to 25, `2` to `7` are 26 to 31. */
#define _NYA_CRYPTO_BASE32_LETTERS 26
#define _NYA_CRYPTO_BASE32_DIGITS  6

/** The character for a five bit value, without a branch or a table indexed by it. */
NYA_INTERNAL char _nya_crypto_base32_character(u32 value) __attr_no_discard;

/** The value of one character, and whether it is in the alphabet at all, without a branch on which. */
NYA_INTERNAL u32 _nya_crypto_base32_value(char character, OUT u32* out_valid) __attr_no_discard;

/**
 * Bytes a final group of `characters` data characters holds, or zero for a count RFC 4648 never produces:
 * one, three and six characters would each end part way through a byte.
 * */
NYA_INTERNAL u64 _nya_crypto_base32_tail_bytes(u64 characters) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_crypto_base32_encode(const u8* data, u64 size, OUT char* out_text, u64 capacity, OUT u64* out_length) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_text != nullptr);
    nya_assert(capacity > 0);
    nya_assert(out_length != nullptr);

    out_text[0] = '\0';
    *out_length = 0;

    nya_assert(size <= (U64_MAX / NYA_CRYPTO_BASE32_GROUP_CHARACTERS) - NYA_CRYPTO_BASE32_GROUP_BYTES, "no buffer holds that much base32");

    u64 length = NYA_CRYPTO_BASE32_LENGTH(size);
    if (length + 1 > capacity)
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, FMTu64 " bytes are " FMTu64 " base32 characters and a terminator", size, length);

    u64 written = 0;

    for (u64 offset = 0; offset < size; offset += NYA_CRYPTO_BASE32_GROUP_BYTES) {
        u64 take = size - offset < NYA_CRYPTO_BASE32_GROUP_BYTES ? size - offset : NYA_CRYPTO_BASE32_GROUP_BYTES;

        // the group as one 40 bit number, short groups zero filled on the right as the RFC says.
        u64 group = 0;
        for (u64 i = 0; i < NYA_CRYPTO_BASE32_GROUP_BYTES; i++) group = (group << 8) | (i < take ? data[offset + i] : 0U);

        u64 characters = ((take * 8) + _NYA_CRYPTO_BASE32_BITS - 1) / _NYA_CRYPTO_BASE32_BITS;

        for (u64 i = 0; i < NYA_CRYPTO_BASE32_GROUP_CHARACTERS; i++) {
            u64 shift = (NYA_CRYPTO_BASE32_GROUP_CHARACTERS - 1 - i) * _NYA_CRYPTO_BASE32_BITS;

            out_text[written++] = i < characters ? _nya_crypto_base32_character((u32)((group >> shift) & 0x1FU)) : _NYA_CRYPTO_BASE32_PADDING;
        }

        nya_crypto_wipe(&group, sizeof(group));
    }

    nya_assert(written == length);

    out_text[written] = '\0';
    *out_length       = written;

    return NYA_OK;
}

NYA_Error nya_crypto_base32_decode(const char* text, u64 length, OUT u8* out_data, u64 capacity, OUT u64* out_size) {
    nya_assert(text != nullptr || length == 0);
    nya_assert(out_data != nullptr || capacity == 0);
    nya_assert(out_size != nullptr);

    *out_size = 0;

    if (length % NYA_CRYPTO_BASE32_GROUP_CHARACTERS != 0) return nya_error(NYA_ERROR_PARSE, "base32 is whole groups of eight characters");

    // everything is checked before a byte is written, so a refused text leaves the caller's buffer alone.
    u64 data_length = 0;
    while (data_length < length && text[data_length] != _NYA_CRYPTO_BASE32_PADDING) data_length++;

    for (u64 i = data_length; i < length; i++) {
        if (text[i] != _NYA_CRYPTO_BASE32_PADDING) return nya_error(NYA_ERROR_PARSE, "base32 character " FMTu64 " follows padding", i);
    }

    u64 tail_characters = data_length % NYA_CRYPTO_BASE32_GROUP_CHARACTERS;
    u64 tail_bytes      = _nya_crypto_base32_tail_bytes(tail_characters);

    if (length - data_length >= NYA_CRYPTO_BASE32_GROUP_CHARACTERS || (tail_characters != 0 && tail_bytes == 0)) {
        return nya_error(NYA_ERROR_PARSE, "base32 padding of " FMTu64 " characters ends no whole byte", length - data_length);
    }

    for (u64 i = 0; i < data_length; i++) {
        u32 valid = 0;
        (void)_nya_crypto_base32_value(text[i], &valid);

        // a branch, but only on a refusal, whose position the error names anyway.
        if (valid == 0) return nya_error(NYA_ERROR_PARSE, "base32 character " FMTu64 " is not in the alphabet", i);
    }

    u64 size = ((data_length / NYA_CRYPTO_BASE32_GROUP_CHARACTERS) * NYA_CRYPTO_BASE32_GROUP_BYTES) + tail_bytes;
    if (size > capacity) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "base32 of " FMTu64 " bytes does not fit " FMTu64, size, capacity);

    // the bits the last character carries past the last byte have to be zero, or one secret would have
    // several spellings and a stored one could be edited into another that still decodes the same.
    if (tail_characters != 0) {
        u32 valid      = 0;
        u32 last       = _nya_crypto_base32_value(text[data_length - 1], &valid);
        u64 spare_bits = (tail_characters * _NYA_CRYPTO_BASE32_BITS) - (tail_bytes * 8);

        nya_assert(valid == 1 && spare_bits > 0 && spare_bits < _NYA_CRYPTO_BASE32_BITS);
        if ((last & ((1U << spare_bits) - 1U)) != 0) return nya_error(NYA_ERROR_PARSE, "the last base32 character sets bits that encode nothing");
    }

    u64 written = 0;

    for (u64 offset = 0; offset < data_length; offset += NYA_CRYPTO_BASE32_GROUP_CHARACTERS) {
        u64 characters = data_length - offset < NYA_CRYPTO_BASE32_GROUP_CHARACTERS ? data_length - offset : NYA_CRYPTO_BASE32_GROUP_CHARACTERS;
        u64 bytes      = characters == NYA_CRYPTO_BASE32_GROUP_CHARACTERS ? NYA_CRYPTO_BASE32_GROUP_BYTES : _nya_crypto_base32_tail_bytes(characters);

        u64 group = 0;
        for (u64 i = 0; i < NYA_CRYPTO_BASE32_GROUP_CHARACTERS; i++) {
            u32 valid = 0;
            u32 value = i < characters ? _nya_crypto_base32_value(text[offset + i], &valid) : 0U;

            group = (group << _NYA_CRYPTO_BASE32_BITS) | value;
        }

        for (u64 i = 0; i < bytes; i++) {
            u64 shift = (NYA_CRYPTO_BASE32_GROUP_BYTES - 1 - i) * 8;

            out_data[written++] = (u8)((group >> shift) & 0xFFU);
        }

        nya_crypto_wipe(&group, sizeof(group));
    }

    nya_assert(written == size);
    *out_size = written;

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

char _nya_crypto_base32_character(u32 value) {
    nya_assert(value < _NYA_CRYPTO_BASE32_LETTERS + _NYA_CRYPTO_BASE32_DIGITS);

    // a comparison becomes a flag, not a jump, so letters and digits cost the same.
    s32 is_digit = (s32)(value >= _NYA_CRYPTO_BASE32_LETTERS);

    return (char)('A' + (s32)value - (is_digit * ('A' + _NYA_CRYPTO_BASE32_LETTERS - '2')));
}

u32 _nya_crypto_base32_value(char character, OUT u32* out_valid) {
    s32 code   = (u8)character;
    s32 letter = code - 'A';
    s32 digit  = code - '2';

    u32 is_letter = (u32)(letter >= 0) & (u32)(letter < _NYA_CRYPTO_BASE32_LETTERS);
    u32 is_digit  = (u32)(digit >= 0) & (u32)(digit < _NYA_CRYPTO_BASE32_DIGITS);

    *out_valid = is_letter | is_digit;

    // each flag widened to a mask selects its candidate; outside the alphabet both are zero.
    u32 letter_mask = (u32)(-(s32)is_letter);
    u32 digit_mask  = (u32)(-(s32)is_digit);

    return (letter_mask & (u32)letter) | (digit_mask & (u32)(digit + _NYA_CRYPTO_BASE32_LETTERS));
}

u64 _nya_crypto_base32_tail_bytes(u64 characters) {
    switch (characters) {
        case 0:  return 0;
        case 2:  return 1;
        case 4:  return 2;
        case 5:  return 3;
        case 7:  return 4;
        default: return 0;
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * BASE64URL
 * ─────────────────────────────────────────────────────────
 */

/** The value of one base64url character, or 64 for anything else. */
NYA_INTERNAL u8 _nya_crypto_base64url_value(char character) __attr_no_discard;

b8 nya_crypto_base64url_encode(const u8* data, u64 size, char* out_text, u64 capacity, u64* out_size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    nya_assert(capacity > 0);

    *out_size   = 0;
    out_text[0] = '\0';

    // three bytes become four characters, and a trailing one or two become two or three.
    u64 encoded = (size / 3) * 4 + (size % 3 == 0 ? 0 : size % 3 + 1);

    if (encoded + 1 > capacity) return false;

    u64 written = 0;

    for (u64 index = 0; index < size; index += 3) {
        u64 remaining = size - index;

        u32 chunk = (u32)data[index] << 16;
        if (remaining > 1) chunk |= (u32)data[index + 1] << 8;
        if (remaining > 2) chunk |= (u32)data[index + 2];

        out_text[written++] = alphabet[(chunk >> 18) & 0x3FU];
        out_text[written++] = alphabet[(chunk >> 12) & 0x3FU];

        if (remaining > 1) out_text[written++] = alphabet[(chunk >> 6) & 0x3FU];
        if (remaining > 2) out_text[written++] = alphabet[chunk & 0x3FU];
    }

    out_text[written] = '\0';
    *out_size         = written;

    return true;
}

b8 nya_crypto_base64url_decode(const char* text, u64 size, u8* out_data, u64 capacity, u64* out_size) {
    *out_size = 0;

    // a base64 group is two, three or four characters; one leftover character encodes nothing and is
    // the shape a truncated token has.
    if (size == 0 || size % 4 == 1) return false;

    u64 decoded = (size / 4) * 3 + (size % 4 == 0 ? 0 : size % 4 - 1);

    if (decoded > capacity) return false;

    u64 written = 0;

    for (u64 index = 0; index < size; index += 4) {
        u64 remaining = size - index;

        u8  values[4] = { 0, 0, 0, 0 };
        u64 group     = remaining < 4 ? remaining : 4;

        for (u64 offset = 0; offset < group; offset++) {
            values[offset] = _nya_crypto_base64url_value(text[index + offset]);
            if (values[offset] == 64) return false;
        }

        /*
         * A short final group carries bits that encode nothing: two characters hold one byte and four
         * spare bits, three hold two bytes and two spare. Those have to be zero, or one signature has
         * several spellings and a token can be edited into a different string that still verifies.
         */
        if (group == 2 && (values[1] & 0x0FU) != 0) return false;
        if (group == 3 && (values[2] & 0x03U) != 0) return false;

        u32 chunk = ((u32)values[0] << 18) | ((u32)values[1] << 12) | ((u32)values[2] << 6) | (u32)values[3];

        out_data[written++] = (u8)((chunk >> 16) & 0xFFU);
        if (group > 2) out_data[written++] = (u8)((chunk >> 8) & 0xFFU);
        if (group > 3) out_data[written++] = (u8)(chunk & 0xFFU);
    }

    *out_size = written;

    return true;
}

u8 _nya_crypto_base64url_value(char character) {
    if (character >= 'A' && character <= 'Z') return (u8)(character - 'A');
    if (character >= 'a' && character <= 'z') return (u8)(character - 'a' + 26);
    if (character >= '0' && character <= '9') return (u8)(character - '0' + 52);
    if (character == '-') return 62;
    if (character == '_') return 63;

    return 64;
}
