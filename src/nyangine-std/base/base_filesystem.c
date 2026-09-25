#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_filesystem.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-std/base/base_path.h"
#include "nyangine-std/os/os_file.h"
#include "nyangine-std/os/os_time.h"

// INTERNAL

/** What the layer below says, as the error kind this API has always reported for it. */
NYA_INTERNAL NYA_ErrorKind _nya_filesystem_error_kind(NYA_OsFileStatus status) {
    switch (status) {
        case NYA_OS_FILE_STATUS_NOT_FOUND:   return NYA_ERROR_NOT_FOUND;

        // BUSY is Windows refusing with a sharing violation or ACCESS_DENIED, so once the waiting is over it means what it always meant.
        case NYA_OS_FILE_STATUS_BUSY:
        case NYA_OS_FILE_STATUS_DENIED:      return NYA_ERROR_PERMISSION_DENIED;

        case NYA_OS_FILE_STATUS_EXISTS:      return NYA_ERROR_ALREADY_EXISTS;
        case NYA_OS_FILE_STATUS_INVALID:     return NYA_ERROR_INVALID_ARGUMENT;
        case NYA_OS_FILE_STATUS_NO_MEMORY:   return NYA_ERROR_OUT_OF_MEMORY;
        case NYA_OS_FILE_STATUS_UNSUPPORTED: return NYA_ERROR_NOT_SUPPORTED;
        case NYA_OS_FILE_STATUS_IO:          return NYA_ERROR_IO;

        default:                             return NYA_ERROR_NOT_OK;
    }
}

/**
 * The failure with the status named at the end, which is as much as the layer below can say about why.
 * `status` is read twice, so pass a variable rather than a call.
 * */
#define _nya_filesystem_error(status, format, ...)                                                                                                   \
    nya_error(_nya_filesystem_error_kind(status), format " (%s)", __VA_ARGS__ __VA_OPT__(, ) NYA_OSFILESTATUS_NAME_MAP[status])

NYA_INTERNAL NYA_FileType _nya_filesystem_type(NYA_OsFileKind kind) {
    switch (kind) {
        case NYA_OS_FILE_KIND_FILE:      return NYA_FILE_TYPE_FILE;
        case NYA_OS_FILE_KIND_DIRECTORY: return NYA_FILE_TYPE_DIRECTORY;
        case NYA_OS_FILE_KIND_SYMLINK:   return NYA_FILE_TYPE_SYMLINK;
        default:                         return NYA_FILE_TYPE_UNKNOWN;
    }
}

/** The os layer answers in the host's own spelling, and Windows' backslashes become '/' like every other path here. */
NYA_INTERNAL NYA_String* _nya_filesystem_path(NYA_Arena* arena, NYA_ConstCString native) {
#if OS_WINDOWS
    return nya_path_normalize(arena, native);
#else
    return nya_string_sprintf(arena, "%s", native);
#endif
}

// FUNCTIONS

// QUERIES

b8 nya_filesystem_exists(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    NYA_OsFileStat info = { 0 };
    return nya_os_file_stat(path, true, &info) == NYA_OS_FILE_STATUS_OK;
}

b8 nya_filesystem_is_file(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    // following links, so what a link points at is what answers: a link to a file is a file.
    NYA_OsFileStat info = { 0 };
    if (nya_os_file_stat(path, true, &info) != NYA_OS_FILE_STATUS_OK) return false;

    return info.kind == NYA_OS_FILE_KIND_FILE;
}

b8 nya_filesystem_is_directory(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    NYA_OsFileStat info = { 0 };
    if (nya_os_file_stat(path, true, &info) != NYA_OS_FILE_STATUS_OK) return false;

    return info.kind == NYA_OS_FILE_KIND_DIRECTORY;
}

NYA_Error nya_filesystem_info(NYA_ConstCString path, OUT NYA_FileInfo* out_info) {
    nya_assert(path != nullptr);
    nya_assert(out_info != nullptr);

    // not following links, so a symlink reports as a symlink rather than as whatever it points at.
    NYA_OsFileStat   info   = { 0 };
    NYA_OsFileStatus status = nya_os_file_stat(path, false, &info);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to stat '%s'", path);

    *out_info = (NYA_FileInfo){
        .type        = _nya_filesystem_type(info.kind),
        .size        = info.size,
        .modified_at = info.modified_ms,
        .created_at  = info.created_ms,
        .accessed_at = info.accessed_ms,
        .readonly    = info.readonly,
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
    nya_assert(path != nullptr);
    nya_assert(out_timestamp != nullptr);

    NYA_OsFileStat   info   = { 0 };
    NYA_OsFileStatus status = nya_os_file_stat(path, true, &info);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to stat '%s'", path);

    *out_timestamp = info.modified_ms;

    return NYA_OK;
}

NYA_Error nya_filesystem_absolute(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_path != nullptr);

    char             resolved[NYA_OS_PATH_MAX];
    NYA_OsFileStatus status = nya_os_path_absolute(path, resolved, sizeof(resolved));
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to resolve '%s'", path);

    *out_path = _nya_filesystem_path(arena, resolved);
    return NYA_OK;
}

// MUTATION

NYA_Error nya_filesystem_move(NYA_ConstCString source, NYA_ConstCString destination) {
    nya_assert(source != nullptr);
    nya_assert(destination != nullptr);

    NYA_OsFileStatus status = nya_os_file_rename(source, destination);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to move '%s' to '%s'", source, destination);

    return NYA_OK;
}

/**
 * Tries a replace gets, and the first wait between them, doubling each time: 1 + 2 + ... + 64 ms is 127 ms at worst.
 * Only Windows ever refuses transiently — two replaces racing onto one name, or a scanner holding the file for a
 * moment — so on POSIX the loop runs exactly once; test_file_atomic's racing writers hit it on every Windows run.
 * */
#define _NYA_FILESYSTEM_REPLACE_ATTEMPTS      8
#define _NYA_FILESYSTEM_REPLACE_FIRST_WAIT_MS 1

NYA_Error nya_filesystem_replace(NYA_ConstCString source, NYA_ConstCString destination) {
    nya_assert(source != nullptr);
    nya_assert(destination != nullptr);

    // a file someone made private stays private: without this the rename hands over the new file's 0644.
    u32 mode = 0;
    if (nya_os_file_mode_get(destination, &mode) == NYA_OS_FILE_STATUS_OK) {
        NYA_OsFileStatus carried = nya_os_file_mode_set(source, mode);
        if (carried != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(carried, "failed to carry the permissions of '%s' over", destination);
    }

    u32 wait_ms = _NYA_FILESYSTEM_REPLACE_FIRST_WAIT_MS;
    for (u32 attempt = 1;; attempt++) {
        NYA_OsFileStatus status = nya_os_file_replace(source, destination);
        if (status == NYA_OS_FILE_STATUS_OK) break;

        if (status != NYA_OS_FILE_STATUS_BUSY || attempt == _NYA_FILESYSTEM_REPLACE_ATTEMPTS) {
            return _nya_filesystem_error(status, "failed to replace '%s' with '%s'", destination, source);
        }

        nya_os_time_sleep_ms(wait_ms);
        wait_ms *= 2;
    }

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "filesystem_replace");
    defer     nya_arena_destroy_on_stack(&scratch);

    // fsync the directory holding it, which is what makes the rename survive a power cut; Windows has no such call and says so, where the write-through in the replace is the whole story.
    NYA_String*      parent = nya_path_dirname(&scratch, destination);
    NYA_OsFileStatus synced = nya_os_directory_sync(nya_string_to_cstring(&scratch, parent));
    if (synced != NYA_OS_FILE_STATUS_OK && synced != NYA_OS_FILE_STATUS_UNSUPPORTED) {
        return _nya_filesystem_error(synced, "failed to sync the directory of '%s'", destination);
    }

    return NYA_OK;
}

NYA_Error nya_filesystem_copy(NYA_ConstCString source, NYA_ConstCString destination) {
    nya_assert(source != nullptr);
    nya_assert(destination != nullptr);

    NYA_File source_file = { 0 };
    NYA_TRY(nya_file_open(source, NYA_FILE_MODE_READ, &source_file));
    defer nya_file_close(&source_file);

    NYA_File destination_file = { 0 };
    NYA_TRY(nya_file_open(destination, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &destination_file));
    defer nya_file_close(&destination_file);

    // in chunks rather than through one string: a copy has no reason to hold the whole file at once.
    u8  buffer[4096];
    u64 got = 0;
    do {
        NYA_TRY(nya_file_read_bytes(&source_file, buffer, sizeof(buffer), &got));
        if (got > 0) NYA_TRY(nya_file_write_bytes(&destination_file, buffer, got));
    } while (got > 0);

    // Carry the source's permissions over: dropping an executable bit would, among other things, break restoring the build system from its backup; set here rather than through the open, whose mode the umask masks.
    u32 mode = 0;
    if (nya_os_file_mode_get(source, &mode) == NYA_OS_FILE_STATUS_OK) (void)nya_os_file_mode_set(destination, mode);

    return NYA_OK;
}

NYA_Error nya_filesystem_delete(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    // Which call is right depends on what is there: unlink refuses a directory and removing one refuses everything else; links are not followed, so deleting a link deletes the link.
    NYA_OsFileStat   info   = { 0 };
    NYA_OsFileStatus status = nya_os_file_stat(path, false, &info);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to delete '%s'", path);

    if (info.kind == NYA_OS_FILE_KIND_DIRECTORY) {
        status = nya_os_directory_destroy(path);
    } else {
        status = nya_os_file_unlink(path);

        // A link to a directory is a file to POSIX and a directory to Windows, and only one of the two calls will take it.
        if (status != NYA_OS_FILE_STATUS_OK && info.kind == NYA_OS_FILE_KIND_SYMLINK) status = nya_os_directory_destroy(path);
    }

    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to delete '%s'", path);

    return NYA_OK;
}

NYA_Error nya_filesystem_create_directory(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    char   partial[NYA_OS_PATH_MAX] = { 0 };
    size_t length                   = strlen(path);
    if (length >= sizeof(partial)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "path too long: '%s'", path);

    // Walk the path creating each component in turn, so missing parents are handled too.
    for (size_t i = 0; i < length; i++) {
        // in the host's own separator, so "C:/a" on Windows creates "C:\a" rather than nothing.
        partial[i] = (path[i] == NYA_PATH_SEPARATOR) ? NYA_PATH_SEPARATOR_NATIVE : path[i];

        b8 is_separator = partial[i] == NYA_PATH_SEPARATOR_NATIVE;
        b8 is_last      = i + 1 == length;
        if (!is_separator && !is_last) continue;

        // Skip the empty component before a leading separator: it names nothing, and creating "" fails with ERROR_PATH_NOT_FOUND on Windows.
        if (i == 0 && is_separator) continue;

        // Do not try to create the bare drive letter in "C:\", which is not ours to make.
        if (i > 0 && partial[i - 1] == ':') continue;

        char saved     = partial[i];
        b8   truncated = is_separator && !is_last;
        if (truncated) partial[i] = '\0';

        NYA_OsFileStatus status = nya_os_directory_create(partial);
        if (status != NYA_OS_FILE_STATUS_OK && status != NYA_OS_FILE_STATUS_EXISTS) {
            return _nya_filesystem_error(status, "failed to create directory '%s'", partial);
        }

        if (truncated) partial[i] = saved;
    }

    return NYA_OK;
}

// DIRECTORIES

NYA_Error nya_filesystem_list(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_ArrayᐸNYA_DirectoryEntryᐳ** out_entries) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_entries != nullptr);

    NYA_OsDirectory  directory = { 0 };
    NYA_OsFileStatus status    = nya_os_directory_open(path, &directory);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to list '%s'", path);
    defer nya_os_directory_close(&directory);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nya_array_create(arena, NYA_DirectoryEntry);

    NYA_OsDirectoryEntry entry = { 0 };
    while (nya_os_directory_next(&directory, &entry)) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) continue;

        NYA_FileType type        = _nya_filesystem_type(entry.kind);
        u64          size        = entry.size;
        u64          modified_at = entry.modified_ms;

        // The listing carries metadata so a browser need not stat every row: POSIX's readdir gives little, so it costs a stat per entry there, while the Windows find data already holds it all.
        if (!entry.has_metadata) {
            NYA_FileInfo info = { 0 };
            (void)nya_filesystem_info(nya_string_to_cstring(arena, nya_path_join(arena, path, entry.name)), &info);

            type        = info.type;
            size        = info.size;
            modified_at = info.modified_at;
        }

        nya_array_push_back(
            entries,
            ((NYA_DirectoryEntry){
                .name        = nya_string_sprintf(arena, "%s", entry.name),
                .type        = type,
                .size        = size,
                .modified_at = modified_at,
            })
        );
    }

    *out_entries = entries;
    return NYA_OK;
}

/**
 * `out_keep_going` is how a callback's stop request reaches the top. Returning NYA_OK from a nested
 * level would only end that level, leaving the parent to carry on walking, which is not what
 * "return false to stop the walk" promises.
 * */
NYA_INTERNAL NYA_Error
_nya_filesystem_walk(NYA_Arena* arena, NYA_ConstCString path, NYA_WalkCallback callback, void* user_data, u32 depth, OUT b8* out_keep_going) {
    nya_assert(depth < NYA_FILESYSTEM_WALK_DEPTH_MAX, "Maximum directory depth exceeded walking '%s' (symlink loop?).", path);

    // A scratch arena per level, so memory tracks tree depth not total size; sized to a mebibyte because the default gibibyte region is poisoned in full on creation under sanitizers.
    NYA_Arena* scratch = nya_arena_create(.region_size = nya_mebyte_to_byte(1));
    defer      nya_arena_destroy(scratch);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_TRY(nya_filesystem_list(scratch, path, &entries));

    nya_array_foreach (entries, entry) {
        NYA_CString full = nya_string_to_cstring(scratch, nya_path_join(scratch, path, nya_string_to_cstring(scratch, entry->name)));

        // Depth first, children before parent, so a caller deleting as it goes never removes a directory that still has contents; links are not followed, since one pointing at an ancestor would walk forever.
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

// RECURSIVE MUTATION

/**
 * Deletes one entry, remembering the first failure.
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

/** Recreates a symlink as a symlink, or says that it could not and the contents are the only copy left. */
NYA_INTERNAL NYA_Error _nya_filesystem_copy_link(NYA_ConstCString source, NYA_ConstCString destination, OUT b8* out_linked) {
    *out_linked = false;

    char             target[NYA_OS_PATH_MAX];
    NYA_OsFileStatus read = nya_os_file_link_read(source, target, sizeof(target));

    // Windows cannot read where a link points, so the new one points at the source instead, and making one needs developer mode or elevation: a refusal falls back to copying the contents.
    if (read == NYA_OS_FILE_STATUS_UNSUPPORTED) {
        *out_linked = nya_os_file_link_set(destination, source) == NYA_OS_FILE_STATUS_OK;
        return NYA_OK;
    }

    if (read != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(read, "failed to read the link '%s'", source);

    NYA_OsFileStatus linked = nya_os_file_link_set(destination, target);
    if (linked != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(linked, "failed to link '%s' at '%s'", target, destination);

    *out_linked = true;
    return NYA_OK;
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

        // A symlink is recreated as a symlink: copying its contents would turn a link into a full duplicate of its target, which for a tree copy is wrong and can expand a small tree into a huge one.
        NYA_FileInfo info;
        NYA_TRY(nya_filesystem_info(source, &info));

        if (info.type == NYA_FILE_TYPE_SYMLINK) {
            b8 linked = false;
            NYA_TRY(_nya_filesystem_copy_link(source, destination, &linked));
            if (linked) return NYA_OK;
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

// FILE HANDLES

NYA_Error nya_file_open(NYA_ConstCString path, u32 mode, OUT NYA_File* out_file) {
    nya_assert(path != nullptr);
    nya_assert(out_file != nullptr);

    // O_EXCL without O_CREAT is undefined, so exclusive is only meaningful on an open that may create.
    nya_assert(!(mode & NYA_FILE_MODE_EXCLUSIVE) || (mode & (NYA_FILE_MODE_CREATE | NYA_FILE_MODE_WRITE | NYA_FILE_MODE_APPEND)));

    u32 flags = 0;
    if (mode & NYA_FILE_MODE_READ) flags |= NYA_OS_FILE_OPEN_READ;
    if (mode & NYA_FILE_MODE_WRITE) flags |= NYA_OS_FILE_OPEN_WRITE;
    if (mode & NYA_FILE_MODE_APPEND) flags |= NYA_OS_FILE_OPEN_APPEND;
    if (mode & NYA_FILE_MODE_TRUNCATE) flags |= NYA_OS_FILE_OPEN_TRUNCATE;
    if (mode & NYA_FILE_MODE_EXCLUSIVE) flags |= NYA_OS_FILE_OPEN_EXCLUSIVE;
    // WRITE and APPEND imply CREATE, since opening to write something that does not exist yet is the common case rather than an error.
    if (mode & (NYA_FILE_MODE_CREATE | NYA_FILE_MODE_WRITE | NYA_FILE_MODE_APPEND)) flags |= NYA_OS_FILE_OPEN_CREATE;

    NYA_OsFile       file   = NYA_OS_FILE_NONE;
    NYA_OsFileStatus status = nya_os_file_open(path, flags, &file);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to open '%s'", path);

    *out_file = (NYA_File){ .os = file, .is_open = true };
    return NYA_OK;
}

void nya_file_close(NYA_File* file) {
    if (file == nullptr || !file->is_open) return;

    nya_os_file_close(&file->os);
    *file = (NYA_File){ .os = NYA_OS_FILE_NONE, .is_open = false };
}

b8 nya_file_is_open(const NYA_File* file) {
    return file != nullptr && file->is_open;
}

NYA_Error nya_file_read_bytes(NYA_File* file, OUT u8* buffer, u64 length, OUT u64* out_read) {
    nya_assert(nya_file_is_open(file), "Cannot read from a file that is not open.");
    nya_assert(buffer != nullptr);
    nya_assert(out_read != nullptr);

    NYA_OsFileStatus status = nya_os_file_read(&file->os, buffer, length, out_read);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to read from file");

    return NYA_OK;
}

NYA_Error nya_file_write_bytes(NYA_File* file, const u8* buffer, u64 length) {
    nya_assert(nya_file_is_open(file), "Cannot write to a file that is not open.");
    nya_assert(buffer != nullptr);

    // both hosts are allowed to write less than asked, so keep going until it is all out.
    u64 written = 0;
    while (written < length) {
        u64              chunk  = 0;
        NYA_OsFileStatus status = nya_os_file_write(&file->os, buffer + written, length - written, &chunk);
        if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to write to file");

        // a write that takes nothing would spin here forever.
        if (chunk == 0) return nya_error(NYA_ERROR_IO, "write made no progress");

        written += chunk;
    }

    return NYA_OK;
}

NYA_Error nya_file_seek(NYA_File* file, s64 offset, NYA_FileSeek origin) {
    nya_assert(nya_file_is_open(file), "Cannot seek a file that is not open.");

    NYA_OsFileSeek where = NYA_OS_FILE_SEEK_SET;
    switch (origin) {
        case NYA_FILE_SEEK_SET:     where = NYA_OS_FILE_SEEK_SET; break;
        case NYA_FILE_SEEK_CURRENT: where = NYA_OS_FILE_SEEK_CURRENT; break;
        case NYA_FILE_SEEK_END:     where = NYA_OS_FILE_SEEK_END; break;
        default:                    nya_unreachable();
    }

    NYA_OsFileStatus status = nya_os_file_seek(&file->os, offset, where);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to seek file");

    return NYA_OK;
}

NYA_Error nya_file_tell(NYA_File* file, OUT u64* out_offset) {
    nya_assert(nya_file_is_open(file), "Cannot tell the position of a file that is not open.");
    nya_assert(out_offset != nullptr);

    NYA_OsFileStatus status = nya_os_file_tell(&file->os, out_offset);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to query file position");

    return NYA_OK;
}

NYA_Error nya_file_truncate(NYA_File* file, u64 length) {
    nya_assert(nya_file_is_open(file), "Cannot truncate a file that is not open.");

    NYA_OsFileStatus status = nya_os_file_truncate(&file->os, length);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to truncate file");

    return NYA_OK;
}

NYA_Error nya_file_flush(NYA_File* file) {
    nya_assert(nya_file_is_open(file), "Cannot flush a file that is not open.");

    NYA_OsFileStatus status = nya_os_file_sync(&file->os);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to flush file");

    return NYA_OK;
}

// WELL KNOWN LOCATIONS

NYA_Error nya_filesystem_working_directory(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char             buffer[NYA_OS_PATH_MAX];
    NYA_OsFileStatus status = nya_os_working_directory_get(buffer, sizeof(buffer));
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to get the working directory");

    *out_path = _nya_filesystem_path(arena, buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_working_directory_set(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    NYA_OsFileStatus status = nya_os_working_directory_set(path);
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to set the working directory to '%s'", path);

    return NYA_OK;
}

NYA_Error nya_filesystem_executable_path(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char             buffer[NYA_OS_PATH_MAX];
    NYA_OsFileStatus status = nya_os_executable_path(buffer, sizeof(buffer));
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to get the executable path");

    *out_path = _nya_filesystem_path(arena, buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_temp_directory(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char             buffer[NYA_OS_PATH_MAX];
    NYA_OsFileStatus status = nya_os_temp_directory(buffer, sizeof(buffer));
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "failed to get the temp directory");

    *out_path = _nya_filesystem_path(arena, buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_user_data_directory(NYA_Arena* arena, NYA_ConstCString application, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(application != nullptr);
    nya_assert(out_path != nullptr);

    char             buffer[NYA_OS_PATH_MAX];
    NYA_OsFileStatus status = nya_os_user_data_directory(buffer, sizeof(buffer));
    if (status != NYA_OS_FILE_STATUS_OK) return _nya_filesystem_error(status, "there is no user data directory");

    NYA_String* root = _nya_filesystem_path(arena, buffer);

    *out_path = nya_path_join(arena, nya_string_to_cstring(arena, root), application);
    return NYA_OK;
}
