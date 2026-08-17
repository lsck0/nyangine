#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nyangine/nyangine.h"

b8 nya_filesystem_exists(NYA_ConstCString path) {
    b8 ok;

    struct stat path_stat;
    ok = stat(path, &path_stat) == 0;

    return ok;
}

NYA_Error nya_filesystem_create_directory(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    char   partial[PATH_MAX] = { 0 };
    size_t length            = strlen(path);
    if (length >= sizeof(partial)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "path too long: '%s'", path);

    // Walk the path creating each component in turn, so missing parents are handled too.
    for (size_t i = 0; i < length; i++) {
        partial[i] = path[i];

        b8 is_separator = partial[i] == '/';
        b8 is_last      = i + 1 == length;
        if (!is_separator && !is_last) continue;

        // Skip the empty component in front of a leading '/'.
        if (i == 0 && is_separator) continue;

        char saved     = partial[i];
        b8   truncated = is_separator && !is_last;
        if (truncated) partial[i] = '\0';

        if (mkdir(partial, 0o755) != 0 && errno != EEXIST) {
            return nya_error(NYA_ERROR_IO, "failed to create directory '%s': %s", partial, strerror(errno));
        }

        if (truncated) partial[i] = saved;
    }

    return NYA_OK;
}

NYA_Error nya_filesystem_last_modified(NYA_ConstCString path, OUT u64* out_timestamp) {
    nya_assert(out_timestamp != nullptr);

    struct stat path_stat;
    b8          ok = stat(path, &path_stat) == 0;
    if (!ok) return nya_error_from_errno();

    *out_timestamp = (u64)path_stat.st_mtim.tv_sec * 1000ULL + (u64)(path_stat.st_mtim.tv_nsec / 1000000ULL);

    return NYA_OK;
}

NYA_Error nya_filesystem_move(NYA_ConstCString source, NYA_ConstCString destination) {
    b8 ok = rename(source, destination) == 0;
    if (!ok) return nya_error_from_errno();

    return NYA_OK;
}

NYA_Error nya_filesystem_copy(NYA_ConstCString source, NYA_ConstCString destination) {
    NYA_File source_file;
    NYA_TRY(nya_file_open(source, NYA_FILE_MODE_READ, &source_file));
    defer nya_file_close(&source_file);

    NYA_File destination_file;
    NYA_TRY(nya_file_open(destination, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &destination_file));
    defer nya_file_close(&destination_file);

    s32 source_fd      = source_file.descriptor;
    s32 destination_fd = destination_file.descriptor;

    // Carry the source's permissions over. Copying an executable and silently dropping its
    // executable bit would, among other things, break restoring the build system from its backup.
    // fchmod rather than open's mode argument, since that one is masked by the umask.
    struct stat source_stat;
    if (fstat(source_fd, &source_stat) == 0) (void)fchmod(destination_fd, source_stat.st_mode & 0o7777);

    NYA_Arena*  arena = nya_arena_create();
    defer       nya_arena_destroy(arena);
    NYA_String* buffer = nya_string_create(arena);

    NYA_TRY(nya_file_read_string(&source_file, buffer));
    NYA_TRY(nya_file_write_string(&destination_file, buffer));

    return NYA_OK;
}

NYA_Error nya_filesystem_delete(NYA_ConstCString path) {
    b8 ok = remove(path) == 0;
    if (!ok) return nya_error_from_errno();

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_FileType _nya_filesystem_type_from_mode(mode_t mode) {
    if (S_ISDIR(mode)) return NYA_FILE_TYPE_DIRECTORY;
    if (S_ISLNK(mode)) return NYA_FILE_TYPE_SYMLINK;
    if (S_ISREG(mode)) return NYA_FILE_TYPE_FILE;

    return NYA_FILE_TYPE_UNKNOWN;
}

b8 nya_filesystem_is_file(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    struct stat path_stat;
    if (stat(path, &path_stat) != 0) return false;

    return S_ISREG(path_stat.st_mode);
}

b8 nya_filesystem_is_directory(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    struct stat path_stat;
    if (stat(path, &path_stat) != 0) return false;

    return S_ISDIR(path_stat.st_mode);
}

NYA_Error nya_filesystem_info(NYA_ConstCString path, OUT NYA_FileInfo* out_info) {
    nya_assert(path != nullptr);
    nya_assert(out_info != nullptr);

    // lstat, so a symlink reports as a symlink rather than as whatever it points at.
    struct stat path_stat;
    if (lstat(path, &path_stat) != 0) return nya_error_from_errno();

    *out_info = (NYA_FileInfo){
        .type        = _nya_filesystem_type_from_mode(path_stat.st_mode),
        .size        = (u64)path_stat.st_size,
        .modified_at = (u64)path_stat.st_mtime,
        .created_at  = (u64)path_stat.st_ctime,
        .accessed_at = (u64)path_stat.st_atime,
        .readonly    = access(path, W_OK) != 0,
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

NYA_Error nya_filesystem_absolute(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_path != nullptr);

    char resolved[PATH_MAX];
    if (realpath(path, resolved) == nullptr) return nya_error_from_errno();

    *out_path = nya_string_sprintf(arena, "%s", resolved);
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

    DIR* directory = opendir(path);
    if (directory == nullptr) return nya_error_from_errno();
    defer closedir(directory);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nya_array_create(arena, NYA_DirectoryEntry);

    struct dirent* dir_entry;
    while ((dir_entry = readdir(directory)) != nullptr) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0) continue;

        NYA_String* full = nya_path_join(arena, path, dir_entry->d_name);

        // The listing carries metadata so a browser does not have to stat every row itself.
        NYA_FileInfo info = { 0 };
        (void)nya_filesystem_info(nya_string_to_cstring(arena, full), &info);

        nya_array_push_back(
            entries,
            ((NYA_DirectoryEntry){
                .name        = nya_string_sprintf(arena, "%s", dir_entry->d_name),
                .type        = info.type,
                .size        = info.size,
                .modified_at = info.modified_at,
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

    // A scratch arena per level, so memory tracks the depth of the tree rather than its total size.
    // Walking a large tree otherwise grows without bound, which is exactly what a file browser does.
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
        // remove a directory that still has contents. Symlinks are not followed: a link pointing at
        // an ancestor would otherwise walk forever.
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

        // A symlink is recreated as a symlink. Copying its contents instead would silently turn a
        // link into a full duplicate of its target, which for a tree copy is both wrong and a way
        // to accidentally expand a small tree into a huge one.
        NYA_FileInfo info;
        NYA_TRY(nya_filesystem_info(source, &info));

        if (info.type == NYA_FILE_TYPE_SYMLINK) {
            char    target[PATH_MAX];
            ssize_t length = readlink(source, target, sizeof(target) - 1);
            if (length < 0) return nya_error_from_errno();

            target[length] = '\0';
            if (symlink(target, destination) != 0) return nya_error_from_errno();

            return NYA_OK;
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

    s32 flags = 0;
    if ((mode & NYA_FILE_MODE_READ) && (mode & NYA_FILE_MODE_WRITE)) {
        flags = O_RDWR;
    } else if (mode & NYA_FILE_MODE_WRITE) {
        flags = O_WRONLY;
    } else if (mode & NYA_FILE_MODE_APPEND) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    if (mode & NYA_FILE_MODE_APPEND) flags |= O_APPEND;
    if (mode & NYA_FILE_MODE_TRUNCATE) flags |= O_TRUNC;
    // WRITE and APPEND imply CREATE, since opening to write something that does not exist yet is
    // the common case rather than an error.
    if (mode & (NYA_FILE_MODE_CREATE | NYA_FILE_MODE_WRITE | NYA_FILE_MODE_APPEND)) flags |= O_CREAT;

    s32 descriptor = open(path, flags, 0o644);
    if (descriptor < 0) return nya_error_from_errno();

    *out_file = (NYA_File){ .descriptor = descriptor, .is_open = true };
    return NYA_OK;
}

void nya_file_close(NYA_File* file) {
    if (file == nullptr || !file->is_open) return;

    (void)close(file->descriptor);
    *file = (NYA_File){ .descriptor = -1, .is_open = false };
}

b8 nya_file_is_open(const NYA_File* file) {
    return file != nullptr && file->is_open;
}

NYA_Error nya_file_read_bytes(NYA_File* file, OUT u8* buffer, u64 length, OUT u64* out_read) {
    nya_assert(nya_file_is_open(file), "Cannot read from a file that is not open.");
    nya_assert(buffer != nullptr);
    nya_assert(out_read != nullptr);

    ssize_t got = read(file->descriptor, buffer, length);
    if (got < 0) return nya_error_from_errno();

    *out_read = (u64)got;
    return NYA_OK;
}

NYA_Error nya_file_write_bytes(NYA_File* file, const u8* buffer, u64 length) {
    nya_assert(nya_file_is_open(file), "Cannot write to a file that is not open.");
    nya_assert(buffer != nullptr);

    // write() is allowed to write less than asked, so keep going until it is all out.
    u64 written = 0;
    while (written < length) {
        ssize_t chunk = write(file->descriptor, buffer + written, length - written);
        if (chunk < 0) return nya_error_from_errno();
        written += (u64)chunk;
    }

    return NYA_OK;
}

NYA_Error nya_file_seek(NYA_File* file, s64 offset, NYA_FileSeek origin) {
    nya_assert(nya_file_is_open(file), "Cannot seek a file that is not open.");

    s32 whence = SEEK_SET;
    switch (origin) {
        case NYA_FILE_SEEK_SET:     whence = SEEK_SET; break;
        case NYA_FILE_SEEK_CURRENT: whence = SEEK_CUR; break;
        case NYA_FILE_SEEK_END:     whence = SEEK_END; break;
        default:                    nya_unreachable();
    }

    if (lseek(file->descriptor, (off_t)offset, whence) < 0) return nya_error_from_errno();
    return NYA_OK;
}

NYA_Error nya_file_tell(NYA_File* file, OUT u64* out_offset) {
    nya_assert(nya_file_is_open(file), "Cannot tell the position of a file that is not open.");
    nya_assert(out_offset != nullptr);

    off_t offset = lseek(file->descriptor, 0, SEEK_CUR);
    if (offset < 0) return nya_error_from_errno();

    *out_offset = (u64)offset;
    return NYA_OK;
}

NYA_Error nya_file_truncate(NYA_File* file, u64 length) {
    nya_assert(nya_file_is_open(file), "Cannot truncate a file that is not open.");

    if (ftruncate(file->descriptor, (off_t)length) != 0) return nya_error_from_errno();
    return NYA_OK;
}

NYA_Error nya_file_flush(NYA_File* file) {
    nya_assert(nya_file_is_open(file), "Cannot flush a file that is not open.");

    if (fsync(file->descriptor) != 0) return nya_error_from_errno();
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

    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) return nya_error_from_errno();

    *out_path = nya_string_sprintf(arena, "%s", buffer);
    return NYA_OK;
}

NYA_Error nya_filesystem_working_directory_set(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    if (chdir(path) != 0) return nya_error_from_errno();
    return NYA_OK;
}

NYA_Error nya_filesystem_executable_path(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    char    buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length < 0) return nya_error_from_errno();

    buffer[length] = '\0';
    *out_path      = nya_string_sprintf(arena, "%s", buffer);

    return NYA_OK;
}

NYA_Error nya_filesystem_temp_directory(NYA_Arena* arena, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(out_path != nullptr);

    NYA_ConstCString temp = getenv("TMPDIR");
    if (temp == nullptr || temp[0] == '\0') temp = "/tmp";

    *out_path = nya_string_sprintf(arena, "%s", temp);
    return NYA_OK;
}

NYA_Error nya_filesystem_user_data_directory(NYA_Arena* arena, NYA_ConstCString application, OUT NYA_String** out_path) {
    nya_assert(arena != nullptr);
    nya_assert(application != nullptr);
    nya_assert(out_path != nullptr);

    // XDG first, falling back to the spec's default of ~/.local/share.
    NYA_ConstCString base = getenv("XDG_DATA_HOME");
    NYA_String*      root = nullptr;

    if (base != nullptr && base[0] != '\0') {
        root = nya_string_sprintf(arena, "%s", base);
    } else {
        NYA_ConstCString home = getenv("HOME");
        if (home == nullptr || home[0] == '\0') return nya_error(NYA_ERROR_NOT_FOUND, "neither XDG_DATA_HOME nor HOME is set");
        root = nya_string_sprintf(arena, "%s/.local/share", home);
    }

    *out_path = nya_path_join(arena, nya_string_to_cstring(arena, root), application);
    return NYA_OK;
}
