#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_SaveSystem* _nya_save_system(void);

/**
 * Whether `relative` stays inside the save root.
 *
 * Rejected rather than normalised. A `..` in a save path is either a bug or a name that came from
 * outside the program, and the useful response to both is the same: refuse it. Normalising instead
 * would mean the API silently supports writing wherever the caller pointed, which is not a feature
 * anything asked for and is one that Steam Auto-Cloud would then silently not sync.
 * */
NYA_INTERNAL b8 _nya_save_relative_is_safe(NYA_ConstCString relative);

/** Creates the directories `absolute`'s parent needs. Succeeds when they already exist. */
NYA_INTERNAL NYA_Error _nya_save_ensure_parent(NYA_Arena* arena, NYA_ConstCString absolute);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_system_save_init(void) {
    NYA_SaveSystem* system = _nya_save_system();

    *system = (NYA_SaveSystem){ 0 };

    // Its own arena rather than the frame allocator: the root string is read for the lifetime of the
    // process, and the frame allocator is emptied sixty times a second.
    system->allocator = nya_arena_create();

    NYA_String* root  = nullptr;
    NYA_Error   error = nya_filesystem_user_data_directory(system->allocator, NYA_SAVE_APPLICATION, &root);

    if (!error.ok) {
        // Warned and returned rather than thrown. A machine with no writable home directory can
        // still be played on; it just cannot save, and taking the whole application down over that
        // is a worse answer than running without saves.
        u8 message[256];
        (void)nya_error_format(&error, message, sizeof(message));
        nya_warn("No user data directory is available, so nothing can be saved: %s", (NYA_CString)message);

        return error;
    }

    NYA_CString root_cstring = nya_string_to_cstring(system->allocator, root);

    error = nya_filesystem_create_directory(root_cstring);
    if (!error.ok) {
        u8 message[256];
        (void)nya_error_format(&error, message, sizeof(message));
        nya_warn("Could not create the save directory '%s', so nothing can be saved: %s", root_cstring, (NYA_CString)message);

        return error;
    }

    system->root = root_cstring;

    nya_info("Save system initialized at '%s'.", system->root);

    return NYA_OK;
}

void nya_system_save_deinit(void) {
    NYA_SaveSystem* system = _nya_save_system();

    if (system->allocator != nullptr) nya_arena_destroy(system->allocator);

    *system = (NYA_SaveSystem){ 0 };

    nya_info("Save system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * PATHS
 * ─────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_save_root(void) {
    return _nya_save_system()->root;
}

NYA_String* nya_save_path(NYA_Arena* arena, NYA_ConstCString relative) {
    nya_assert(arena != nullptr);
    nya_assert(relative != nullptr);

    NYA_ConstCString root = nya_save_root();
    if (root == nullptr) return nullptr;

    if (!_nya_save_relative_is_safe(relative)) {
        nya_log_error("Save path '%s' leaves the save root; use a plain relative path.", relative);
        return nullptr;
    }

    return nya_path_join(arena, root, relative);
}

/*
 * ─────────────────────────────────────────────────────────
 * OBJECTS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_save_write(NYA_ConstCString relative, const NYA_Object* object, NYA_SerdeFlags flags) {
    nya_assert(object != nullptr);

    NYA_Arena* scratch = nya_arena_create();
    defer nya_arena_destroy(scratch);

    NYA_String* path = nya_save_path(scratch, relative);
    if (path == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no writable save directory");

    NYA_CString path_cstring = nya_string_to_cstring(scratch, path);

    NYA_TRY(_nya_save_ensure_parent(scratch, path_cstring));

    /*
     * Beside the target, not in the temp directory.
     *
     * A rename is only atomic within one filesystem, and $TMPDIR is routinely a different one — on
     * Linux it is frequently a tmpfs. Renaming across that boundary is a copy and a delete, which is
     * exactly the non-atomic write this exists to avoid, and it fails outright on some systems.
     */
    NYA_String* temporary_path = nya_string_sprintf(scratch, "%s.tmp", path_cstring);
    NYA_CString temporary      = nya_string_to_cstring(scratch, temporary_path);

    NYA_TRY(nya_serde_save_file(object, temporary, flags));

    NYA_Error moved = nya_filesystem_move(temporary, path_cstring);
    if (!moved.ok) {
        // The half-written file is the whole problem this function exists to prevent, so it does not
        // get to survive a failed rename and be mistaken for a save next time.
        (void)nya_filesystem_delete(temporary);
        return moved;
    }

    return NYA_OK;
}

NYA_Error nya_save_read(NYA_Arena* arena, NYA_ConstCString relative, NYA_SerdeFlags flags, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    *out_object = nullptr;

    NYA_String* path = nya_save_path(arena, relative);
    if (path == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no writable save directory");

    NYA_CString path_cstring = nya_string_to_cstring(arena, path);

    // Distinguished from a parse failure on purpose: "there is no save yet" is the ordinary first
    // run and wants defaults, while "there is a save and it is broken" wants to be noticed.
    if (!nya_filesystem_is_file(path_cstring)) return nya_error(NYA_ERROR_NOT_FOUND, "save file does not exist");

    return nya_serde_load_file(arena, path_cstring, flags, out_object);
}

b8 nya_save_exists(NYA_ConstCString relative) {
    NYA_Arena* scratch = nya_arena_create();
    defer nya_arena_destroy(scratch);

    NYA_String* path = nya_save_path(scratch, relative);
    if (path == nullptr) return false;

    return nya_filesystem_is_file(nya_string_to_cstring(scratch, path));
}

NYA_Error nya_save_delete(NYA_ConstCString relative) {
    NYA_Arena* scratch = nya_arena_create();
    defer nya_arena_destroy(scratch);

    NYA_String* path = nya_save_path(scratch, relative);
    if (path == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no writable save directory");

    NYA_CString path_cstring = nya_string_to_cstring(scratch, path);

    // Deleting what is not there is what a caller means by "make sure this slot is empty", so it is
    // not an error to say so.
    if (!nya_filesystem_exists(path_cstring)) return NYA_OK;

    return nya_filesystem_delete(path_cstring);
}

/*
 * ─────────────────────────────────────────────────────────
 * DATABASES
 * ─────────────────────────────────────────────────────────
 */

#ifdef NYA_PLUGIN_SQLITE

NYA_Error nya_save_database_open(NYA_Arena* arena, NYA_ConstCString relative, OUT NYA_Database** out_database) {
    nya_assert(arena != nullptr);
    nya_assert(out_database != nullptr);

    *out_database = nullptr;

    NYA_String* path = nya_save_path(arena, relative);
    if (path == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no writable save directory");

    NYA_CString path_cstring = nya_string_to_cstring(arena, path);

    // SQLite creates the file but not the directories above it, and answers "unable to open database
    // file" either way — which is a genuinely unhelpful message to debug a missing subdirectory from.
    NYA_TRY(_nya_save_ensure_parent(arena, path_cstring));

    return nya_sql_open(arena, path_cstring, out_database);
}

#endif // NYA_PLUGIN_SQLITE

/*
 * ─────────────────────────────────────────────────────────
 * VERSIONING
 * ─────────────────────────────────────────────────────────
 */

u32 nya_save_version(const NYA_Object* object) {
    if (object == nullptr) return 0;

    NYA_Value* value = nya_object_get(object, NYA_SAVE_VERSION_KEY);
    if (value == nullptr) return 0;

    /*
     * Both widths, because JSON has one number type and reads every integer back as an S64.
     *
     * A version written as a u32 into a `.json` save comes back as something else entirely, and a
     * loader that only accepted the width it wrote would treat every JSON save as unversioned — then
     * refuse to migrate it, or migrate it twice.
     */
    switch (value->type) {
        case NYA_TYPE_U32: return value->as_u32;
        case NYA_TYPE_S64: return value->as_s64 > 0 ? (u32)value->as_s64 : 0;
        case NYA_TYPE_U64: return (u32)value->as_u64;
        default:           return 0;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_SaveSystem* _nya_save_system(void) {
    return &nya_app_get()->save_system;
}

b8 _nya_save_relative_is_safe(NYA_ConstCString relative) {
    if (relative[0] == '\0') return false;

    // An absolute path is not relative to anything, which is the whole contract of this argument.
    if (nya_path_is_absolute(relative)) return false;

    // Both separators, because a Windows path reaches this code on Linux too — a save name that came
    // out of a file written on the other platform, for instance.
    if (relative[0] == '/' || relative[0] == '\\') return false;

    for (const char* cursor = relative; *cursor != '\0'; cursor++) {
        if (cursor[0] != '.' || cursor[1] != '.') continue;

        // Only a whole `..` segment escapes. A file honestly called `..config` or `save..nya` does
        // not, and refusing those would be a rule nobody could predict.
        b8 at_segment_start = cursor == relative || cursor[-1] == '/' || cursor[-1] == '\\';
        b8 at_segment_end   = cursor[2] == '\0' || cursor[2] == '/' || cursor[2] == '\\';

        if (at_segment_start && at_segment_end) return false;
    }

    return true;
}

NYA_Error _nya_save_ensure_parent(NYA_Arena* arena, NYA_ConstCString absolute) {
    NYA_String* parent = nya_path_dirname(arena, absolute);
    if (parent == nullptr || parent->length == 0) return NYA_OK;

    return nya_filesystem_create_directory(nya_string_to_cstring(arena, parent));
}
