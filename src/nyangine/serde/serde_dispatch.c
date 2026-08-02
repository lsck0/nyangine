#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_String* nya_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFormat format, NYA_SerdeFlags flags) {
    nya_assert(arena != nullptr);
    nya_assert(object != nullptr);

    switch (format) {
        case NYA_SERDE_FORMAT_NYA:  return nya_serde_nya_serialize(arena, object, flags);
        case NYA_SERDE_FORMAT_JSON: return nya_serde_json_serialize(arena, object, flags);

        default:                    nya_panic("Unknown serialization format %d.", (int)format);
    }
    static_assert(NYA_SERDE_FORMAT_COUNT == 2, "Unhandled NYA_SerdeFormat value.");
}

NYA_Error nya_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFormat format, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    switch (format) {
        case NYA_SERDE_FORMAT_NYA:  return nya_serde_nya_deserialize(arena, data, size, flags, out_object);
        case NYA_SERDE_FORMAT_JSON: return nya_serde_json_deserialize(arena, data, size, flags, out_object);

        default:                    nya_panic("Unknown serialization format %d.", (int)format);
    }
    static_assert(NYA_SERDE_FORMAT_COUNT == 2, "Unhandled NYA_SerdeFormat value.");
}

NYA_SerdeFormat nya_serde_detect_format(const u8* data, u64 size) {
    if (data == nullptr || size == 0) return NYA_SERDE_FORMAT_COUNT;

    // An obfuscated nya document is identified by its leading magic byte, which is not valid at the
    // start of any text format.
    if (data[0] == 0xA7) return NYA_SERDE_FORMAT_NYA;

    u64 cursor = 0;
    while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t' || data[cursor] == '\n' || data[cursor] == '\r')) cursor++;

    if (cursor >= size) return NYA_SERDE_FORMAT_COUNT;

    if (data[cursor] == '{' || data[cursor] == '[') return NYA_SERDE_FORMAT_JSON;

    u64 magic_length = strlen(NYA_SERDE_NYA_MAGIC);
    if (size - cursor >= magic_length && strncmp((const char*)data + cursor, NYA_SERDE_NYA_MAGIC, magic_length) == 0) return NYA_SERDE_FORMAT_NYA;

    return NYA_SERDE_FORMAT_COUNT;
}
