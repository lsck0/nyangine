#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The wire tags, fixed by serde_nya_binary.h. Explicit values, since these are the format and must
 * not follow an edit to the enum's order.
 */
typedef enum {
    _NYA_SERDE_NYA_BINARY_TAG_NULL   = 0x00,
    _NYA_SERDE_NYA_BINARY_TAG_B8     = 0x01,
    _NYA_SERDE_NYA_BINARY_TAG_B16    = 0x02,
    _NYA_SERDE_NYA_BINARY_TAG_B32    = 0x03,
    _NYA_SERDE_NYA_BINARY_TAG_B64    = 0x04,
    _NYA_SERDE_NYA_BINARY_TAG_B128   = 0x05,
    _NYA_SERDE_NYA_BINARY_TAG_U8     = 0x06,
    _NYA_SERDE_NYA_BINARY_TAG_U16    = 0x07,
    _NYA_SERDE_NYA_BINARY_TAG_U32    = 0x08,
    _NYA_SERDE_NYA_BINARY_TAG_U64    = 0x09,
    _NYA_SERDE_NYA_BINARY_TAG_U128   = 0x0A,
    _NYA_SERDE_NYA_BINARY_TAG_S8     = 0x0B,
    _NYA_SERDE_NYA_BINARY_TAG_S16    = 0x0C,
    _NYA_SERDE_NYA_BINARY_TAG_S32    = 0x0D,
    _NYA_SERDE_NYA_BINARY_TAG_S64    = 0x0E,
    _NYA_SERDE_NYA_BINARY_TAG_S128   = 0x0F,
    _NYA_SERDE_NYA_BINARY_TAG_F16    = 0x10,
    _NYA_SERDE_NYA_BINARY_TAG_F32    = 0x11,
    _NYA_SERDE_NYA_BINARY_TAG_F64    = 0x12,
    _NYA_SERDE_NYA_BINARY_TAG_F128   = 0x13,
    _NYA_SERDE_NYA_BINARY_TAG_CHAR   = 0x14,
    _NYA_SERDE_NYA_BINARY_TAG_STRING = 0x15,
    _NYA_SERDE_NYA_BINARY_TAG_OBJECT = 0x16,
    _NYA_SERDE_NYA_BINARY_TAG_ARRAY  = 0x17,

    /** Tags below this carry a value. */
    _NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT,

    /** An array's element tag only. */
    _NYA_SERDE_NYA_BINARY_TAG_ANY = _NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT,
} _NYA_SerdeNyaBinaryTag;

static_assert(_NYA_SERDE_NYA_BINARY_TAG_ANY == 0x18, "the wire tags are fixed; see serde_nya_binary.h");

/** What each value tag decodes to. The only table between the two numberings. */
NYA_INTERNAL const NYA_Type _NYA_SERDE_NYA_BINARY_TYPE_OF_TAG[_NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT] = {
    [_NYA_SERDE_NYA_BINARY_TAG_NULL] = NYA_TYPE_NULL,     [_NYA_SERDE_NYA_BINARY_TAG_B8] = NYA_TYPE_B8,
    [_NYA_SERDE_NYA_BINARY_TAG_B16] = NYA_TYPE_B16,       [_NYA_SERDE_NYA_BINARY_TAG_B32] = NYA_TYPE_B32,
    [_NYA_SERDE_NYA_BINARY_TAG_B64] = NYA_TYPE_B64,       [_NYA_SERDE_NYA_BINARY_TAG_B128] = NYA_TYPE_B128,
    [_NYA_SERDE_NYA_BINARY_TAG_U8] = NYA_TYPE_U8,         [_NYA_SERDE_NYA_BINARY_TAG_U16] = NYA_TYPE_U16,
    [_NYA_SERDE_NYA_BINARY_TAG_U32] = NYA_TYPE_U32,       [_NYA_SERDE_NYA_BINARY_TAG_U64] = NYA_TYPE_U64,
    [_NYA_SERDE_NYA_BINARY_TAG_U128] = NYA_TYPE_U128,     [_NYA_SERDE_NYA_BINARY_TAG_S8] = NYA_TYPE_S8,
    [_NYA_SERDE_NYA_BINARY_TAG_S16] = NYA_TYPE_S16,       [_NYA_SERDE_NYA_BINARY_TAG_S32] = NYA_TYPE_S32,
    [_NYA_SERDE_NYA_BINARY_TAG_S64] = NYA_TYPE_S64,       [_NYA_SERDE_NYA_BINARY_TAG_S128] = NYA_TYPE_S128,
    [_NYA_SERDE_NYA_BINARY_TAG_F16] = NYA_TYPE_F16,       [_NYA_SERDE_NYA_BINARY_TAG_F32] = NYA_TYPE_F32,
    [_NYA_SERDE_NYA_BINARY_TAG_F64] = NYA_TYPE_F64,       [_NYA_SERDE_NYA_BINARY_TAG_F128] = NYA_TYPE_F128,
    [_NYA_SERDE_NYA_BINARY_TAG_CHAR] = NYA_TYPE_CHAR,     [_NYA_SERDE_NYA_BINARY_TAG_STRING] = NYA_TYPE_STRING,
    [_NYA_SERDE_NYA_BINARY_TAG_OBJECT] = NYA_TYPE_OBJECT, [_NYA_SERDE_NYA_BINARY_TAG_ARRAY] = NYA_TYPE_ARRAY,
};

/**
 * The significant bytes of an f128, which is x87 extended precision on every target this engine builds:
 * 64 bits of significand and 16 of sign and exponent, padded to 16 bytes with whatever the stack held.
 * The padding is not written, so one value has one encoding. A port whose long double is something
 * else fails here rather than writing bytes another build misreads.
 * */
#define _NYA_SERDE_NYA_BINARY_F128_BYTES 10
static_assert(__LDBL_MANT_DIG__ == 64, "the binary .nya writes an f128 as x87 extended precision");

/** Bytes of a count or a string length on the wire. */
#define _NYA_SERDE_NYA_BINARY_COUNT_BYTES 4

/** Least bytes one object member occupies: a key length and a tag, for an empty key naming a null. */
#define _NYA_SERDE_NYA_BINARY_MEMBER_BYTES_MIN 2

static_assert(NYA_SERDE_NYA_BINARY_SIZE_MAX <= U32_MAX, "a u32 count or length has to be able to say every size the format allows");
static_assert(NYA_SERDE_NYA_BINARY_KEY_BYTES_MAX <= U8_MAX, "a key's length prefix is one byte");

typedef struct _NYA_SerdeNyaBinaryWriter _NYA_SerdeNyaBinaryWriter;
typedef struct _NYA_SerdeNyaBinaryReader _NYA_SerdeNyaBinaryReader;

/** Encode state. `values` counts every value written, against NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX. */
struct _NYA_SerdeNyaBinaryWriter {
    NYA_Arena*  arena;
    NYA_String* out;
    u64         values;
    u32         depth;
};

/** Decode state. `cursor` never passes `size`; every read checks against what is left first. */
struct _NYA_SerdeNyaBinaryReader {
    NYA_Arena* arena;
    const u8*  data;
    u64        size;
    u64        cursor;
    u64        values;
    u32        depth;
};

NYA_INTERNAL b8 _nya_serde_nya_binary_tag_of(NYA_Type type, OUT u8* out_tag);

NYA_INTERNAL void      _nya_serde_nya_binary_put(_NYA_SerdeNyaBinaryWriter* writer, const void* bytes, u64 count);
NYA_INTERNAL void      _nya_serde_nya_binary_put_uint(_NYA_SerdeNyaBinaryWriter* writer, u128 value, u32 bytes);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_count_values(_NYA_SerdeNyaBinaryWriter* writer, u64 count);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_write_object(_NYA_SerdeNyaBinaryWriter* writer, const NYA_Object* object);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_write_array(_NYA_SerdeNyaBinaryWriter* writer, const NYA_ArrayᐸNYA_Valueᐳ* array);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_write_value(_NYA_SerdeNyaBinaryWriter* writer, const NYA_Value* value);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_write_payload(_NYA_SerdeNyaBinaryWriter* writer, const NYA_Value* value);

NYA_INTERNAL b8        _nya_serde_nya_binary_take(_NYA_SerdeNyaBinaryReader* reader, u64 count, OUT const u8** out_bytes);
NYA_INTERNAL b8        _nya_serde_nya_binary_take_uint(_NYA_SerdeNyaBinaryReader* reader, u32 bytes, OUT u128* out_value);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_claim_values(_NYA_SerdeNyaBinaryReader* reader, u64 count);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_read_header(_NYA_SerdeNyaBinaryReader* reader, const NYA_TypeReflection* type);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_read_object(_NYA_SerdeNyaBinaryReader* reader, OUT NYA_Object* out_object);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_read_array(_NYA_SerdeNyaBinaryReader* reader, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_read_value(_NYA_SerdeNyaBinaryReader* reader, OUT NYA_Value* out_value);
NYA_INTERNAL NYA_Error _nya_serde_nya_binary_read_payload(_NYA_SerdeNyaBinaryReader* reader, u8 tag, OUT NYA_Value* out_value);

NYA_INTERNAL s32 _nya_serde_nya_binary_compare_keys(const void* left, const void* right);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_serde_nya_binary_encode(NYA_Arena* arena, const NYA_Object* object, const NYA_TypeReflection* type, OUT NYA_String** out_bytes) {
    nya_assert(arena != nullptr);
    nya_assert(object != nullptr);
    nya_assert(out_bytes != nullptr);
    nya_assert(type == nullptr || type->kind == NYA_REFLECT_STRUCT || type->kind == NYA_REFLECT_UNION, "only a struct or union describes a document");

    *out_bytes = nullptr;

    // a typed document is a claim about its shape, so the claim is checked where it is made.
    if (type != nullptr) {
        u32 problems = nya_reflect_check(type, object, nullptr, nullptr);
        if (problems > 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the object does not fit %s (" FMTu32 " problems)", type->name, problems);
    }

    u64 layout_hash = type != nullptr ? nya_reflect_layout_hash(type) : 0;
    u16 flags       = type != nullptr ? NYA_SERDE_NYA_BINARY_FLAG_TYPED : 0;

    _NYA_SerdeNyaBinaryWriter writer = { .arena = arena, .out = nya_string_create_with_capacity(arena, 256) };

    _nya_serde_nya_binary_put(&writer, NYA_SERDE_NYA_BINARY_MAGIC, NYA_SERDE_NYA_BINARY_MAGIC_BYTES);
    _nya_serde_nya_binary_put_uint(&writer, NYA_SERDE_NYA_BINARY_VERSION, sizeof(u16));
    _nya_serde_nya_binary_put_uint(&writer, flags, sizeof(u16));
    _nya_serde_nya_binary_put_uint(&writer, layout_hash, sizeof(u64));
    nya_assert(writer.out->length == NYA_SERDE_NYA_BINARY_HEADER_BYTES);

    NYA_TRY(_nya_serde_nya_binary_write_object(&writer, object));

    // checked once at the end: the object is already in memory, so its size costs nothing more to learn.
    if (writer.out->length > NYA_SERDE_NYA_BINARY_SIZE_MAX) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "the document is " FMTu64 " bytes, past the %llu the format allows",
            writer.out->length,
            NYA_SERDE_NYA_BINARY_SIZE_MAX
        );
    }

    nya_assert(writer.depth == 0);
    nya_assert(writer.values <= NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX);

    *out_bytes = writer.out;
    return NYA_OK;
}

NYA_Error nya_serde_nya_binary_decode(NYA_Arena* arena, const u8* data, u64 size, const NYA_TypeReflection* type, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);
    nya_assert(type == nullptr || type->kind == NYA_REFLECT_STRUCT || type->kind == NYA_REFLECT_UNION, "only a struct or union describes a document");

    *out_object = nullptr;

    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_PARSE, "empty input");

    if (size > NYA_SERDE_NYA_BINARY_SIZE_MAX) {
        return nya_error(NYA_ERROR_PARSE, "the document is " FMTu64 " bytes, past the %llu the format allows", size, NYA_SERDE_NYA_BINARY_SIZE_MAX);
    }

    _NYA_SerdeNyaBinaryReader reader = { .arena = arena, .data = data, .size = size };

    NYA_TRY(_nya_serde_nya_binary_read_header(&reader, type));

    NYA_Object* object = nya_arena_alloc(arena, sizeof(NYA_Object));
    NYA_TRY(_nya_serde_nya_binary_read_object(&reader, object));

    if (reader.cursor != reader.size) {
        return nya_error(NYA_ERROR_PARSE, FMTu64 " trailing bytes after the document, from byte " FMTu64, reader.size - reader.cursor, reader.cursor);
    }

    nya_assert(reader.depth == 0);
    nya_assert(reader.values <= NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX);

    if (type != nullptr) {
        u32 problems = nya_reflect_check(type, object, nullptr, nullptr);
        if (problems > 0)
            return nya_error(NYA_ERROR_PARSE, "the document claims %s's layout but does not fit it (" FMTu32 " problems)", type->name, problems);
    }

    *out_object = object;
    return NYA_OK;
}

NYA_String* nya_serde_nya_binary_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) {
    nya_unused(flags);

    NYA_String* bytes   = nullptr;
    NYA_Error   encoded = nya_serde_nya_binary_encode(arena, object, nullptr, &bytes);

    if (!encoded.ok) {
        nya_log_error("Could not encode a binary .nya document: %s", (NYA_ConstCString)encoded.message);
        return nullptr;
    }

    return bytes;
}

NYA_Error nya_serde_nya_binary_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_unused(flags);

    return nya_serde_nya_binary_decode(arena, data, size, nullptr, out_object);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_serde_nya_binary_tag_of(NYA_Type type, OUT u8* out_tag) {
    for (u32 tag = 0; tag < _NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT; tag++) {
        if (_NYA_SERDE_NYA_BINARY_TYPE_OF_TAG[tag] != type) continue;

        *out_tag = (u8)tag;
        return true;
    }

    return false;
}

s32 _nya_serde_nya_binary_compare_keys(const void* left, const void* right) {
    return strcmp(*(NYA_ConstCString const*)left, *(NYA_ConstCString const*)right);
}

/*
 * ─────────────────────────────────────────────────────────
 * WRITING
 * ─────────────────────────────────────────────────────────
 */

void _nya_serde_nya_binary_put(_NYA_SerdeNyaBinaryWriter* writer, const void* bytes, u64 count) {
    if (count == 0) return;

    nya_string_reserve(writer->out, writer->out->length + count);
    nya_memcpy(writer->out->items + writer->out->length, bytes, count);
    writer->out->length += count;
}

/** The low `bytes` bytes of `value`, least significant first. */
void _nya_serde_nya_binary_put_uint(_NYA_SerdeNyaBinaryWriter* writer, u128 value, u32 bytes) {
    nya_assert(bytes >= 1 && bytes <= sizeof(u128));

    u8 buffer[sizeof(u128)];
    for (u32 i = 0; i < bytes; i++) buffer[i] = (u8)(value >> (i * 8));

    _nya_serde_nya_binary_put(writer, buffer, bytes);
}

NYA_Error _nya_serde_nya_binary_count_values(_NYA_SerdeNyaBinaryWriter* writer, u64 count) {
    if (count > NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX - writer->values) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the object holds more than the %u values a document may", NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX);
    }

    writer->values += count;
    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_write_object(_NYA_SerdeNyaBinaryWriter* writer, const NYA_Object* object) {
    if (writer->depth >= NYA_SERDE_NYA_BINARY_DEPTH_MAX)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the object nests deeper than %d", NYA_SERDE_NYA_BINARY_DEPTH_MAX);
    NYA_TRY(_nya_serde_nya_binary_count_values(writer, object->length));

    writer->depth++;

    // sorted, so the same object always writes the same bytes whatever order its table holds the keys in.
    NYA_ConstCString* keys  = object->length > 0 ? nya_arena_alloc(writer->arena, object->length * sizeof(NYA_ConstCString)) : nullptr;
    u64               count = 0;

    nya_dict_foreach_key (object, key) {
        nya_assert(count < object->length, "the object's table holds more keys than its length says");
        keys[count++] = *key;
    }
    nya_assert(count == object->length);

    if (count > 1) qsort(keys, count, sizeof(NYA_ConstCString), _nya_serde_nya_binary_compare_keys);

    _nya_serde_nya_binary_put_uint(writer, count, _NYA_SERDE_NYA_BINARY_COUNT_BYTES);

    for (u64 i = 0; i < count; i++) {
        u64 key_length = strlen(keys[i]);
        if (key_length > NYA_SERDE_NYA_BINARY_KEY_BYTES_MAX) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "the key '%.32s...' is " FMTu64 " bytes, past the %d a key may be",
                keys[i],
                key_length,
                NYA_SERDE_NYA_BINARY_KEY_BYTES_MAX
            );
        }

        _nya_serde_nya_binary_put_uint(writer, key_length, sizeof(u8));
        _nya_serde_nya_binary_put(writer, keys[i], key_length);

        NYA_TRY(_nya_serde_nya_binary_write_value(writer, nya_object_get(object, (NYA_CString)keys[i])));
    }

    writer->depth--;
    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_write_array(_NYA_SerdeNyaBinaryWriter* writer, const NYA_ArrayᐸNYA_Valueᐳ* array) {
    if (writer->depth >= NYA_SERDE_NYA_BINARY_DEPTH_MAX)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the object nests deeper than %d", NYA_SERDE_NYA_BINARY_DEPTH_MAX);
    NYA_TRY(_nya_serde_nya_binary_count_values(writer, array->length));

    _nya_serde_nya_binary_put_uint(writer, array->length, _NYA_SERDE_NYA_BINARY_COUNT_BYTES);

    // an empty array has no elements to name a type for, so it writes none; see "one encoding".
    if (array->length == 0) return NYA_OK;

    writer->depth++;

    NYA_Type shared      = array->items[0].type;
    b8       homogeneous = shared != NYA_TYPE_NULL;
    for (u64 i = 1; i < array->length && homogeneous; i++) homogeneous = array->items[i].type == shared;

    u8 tag = _NYA_SERDE_NYA_BINARY_TAG_ANY;
    if (homogeneous && !_nya_serde_nya_binary_tag_of(shared, &tag)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an array of %s has no binary encoding", NYA_TYPE_NAME_MAP[shared]);
    }
    _nya_serde_nya_binary_put_uint(writer, tag, sizeof(u8));

    nya_array_foreach (array, element) {
        if (homogeneous) {
            NYA_TRY(_nya_serde_nya_binary_write_payload(writer, element));
        } else {
            NYA_TRY(_nya_serde_nya_binary_write_value(writer, element));
        }
    }

    writer->depth--;
    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_write_value(_NYA_SerdeNyaBinaryWriter* writer, const NYA_Value* value) {
    u8 tag = 0;
    if (!_nya_serde_nya_binary_tag_of(value->type, &tag)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a value of type %s has no binary encoding", NYA_TYPE_NAME_MAP[value->type]);
    }

    _nya_serde_nya_binary_put_uint(writer, tag, sizeof(u8));
    return _nya_serde_nya_binary_write_payload(writer, value);
}

NYA_Error _nya_serde_nya_binary_write_payload(_NYA_SerdeNyaBinaryWriter* writer, const NYA_Value* value) {
    switch (value->type) {
        case NYA_TYPE_NULL: return NYA_OK;

        // the one bit they mean, as the text form and the checksum read them: a b32 holding 2 is true.
        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128: _nya_serde_nya_binary_put_uint(writer, _nya_serde_nya_value_is_true(value) ? 1 : 0, sizeof(u8)); return NYA_OK;

        case NYA_TYPE_U8:   _nya_serde_nya_binary_put_uint(writer, value->as_u8, sizeof(u8)); return NYA_OK;
        case NYA_TYPE_U16:  _nya_serde_nya_binary_put_uint(writer, value->as_u16, sizeof(u16)); return NYA_OK;
        case NYA_TYPE_U32:  _nya_serde_nya_binary_put_uint(writer, value->as_u32, sizeof(u32)); return NYA_OK;
        case NYA_TYPE_U64:  _nya_serde_nya_binary_put_uint(writer, value->as_u64, sizeof(u64)); return NYA_OK;
        case NYA_TYPE_U128: _nya_serde_nya_binary_put_uint(writer, value->as_u128, sizeof(u128)); return NYA_OK;

        // widened through s128, so the low bytes written are the value's two's complement.
        case NYA_TYPE_S8:   _nya_serde_nya_binary_put_uint(writer, (u128)(s128)value->as_s8, sizeof(s8)); return NYA_OK;
        case NYA_TYPE_S16:  _nya_serde_nya_binary_put_uint(writer, (u128)(s128)value->as_s16, sizeof(s16)); return NYA_OK;
        case NYA_TYPE_S32:  _nya_serde_nya_binary_put_uint(writer, (u128)(s128)value->as_s32, sizeof(s32)); return NYA_OK;
        case NYA_TYPE_S64:  _nya_serde_nya_binary_put_uint(writer, (u128)(s128)value->as_s64, sizeof(s64)); return NYA_OK;
        case NYA_TYPE_S128: _nya_serde_nya_binary_put_uint(writer, (u128)value->as_s128, sizeof(s128)); return NYA_OK;

        case NYA_TYPE_F16:  {
            u16 bits = 0;
            nya_memcpy(&bits, &value->as_f16, sizeof(bits));
            _nya_serde_nya_binary_put_uint(writer, bits, sizeof(bits));
            return NYA_OK;
        }
        case NYA_TYPE_F32: {
            u32 bits = 0;
            nya_memcpy(&bits, &value->as_f32, sizeof(bits));
            _nya_serde_nya_binary_put_uint(writer, bits, sizeof(bits));
            return NYA_OK;
        }
        case NYA_TYPE_F64: {
            u64 bits = 0;
            nya_memcpy(&bits, &value->as_f64, sizeof(bits));
            _nya_serde_nya_binary_put_uint(writer, bits, sizeof(bits));
            return NYA_OK;
        }

        // raw, since x87 extended is little endian on every host that has it.
        case NYA_TYPE_F128:   _nya_serde_nya_binary_put(writer, &value->as_f128, _NYA_SERDE_NYA_BINARY_F128_BYTES); return NYA_OK;

        case NYA_TYPE_CHAR:   _nya_serde_nya_binary_put_uint(writer, (u8)value->as_char, sizeof(u8)); return NYA_OK;

        case NYA_TYPE_STRING: {
            NYA_ConstCString text   = value->as_string != nullptr ? value->as_string : "";
            u64              length = strlen(text);

            if (length > NYA_SERDE_NYA_BINARY_SIZE_MAX) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "a string of " FMTu64 " bytes is past the %llu a document may be",
                    length,
                    NYA_SERDE_NYA_BINARY_SIZE_MAX
                );
            }

            _nya_serde_nya_binary_put_uint(writer, length, _NYA_SERDE_NYA_BINARY_COUNT_BYTES);
            _nya_serde_nya_binary_put(writer, text, length);
            return NYA_OK;
        }

        case NYA_TYPE_OBJECT: return _nya_serde_nya_binary_write_object(writer, &value->as_object);
        case NYA_TYPE_ARRAY:  return _nya_serde_nya_binary_write_array(writer, &value->as_array);

        default:              return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a value of type %s has no binary encoding", NYA_TYPE_NAME_MAP[value->type]);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * READING
 * ─────────────────────────────────────────────────────────
 */

b8 _nya_serde_nya_binary_take(_NYA_SerdeNyaBinaryReader* reader, u64 count, OUT const u8** out_bytes) {
    nya_assert(reader->cursor <= reader->size);

    if (count > reader->size - reader->cursor) return false;

    *out_bytes      = reader->data + reader->cursor;
    reader->cursor += count;
    return true;
}

b8 _nya_serde_nya_binary_take_uint(_NYA_SerdeNyaBinaryReader* reader, u32 bytes, OUT u128* out_value) {
    nya_assert(bytes >= 1 && bytes <= sizeof(u128));

    const u8* data = nullptr;
    if (!_nya_serde_nya_binary_take(reader, bytes, &data)) return false;

    u128 value = 0;
    for (u32 i = 0; i < bytes; i++) value |= (u128)data[i] << (i * 8);

    *out_value = value;
    return true;
}

/** Takes `count` more values out of the document's allowance, before anything is allocated for them. */
NYA_Error _nya_serde_nya_binary_claim_values(_NYA_SerdeNyaBinaryReader* reader, u64 count) {
    if (count > NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX - reader->values) {
        return nya_error(
            NYA_ERROR_PARSE,
            "at byte " FMTu64 ": more than the %u values a document may hold",
            reader->cursor,
            NYA_SERDE_NYA_BINARY_VALUE_COUNT_MAX
        );
    }

    reader->values += count;
    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_read_header(_NYA_SerdeNyaBinaryReader* reader, const NYA_TypeReflection* type) {
    const u8* magic = nullptr;
    if (!_nya_serde_nya_binary_take(reader, NYA_SERDE_NYA_BINARY_MAGIC_BYTES, &magic) ||
        nya_memcmp(magic, NYA_SERDE_NYA_BINARY_MAGIC, NYA_SERDE_NYA_BINARY_MAGIC_BYTES) != 0) {
        return nya_error(NYA_ERROR_PARSE, "not a binary .nya document: the magic is missing");
    }

    u128 version = 0;
    u128 flags   = 0;
    u128 hash    = 0;

    if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u16), &version) || !_nya_serde_nya_binary_take_uint(reader, sizeof(u16), &flags) ||
        !_nya_serde_nya_binary_take_uint(reader, sizeof(u64), &hash)) {
        return nya_error(NYA_ERROR_PARSE, "the header is truncated: " FMTu64 " of %d bytes", reader->size, NYA_SERDE_NYA_BINARY_HEADER_BYTES);
    }
    nya_assert(reader->cursor == NYA_SERDE_NYA_BINARY_HEADER_BYTES);

    if (version != NYA_SERDE_NYA_BINARY_VERSION) {
        return nya_error(
            NYA_ERROR_PARSE,
            "unsupported binary .nya version " FMTu64 ", this build reads %d",
            (u64)version,
            NYA_SERDE_NYA_BINARY_VERSION
        );
    }

    if ((flags & ~(u128)NYA_SERDE_NYA_BINARY_FLAG_TYPED) != 0) {
        return nya_error(NYA_ERROR_PARSE, "unknown header flags 0x%04llx", (unsigned long long)(flags & ~(u128)NYA_SERDE_NYA_BINARY_FLAG_TYPED));
    }

    b8 typed = (flags & NYA_SERDE_NYA_BINARY_FLAG_TYPED) != 0;

    if (!typed && hash != 0) return nya_error(NYA_ERROR_PARSE, "an untyped document carries a layout hash (0x%016llx)", (unsigned long long)hash);

    if (typed && type == nullptr) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "the document describes a reflected type (layout 0x%016llx) and this reader expected an untyped one",
            (unsigned long long)hash
        );
    }

    if (!typed && type != nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the document is untyped and this reader expected %s", type->name);
    }

    if (typed) {
        u64 expected = nya_reflect_layout_hash(type);
        if ((u64)hash != expected) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT,
                "layout mismatch: the document was written for layout 0x%016llx, this build's %s is 0x%016llx",
                (unsigned long long)hash,
                type->name,
                (unsigned long long)expected
            );
        }
    }

    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_read_object(_NYA_SerdeNyaBinaryReader* reader, OUT NYA_Object* out_object) {
    if (reader->depth >= NYA_SERDE_NYA_BINARY_DEPTH_MAX)
        return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": nesting deeper than %d", reader->cursor, NYA_SERDE_NYA_BINARY_DEPTH_MAX);

    u64  at    = reader->cursor;
    u128 count = 0;
    if (!_nya_serde_nya_binary_take_uint(reader, _NYA_SERDE_NYA_BINARY_COUNT_BYTES, &count))
        return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": an object's member count", at);

    // every member is at least two bytes, so a count the rest of the input cannot hold is refused
    // before the table is sized from it.
    if (count > (reader->size - reader->cursor) / _NYA_SERDE_NYA_BINARY_MEMBER_BYTES_MIN) {
        return nya_error(
            NYA_ERROR_PARSE,
            "at byte " FMTu64 ": an object claims " FMTu64 " members and " FMTu64 " bytes are left",
            at,
            (u64)count,
            reader->size - reader->cursor
        );
    }
    NYA_TRY(_nya_serde_nya_binary_claim_values(reader, (u64)count));

    reader->depth++;

    // sized so that no insertion below grows the table: nya_dict_add grows past a load of three quarters.
    u64        capacity = count == 0 ? 0 : (u64)count + (u64)count / 3 + 1;
    NYA_Object object   = nya_dict_create_with_capacity_on_stack(reader->arena, NYA_Value, capacity);

    NYA_ConstCString previous = nullptr;

    for (u64 i = 0; i < (u64)count; i++) {
        u64       key_at     = reader->cursor;
        u128      key_length = 0;
        const u8* key_bytes  = nullptr;

        if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u8), &key_length) || !_nya_serde_nya_binary_take(reader, (u64)key_length, &key_bytes)) {
            return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a key", key_at);
        }
        if (memchr(key_bytes, '\0', (u64)key_length) != nullptr)
            return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": a key holds a zero byte", key_at);

        NYA_CString key = nya_arena_alloc(reader->arena, (u64)key_length + 1);
        nya_memcpy(key, key_bytes, (u64)key_length);
        key[key_length] = '\0';

        // the offset and not the key: a key is a stranger's bytes, and an error message ends up in a log.
        if (previous != nullptr) {
            s32 order = strcmp(previous, key);
            if (order == 0) return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": a key appears twice", key_at);
            if (order > 0) return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": a key is out of order after the one before it", key_at);
        }
        previous = key;

        NYA_Value value = { 0 };
        NYA_TRY(_nya_serde_nya_binary_read_value(reader, &value));

        nya_object_add(&object, key, value);
    }

    nya_assert(object.length == (u64)count, "a member was dropped or merged although every key was distinct");
    nya_assert(object.capacity == capacity, "the table grew although it was sized for every member");

    reader->depth--;

    *out_object = object;
    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_read_array(_NYA_SerdeNyaBinaryReader* reader, OUT NYA_Value* out_value) {
    if (reader->depth >= NYA_SERDE_NYA_BINARY_DEPTH_MAX)
        return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": nesting deeper than %d", reader->cursor, NYA_SERDE_NYA_BINARY_DEPTH_MAX);

    u64  at    = reader->cursor;
    u128 count = 0;
    if (!_nya_serde_nya_binary_take_uint(reader, _NYA_SERDE_NYA_BINARY_COUNT_BYTES, &count))
        return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": an array's length", at);

    if (count == 0) {
        *out_value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = nya_array_create_with_capacity_on_stack(reader->arena, NYA_Value, 0) };
        return NYA_OK;
    }

    u128 tag = 0;
    if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u8), &tag))
        return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": an array's element tag", reader->cursor);

    b8 any = tag == _NYA_SERDE_NYA_BINARY_TAG_ANY;
    if (!any && (tag >= _NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT || tag == _NYA_SERDE_NYA_BINARY_TAG_NULL)) {
        return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": 0x%02x is not an element tag", reader->cursor - 1, (u32)tag);
    }

    // every element is at least one byte, its tag or its payload, which is what null not being an
    // element type buys. So this refuses a length the input cannot back before sizing anything.
    if (count > reader->size - reader->cursor) {
        return nya_error(
            NYA_ERROR_PARSE,
            "at byte " FMTu64 ": an array claims " FMTu64 " elements and " FMTu64 " bytes are left",
            at,
            (u64)count,
            reader->size - reader->cursor
        );
    }
    NYA_TRY(_nya_serde_nya_binary_claim_values(reader, (u64)count));

    reader->depth++;

    NYA_ArrayᐸNYA_Valueᐳ elements = nya_array_create_with_capacity_on_stack(reader->arena, NYA_Value, (u64)count);

    for (u64 i = 0; i < (u64)count; i++) {
        NYA_Value element = { 0 };

        if (any) {
            NYA_TRY(_nya_serde_nya_binary_read_value(reader, &element));
        } else {
            NYA_TRY(_nya_serde_nya_binary_read_payload(reader, (u8)tag, &element));
        }

        nya_array_push_back(&elements, element);
    }

    nya_assert(elements.length == (u64)count);
    nya_assert(elements.capacity == (u64)count, "the array grew although it was sized for every element");

    if (any) {
        NYA_Type shared = elements.items[0].type;
        b8       same   = true;
        for (u64 i = 1; i < elements.length && same; i++) same = elements.items[i].type == shared;

        if (same && shared != NYA_TYPE_NULL) {
            return nya_error(
                NYA_ERROR_PARSE,
                "at byte " FMTu64 ": an 'any' array whose elements are all %s; it is written with that tag",
                at,
                NYA_TYPE_NAME_MAP[shared]
            );
        }
    }

    reader->depth--;

    *out_value = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = elements };
    return NYA_OK;
}

NYA_Error _nya_serde_nya_binary_read_value(_NYA_SerdeNyaBinaryReader* reader, OUT NYA_Value* out_value) {
    u64  at  = reader->cursor;
    u128 tag = 0;

    if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u8), &tag))
        return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a value's tag", at);
    if (tag >= _NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT) return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": unknown tag 0x%02x", at, (u32)tag);

    return _nya_serde_nya_binary_read_payload(reader, (u8)tag, out_value);
}

NYA_Error _nya_serde_nya_binary_read_payload(_NYA_SerdeNyaBinaryReader* reader, u8 tag, OUT NYA_Value* out_value) {
    nya_assert(tag < _NYA_SERDE_NYA_BINARY_TAG_VALUE_COUNT, "the caller checks the tag against the table");

    NYA_Type type = _NYA_SERDE_NYA_BINARY_TYPE_OF_TAG[tag];
    u64      at   = reader->cursor;

    // the widest member, zeroed, so a narrower payload written below leaves nothing stale beside it.
    NYA_Value value = { .type = type };
    nya_memset(&value.as_u128, 0, sizeof(value.as_u128));

    u128 raw = 0;

    switch (type) {
        case NYA_TYPE_NULL: break;

        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64:
        case NYA_TYPE_B128: {
            if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u8), &raw))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a %s", at, NYA_TYPE_NAME_MAP[type]);
            if (raw > 1)
                return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": a %s of %u, which is neither 0 nor 1", at, NYA_TYPE_NAME_MAP[type], (u32)raw);

            _nya_serde_nya_set_boolean(&value, raw == 1);
        } break;

        case NYA_TYPE_U8:
        case NYA_TYPE_S8:
        case NYA_TYPE_CHAR: {
            if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u8), &raw))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a %s", at, NYA_TYPE_NAME_MAP[type]);

            if (type == NYA_TYPE_U8) value.as_u8 = (u8)raw;
            if (type == NYA_TYPE_S8) value.as_s8 = (s8)(u8)raw;
            if (type == NYA_TYPE_CHAR) value.as_char = (char)(u8)raw;
        } break;

        case NYA_TYPE_U16:
        case NYA_TYPE_S16:
        case NYA_TYPE_F16: {
            if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u16), &raw))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a %s", at, NYA_TYPE_NAME_MAP[type]);

            u16 bits = (u16)raw;
            if (type == NYA_TYPE_U16) value.as_u16 = bits;
            if (type == NYA_TYPE_S16) value.as_s16 = (s16)bits;
            if (type == NYA_TYPE_F16) nya_memcpy(&value.as_f16, &bits, sizeof(bits));
        } break;

        case NYA_TYPE_U32:
        case NYA_TYPE_S32:
        case NYA_TYPE_F32: {
            if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u32), &raw))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a %s", at, NYA_TYPE_NAME_MAP[type]);

            u32 bits = (u32)raw;
            if (type == NYA_TYPE_U32) value.as_u32 = bits;
            if (type == NYA_TYPE_S32) value.as_s32 = (s32)bits;
            if (type == NYA_TYPE_F32) nya_memcpy(&value.as_f32, &bits, sizeof(bits));
        } break;

        case NYA_TYPE_U64:
        case NYA_TYPE_S64:
        case NYA_TYPE_F64: {
            if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u64), &raw))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a %s", at, NYA_TYPE_NAME_MAP[type]);

            u64 bits = (u64)raw;
            if (type == NYA_TYPE_U64) value.as_u64 = bits;
            if (type == NYA_TYPE_S64) value.as_s64 = (s64)bits;
            if (type == NYA_TYPE_F64) nya_memcpy(&value.as_f64, &bits, sizeof(bits));
        } break;

        case NYA_TYPE_U128:
        case NYA_TYPE_S128: {
            if (!_nya_serde_nya_binary_take_uint(reader, sizeof(u128), &raw))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a %s", at, NYA_TYPE_NAME_MAP[type]);

            if (type == NYA_TYPE_U128) value.as_u128 = raw;
            if (type == NYA_TYPE_S128) value.as_s128 = (s128)raw;
        } break;

        case NYA_TYPE_F128: {
            const u8* bytes = nullptr;
            if (!_nya_serde_nya_binary_take(reader, _NYA_SERDE_NYA_BINARY_F128_BYTES, &bytes))
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": an f128", at);

            nya_memcpy(&value.as_f128, bytes, _NYA_SERDE_NYA_BINARY_F128_BYTES);
        } break;

        case NYA_TYPE_STRING: {
            u128      length = 0;
            const u8* bytes  = nullptr;

            // checked against what is left by the take itself, before the copy is allocated.
            if (!_nya_serde_nya_binary_take_uint(reader, _NYA_SERDE_NYA_BINARY_COUNT_BYTES, &length) ||
                !_nya_serde_nya_binary_take(reader, (u64)length, &bytes)) {
                return nya_error(NYA_ERROR_PARSE, "truncated at byte " FMTu64 ": a string", at);
            }
            if (memchr(bytes, '\0', (u64)length) != nullptr) return nya_error(NYA_ERROR_PARSE, "at byte " FMTu64 ": a string holds a zero byte", at);

            NYA_CString text = nya_arena_alloc(reader->arena, (u64)length + 1);
            nya_memcpy(text, bytes, (u64)length);
            text[length] = '\0';

            value.as_string = text;
        } break;

        case NYA_TYPE_OBJECT: NYA_TRY(_nya_serde_nya_binary_read_object(reader, &value.as_object)); break;

        case NYA_TYPE_ARRAY:  NYA_TRY(_nya_serde_nya_binary_read_array(reader, &value)); break;

        default:              nya_unreachable();
    }

    nya_assert(value.type == type);

    *out_value = value;
    return NYA_OK;
}
