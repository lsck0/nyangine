#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_UUID_NIBBLE_TO_HEX(n) ((n) < 10 ? ('0' + (n)) : ('a' + ((n) - 10)))

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Uuid nya_uuid_v4_create(NYA_RNG* rng) {
    nya_assert(rng != nullptr);

    u128 raw = 0;
    nya_rng_gen_bytes(rng, (u8*)&raw, sizeof(raw));

    // version 4 in byte 6 high nibble (lo bits 52-55)
    raw = (raw & ~(u128)0xF0000000000000ULL) | (u128)0x40000000000000ULL;
    // variant RFC 4122 (10xx) in byte 8 high 2 bits (hi bits 6-7)
    raw = (raw & ~((u128)0xC0 << 64)) | ((u128)0x80 << 64);

    return raw;
}

NYA_Uuid nya_uuid_v7_create(NYA_RNG* rng) {
    nya_assert(rng != nullptr);

    u64 timestamp = nya_clock_get_timestamp_ms();
    u8  rand_bytes[10];
    nya_rng_gen_bytes(rng, rand_bytes, sizeof(rand_bytes));

    u8 bytes[16];
    // bytes 0-5: 48-bit timestamp (big-endian)
    bytes[0] = (timestamp >> 40) & 0xFF;
    bytes[1] = (timestamp >> 32) & 0xFF;
    bytes[2] = (timestamp >> 24) & 0xFF;
    bytes[3] = (timestamp >> 16) & 0xFF;
    bytes[4] = (timestamp >> 8) & 0xFF;
    bytes[5] = timestamp & 0xFF;
    // byte 6: version 7 + rand_a high nibble
    bytes[6] = 0x70 | (rand_bytes[0] & 0x0F);
    bytes[7] = rand_bytes[1];
    // byte 8: variant (10xx) + rand_b
    bytes[8] = 0x80 | (rand_bytes[2] & 0x3F);
    for (u64 i = 0; i < 7; i++) bytes[9 + i] = rand_bytes[3 + i];

    u128 lo = 0, hi = 0;
    for (u64 i = 0; i < 8; i++) lo |= ((u128)bytes[i]) << (i * 8);
    for (u64 i = 0; i < 8; i++) hi |= ((u128)bytes[i + 8]) << (i * 8);
    return lo | (hi << 64);
}

b8 nya_uuid_parse(NYA_ConstCString str, NYA_Uuid* out) {
    nya_assert(str != nullptr);
    nya_assert(out != nullptr);

    u8  buffer[16] = { 0 };
    u64 len        = 0;
    while (str[len] != '\0') len++;

    // Validate hyphen positions if present (standard format: 8-4-4-4-12)
    if (len == 36) {
        if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') return false;
    } else if (len != 32) {
        return false;
    }

    char normalized[32] = { 0 };
    u64  j              = 0;
    for (u64 i = 0; i < len; i++) {
        if (str[i] == '-') continue;
        if (j >= 32) return false;
        normalized[j++] = str[i];
    }

    if (j != 32) return false;

    for (u64 i = 0; i < 32; i++) {
        char c = normalized[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }

    for (u64 i = 0; i < 16; i++) {
        char high = normalized[i * 2];
        char low  = normalized[i * 2 + 1];

        u8 h      = (high >= 'a') ? (high - 'a' + 10) : (high >= 'A') ? (high - 'A' + 10) : (high - '0');
        u8 l      = (low >= 'a') ? (low - 'a' + 10) : (low >= 'A') ? (low - 'A' + 10) : (low - '0');
        buffer[i] = (h << 4) | l;
    }

    u128 lo = 0;
    u128 hi = 0;
    for (u64 i = 0; i < 8; i++) lo |= ((u128)buffer[i]) << (i * 8);
    for (u64 i = 0; i < 8; i++) hi |= ((u128)buffer[i + 8]) << (i * 8);

    *out = lo | (hi << 64);
    return true;
}

void nya_uuid_format(NYA_Uuid uuid, char buffer[NYA_UUID_STRING_BUFFER_SIZE]) {
    nya_assert(buffer != nullptr);

    u8   bytes[16];
    u128 lo = uuid & 0xFFFFFFFFFFFFFFFFULL;
    u128 hi = uuid >> 64;
    for (u64 i = 0; i < 8; i++) bytes[i] = (lo >> (i * 8)) & 0xFF;
    for (u64 i = 0; i < 8; i++) bytes[i + 8] = (hi >> (i * 8)) & 0xFF;

    u64 idx = 0;

    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[0] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[0] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[1] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[1] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[2] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[2] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[3] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[3] & 0xF);
    buffer[idx++] = '-';
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[4] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[4] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[5] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[5] & 0xF);
    buffer[idx++] = '-';
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[6] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[6] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[7] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[7] & 0xF);
    buffer[idx++] = '-';
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[8] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[8] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[9] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[9] & 0xF);
    buffer[idx++] = '-';
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[10] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[10] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[11] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[11] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[12] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[12] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[13] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[13] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[14] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[14] & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX((bytes[15] >> 4) & 0xF);
    buffer[idx++] = _NYA_UUID_NIBBLE_TO_HEX(bytes[15] & 0xF);

    buffer[36] = '\0';
}
