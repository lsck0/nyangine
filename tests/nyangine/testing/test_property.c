/**
 * The laws, rather than the examples. Every _encode/_decode and _serialize/_deserialize pair round
 * trips, the containers behave like the obvious model of themselves, and the math identities hold.
 *
 * A failing law prints the shrunk input that breaks it; see testing_property.h.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

nya_derive_hset(u32);
nya_derive_ring(u32);
nya_derive_dict(u32);

/** Cases per law. Enough that a one-in-a-thousand shape shows up, quick enough to run with the suite. */
#define CASES 2000

/** The seed every law is drawn under. Fixed, so the suite is the same run every time; see the report line. */
#define SEED 0x6E79616E67696E65ULL

/** Bytes the byte-oriented laws generate at most. */
#define BYTES_MAX 256

/** Items the container laws push at most. */
#define ITEMS_MAX 64

/** How far two floats may drift and still count as equal after a round trip through a transform. */
#define TOLERANCE 1.0e-3F

/* ENCODING LAWS */

/** base64: decoding what was encoded gives back exactly the bytes that went in. */
/** Clamping lands inside the range for every float, infinities and NaN included, since settings files and peers send those too. */
static b8 law_clamp_lands_in_range(NYA_Property* property) {
    f32 value   = nya_property_draw_f32_any(property);
    f32 clamped = nya_clamp(value, 0.0F, 1.0F);

    nya_property_note(property, "clamp(%f) = %f", (f64)value, (f64)clamped);
    return clamped >= 0.0F && clamped <= 1.0F;
}

static b8 law_base64_round_trips(NYA_Property* property) {
    u8  bytes[BYTES_MAX];
    u32 count = (u32)nya_property_draw_below(property, BYTES_MAX + 1);

    nya_property_draw_bytes(property, bytes, count);

    NYA_String* encoded = nya_string_create(property->allocator);
    nya_base64_encode(encoded, bytes, count);

    NYA_String* decoded = nya_string_create(property->allocator);
    nya_base64_decode(decoded, (const u8*)encoded->items, encoded->length);

    if (decoded->length != count) {
        nya_property_note(property, "%u bytes in, %llu out", count, (unsigned long long)decoded->length);
        return false;
    }

    return count == 0 || nya_memcmp(decoded->items, bytes, count) == 0;
}

/** compression: decompressing what was compressed gives back exactly the bytes that went in. */
static b8 law_compress_round_trips(NYA_Property* property) {
    u8  bytes[BYTES_MAX];
    u32 count = (u32)nya_property_draw_below(property, BYTES_MAX + 1);

    nya_property_draw_bytes(property, bytes, count);

    u64 bound      = nya_compress_bound(count);
    u8* compressed = nya_arena_alloc(property->allocator, bound + 1);

    u64 written = nya_compress(bytes, count, compressed, bound);

    // documented: an empty input does not compress, and the caller stores it as it is. The round trip
    // is a law about blocks that exist.
    if (count == 0) {
        nya_property_note(property, "an empty input compressed to %llu bytes", (unsigned long long)written);
        return written == 0;
    }

    if (written == 0) {
        nya_property_note(property, "compressing %u bytes produced nothing", count);
        return false;
    }

    u8* restored = nya_arena_alloc(property->allocator, count + 1);

    if (!nya_decompress(compressed, written, restored, count)) {
        nya_property_note(property, "%u bytes compressed to %llu would not decompress", count, (unsigned long long)written);
        return false;
    }

    return nya_memcmp(restored, bytes, count) == 0;
}

/**
 * A key drawn from the property's entropy: a letter and then identifier characters.
 *
 * Not the printable range the string values use. A key is an identifier in all three formats, and the
 * .nya grammar has no escape for a ':' or a '{' inside one, so a law that generated them would state
 * something the formats never claimed. What an arbitrary byte sequence does to a *parser* is a fuzz
 * question, and tests/fuzz asks it.
 * */
static NYA_CString draw_key(NYA_Property* property, u32 length_max) {
    static const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

    /** Letters in ALPHABET, which a key has to start with. */
    static const u32 LETTER_COUNT = 52;

    u32 length = 1 + (u32)nya_property_draw_below(property, length_max);

    NYA_CString key = nya_arena_alloc(property->allocator, (u64)length + 1);

    // a letter first, so a key is never something a reader could take for a number.
    key[0] = ALPHABET[nya_property_draw_u8(property) % LETTER_COUNT];

    for (u32 i = 1; i < length; i++) key[i] = ALPHABET[nya_property_draw_u8(property) % (nya_carray_length(ALPHABET) - 1)];

    key[length] = '\0';

    return key;
}

/** A document drawn from the property's entropy: flat, but with every value type in it. */
static NYA_Object* draw_object(NYA_Property* property) {
    NYA_Object* object = nya_object_create(property->allocator);

    u32 fields = (u32)nya_property_draw_below(property, 12);

    for (u32 i = 0; i < fields; i++) {
        NYA_CString key = draw_key(property, 12);

        switch (nya_property_draw_u8(property) % 5) {
            case 0: nya_object_add(object, key, (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_property_draw_u64(property) }); break;
            case 1: nya_object_add(object, key, (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)nya_property_draw_u64(property) }); break;
            case 2: nya_object_add(object, key, (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = nya_property_draw_bool(property, 50) }); break;
            case 3: nya_object_add(object, key, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_property_draw_text(property, 24) }); break;

            // the finite range only: a NaN is not equal to itself, so a round trip that preserved it
            // perfectly would still fail the comparison. What a NaN does to a parser is a fuzz
            // question, and tests/fuzz asks it.
            default: nya_object_add(object, key, (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)nya_property_draw_f32(property, -1.0e6F, 1.0e6F) }); break;
        }
    }

    return object;
}

/** The magnitude of a signed integer, in u64. Negating S64_MIN within s64 overflows; in u64 it does not. */
static u64 magnitude_of(s64 value) {
    return value < 0 ? (u64)(-(value + 1)) + 1 : (u64)value;
}

/**
 * An integer pulled out of a value, as a sign and a magnitude.
 *
 * Not as an s64: a u64 past S64_MAX casts to a negative one, and two values that differ only in that
 * cast compare equal to each other and unequal to the real number. The whole point of this comparison
 * is that it does not lie about large numbers.
 * */
typedef struct {
    b8  negative;
    u64 magnitude;
} PropertyInteger;

/** Whether a value came back as an integer at all, and what it says. */
static b8 value_as_integer(NYA_Value value, OUT PropertyInteger* out_integer) {
    *out_integer = (PropertyInteger){ 0 };

    switch (value.type) {
        case NYA_TYPE_U8:  out_integer->magnitude = value.as_u8; return true;
        case NYA_TYPE_U16: out_integer->magnitude = value.as_u16; return true;
        case NYA_TYPE_U32: out_integer->magnitude = value.as_u32; return true;
        case NYA_TYPE_U64: out_integer->magnitude = value.as_u64; return true;

        case NYA_TYPE_S8:  out_integer->negative = value.as_s8 < 0; out_integer->magnitude = magnitude_of(value.as_s8); return true;
        case NYA_TYPE_S16: out_integer->negative = value.as_s16 < 0; out_integer->magnitude = magnitude_of(value.as_s16); return true;
        case NYA_TYPE_S32: out_integer->negative = value.as_s32 < 0; out_integer->magnitude = magnitude_of(value.as_s32); return true;
        case NYA_TYPE_S64: out_integer->negative = value.as_s64 < 0; out_integer->magnitude = magnitude_of(value.as_s64); return true;

        default: return false;
    }
}

/** Whether a value came back as a real number, and what it says. */
static b8 value_as_real(NYA_Value value, OUT f64* out_real) {
    switch (value.type) {
        case NYA_TYPE_F32: *out_real = (f64)value.as_f32; return true;
        case NYA_TYPE_F64: *out_real = value.as_f64; return true;

        default: return false;
    }
}

/**
 * Whether two values carry the same thing.
 *
 * Neither the width nor the integer-ness is part of the law. JSON has one number type: 0.5 comes back
 * as a real, 0.0 comes back as the integer zero because that is what "0" is, and an integer past
 * 2^53 comes back as the nearest real because that is as much as the format records. What a document
 * stores is a number, and what has to survive is its value.
 * */
static b8 values_match(NYA_Value a, NYA_Value b) {
    PropertyInteger left_integer  = { 0 };
    PropertyInteger right_integer = { 0 };

    b8 a_is_integer = value_as_integer(a, &left_integer);
    b8 b_is_integer = value_as_integer(b, &right_integer);

    // two integers compare exactly: there is no rounding to allow for, and a tolerance wide enough
    // for a u64 would swallow sixteen digits of difference.
    if (a_is_integer && b_is_integer) {
        return left_integer.magnitude == right_integer.magnitude && (left_integer.negative == right_integer.negative || left_integer.magnitude == 0);
    }

    f64 left_real  = 0.0;
    f64 right_real = 0.0;

    b8 a_is_number = a_is_integer || value_as_real(a, &left_real);
    b8 b_is_number = b_is_integer || value_as_real(b, &right_real);

    if (a_is_number || b_is_number) {
        if (!a_is_number || !b_is_number) return false;

        if (a_is_integer) left_real = left_integer.negative ? -(f64)left_integer.magnitude : (f64)left_integer.magnitude;
        if (b_is_integer) right_real = right_integer.negative ? -(f64)right_integer.magnitude : (f64)right_integer.magnitude;

        // a real survives the decimal text formats only within the digits they print, so this is the
        // one comparison with a tolerance and it is relative.
        f64 scale = nya_max(1.0, fabs(left_real));
        return fabs(left_real - right_real) <= (f64)TOLERANCE * scale;
    }

    if (a.type != b.type) return false;

    switch (a.type) {
        case NYA_TYPE_B8:     return a.as_b8 == b.as_b8;
        case NYA_TYPE_STRING: return nya_string_equals(a.as_string, b.as_string);

        default: return false;
    }
}

/** One format's round trip, shared by the three laws below. */
static b8 serde_round_trips(NYA_Property* property, NYA_SerdeFormat format) {
    NYA_Object* original = draw_object(property);

    NYA_SerdeFlags flags = nya_property_draw_bool(property, 50) ? NYA_SERDE_PRETTY : NYA_SERDE_NONE;

    NYA_String* text = nya_serialize(property->allocator, original, format, flags);
    if (text == nullptr) {
        nya_property_note(property, "%s would not serialize an object of %llu fields", NYA_SERDE_FORMAT_NAME_MAP[format],
                          (unsigned long long)original->length);
        return false;
    }

    NYA_Object* restored = nullptr;
    NYA_Error   parsed   = nya_deserialize(property->allocator, (const u8*)text->items, text->length, format, flags, &restored);

    if (!parsed.ok || restored == nullptr) {
        nya_property_note(property, "%s would not read back what it wrote: %s", NYA_SERDE_FORMAT_NAME_MAP[format], (NYA_ConstCString)parsed.message);
        return false;
    }

    if (restored->length != original->length) {
        nya_property_note(property, "%s: %llu fields in, %llu out", NYA_SERDE_FORMAT_NAME_MAP[format], (unsigned long long)original->length,
                          (unsigned long long)restored->length);
        return false;
    }

    // the iterator hands out a pointer to the stored key, so every use below dereferences it once.
    nya_dict_foreach_key (original, slot) {
        NYA_CString key = *slot;

        NYA_Value* before = nya_dict_get(original, key);
        NYA_Value* after  = nya_dict_get(restored, key);

        if (after == nullptr) {
            nya_property_note(property, "%s dropped the field '%s'", NYA_SERDE_FORMAT_NAME_MAP[format], key);
            return false;
        }

        if (!values_match(*before, *after)) {
            // the two type names, because "changed" without them is a counterexample nobody can act
            // on: a number that came back narrower is a different finding from one that came back
            // as a string.
            nya_property_note(property, "%s changed the field '%s': %s in, %s out", NYA_SERDE_FORMAT_NAME_MAP[format],
                              key, NYA_TYPE_NAME_MAP[before->type], NYA_TYPE_NAME_MAP[after->type]);
            return false;
        }
    }

    return true;
}

static b8 law_serde_nya_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_NYA);
}

static b8 law_serde_json_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_JSON);
}

static b8 law_serde_jsonc_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_JSONC);
}

static b8 law_serde_nya_binary_round_trips(NYA_Property* property) {
    return serde_round_trips(property, NYA_SERDE_FORMAT_NYA_BINARY);
}

/*
 * The binary .nya against the text one, over documents with every type and nesting, compared as
 * bytes. The binary form has one encoding per object, so two objects that encode alike hold the same
 * values bit for bit, where the flat laws above have to allow for a decimal format's rounding.
 */

/** How deep the generated documents nest: an array of objects holding arrays. The bound itself is a table case in test_serde_nya_binary.c. */
#define NESTING_MAX 3

/** Members or elements one container gets at most. */
#define CONTAINER_ITEMS_MAX 6

/** Every scalar type the binary form has a tag for, which is every one the text form writes. */
static const NYA_Type SCALAR_TYPES[] = {
    NYA_TYPE_NULL, NYA_TYPE_B8,  NYA_TYPE_B16,  NYA_TYPE_B32,  NYA_TYPE_B64,  NYA_TYPE_B128,   NYA_TYPE_U8,  NYA_TYPE_U16,
    NYA_TYPE_U32,  NYA_TYPE_U64, NYA_TYPE_U128, NYA_TYPE_S8,   NYA_TYPE_S16,  NYA_TYPE_S32,    NYA_TYPE_S64, NYA_TYPE_S128,
    NYA_TYPE_F16,  NYA_TYPE_F32, NYA_TYPE_F64,  NYA_TYPE_F128, NYA_TYPE_CHAR, NYA_TYPE_STRING,
};

static NYA_Value draw_scalar(NYA_Property* property, NYA_Type type) {
    NYA_Value value = { .type = type };
    nya_memset(&value.as_u128, 0, sizeof(value.as_u128));

    u64 bits = nya_property_draw_u64(property);

    switch (type) {
        case NYA_TYPE_NULL:   break;

        case NYA_TYPE_B8:     value.as_b8 = (b8)(bits & 1); break;
        case NYA_TYPE_B16:    value.as_b16 = (b16)(bits & 1); break;
        case NYA_TYPE_B32:    value.as_b32 = (b32)(bits & 1); break;
        case NYA_TYPE_B64:    value.as_b64 = (b64)(bits & 1); break;
        case NYA_TYPE_B128:   value.as_b128 = (b128)(bits & 1); break;

        case NYA_TYPE_U8:     value.as_u8 = (u8)bits; break;
        case NYA_TYPE_U16:    value.as_u16 = (u16)bits; break;
        case NYA_TYPE_U32:    value.as_u32 = (u32)bits; break;
        case NYA_TYPE_U64:    value.as_u64 = bits; break;
        case NYA_TYPE_U128:   value.as_u128 = ((u128)nya_property_draw_u64(property) << 64) | bits; break;

        case NYA_TYPE_S8:     value.as_s8 = (s8)(u8)bits; break;
        case NYA_TYPE_S16:    value.as_s16 = (s16)(u16)bits; break;
        case NYA_TYPE_S32:    value.as_s32 = (s32)(u32)bits; break;
        case NYA_TYPE_S64:    value.as_s64 = (s64)bits; break;
        case NYA_TYPE_S128:   value.as_s128 = (s128)(((u128)nya_property_draw_u64(property) << 64) | bits); break;

        // finite only, for the reason draw_object gives. The divisions fill the significand, so the
        // text form's hexadecimal has every bit to carry.
        case NYA_TYPE_F16:    value.as_f16 = (f16)nya_property_draw_f32(property, -1000.0F, 1000.0F); break;
        case NYA_TYPE_F32:    value.as_f32 = nya_property_draw_f32(property, -1.0e6F, 1.0e6F); break;
        case NYA_TYPE_F64:    value.as_f64 = (f64)nya_property_draw_f32(property, -1.0e6F, 1.0e6F) / 3.0; break;
        case NYA_TYPE_F128:   value.as_f128 = (f128)nya_property_draw_f32(property, -1.0e6F, 1.0e6F) / 3.0L; break;

        // a letter: the text form writes a char as a one character string and reads its first byte,
        // so an escaped quote would come back as the backslash.
        case NYA_TYPE_CHAR:   value.as_char = (char)('a' + bits % 26); break;
        case NYA_TYPE_STRING: value.as_string = nya_property_draw_text(property, 16); break;

        default:              nya_unreachable();
    }

    return value;
}

static NYA_Value draw_value(NYA_Property* property, u32 depth);

static NYA_Object* draw_document(NYA_Property* property, u32 depth) {
    NYA_Object* object = nya_object_create(property->allocator);

    u32 members = (u32)nya_property_draw_below(property, CONTAINER_ITEMS_MAX + 1);
    for (u32 i = 0; i < members; i++) nya_object_add(object, draw_key(property, 8), draw_value(property, depth + 1));

    return object;
}

static NYA_Value draw_value(NYA_Property* property, u32 depth) {
    u64 shape = depth < NESTING_MAX ? nya_property_draw_below(property, 4) : 0;

    switch (shape) {
        case 1: return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *draw_document(property, depth) };

        // one element type throughout, which both forms write once for the whole array.
        case 2: {
            NYA_Type type     = SCALAR_TYPES[nya_property_draw_below(property, nya_carray_length(SCALAR_TYPES))];
            u32      elements = (u32)nya_property_draw_below(property, CONTAINER_ITEMS_MAX + 1);

            NYA_ArrayᐸNYA_Valueᐳ* array = nya_array_create(property->allocator, NYA_Value);
            for (u32 i = 0; i < elements; i++) nya_array_push_back(array, draw_scalar(property, type));

            return (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *array };
        }

        // anything at all, containers included, which is where `any` and nested arrays come from.
        case 3: {
            u32 elements = (u32)nya_property_draw_below(property, CONTAINER_ITEMS_MAX + 1);

            NYA_ArrayᐸNYA_Valueᐳ* array = nya_array_create(property->allocator, NYA_Value);
            for (u32 i = 0; i < elements; i++) nya_array_push_back(array, draw_value(property, depth + 1));

            return (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *array };
        }

        default: return draw_scalar(property, SCALAR_TYPES[nya_property_draw_below(property, nya_carray_length(SCALAR_TYPES))]);
    }
}

/** Whether `object` encodes to exactly `expected`. Notes which stage lost something when it does not. */
static b8 encodes_to(NYA_Property* property, const NYA_Object* object, const NYA_String* expected, NYA_ConstCString stage) {
    NYA_String* bytes   = nullptr;
    NYA_Error   encoded = nya_serde_nya_binary_encode(property->allocator, object, nullptr, &bytes);

    if (!encoded.ok) {
        nya_property_note(property, "%s would not encode: %s", stage, (NYA_ConstCString)encoded.message);
        return false;
    }

    if (bytes->length != expected->length || nya_memcmp(bytes->items, expected->items, bytes->length) != 0) {
        nya_property_note(
            property,
            "%s encodes to %llu bytes that differ from the original's %llu",
            stage,
            (unsigned long long)bytes->length,
            (unsigned long long)expected->length
        );
        return false;
    }

    return true;
}

/** object -> binary -> object -> text -> object -> binary: every step keeps every value, bit for bit. */
static b8 law_serde_nya_binary_matches_text(NYA_Property* property) {
    NYA_Object* original = draw_document(property, 0);

    NYA_String* bytes   = nullptr;
    NYA_Error   encoded = nya_serde_nya_binary_encode(property->allocator, original, nullptr, &bytes);
    if (!encoded.ok) {
        nya_property_note(property, "the original would not encode: %s", (NYA_ConstCString)encoded.message);
        return false;
    }

    NYA_Object* decoded = nullptr;
    NYA_Error   parsed  = nya_serde_nya_binary_decode(property->allocator, bytes->items, bytes->length, nullptr, &decoded);
    if (!parsed.ok) {
        nya_property_note(property, "binary would not read back what it wrote: %s", (NYA_ConstCString)parsed.message);
        return false;
    }
    if (!encodes_to(property, decoded, bytes, "the decoded object")) return false;

    NYA_SerdeFlags flags = nya_property_draw_bool(property, 50) ? NYA_SERDE_PRETTY : NYA_SERDE_NONE;
    NYA_String*    text  = nya_serialize(property->allocator, decoded, NYA_SERDE_FORMAT_NYA, flags);

    NYA_Object* from_text = nullptr;
    NYA_Error   read      = nya_deserialize(property->allocator, text->items, text->length, NYA_SERDE_FORMAT_NYA, flags, &from_text);
    if (!read.ok) {
        nya_property_note(property, "the text form would not read what it wrote for a decoded document: %s", (NYA_ConstCString)read.message);
        return false;
    }

    return encodes_to(property, from_text, bytes, "the document read back from text");
}

/** A run of commands survives the wire: same count, same ticks, same inputs. */
static b8 law_net_command_round_trips(NYA_Property* property) {
    NYA_NetCommand sent[NYA_NET_COMMAND_REDUNDANCY] = { 0 };

    u32 count = 1 + (u32)nya_property_draw_below(property, NYA_NET_COMMAND_REDUNDANCY);
    u64 tick  = nya_property_draw_below(property, 1u << 20);

    for (u32 i = 0; i < count; i++) {
        // strictly increasing, which is what a run is; the decoder rejects anything else, and the
        // fuzz target is what checks that it does.
        tick += 1 + nya_property_draw_below(property, 4);

        sent[i] = (NYA_NetCommand){
            .tick    = tick,
            .actions = nya_property_draw_u64(property),
            .aim     = { nya_property_draw_f32(property, -1.0F, 1.0F), nya_property_draw_f32(property, -1.0F, 1.0F) },
            .analog  = nya_property_draw_f32(property, -1.0F, 1.0F),
        };
    }

    NYA_String* encoded = nya_string_create(property->allocator);
    if (!nya_net_command_encode(encoded, sent, count).ok) {
        nya_property_note(property, "a run of %u commands would not encode", count);
        return false;
    }

    NYA_NetCommand received[NYA_NET_COMMAND_REDUNDANCY] = { 0 };
    u32            received_count                       = 0;

    NYA_Error decoded = nya_net_command_decode((const u8*)encoded->items, encoded->length, received, &received_count);

    if (!decoded.ok) {
        nya_property_note(property, "a run of %u commands would not decode: %s", count, (NYA_ConstCString)decoded.message);
        return false;
    }

    if (received_count != count) {
        nya_property_note(property, "%u commands in, %u out", count, received_count);
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        if (received[i].tick != sent[i].tick || received[i].actions != sent[i].actions) {
            nya_property_note(property, "command %u came back changed", i);
            return false;
        }
    }

    return true;
}

/** A snapshot survives the wire: same entities, same handles, same order. */
static b8 law_net_snapshot_round_trips(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, 17);

    NYA_NetEntityState states[16] = { 0 };

    u32 index = 0;

    for (u32 i = 0; i < count; i++) {
        // ascending and non-zero: that is what the encoder is given and what the decoder promises
        // back, and a decoder handed anything else is a fuzz question rather than a law.
        index += 1 + (u32)nya_property_draw_below(property, 8);

        states[i] = (NYA_NetEntityState){
            .handle   = { .index = index, .generation = 1 + (u32)nya_property_draw_below(property, 64) },
            .type     = (u32)nya_property_draw_below(property, 8),
            .flags    = nya_property_draw_u64(property),
            .position = { nya_property_draw_f32(property, -1000.0F, 1000.0F), nya_property_draw_f32(property, -1000.0F, 1000.0F),
                          nya_property_draw_f32(property, -1000.0F, 1000.0F) },
            .scale    = { 1.0F, 1.0F, 1.0F },
            .rotation = nya_quaternion_identity,
        };
    }

    NYA_NetSnapshot sent = { .tick = nya_property_draw_below(property, 1u << 20), .entities = states, .entity_count = count };

    NYA_String* encoded = nya_string_create(property->allocator);
    if (!nya_net_snapshot_encode(property->allocator, &sent, nullptr, encoded).ok) {
        nya_property_note(property, "a snapshot of %u entities would not encode", count);
        return false;
    }

    NYA_NetSnapshot received = { 0 };
    NYA_Error       decoded  = nya_net_snapshot_decode(property->allocator, (const u8*)encoded->items, encoded->length, nullptr, &received);

    if (!decoded.ok) {
        nya_property_note(property, "a snapshot of %u entities would not decode: %s", count, (NYA_ConstCString)decoded.message);
        return false;
    }

    if (received.entity_count != count || received.tick != sent.tick) {
        nya_property_note(property, "%u entities at tick %llu in, %u at %llu out", count, (unsigned long long)sent.tick, received.entity_count,
                          (unsigned long long)received.tick);
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        if (received.entities[i].handle.index != states[i].handle.index || received.entities[i].handle.generation != states[i].handle.generation) {
            nya_property_note(property, "entity %u came back as a different handle", i);
            return false;
        }

        if (received.entities[i].flags != states[i].flags) {
            nya_property_note(property, "entity %u came back with different flags", i);
            return false;
        }
    }

    return true;
}

/** percent encoding: decoding what was encoded gives back the bytes, and the encoding holds nothing a URL would refuse. */
static b8 law_percent_round_trips(NYA_Property* property) {
    u8  bytes[BYTES_MAX];
    u32 count = (u32)nya_property_draw_below(property, BYTES_MAX + 1);

    nya_property_draw_bytes(property, bytes, count);

    char encoded[BYTES_MAX * 3 + 1];
    u64  encoded_length = 0;

    if (!nya_percent_encode(bytes, count, encoded, sizeof(encoded), &encoded_length).ok) {
        nya_property_note(property, "%u bytes did not encode into three times their size", count);
        return false;
    }

    // every byte out is unreserved or starts an escape of two upper case hex digits.
    for (u64 i = 0; i < encoded_length; i++) {
        char c = encoded[i];

        if (c == '%') {
            b8 hex = i + 2 < encoded_length && isxdigit((u8)encoded[i + 1]) && isxdigit((u8)encoded[i + 2]) && !islower((u8)encoded[i + 1]) &&
                     !islower((u8)encoded[i + 2]);

            if (!hex) {
                nya_property_note(property, "a '%%' at %llu does not start an upper case escape", (unsigned long long)i);
                return false;
            }

            i += 2;
            continue;
        }

        if (!isalnum((u8)c) && c != '-' && c != '.' && c != '_' && c != '~') {
            nya_property_note(property, "the encoding carries a raw 0x%02x", (unsigned)(u8)c);
            return false;
        }
    }

    u8  decoded[BYTES_MAX];
    u64 decoded_length = 0;

    if (!nya_percent_decode(encoded, encoded_length, decoded, sizeof(decoded), &decoded_length).ok) {
        nya_property_note(property, "the decoder refused what the encoder wrote");
        return false;
    }

    if (decoded_length != count) {
        nya_property_note(property, "%u bytes in, %llu out", count, (unsigned long long)decoded_length);
        return false;
    }

    return nya_memcmp(decoded, bytes, count) == 0;
}

/** Appends `text` to a URL being built, the bound asserted rather than trusted. */
static void url_append(char* url, u64* length, NYA_ConstCString text, u64 text_length) {
    nya_assert(*length + text_length < NYA_URL_MAX_BYTES, "a generated url outgrew the bound");

    nya_memcpy(url + *length, text, text_length);
    *length += text_length;
}

/** Longest raw component the URL law draws before encoding it. */
#define URL_PART_MAX 16

/**
 * Between `length_min` and `length_max` raw bytes for one component. Never NUL, since no component may
 * decode to one; for a path segment also no control and no '/', and never "." or "..".
 * */
static u32 url_draw_raw(NYA_Property* property, OUT u8* raw, u32 length_min, u32 length_max, b8 path_segment) {
    nya_assert(length_min <= length_max && length_max <= URL_PART_MAX);

    u32 count = length_min + (u32)nya_property_draw_below(property, length_max - length_min + 1);

    for (u32 i = 0; i < count; i++) {
        u8 byte = nya_property_draw_u8(property);

        if (byte == 0 || (path_segment && (byte < 0x20 || byte == 0x7F || byte == '/'))) byte = 'a';

        raw[i] = byte;
    }

    // refused however it is spelled, so a generated segment is never one.
    if (path_segment && count > 0 && count <= 2 && raw[0] == '.' && raw[count - 1] == '.') raw[0] = 'd';

    return count;
}

/** `raw` percent-encoded onto the URL. */
static void url_append_encoded(char* url, u64* length, const u8* raw, u32 count) {
    char encoded[URL_PART_MAX * 3 + 1];
    u64  encoded_length = 0;

    NYA_EXPECT(nya_percent_encode(raw, count, encoded, sizeof(encoded), &encoded_length));
    url_append(url, length, encoded, encoded_length);
}

/** A host of the drawn kind, written as a URL writes it. */
static void url_append_host(NYA_Property* property, char* url, u64* length) {
    char host[64];
    s32  written = 0;

    switch (nya_property_draw_below(property, 3)) {
        case 0: {
            u8 octets[4];
            nya_property_draw_bytes(property, octets, sizeof(octets));

            written = snprintf(host, sizeof(host), "%u.%u.%u.%u", (unsigned)octets[0], (unsigned)octets[1], (unsigned)octets[2], (unsigned)octets[3]);
        } break;

        case 1: {
            // eight groups, with the zero run in the middle compressed some of the time.
            unsigned groups[8];
            for (u32 i = 0; i < 8; i++) groups[i] = (unsigned)nya_property_draw_below(property, 0x10000);

            if (nya_property_draw_bool(property, 50)) {
                written = snprintf(host, sizeof(host), "[%x:%x::%x:%x]", groups[0], groups[1], groups[6], groups[7]);
            } else {
                written = snprintf(
                    host,
                    sizeof(host),
                    "[%x:%x:%x:%x:%x:%x:%x:%x]",
                    groups[0],
                    groups[1],
                    groups[2],
                    groups[3],
                    groups[4],
                    groups[5],
                    groups[6],
                    groups[7]
                );
            }
        } break;

        default: {
            // labels of letters, digits and '_', the last starting with a letter so it is never read as an address.
            static const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

            u32 labels = 1 + (u32)nya_property_draw_below(property, 4);

            for (u32 label = 0; label < labels; label++) {
                u32 label_length = 1 + (u32)nya_property_draw_below(property, 8);

                if (label > 0) host[written++] = '.';

                for (u32 i = 0; i < label_length; i++) {
                    u32 letters = (label == labels - 1 && i == 0) ? 52 : (u32)(nya_carray_length(ALPHABET) - 1);
                    host[written++] = ALPHABET[nya_property_draw_u8(property) % letters];
                }
            }
        } break;
    }

    nya_assert(written > 0 && (u64)written < sizeof(host));
    url_append(url, length, host, (u64)written);
}

/** Whether two parsed URLs say the same thing, component by component. */
static b8 url_equals(const NYA_Url* a, const NYA_Url* b) {
    b8 flags = a->scheme == b->scheme && a->host_kind == b->host_kind && a->has_userinfo == b->has_userinfo && a->has_port == b->has_port &&
               a->port == b->port && a->has_query == b->has_query && a->has_fragment == b->has_fragment;

    NYA_UrlSpan a_spans[] = { a->host, a->path, a->query, a->fragment };
    NYA_UrlSpan b_spans[] = { b->host, b->path, b->query, b->fragment };

    for (u32 i = 0; i < nya_carray_length(a_spans) && flags; i++) {
        flags = a_spans[i].length == b_spans[i].length && nya_memcmp(a->text + a_spans[i].offset, b->text + b_spans[i].offset, a_spans[i].length) == 0;
    }

    return flags;
}

/**
 * URLs: anything built from legal parts parses, renders, and parses again to the same URL, and a query
 * parameter written through nya_percent_encode is found and decodes to exactly what was written.
 * */
static b8 law_url_round_trips(NYA_Property* property) {
    static const NYA_ConstCString SCHEMES[] = { "http://", "HTTPS://", "ws://", "wss://" };

    char url_text[NYA_URL_MAX_BYTES];
    u64  length = 0;
    b8   target = nya_property_draw_bool(property, 25);

    if (!target) {
        NYA_ConstCString scheme = SCHEMES[nya_property_draw_below(property, nya_carray_length(SCHEMES))];
        url_append(url_text, &length, scheme, strlen(scheme));
        url_append_host(property, url_text, &length);

        if (nya_property_draw_bool(property, 50)) {
            char port[8];
            s32  written = snprintf(port, sizeof(port), ":%u", 1U + (u32)nya_property_draw_below(property, 65535));
            url_append(url_text, &length, port, (u64)written);
        }
    }

    // a target always has a path; an absolute URL may have none.
    u32 segments = (u32)nya_property_draw_below(property, 5) + (target ? 1U : 0U);
    for (u32 i = 0; i < segments; i++) {
        u8  raw[URL_PART_MAX];
        u32 count = url_draw_raw(property, raw, 1, 8, true);

        url_append(url_text, &length, "/", 1);
        url_append_encoded(url_text, &length, raw, count);
    }

    // the query is k<i>=<value>, so the names are distinct and the law knows what each one should hold.
    u32 pairs     = 0;
    b8  has_query = nya_property_draw_bool(property, 60);
    u8  values[4][URL_PART_MAX];
    u32 value_lengths[4] = { 0 };

    if (has_query) {
        url_append(url_text, &length, "?", 1);
        pairs = (u32)nya_property_draw_below(property, 5);

        for (u32 i = 0; i < pairs; i++) {
            char key[8];
            s32  written = snprintf(key, sizeof(key), "%sk%u=", i > 0 ? "&" : "", i);
            url_append(url_text, &length, key, (u64)written);

            value_lengths[i] = url_draw_raw(property, values[i], 0, URL_PART_MAX, false);
            url_append_encoded(url_text, &length, values[i], value_lengths[i]);
        }
    }

    if (!target && nya_property_draw_bool(property, 30)) {
        u8  raw[URL_PART_MAX];
        u32 count = url_draw_raw(property, raw, 0, 8, false);

        url_append(url_text, &length, "#", 1);
        url_append_encoded(url_text, &length, raw, count);
    }

    NYA_Url        first   = { 0 };
    NYA_UrlFailure failure = { 0 };
    NYA_Error      parsed  = target ? nya_url_parse_target(url_text, length, &first, &failure) : nya_url_parse(url_text, length, &first, &failure);

    if (!parsed.ok) {
        nya_property_note(property, "'%.*s' was refused: %s at %u", (int)length, url_text, nya_url_rule_text(failure.rule), failure.offset);
        return false;
    }

    char formatted[NYA_URL_MAX_BYTES + 1];
    u64  formatted_length = 0;
    NYA_EXPECT(nya_url_format(&first, formatted, sizeof(formatted), &formatted_length));

    NYA_Url second = { 0 };
    parsed         = target ? nya_url_parse_target(formatted, formatted_length, &second, &failure) : nya_url_parse(formatted, formatted_length, &second, &failure);

    if (!parsed.ok || !url_equals(&first, &second)) {
        nya_property_note(property, "'%.*s' rendered as '%s', which does not parse back to it", (int)length, url_text, formatted);
        return false;
    }

    for (u32 i = 0; i < pairs; i++) {
        char name[8];
        (void)snprintf(name, sizeof(name), "k%u", i);

        char value[32];
        b8   found = false;

        if (!nya_url_query_find(&second, name, value, sizeof(value), &found).ok || !found) {
            nya_property_note(property, "'%s' in '%s' was not found", name, formatted);
            return false;
        }

        if (strlen(value) != value_lengths[i] || nya_memcmp(value, values[i], value_lengths[i]) != 0) {
            nya_property_note(property, "'%s' in '%s' decoded to '%s'", name, formatted, value);
            return false;
        }
    }

    return true;
}

/* CONTAINER LAWS */

/** An array is a stack: what goes in with push_back comes out of pop_back in reverse. */
static b8 law_array_is_a_stack(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    NYA_Arrayᐸu32ᐳ* array = nya_array_create(property->allocator, u32);

    u32 pushed[ITEMS_MAX];

    for (u32 i = 0; i < count; i++) {
        pushed[i] = (u32)nya_property_draw_u64(property);
        nya_array_push_back(array, pushed[i]);

        if (array->length != i + 1) {
            nya_property_note(property, "pushing %u items left a length of %llu", i + 1, (unsigned long long)array->length);
            return false;
        }
    }

    for (u32 i = count; i > 0; i--) {
        if (array->items[array->length - 1] != pushed[i - 1]) {
            nya_property_note(property, "item %u came back as %u", i - 1, array->items[array->length - 1]);
            return false;
        }

        nya_array_pop_back(array);
    }

    return array->length == 0;
}

/** A dictionary remembers what it was told, and forgets what was removed. */
static b8 law_dict_remembers(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    NYA_Dictᐸu32ᐳ* dict = nya_dict_create(property->allocator, u32);

    for (u32 i = 0; i < count; i++) {
        NYA_CString key = nya_property_draw_text(property, 16);
        if (key[0] == '\0') continue;

        u32 value = (u32)nya_property_draw_u64(property);

        nya_dict_add(dict, key, value);

        u32* read = nya_dict_get(dict, key);

        if (read == nullptr || *read != value) {
            nya_property_note(property, "'%s' was set to %u and read back %s", key, value, read == nullptr ? "nothing" : "something else");
            return false;
        }

        if (nya_property_draw_bool(property, 30)) {
            nya_dict_remove(dict, key);

            if (nya_dict_contains(dict, key)) {
                nya_property_note(property, "'%s' survived being removed", key);
                return false;
            }
        }
    }

    return true;
}

/** A set holds each member once, and a removed member is gone. */
static b8 law_hset_holds_members_once(NYA_Property* property) {
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    NYA_HSetᐸu32ᐳ* set = nya_hset_create(property->allocator, u32);

    for (u32 i = 0; i < count; i++) {
        u32 item = (u32)nya_property_draw_below(property, 64);

        u64 before = set->length;
        b8  known  = nya_hset_contains(set, item);

        nya_hset_add(set, item);

        if (!nya_hset_contains(set, item)) {
            nya_property_note(property, "%u was inserted and is not in the set", item);
            return false;
        }

        // a set grows only when it learns something new. A length that grew on a repeat is a
        // duplicate, which is the one thing a set must not have.
        u64 expected = known ? before : before + 1;

        if (set->length != expected) {
            nya_property_note(property, "inserting %u took the length from %llu to %llu", item, (unsigned long long)before,
                              (unsigned long long)set->length);
            return false;
        }
    }

    return true;
}

/** A ring is a queue: what goes in first comes out first. */
static b8 law_ring_is_a_queue(NYA_Property* property) {
    NYA_Ringᐸu32ᐳ* ring = nya_ring_create_with_capacity(property->allocator, u32, ITEMS_MAX);

    u32 queued[ITEMS_MAX];
    u32 count = (u32)nya_property_draw_below(property, ITEMS_MAX + 1);

    for (u32 i = 0; i < count; i++) {
        queued[i] = (u32)nya_property_draw_u64(property);
        nya_ring_push(ring, queued[i]);
    }

    if (nya_ring_length(ring) != count) {
        nya_property_note(property, "%u pushed, %llu held", count, (unsigned long long)nya_ring_length(ring));
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        u32 popped = nya_ring_pop(ring);

        if (popped != queued[i]) {
            nya_property_note(property, "item %u came out as %u instead of %u", i, popped, queued[i]);
            return false;
        }
    }

    return nya_ring_is_empty(ring);
}

/* MATH LAWS */

/** A drawn rotation. Normalised, since every identity below is about unit quaternions. */
static NYA_Quaternion draw_rotation(NYA_Property* property) {
    NYA_Quaternion quaternion = nya_quaternion_from_euler(
        nya_property_draw_f32(property, -3.14F, 3.14F),
        nya_property_draw_f32(property, -3.14F, 3.14F),
        nya_property_draw_f32(property, -3.14F, 3.14F)
    );

    return nya_quaternion_normalize(quaternion);
}

/** A unit quaternion times its own conjugate is the identity rotation. */
static b8 law_quaternion_conjugate_undoes(NYA_Property* property) {
    NYA_Quaternion quaternion = draw_rotation(property);
    NYA_Quaternion undone     = nya_quaternion_multiply(quaternion, nya_quaternion_conjugate(quaternion));

    if (!nya_quaternion_approx_equals(undone, nya_quaternion_identity, TOLERANCE)) {
        nya_property_note(property, "q * conj(q) = (%f, %f, %f, %f)", (f64)undone.x, (f64)undone.y, (f64)undone.z, (f64)undone.w);
        return false;
    }

    return true;
}

/** Normalising twice is normalising once, and the result has length one. */
static b8 law_quaternion_normalize_is_idempotent(NYA_Property* property) {
    NYA_Quaternion once  = draw_rotation(property);
    NYA_Quaternion twice = nya_quaternion_normalize(once);

    if (!nya_quaternion_approx_equals(once, twice, TOLERANCE)) {
        nya_property_note(property, "normalising twice moved the rotation");
        return false;
    }

    f32 length = nya_quaternion_length(once);

    if (fabsf(length - 1.0F) > TOLERANCE) {
        nya_property_note(property, "a normalised quaternion has length %f", (f64)length);
        return false;
    }

    return true;
}

/** A rotation is rigid: it turns a vector without changing how long it is. */
static b8 law_quaternion_rotation_preserves_length(NYA_Property* property) {
    NYA_Quaternion quaternion = draw_rotation(property);

    f32x3 vector = {
        nya_property_draw_f32(property, -100.0F, 100.0F),
        nya_property_draw_f32(property, -100.0F, 100.0F),
        nya_property_draw_f32(property, -100.0F, 100.0F),
    };

    f32 before = nya_vector_length(vector);
    f32 after  = nya_vector_length(nya_quaternion_rotate(quaternion, vector));

    // relative, because the absolute error of a rotation grows with the vector it turns.
    f32 allowed = TOLERANCE * nya_max(1.0F, before);

    if (fabsf(before - after) > allowed) {
        nya_property_note(property, "a rotation took a length of %f to %f", (f64)before, (f64)after);
        return false;
    }

    return true;
}

/** A rotation as a matrix is the same rotation: it turns a vector to the same place. */
static b8 law_quaternion_matrix_agrees(NYA_Property* property) {
    NYA_Quaternion quaternion = draw_rotation(property);

    f32x3 vector = {
        nya_property_draw_f32(property, -10.0F, 10.0F),
        nya_property_draw_f32(property, -10.0F, 10.0F),
        nya_property_draw_f32(property, -10.0F, 10.0F),
    };

    f32x3 rotated = nya_quaternion_rotate(quaternion, vector);
    f32x3 matrixed = nya_matrix_times_vector(nya_quaternion_to_matrix3(quaternion), vector);

    f32 drift   = nya_vector_length(rotated - matrixed);
    f32 allowed = TOLERANCE * nya_max(1.0F, nya_vector_length(vector));

    if (drift > allowed) {
        nya_property_note(property, "the quaternion and its matrix disagree by %f", (f64)drift);
        return false;
    }

    return true;
}

/* THE SUITE */

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { .time_step_ns = nya_time_ms_to_ns(16) } };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    u32 failures = 0;

    printf("TEST: round trips\n");
    failures += nya_property_check("clamping lands in the range for any float", CASES, SEED, law_clamp_lands_in_range);
    failures += nya_property_check("base64 round trips", CASES, SEED, law_base64_round_trips);
    failures += nya_property_check("compression round trips", CASES, SEED, law_compress_round_trips);
    failures += nya_property_check("percent encoding round trips", CASES, SEED, law_percent_round_trips);
    failures += nya_property_check("urls round trip through their rendering", CASES, SEED, law_url_round_trips);
    failures += nya_property_check("serde nya round trips", CASES, SEED, law_serde_nya_round_trips);
    failures += nya_property_check("serde json round trips", CASES, SEED, law_serde_json_round_trips);
    failures += nya_property_check("serde jsonc round trips", CASES, SEED, law_serde_jsonc_round_trips);
    failures += nya_property_check("serde binary nya round trips", CASES, SEED, law_serde_nya_binary_round_trips);
    failures += nya_property_check("binary nya and text nya agree bit for bit", CASES, SEED, law_serde_nya_binary_matches_text);
    failures += nya_property_check("net commands round trip", CASES, SEED, law_net_command_round_trips);
    failures += nya_property_check("net snapshots round trip", CASES, SEED, law_net_snapshot_round_trips);

    printf("TEST: containers behave like the obvious model of themselves\n");
    failures += nya_property_check("an array is a stack", CASES, SEED, law_array_is_a_stack);
    failures += nya_property_check("a dictionary remembers", CASES, SEED, law_dict_remembers);
    failures += nya_property_check("a set holds members once", CASES, SEED, law_hset_holds_members_once);
    failures += nya_property_check("a ring is a queue", CASES, SEED, law_ring_is_a_queue);

    printf("TEST: math identities\n");
    failures += nya_property_check("conjugating undoes a rotation", CASES, SEED, law_quaternion_conjugate_undoes);
    failures += nya_property_check("normalising is idempotent", CASES, SEED, law_quaternion_normalize_is_idempotent);
    failures += nya_property_check("a rotation preserves length", CASES, SEED, law_quaternion_rotation_preserves_length);
    failures += nya_property_check("a rotation matrix agrees with its quaternion", CASES, SEED, law_quaternion_matrix_agrees);

    if (failures > 0) {
        printf("FAILED: test_property (%u failures)\n", failures);
        return EXIT_FAILURE;
    }

    printf("PASSED: test_property (0 failures)\n");

    return EXIT_SUCCESS;
}
