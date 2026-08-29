#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What _nya_pp_collect_newest accumulates into while the walk runs. */
typedef struct {
    u64              newest;
    NYA_ConstCString extension;
} _NYA_PPNewest;

NYA_INTERNAL b8 _nya_pp_collect_newest(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL b8 _nya_pp_has_extension(NYA_ConstCString path, NYA_ConstCString extension) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u64 nya_pp_newest(NYA_ConstCString* paths, NYA_ConstCString extension) {
    nya_assert(paths != nullptr);

    NYA_Arena* arena = nya_arena_create(.region_size = nya_mebyte_to_byte(1UL));
    defer      nya_arena_destroy(arena);

    _NYA_PPNewest state = { .extension = extension };

    for (u64 i = 0; paths[i] != nullptr; i++) {
        NYA_FileInfo info = { 0 };
        if (!nya_filesystem_info(paths[i], &info).ok) continue;

        if (info.type != NYA_FILE_TYPE_DIRECTORY) {
            if (!_nya_pp_has_extension(paths[i], extension)) continue;
            if (info.modified_at > state.newest) state.newest = info.modified_at;
            continue;
        }

        // The root's own mtime, which the walk never reports: it only ever sees a directory as
        // somebody's child, and the top of the tree is nobody's child.
        if (info.modified_at > state.newest) state.newest = info.modified_at;

        NYA_EXPECT(nya_filesystem_walk(arena, paths[i], _nya_pp_collect_newest, &state), "while timestamping '%s'", paths[i]);
    }

    return state.newest;
}

b8 nya_pp_is_current(NYA_ConstCString pass, NYA_ConstCString* inputs, NYA_ConstCString* outputs) {
    nya_assert(pass != nullptr);
    nya_assert(inputs != nullptr);
    nya_assert(outputs != nullptr);

    if (regenerate_flag.value.as_b8) return false;

    u64 oldest_output = UINT64_MAX;
    for (u64 i = 0; outputs[i] != nullptr; i++) {
        u64 modified_at = 0;
        if (!nya_filesystem_last_modified(outputs[i], &modified_at).ok) return false;
        if (modified_at < oldest_output) oldest_output = modified_at;
    }

    // No outputs at all is a caller bug, not a pass with nothing to write.
    nya_assert(oldest_output != UINT64_MAX, "nya_pp_is_current('%s') was given no outputs.", pass);

    u64 newest_input = nya_pp_newest(inputs, nullptr);
    if (newest_input >= oldest_output) return false;

    nya_log_trace("%s is up to date, skipping.", pass);
    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_pp_collect_newest(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    _NYA_PPNewest* state = (_NYA_PPNewest*)user_data;

    // A directory always counts, whatever the filter says. Its mtime is the only record that a file
    // matching the filter used to be there and no longer is.
    if (entry->type != NYA_FILE_TYPE_DIRECTORY && !_nya_pp_has_extension(path, state->extension)) return true;

    if (entry->modified_at > state->newest) state->newest = entry->modified_at;
    return true;
}

NYA_INTERNAL b8 _nya_pp_has_extension(NYA_ConstCString path, NYA_ConstCString extension) {
    if (extension == nullptr) return true;

    u64 path_length      = strlen(path);
    u64 extension_length = strlen(extension);
    if (extension_length > path_length) return false;

    return strcmp(path + path_length - extension_length, extension) == 0;
}
