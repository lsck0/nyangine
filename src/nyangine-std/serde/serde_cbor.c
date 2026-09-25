#include "nyangine-std/serde/serde_cbor.h"
#include "nyangine-std/base/base_assert.h"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** The five bits below the major type of an initial byte: an inline value, or how many bytes the argument takes. */
#define _NYA_CBOR_INFO_1_BYTE  24
#define _NYA_CBOR_INFO_2_BYTES 25
#define _NYA_CBOR_INFO_4_BYTES 26
#define _NYA_CBOR_INFO_8_BYTES 27

#define _NYA_CBOR_MAJOR_UNSIGNED 0
#define _NYA_CBOR_MAJOR_NEGATIVE 1
#define _NYA_CBOR_MAJOR_BYTES    2
#define _NYA_CBOR_MAJOR_TEXT     3
#define _NYA_CBOR_MAJOR_ARRAY    4
#define _NYA_CBOR_MAJOR_MAP      5
#define _NYA_CBOR_MAJOR_TAG      6
#define _NYA_CBOR_MAJOR_SIMPLE   7

/**
 * Reads the initial byte of an item: its major type, and the argument the additional-info bits carry.
 *
 * This is the one place a byte is taken off the buffer, so it is the one place the bounds check for the
 * argument's own bytes lives. An indefinite-length item (info 31) is refused here for every major type,
 * which is what keeps every reader below from having to think about one. The reader advances past the
 * initial byte and the argument bytes, and no further.
 * */
NYA_INTERNAL b8 _nya_cbor_head(NYA_CborReader* reader, OUT u8* out_major, OUT u64* out_argument) __attr_no_discard;

/** The skip, carrying the depth left so a nested structure cannot recurse past NYA_CBOR_MAX_DEPTH. */
NYA_INTERNAL b8 _nya_cbor_skip(NYA_CborReader* reader, u32 depth_left) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_CborReader nya_cbor_reader(const u8* data, u64 size) {
    return (NYA_CborReader){
        .data   = data,
        .size   = data != nullptr ? size : 0,
        .offset = 0,
    };
}

b8 nya_cbor_read_u64(NYA_CborReader* reader, u64* out_value) {
    nya_assert(reader != nullptr && out_value != nullptr);

    *out_value = 0;

    u8  major    = 0;
    u64 argument = 0;

    if (!_nya_cbor_head(reader, &major, &argument)) return false;
    if (major != _NYA_CBOR_MAJOR_UNSIGNED) return false;

    *out_value = argument;

    return true;
}

b8 nya_cbor_read_int(NYA_CborReader* reader, s64* out_value) {
    nya_assert(reader != nullptr && out_value != nullptr);

    *out_value = 0;

    u8  major    = 0;
    u64 argument = 0;

    if (!_nya_cbor_head(reader, &major, &argument)) return false;

    if (major == _NYA_CBOR_MAJOR_UNSIGNED) {
        // A positive integer past what s64 holds is refused rather than wrapped, since a COSE label reader expects the number that was written.
        if (argument > (u64)S64_MAX) return false;

        *out_value = (s64)argument;

        return true;
    }

    if (major == _NYA_CBOR_MAJOR_NEGATIVE) {
        // A negative CBOR integer encodes -1 - n, so its most negative value is S64_MIN; anything past that will not fit and is refused.
        if (argument > (u64)S64_MAX) return false;

        *out_value = -1 - (s64)argument;

        return true;
    }

    return false;
}

b8 nya_cbor_read_bytes(NYA_CborReader* reader, const u8** out_bytes, u64* out_size) {
    nya_assert(reader != nullptr && out_bytes != nullptr && out_size != nullptr);

    *out_bytes = nullptr;
    *out_size  = 0;

    u8  major  = 0;
    u64 length = 0;

    if (!_nya_cbor_head(reader, &major, &length)) return false;
    if (major != _NYA_CBOR_MAJOR_BYTES) return false;

    // The length is checked against what is left before the pointer is handed back, so an over-claiming string is a refused read, not an out-of-bounds one.
    if (length > reader->size - reader->offset) return false;

    *out_bytes = reader->data + reader->offset;
    *out_size  = length;

    reader->offset += length;

    return true;
}

b8 nya_cbor_read_text(NYA_CborReader* reader, const u8** out_text, u64* out_size) {
    nya_assert(reader != nullptr && out_text != nullptr && out_size != nullptr);

    *out_text = nullptr;
    *out_size = 0;

    u8  major  = 0;
    u64 length = 0;

    if (!_nya_cbor_head(reader, &major, &length)) return false;
    if (major != _NYA_CBOR_MAJOR_TEXT) return false;

    if (length > reader->size - reader->offset) return false;

    *out_text = reader->data + reader->offset;
    *out_size = length;

    reader->offset += length;

    return true;
}

b8 nya_cbor_read_map(NYA_CborReader* reader, u64* out_count) {
    nya_assert(reader != nullptr && out_count != nullptr);

    *out_count = 0;

    u8  major = 0;
    u64 count = 0;

    if (!_nya_cbor_head(reader, &major, &count)) return false;
    if (major != _NYA_CBOR_MAJOR_MAP) return false;

    *out_count = count;

    return true;
}

b8 nya_cbor_read_array(NYA_CborReader* reader, u64* out_count) {
    nya_assert(reader != nullptr && out_count != nullptr);

    *out_count = 0;

    u8  major = 0;
    u64 count = 0;

    if (!_nya_cbor_head(reader, &major, &count)) return false;
    if (major != _NYA_CBOR_MAJOR_ARRAY) return false;

    *out_count = count;

    return true;
}

b8 nya_cbor_skip(NYA_CborReader* reader) {
    nya_assert(reader != nullptr);

    return _nya_cbor_skip(reader, NYA_CBOR_MAX_DEPTH);
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

b8 _nya_cbor_head(NYA_CborReader* reader, u8* out_major, u64* out_argument) {
    *out_major    = 0;
    *out_argument = 0;

    if (reader->offset >= reader->size) return false;

    u8 initial = reader->data[reader->offset];

    reader->offset += 1;

    u8 major = (u8)(initial >> 5);
    u8 info  = (u8)(initial & 0x1F);

    *out_major = major;

    // An inline value: the argument is the additional-info bits themselves, 0 through 23.
    if (info < _NYA_CBOR_INFO_1_BYTE) {
        *out_argument = info;
        return true;
    }

    // The number of argument bytes the additional info names; reserved encodings and the indefinite-length marker are refused here, so nothing below handles a break code.
    u32 width = 0;

    switch (info) {
        case _NYA_CBOR_INFO_1_BYTE: width = 1; break;
        case _NYA_CBOR_INFO_2_BYTES: width = 2; break;
        case _NYA_CBOR_INFO_4_BYTES: width = 4; break;
        case _NYA_CBOR_INFO_8_BYTES: width = 8; break;
        default: return false;
    }

    // The argument's own bytes have to be there, checked before any is read.
    if ((u64)width > reader->size - reader->offset) return false;

    u64 argument = 0;

    for (u32 index = 0; index < width; index++) {
        argument = (argument << 8) | (u64)reader->data[reader->offset + index];
    }

    reader->offset += width;

    *out_argument = argument;

    return true;
}

b8 _nya_cbor_skip(NYA_CborReader* reader, u32 depth_left) {
    if (depth_left == 0) return false;

    u8  major    = 0;
    u64 argument = 0;

    if (!_nya_cbor_head(reader, &major, &argument)) return false;

    switch (major) {
        case _NYA_CBOR_MAJOR_UNSIGNED:
        case _NYA_CBOR_MAJOR_NEGATIVE:
            // The whole value was the head and its argument, already consumed.
            return true;

        case _NYA_CBOR_MAJOR_BYTES:
        case _NYA_CBOR_MAJOR_TEXT:
            // A string's argument is its length; step over exactly that many bytes, bounds checked.
            if (argument > reader->size - reader->offset) return false;
            reader->offset += argument;
            return true;

        case _NYA_CBOR_MAJOR_ARRAY:
            // The argument is the item count; skip each in turn, one level deeper.
            for (u64 index = 0; index < argument; index++) {
                if (!_nya_cbor_skip(reader, depth_left - 1)) return false;
            }
            return true;

        case _NYA_CBOR_MAJOR_MAP:
            // The argument is the pair count; a key and a value is two values to skip.
            for (u64 index = 0; index < argument; index++) {
                if (!_nya_cbor_skip(reader, depth_left - 1)) return false;
                if (!_nya_cbor_skip(reader, depth_left - 1)) return false;
            }
            return true;

        case _NYA_CBOR_MAJOR_TAG:
            // A tag wraps the one value that follows it; skip that value.
            return _nya_cbor_skip(reader, depth_left - 1);

        case _NYA_CBOR_MAJOR_SIMPLE:
            // A simple value or a float: its payload was the argument bytes the head consumed, so nothing is left; the break (info 31) was refused by the head.
            return true;

        default: return false;
    }
}
