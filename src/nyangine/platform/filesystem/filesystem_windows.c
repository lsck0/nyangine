#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Win32 counts 100ns ticks from 1601; the rest of the engine uses unix seconds. */
NYA_INTERNAL u64 _nya_filesystem_time_from_filetime(FILETIME time) {
    ULARGE_INTEGER ticks;
    ticks.LowPart  = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;

    if (ticks.QuadPart == 0) return 0;

    return (ticks.QuadPart / 10000000ULL) - 11644473600ULL;
}

NYA_INTERNAL NYA_FileType _nya_filesystem_type_from_attributes(DWORD attributes) {
    if (attributes == INVALID_FILE_ATTRIBUTES) return NYA_FILE_TYPE_UNKNOWN;

    // A reparse point is checked first: a directory symlink carries both bits, and reporting it as a
    // plain directory is what makes a tree walk follow links into a loop.
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return NYA_FILE_TYPE_SYMLINK;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return NYA_FILE_TYPE_DIRECTORY;

    return NYA_FILE_TYPE_FILE;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

b8 nya_filesystem_exists(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

b8 nya_filesystem_is_file(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;

    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

b8 nya_filesystem_is_directory(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;

    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

NYA_Error nya_filesystem_info(NYA_ConstCString path, OUT NYA_FileInfo* out_info) {
    nya_assert(path != nullptr);
    nya_assert(out_info != nullptr);

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return nya_error(NYA_ERROR_NOT_FOUND, "failed to stat '%s'", path);

    ULARGE_INTEGER size;
    size.LowPart  = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;

    *out_info = (NYA_FileInfo){
        .type        = _nya_filesystem_type_from_attributes(data.dwFileAttributes),
        .size        = (u64)size.QuadPart,
        .modified_at = _nya_filesystem_time_from_filetime(data.ftLastWriteTime),
        .created_at  = _nya_filesystem_time_from_filetime(data.ftCreationTime),
        .accessed_at = _nya_filesystem_time_from_filetime(data.ftLastAccessTime),
        .readonly    = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0,
    };

    return NYA_OK;
}

NYA_Error nya_filesystem_size(NYA_ConstCString path, OUT u64* out_size) {
    nya_assert(out_size != nullptr);

    NYA_FileInfo info;
    NYA_TRY(nya_filesystem_info(path, &info));
    *out_size = info.size;

    return NYA_OK;
}

NYA_Error nya_filesystem_last_modified(NYA_ConstCString path, OUT u64* out_timestamp) {
    nya_assert(out_timestamp != nullptr);

    NYA_FileInfo info;
    NYA_TRY(nya_filesystem_info(path, &info));
    *out_timestamp = info.modified_at;

    return NYA_OK;
}

NYA_Error nya_filesystem_absolute(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_path != nullptr);

    char  resolved[MAX_PATH];
    DWORD length = GetFullPathNameA(path, sizeof(resolved), resolved, nullptr);
    if (length == 0 || length >= sizeof(resolved)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "failed to resolve '%s'", path);

    // Normalising converts the backslashes, so a resolved path looks like every other path here.
    *out_path = nya_path_normalize(arena, resolved);
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * MUTATION
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_filesystem_move(NYA_ConstCString old_path, NYA_ConstCString new_path) {
    nya_assert(old_path != nullptr);
    nya_assert(new_path != nullptr);

    /*
     * MoveFileExA with MOVEFILE_REPLACE_EXISTING matches the behavior of rename() on POSIX.
     *
     * Without it, moving a file over an existing one fails with ERROR_ALREADY_EXISTS. This is
     * exactly what happens when the build system tries to restore its backup after a failed
     * rebuild: the compiler may have already created a (broken) executable at the destination.
     */
    if (!MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return nya_error(NYA_ERROR_IO, "failed to move '%s' to '%s' (error %lu)", old_path, new_path, GetLastError());
    }

    return NYA_OK;
}

NYA_Error nya_filesystem_copy(NYA_ConstCString source, NYA_ConstCString destination) {
    nya_assert(source != nullptr);
    nya_assert(destination != nullptr);

    if (!CopyFileA(source, destination, FALSE)) return nya_error(NYA_ERROR_IO, "failed to copy '%s' to '%s'", source, destination);
    return NYA_OK;
}

NYA_Error nya_filesystem_delete(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    // DeleteFileA refuses directories, so the right call depends on what is actually there.
    if (nya_filesystem_is_directory(path)) {
        if (!RemoveDirectoryA(path)) return nya_error(NYA_ERROR_IO, "failed to delete directory '%s'", path);
        return NYA_OK;
    }

    if (!DeleteFileA(path)) return nya_error(NYA_ERROR_IO, "failed to delete '%s'", path);
    return NYA_OK;
}

NYA_Error nya_filesystem_create_directory(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    char   partial[MAX_PATH] = { 0 };
    size_t length            = strlen(path);
    if (length >= sizeof(partial)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "path too long: '%s'", path);

    // Walk the path creating each component in turn, so missing parents are handled too.
    for (size_t i = 0; i < length; i++) {
        partial[i] = (path[i] == '/') ? '\\' : path[i];

        b8 is_separator = partial[i] == '\\';
        b8 is_last      = i + 1 == length;
        if (!is_separator && !is_last) continue;

        // Skip the empty component in front of a leading separator, as the POSIX side does. Without
        // it an absolute path truncated `partial` to "" on the first iteration and handed that to
        // CreateDirectoryA, which fails with ERROR_PATH_NOT_FOUND rather than ERROR_ALREADY_EXISTS —
        // so the whole call returned an error for a path that works on Linux.
        if (i == 0 && is_separator) continue;

        // Do not try to create the bare drive letter in "C:\".
        if (i > 0 && partial[i - 1] == ':') continue;

        char saved     = partial[i];
        b8   truncated = is_separator && !is_last;
        if (truncated) partial[i] = '\0';

        if (!CreateDirectoryA(partial, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return nya_error(NYA_ERROR_IO, "failed to create directory '%s'", partial);
        }

        if (truncated) partial[i] = saved;
    }

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * DIRECTORIES
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_filesystem_list(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_ArrayᐸNYA_DirectoryEntryᐳ** out_entries) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_entries != nullptr);

    // FindFirstFile wants a wildcard rather than a directory.
    NYA_String* pattern = nya_path_join(arena, path, "*");

    WIN32_FIND_DATAA find_data;
    HANDLE           find = FindFirstFileA(nya_string_to_cstring(arena, pattern), &find_data);
    if (find == INVALID_HANDLE_VALUE) return nya_error(NYA_ERROR_NOT_FOUND, "failed to list '%s'", path);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nya_array_create(arena, NYA_DirectoryEntry);

    do {
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) continue;

        ULARGE_INTEGER size;
        size.LowPart  = find_data.nFileSizeLow;
        size.HighPart = find_data.nFileSizeHigh;

        // The find data already carries everything, so unlike POSIX there is no second stat per entry.
        nya_array_push_back(
            entries,
            ((NYA_DirectoryEntry){
                .name        = nya_string_sprintf(arena, "%s", find_data.cFileName),
                .type        = _nya_filesystem_type_from_attributes(find_data.dwFileAttributes),
                .size        = (u64)size.QuadPart,
                .modified_at = _nya_filesystem_time_from_filetime(find_data.ftLastWriteTime),
            })
        );
    } while (FindNextFileA(find, &find_data));

    FindClose(find);

    *out_entries = entries;
    return NYA_OK;
}

/**
 * `out_keep_going` is how a callback's stop request reaches the top. Returning NYA_OK from a nested
 * level would only end that level, leaving the parent to carry on walking.
 * */
NYA_INTERNAL NYA_Error
_nya_filesystem_walk(NYA_Arena* arena, NYA_ConstCString path, NYA_WalkCallback callback, void* user_data, u32 depth, OUT b8* out_keep_going) {
    nya_assert(depth < NYA_FILESYSTEM_WALK_DEPTH_MAX, "Maximum directory depth exceeded walking '%s' (symlink loop?).", path);

    // A scratch arena per level, so memory tracks the depth of the tree rather than its total size.
    //
    // Explicitly sized because the default region is a gibibyte, which a sanitized build poisons in
    // full on creation: a fixed cost per directory that dwarfed the walk itself. A mebibyte holds a
    // few thousand entries and their joined paths, and a directory larger than that just chains
    // another region.
    NYA_Arena* scratch = nya_arena_create(.region_size = nya_mebyte_to_byte(1UL));
    defer      nya_arena_destroy(scratch);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_TRY(nya_filesystem_list(scratch, path, &entries));

    nya_array_foreach (entries, entry) {
        NYA_CString full = nya_string_to_cstring(scratch, nya_path_join(scratch, path, nya_string_to_cstring(scratch, entry->name)));

        // Depth first, children before their parent, so a caller deleting as it goes never has to
        // remove a directory that still has contents. Reparse points are not descended into.
        if (entry->type == NYA_FILE_TYPE_DIRECTORY) {
            NYA_TRY(_nya_filesystem_walk(arena, full, callback, user_data, depth + 1, out_keep_going));
            if (!*out_keep_going) return NYA_OK;
        }

        if (!callback(full, entry, user_data)) {
            *out_keep_going = false;
            return NYA_OK;
        }
    }

    return NYA_OK;
}

NYA_Error nya_filesystem_walk(NYA_Arena* arena, NYA_ConstCString path, NYA_WalkCallback callback, void* user_data) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(callback != nullptr);

    b8 keep_going = true;
    return _nya_filesystem_walk(arena, path, callback, user_data, 0, &keep_going);
}

/*
 * ─────────────────────────────────────────────────────────
 * RECURSIVE MUTATION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Deletes one entry, remembering the first failure.
 *
 * The walk keeps going after a failure so that as much as can be removed is removed, but the error
 * has to survive: without it the only symptom is the parent rmdir failing with "directory not
 * empty", which points at the wrong path and hides the real reason (a permission denied on one
 * file, say).
 * */
NYA_INTERNAL b8 _nya_filesystem_delete_walk(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    nya_unused(entry);

    NYA_Error* first_error = user_data;
    NYA_Error  result      = nya_filesystem_delete(path);

    if (!result.ok && first_error->ok) *first_error = result;

    return true;
}

NYA_Error nya_filesystem_delete_recursive(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    if (!nya_filesystem_is_directory(path)) return nya_filesystem_delete(path);

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_Error first_error = NYA_OK;
    NYA_TRY(nya_filesystem_walk(arena, path, _nya_filesystem_delete_walk, &first_error));
    if (!first_error.ok) return first_error;

    return nya_filesystem_delete(path);
}

NYA_Error nya_filesystem_copy_recursive(NYA_ConstCString source, NYA_ConstCString destination) {
    nya_assert(source != nullptr);
    nya_assert(destination != nullptr);

    static u32 depth = 0;
    nya_assert(depth < NYA_FILESYSTEM_WALK_DEPTH_MAX, "Maximum directory depth exceeded copying '%s' (symlink loop?).", source);
    depth++;
    defer depth--;

    if (!nya_filesystem_is_directory(source)) {
        NYA_Arena* arena = nya_arena_create();
        defer      nya_arena_destroy(arena);

        NYA_String* parent = nya_path_dirname(arena, destination);
        NYA_TRY(nya_filesystem_create_directory(nya_string_to_cstring(arena, parent)));

        // A symlink is recreated rather than dereferenced. Creating one needs either developer mode or
        // elevation on Windows, so falling back to a content copy keeps the tree copy working instead
        // of failing outright.
        NYA_FileInfo info;
        NYA_TRY(nya_filesystem_info(source, &info));

        if (info.type == NYA_FILE_TYPE_SYMLINK) {
            DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
            if (CreateSymbolicLinkA(destination, source, flags)) return NYA_OK;
        }

        return nya_filesystem_copy(source, destination);
    }

    NYA_TRY(nya_filesystem_create_directory(destination));

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_TRY(nya_filesystem_list(arena, source, &entries));

    nya_array_foreach (entries, entry) {
        NYA_CString name         = nya_string_to_cstring(arena, entry->name);
        NYA_CString child_source = nya_string_to_cstring(arena, nya_path_join(arena, source, name));
        NYA_CString child_dest   = nya_string_to_cstring(arena, nya_path_join(arena, destination, name));

        NYA_TRY(nya_filesystem_copy_recursive(child_source, child_dest));
    }

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * FILE HANDLES
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_file_open(NYA_ConstCString path, NYA_FileMode mode, OUT NYA_File* out_file) {
    nya_assert(path != nullptr);
    nya_assert(out_file != nullptr);

    DWORD access = 0;
    if (mode & NYA_FILE_MODE_READ) access |= GENERIC_READ;
    if (mode & NYA_FILE_MODE_WRITE) access |= GENERIC_WRITE;
    // FILE_APPEND_DATA rather than GENERIC_WRITE, so writes always land at the end even if something
    // else seeks the handle.
    if (mode & NYA_FILE_MODE_APPEND) access |= FILE_APPEND_DATA;
    if (access == 0) access = GENERIC_READ;

    // WRITE and APPEND imply CREATE, matching the POSIX side.
    DWORD creation = OPEN_EXISTING;
    if (mode & NYA_FILE_MODE_TRUNCATE) {
        creation = CREATE_ALWAYS;
    } else if (mode & (NYA_FILE_MODE_CREATE | NYA_FILE_MODE_WRITE | NYA_FILE_MODE_APPEND)) {
        creation = OPEN_ALWAYS;
    }

    HANDLE handle = CreateFileA(path, access, FILE_SHARE_READ, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nya_error(NYA_ERROR_IO, "failed to open '%s'", path);

    *out_file = (NYA_File){ .handle = handle, .is_open = true };
    return NYA_OK;
}

void nya_file_close(NYA_File* file) {
    if (file == nullptr || !file->is_open) return;

    (void)CloseHandle((HANDLE)file->handle);
    *file = (NYA_File){ .handle = nullptr, .is_open = false };
}

b8 nya_file_is_open(const NYA_File* file) {
    return file != nullptr && file->is_open;
}

NYA_Error nya_file_read_bytes(NYA_File* file, OUT u8* buffer, u64 length, OUT u64* out_read) {
    nya_assert(nya_file_is_open(file), "Cannot read from a file that is not open.");
    nya_assert(buffer != nullptr);
    nya_assert(out_read != nullptr);

    DWORD got = 0;
    if (!ReadFile((HANDLE)file->handle, buffer, (DWORD)length, &got, nullptr)) return nya_error(NYA_ERROR_IO, "failed to read from file");

    *out_read = (u64)got;
    return NYA_OK;
}

NYA_Error nya_file_write_bytes(NYA_File* file, const u8* buffer, u64 length) {
    nya_assert(nya_file_is_open(file), "Cannot write to a file that is not open.");
    nya_assert(buffer != nullptr);

    // WriteFile is allowed to write less than asked, so keep going until it is all out.
    u64 written = 0;
    while (written < length) {
        DWORD chunk = 0;
        if (!WriteFile((HANDLE)file->handle, buffer + written, (DWORD)(length - written), &chunk, nullptr)) {
            return nya_error(NYA_ERROR_IO, "failed to write to file");
        }
        if (chunk == 0) return nya_error(NYA_ERROR_IO, "write made no progress");

        written += (u64)chunk;
    }

    return NYA_OK;
}

NYA_Error nya_file_seek(NYA_File* file, s64 offset, NYA_FileSeek origin) {
    nya_assert(nya_file_is_open(file), "Cannot seek a file that is not open.");

    DWORD method = FILE_BEGIN;
    switch (origin) {
        case NYA_FILE_SEEK_SET:     method = FILE_BEGIN; break;
        case NYA_FILE_SEEK_CURRENT: method = FILE_CURRENT; break;
        case NYA_FILE_SEEK_END:     method = FILE_END; break;
        default:                    nya_unreachable();
    }

    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    if (!SetFilePointerEx((HANDLE)file->handle, distance, nullptr, method)) return nya_error(NYA_ERROR_IO, "failed to seek file");

    return NYA_OK;
}

NYA_Error nya_file_tell(NYA_File* file, OUT u64* out_offset) {
    nya_assert(nya_file_is_open(file), "Cannot tell the position of a file that is not open.");
    nya_assert(out_offset != nullptr);

    LARGE_INTEGER zero     = { 0 };
    LARGE_INTEGER position = { 0 };
    if (!SetFilePointerEx((HANDLE)file->handle, zero, &position, FILE_CURRENT)) return nya_error(NYA_ERROR_IO, "failed to query file position");

    *out_offset = (u64)position.QuadPart;
    return NYA_OK;
}

NYA_Error nya_file_truncate(NYA_File* file, u64 length) {
    nya_assert(nya_file_is_open(file), "Cannot truncate a file that is not open.");

    // SetEndOfFile truncates wherever the pointer is, so it has to be moved there first.
    NYA_TRY(nya_file_seek(file, (s64)length, NYA_FILE_SEEK_SET));
    if (!SetEndOfFile((HANDLE)file->handle)) return nya_error(NYA_ERROR_IO, "failed to truncate file");

    return NYA_OK;
}

NYA_Error nya_file_flush(NYA_File* file) {
    nya_assert(nya_file_is_open(file), "Cannot flush a file that is not open.");

    if (!FlushFileBuffers((HANDLE)file->handle)) return nya_error(NYA_ERROR_IO, "failed to flush file");
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * WELL KNOWN LOCATIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_filesystem_working_directory(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char  buffer[MAX_PATH];
    DWORD length = GetCurrentDirectoryA(sizeof(buffer), buffer);
    if (length == 0 || length >= sizeof(buffer)) return nya_error(NYA_ERROR_IO, "failed to get the working directory");

    *out_path = nya_path_normalize(arena, buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_working_directory_set(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    if (!SetCurrentDirectoryA(path)) return nya_error(NYA_ERROR_IO, "failed to set the working directory to '%s'", path);
    return NYA_OK;
}

NYA_Error nya_filesystem_executable_path(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char  buffer[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) return nya_error(NYA_ERROR_IO, "failed to get the executable path");

    *out_path = nya_path_normalize(arena, buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_temp_directory(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char  buffer[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(buffer), buffer);
    if (length == 0 || length >= sizeof(buffer)) return nya_error(NYA_ERROR_IO, "failed to get the temp directory");

    *out_path = nya_path_normalize(arena, buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_user_data_directory(NYA_Arena* arena, NYA_ConstCString application, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(application != nullptr);
    nya_assert(out_path != nullptr);

    // APPDATA rather than SHGetKnownFolderPath, which would drag in shell32 and ole32 for one string.
    NYA_ConstCString roaming = getenv("APPDATA");
    if (roaming == nullptr || roaming[0] == '\0') return nya_error(NYA_ERROR_NOT_FOUND, "APPDATA is not set");

    *out_path = nya_path_join(arena, roaming, application);
    return NYA_OK;
}
