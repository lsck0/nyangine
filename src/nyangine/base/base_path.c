#include "nyangine/base/base_basic.h"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_path_is_separator(char c);

/** Index just past the last separator, so 0 when there is none. */
NYA_INTERNAL u64 _nya_path_basename_start(NYA_ConstCString path, u64 length);

/**
 * Index of the extension dot, or `length` when there is none.
 *
 * A dot that starts the basename is a hidden file rather than an extension, which is why this
 * searches from the basename rather than the whole path.
 * */
NYA_INTERNAL u64 _nya_path_extension_start(NYA_ConstCString path, u64 length);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_String* nya_path_join(NYA_Arena* arena, NYA_ConstCString head, NYA_ConstCString tail) {
    nya_assert(arena != nullptr);
    nya_assert(head != nullptr);
    nya_assert(tail != nullptr);

    // An absolute tail wins outright, rather than being appended to produce nonsense.
    if (nya_path_is_absolute(tail)) return nya_path_normalize(arena, tail);

    u64 head_length = strlen(head);
    while (head_length > 0 && _nya_path_is_separator(head[head_length - 1])) head_length--;

    while (*tail != '\0' && _nya_path_is_separator(*tail)) tail++;

    if (head_length == 0) return nya_path_normalize(arena, tail);
    if (*tail == '\0') return nya_path_normalize(arena, head);

    NYA_String* joined = nya_string_sprintf(arena, "%.*s%c%s", (int)head_length, head, NYA_PATH_SEPARATOR, tail);
    return nya_path_normalize(arena, nya_string_to_cstring(arena, joined));
}

NYA_String* nya_path_basename(NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);

    u64 length = strlen(path);

    // A trailing separator is not part of the name: "a/b/" names "b".
    while (length > 1 && _nya_path_is_separator(path[length - 1])) length--;

    u64 start = _nya_path_basename_start(path, length);
    return nya_string_sprintf(arena, "%.*s", (int)(length - start), path + start);
}

NYA_String* nya_path_dirname(NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);

    u64 length = strlen(path);
    while (length > 1 && _nya_path_is_separator(path[length - 1])) length--;

    u64 start = _nya_path_basename_start(path, length);
    if (start == 0) return nya_string_sprintf(arena, ".");

    // Keep the root's separator, so dirname("/a") is "/" rather than "".
    u64 end = start - 1;
    if (end == 0) return nya_string_sprintf(arena, "%c", NYA_PATH_SEPARATOR);

    return nya_string_sprintf(arena, "%.*s", (int)end, path);
}

NYA_String* nya_path_extension(NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);

    u64 length = strlen(path);
    u64 dot    = _nya_path_extension_start(path, length);
    if (dot >= length) return nya_string_create(arena);

    return nya_string_sprintf(arena, "%.*s", (int)(length - dot), path + dot);
}

NYA_String* nya_path_stem(NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);

    u64 length = strlen(path);
    while (length > 1 && _nya_path_is_separator(path[length - 1])) length--;

    u64 start = _nya_path_basename_start(path, length);
    u64 dot   = _nya_path_extension_start(path, length);
    if (dot > length) dot = length;

    return nya_string_sprintf(arena, "%.*s", (int)(dot - start), path + start);
}

NYA_String* nya_path_with_extension(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString extension) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(extension != nullptr);

    u64 length = strlen(path);
    u64 dot    = _nya_path_extension_start(path, length);
    if (dot > length) dot = length;

    NYA_ConstCString dot_prefix = (extension[0] == '.' || extension[0] == '\0') ? "" : ".";
    return nya_string_sprintf(arena, "%.*s%s%s", (int)dot, path, dot_prefix, extension);
}

b8 nya_path_is_absolute(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    if (path[0] == '\0') return false;
    if (_nya_path_is_separator(path[0])) return true;

    // "C:\..." and "C:/...". A bare "C:" is relative to that drive's working directory, so it does
    // not count as absolute.
    if (isalpha((unsigned char)path[0]) && path[1] == ':' && _nya_path_is_separator(path[2])) return true;

    return false;
}

NYA_String* nya_path_normalize(NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);

    u64 length = strlen(path);

    /*
     * Sized to the segment table below rather than left at the default, which is a gibibyte.
     *
     * A region is malloc'd whole and then, under ASan, poisoned whole — a gibibyte of region is a
     * hundred and twenty eight mebibytes of shadow writes, about sixteen milliseconds, to back a
     * table that never needs more than a few kibibytes. Nothing here notices except by being slow,
     * and this is called twice per directory entry by nya_filesystem_walk, which is how indexing
     * fourteen hundred assets came to take forty seconds of a sanitized build.
     *
     * Undersizing is safe: the arena chains another region. Only oversizing costs anything.
     * */
    u64 table_size   = (length + 1) * (sizeof(NYA_ConstCString) + sizeof(u64));
    u64 scratch_size = nya_max(nya_kibyte_to_byte(4UL), table_size + nya_kibyte_to_byte(1UL));

    NYA_Arena* scratch = nya_arena_create(.region_size = scratch_size);
    defer      nya_arena_destroy(scratch);

    // Segment table, so ".." can drop the previous entry without rescanning the output.
    NYA_ConstCString* segment_starts  = nya_arena_alloc(scratch, (length + 1) * sizeof(NYA_ConstCString));
    u64*              segment_lengths = nya_arena_alloc(scratch, (length + 1) * sizeof(u64));
    u64               segment_count   = 0;

    b8 absolute = nya_path_is_absolute(path);

    // A drive prefix is carried through untouched; it is not a segment that ".." may remove.
    u64 cursor = 0;
    u64 prefix = 0;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') {
        prefix = 2;
        cursor = 2;
    }

    while (cursor < length) {
        while (cursor < length && _nya_path_is_separator(path[cursor])) cursor++;

        u64 start = cursor;
        while (cursor < length && !_nya_path_is_separator(path[cursor])) cursor++;

        u64 segment_length = cursor - start;
        if (segment_length == 0) continue;
        if (segment_length == 1 && path[start] == '.') continue;

        b8 is_parent = segment_length == 2 && path[start] == '.' && path[start + 1] == '.';
        if (is_parent && segment_count > 0) {
            b8 previous_is_parent =
                segment_lengths[segment_count - 1] == 2 && segment_starts[segment_count - 1][0] == '.' && segment_starts[segment_count - 1][1] == '.';
            if (!previous_is_parent) {
                segment_count--;
                continue;
            }
        }
        // A leading ".." on an absolute path has nothing above it to remove, so it is dropped.
        if (is_parent && segment_count == 0 && absolute) continue;

        segment_starts[segment_count]  = path + start;
        segment_lengths[segment_count] = segment_length;
        segment_count++;
    }

    NYA_String* result = nya_string_create(arena);

    if (prefix > 0) nya_string_extend_sprintf(result, "%.*s", (int)prefix, path);
    if (absolute) nya_string_extend_sprintf(result, "%c", NYA_PATH_SEPARATOR);

    for (u64 i = 0; i < segment_count; i++) {
        if (i > 0) nya_string_extend_sprintf(result, "%c", NYA_PATH_SEPARATOR);
        nya_string_extend_sprintf(result, "%.*s", (int)segment_lengths[i], segment_starts[i]);
    }

    // Everything cancelled out, which is the current directory rather than nothing.
    if (result->length == 0) nya_string_extend(result, ".");

    return result;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_path_is_separator(char c) {
    // Backslash counts everywhere, not just on Windows: paths handed back by Windows APIs get
    // parsed on whatever host is reading them.
    return c == '/' || c == '\\';
}

NYA_INTERNAL u64 _nya_path_basename_start(NYA_ConstCString path, u64 length) {
    for (u64 i = length; i > 0; i--) {
        if (_nya_path_is_separator(path[i - 1])) return i;
    }

    return 0;
}

NYA_INTERNAL u64 _nya_path_extension_start(NYA_ConstCString path, u64 length) {
    u64 start = _nya_path_basename_start(path, length);

    for (u64 i = length; i > start + 1; i--) {
        if (path[i - 1] == '.') return i - 1;
    }

    return length;
}
