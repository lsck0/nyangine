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
        case NYA_SERDE_FORMAT_NYA:   return nya_serde_nya_serialize(arena, object, flags);
        case NYA_SERDE_FORMAT_JSON:  return nya_serde_json_serialize(arena, object, flags);
        case NYA_SERDE_FORMAT_JSONC: return nya_serde_jsonc_serialize(arena, object, flags);

        default:                     nya_panic("Unknown serialization format %d.", (int)format);
    }
    static_assert(NYA_SERDE_FORMAT_COUNT == 3, "Unhandled NYA_SerdeFormat value.");
}

NYA_Error nya_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFormat format, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    switch (format) {
        case NYA_SERDE_FORMAT_NYA:   return nya_serde_nya_deserialize(arena, data, size, flags, out_object);
        case NYA_SERDE_FORMAT_JSON:  return nya_serde_json_deserialize(arena, data, size, flags, out_object);
        case NYA_SERDE_FORMAT_JSONC: return nya_serde_jsonc_deserialize(arena, data, size, flags, out_object);

        default:                     nya_panic("Unknown serialization format %d.", (int)format);
    }
    static_assert(NYA_SERDE_FORMAT_COUNT == 3, "Unhandled NYA_SerdeFormat value.");
}

NYA_SerdeFormat nya_serde_detect_format(const u8* data, u64 size) {
    if (data == nullptr || size == 0) return NYA_SERDE_FORMAT_COUNT;

    // An obfuscated nya document is identified by its leading magic byte, which is not valid at the
    // start of any text format.
    if (data[0] == 0xA7) return NYA_SERDE_FORMAT_NYA;

    u64 cursor = 0;
    while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t' || data[cursor] == '\n' || data[cursor] == '\r')) cursor++;

    if (cursor >= size) return NYA_SERDE_FORMAT_COUNT;

    /*
     * A leading comment can only be JSONC: strict JSON has nowhere to put one.
     *
     * The reverse is not decidable from the first bytes. A document opening with '{' is reported as
     * JSON even if a comment appears three lines down, because scanning the whole input to find out
     * would make detection cost as much as parsing. A caller reading a file a person may have edited
     * should ask for NYA_SERDE_FORMAT_JSONC outright rather than detect — JSONC reads everything
     * JSON does, so nothing is given up by doing so.
     */
    if (data[cursor] == '/' && cursor + 1 < size && (data[cursor + 1] == '/' || data[cursor + 1] == '*')) return NYA_SERDE_FORMAT_JSONC;

    if (data[cursor] == '{' || data[cursor] == '[') return NYA_SERDE_FORMAT_JSON;

    u64 magic_length = strlen(NYA_SERDE_NYA_MAGIC);
    if (size - cursor >= magic_length && strncmp((const char*)data + cursor, NYA_SERDE_NYA_MAGIC, magic_length) == 0) return NYA_SERDE_FORMAT_NYA;

    return NYA_SERDE_FORMAT_COUNT;
}

/*
 * ─────────────────────────────────────────────────────────
 * FILES
 * ─────────────────────────────────────────────────────────
 */

/** True when the path ends in .json, which is the one extension that is not the native format. */
NYA_INTERNAL b8 _nya_serde_path_is_json(NYA_ConstCString path) {
    u64 length = strlen(path);
    if (length < 5) return false;

    return strcmp(path + length - 5, ".json") == 0;
}

NYA_Error nya_serde_save_file(const NYA_Object* object, NYA_ConstCString path, NYA_SerdeFlags flags) {
    nya_assert(object != nullptr);
    nya_assert(path != nullptr);

    // A scratch arena for the text, which does not outlive the write. The caller should not have to
    // supply somewhere to put something they never see.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "serde_save_file");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_SerdeFormat format = _nya_serde_path_is_json(path) ? NYA_SERDE_FORMAT_JSON : NYA_SERDE_FORMAT_NYA;

    NYA_String* text = nya_serialize(&scratch, object, format, flags);
    if (text == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not serialize the object for '%s'", path);

    /*
     * The length carrying overload, not the cstring one.
     *
     * An obfuscated nya document is base64 XORed against a repeating key, so a zero byte appears
     * wherever the key happens to match the encoded character — and nya_serde_detect_format keys off
     * the leading 0xA7 precisely because that output is binary rather than text. Converting to a
     * cstring made nya_file_write measure it with strlen, which truncated the file at the first such
     * byte and produced a save that would not load back.
     */
    return nya_file_write(path, text);
}

NYA_Error nya_serde_load_file(NYA_Arena* arena, NYA_ConstCString path, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_object != nullptr);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "serde_load_file");
    defer     nya_arena_destroy_on_stack(&scratch);

    // Created against the arena rather than zero initialised: NYA_String carries the arena it grows
    // in, and a zeroed one reallocs against a null arena the moment the file is longer than nothing.
    NYA_String contents = nya_string_create_on_stack(&scratch);
    defer      nya_string_destroy_on_stack(&contents);

    NYA_TRY(nya_file_read(path, &contents));

    NYA_SerdeFormat format = nya_serde_detect_format(contents.items, contents.length);
    if (format == NYA_SERDE_FORMAT_COUNT) return nya_error(NYA_ERROR_CORRUPT, "'%s' is neither the native format nor JSON", path);

    // Into the caller's arena, not the scratch one — the tree is what survives this call.
    return nya_deserialize(arena, contents.items, contents.length, format, flags, out_object);
}
