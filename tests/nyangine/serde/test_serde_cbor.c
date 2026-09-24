/**
 * The minimal CBOR reader: it reads the shapes WebAuthn uses, and it refuses everything else without
 * ever reading past the buffer it was handed.
 *
 * The "without reading past the buffer" half is what the sanitizer build proves: the malformed and
 * truncated cases below are here as much to be run under ASan as to have their return value checked.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <string.h>

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // unsigned integers, in each width
  {
    u8 inline_seven[] = {0x07};
    NYA_CborReader reader = nya_cbor_reader(inline_seven, sizeof(inline_seven));
    u64 value = 0;
    nya_check(nya_cbor_read_u64(&reader, &value) && value == 7, "an inline unsigned reads");

    u8 one_byte[] = {0x18, 0xFF};
    reader = nya_cbor_reader(one_byte, sizeof(one_byte));
    nya_check(nya_cbor_read_u64(&reader, &value) && value == 255, "a one-byte unsigned reads");

    u8 two_byte[] = {0x19, 0x01, 0x00};
    reader = nya_cbor_reader(two_byte, sizeof(two_byte));
    nya_check(nya_cbor_read_u64(&reader, &value) && value == 256, "a two-byte unsigned reads");

    u8 four_byte[] = {0x1A, 0x00, 0x01, 0x00, 0x00};
    reader = nya_cbor_reader(four_byte, sizeof(four_byte));
    nya_check(nya_cbor_read_u64(&reader, &value) && value == 65536, "a four-byte unsigned reads");
  }

  // signed integers, including the negatives COSE labels use
  {
    u8 minus_one[] = {0x20}; // major 1, argument 0 -> -1
    NYA_CborReader reader = nya_cbor_reader(minus_one, sizeof(minus_one));
    s64 value = 0;
    nya_check(nya_cbor_read_int(&reader, &value) && value == -1, "-1 reads");

    u8 minus_eight[] = {0x27}; // major 1, argument 7 -> -8 (EdDSA)
    reader = nya_cbor_reader(minus_eight, sizeof(minus_eight));
    nya_check(nya_cbor_read_int(&reader, &value) && value == -8, "-8 reads");

    u8 positive[] = {0x0A};
    reader = nya_cbor_reader(positive, sizeof(positive));
    nya_check(nya_cbor_read_int(&reader, &value) && value == 10, "a positive integer reads as signed too");

    // A negative integer whose magnitude is the whole u64 range cannot fit an s64 and is refused.
    u8 too_big[] = {0x3B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    reader = nya_cbor_reader(too_big, sizeof(too_big));
    nya_check(!nya_cbor_read_int(&reader, &value), "a negative past the s64 range is refused, not wrapped");
  }

  // byte and text strings return a pointer into the buffer
  {
    u8 bytes[] = {0x43, 0xDE, 0xAD, 0xBE}; // bstr of 3
    NYA_CborReader reader = nya_cbor_reader(bytes, sizeof(bytes));
    const u8 *out = nullptr;
    u64 size = 0;
    nya_check(nya_cbor_read_bytes(&reader, &out, &size) && size == 3 && out == bytes + 1, "a byte string points into the buffer");
    nya_check(out[0] == 0xDE && out[2] == 0xBE, "with the right bytes");

    u8 text[] = {0x63, 'f', 'm', 't'}; // tstr "fmt"
    reader = nya_cbor_reader(text, sizeof(text));
    nya_check(nya_cbor_read_text(&reader, &out, &size) && size == 3 && memcmp(out, "fmt", 3) == 0, "a text string reads");
  }

  // maps and arrays hand back their counts
  {
    u8 map[] = {0xA2, 0x01, 0x02, 0x03, 0x04}; // {1:2, 3:4}
    NYA_CborReader reader = nya_cbor_reader(map, sizeof(map));
    u64 count = 0;
    nya_check(nya_cbor_read_map(&reader, &count) && count == 2, "a map header gives its pair count");

    u8 array[] = {0x83, 0x01, 0x02, 0x03}; // [1, 2, 3]
    reader = nya_cbor_reader(array, sizeof(array));
    nya_check(nya_cbor_read_array(&reader, &count) && count == 3, "an array header gives its item count");
    // and each item reads in turn
    u64 value = 0;
    nya_check(nya_cbor_read_u64(&reader, &value) && value == 1, "the array's first item reads");
  }

  // skip steps over any whole value, nesting and all
  {
    // {1: [2, 3], "x": 4} -- skip the array value, then read the "x" key
    u8 nested[] = {0xA2, 0x01, 0x82, 0x02, 0x03, 0x61, 'x', 0x04};
    NYA_CborReader reader = nya_cbor_reader(nested, sizeof(nested));
    u64 count = 0;
    nya_check(nya_cbor_read_map(&reader, &count) && count == 2, "the outer map reads");

    s64 label = 0;
    nya_check(nya_cbor_read_int(&reader, &label) && label == 1, "the first label reads");
    nya_check(nya_cbor_skip(&reader), "the array value is skipped whole");

    const u8 *text = nullptr;
    u64 size = 0;
    nya_check(nya_cbor_read_text(&reader, &text, &size) && size == 1 && text[0] == 'x', "and the next key is right where it should be");
  }

  // malformed and truncated input is refused, and (under ASan) reads nothing out of bounds
  {
    NYA_CborReader empty = nya_cbor_reader(nullptr, 0);
    u64 value = 0;
    nya_check(!nya_cbor_read_u64(&empty, &value), "an empty reader reads nothing");

    u8 reserved[] = {0x1C}; // additional info 28: a reserved encoding
    NYA_CborReader reader = nya_cbor_reader(reserved, sizeof(reserved));
    nya_check(!nya_cbor_read_u64(&reader, &value), "a reserved additional-info is refused");

    u8 indefinite[] = {0x5F}; // indefinite-length byte string: refused, not chased
    reader = nya_cbor_reader(indefinite, sizeof(indefinite));
    const u8 *out = nullptr;
    u64 size = 0;
    nya_check(!nya_cbor_read_bytes(&reader, &out, &size), "an indefinite-length byte string is refused");

    u8 short_argument[] = {0x1A, 0x00, 0x01}; // says four argument bytes follow, only two do
    reader = nya_cbor_reader(short_argument, sizeof(short_argument));
    nya_check(!nya_cbor_read_u64(&reader, &value), "an argument that runs off the end is refused");

    u8 lying_length[] = {0x58, 0x40, 0x01, 0x02}; // bstr claims 64 bytes; two follow
    reader = nya_cbor_reader(lying_length, sizeof(lying_length));
    nya_check(!nya_cbor_read_bytes(&reader, &out, &size), "a byte string longer than the buffer is refused");

    u8 skip_off_end[] = {0x82, 0x01}; // an array of two, but only one item present
    reader = nya_cbor_reader(skip_off_end, sizeof(skip_off_end));
    nya_check(!nya_cbor_skip(&reader), "skipping an array that runs short is refused");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
