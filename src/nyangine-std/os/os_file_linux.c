#include "nyangine-std/os/os_file.h"

// After the engine's own header: base_basic.h asks for POSIX 2008, and these declare what it wants only once they have seen that.
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

static_assert(NYA_OS_PATH_MAX == PATH_MAX, "NYA_OS_PATH_MAX must be what the host's calls take");

// INTERNAL

/**
 * errno as one of the few kinds both platforms can tell apart. The same split base_error.c makes, so
 * moving a call down here does not change what a caller sees.
 * */
NYA_INTERNAL NYA_OsFileStatus _nya_os_file_status(void) {
    switch (errno) {
        case ENOENT:
        case ESRCH:
        case ENXIO:
        case ENODEV:
        case ENOTDIR:      return NYA_OS_FILE_STATUS_NOT_FOUND;

        case EACCES:
        case EPERM:        return NYA_OS_FILE_STATUS_DENIED;

        case EEXIST:       return NYA_OS_FILE_STATUS_EXISTS;

        case EINVAL:
        case ENAMETOOLONG: return NYA_OS_FILE_STATUS_INVALID;

        case ENOMEM:       return NYA_OS_FILE_STATUS_NO_MEMORY;

        case ENOSYS:
        case ENOTSUP:      return NYA_OS_FILE_STATUS_UNSUPPORTED;

        case EIO:          return NYA_OS_FILE_STATUS_IO;

        default:           return NYA_OS_FILE_STATUS_FAILED;
    }
}

/** A stat timestamp as the milliseconds since the unix epoch NYA_OsFileStat documents. */
NYA_INTERNAL u64 _nya_os_file_time_ms(struct timespec time) {
    return (u64)time.tv_sec * 1000ULL + (u64)(time.tv_nsec / 1000000L);
}

NYA_INTERNAL NYA_OsFileKind _nya_os_file_kind(mode_t mode) {
    if (S_ISDIR(mode)) return NYA_OS_FILE_KIND_DIRECTORY;
    if (S_ISLNK(mode)) return NYA_OS_FILE_KIND_SYMLINK;
    if (S_ISREG(mode)) return NYA_OS_FILE_KIND_FILE;

    return NYA_OS_FILE_KIND_UNKNOWN;
}

// HANDLES

NYA_OsFileStatus nya_os_file_open(const char* path, u32 flags, OUT NYA_OsFile* out_file) {
    if (path == nullptr || out_file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    // append is a kind of write, so READ with APPEND is read-write like READ with WRITE.
    b8  reads      = (flags & NYA_OS_FILE_OPEN_READ) != 0;
    b8  writes     = (flags & (NYA_OS_FILE_OPEN_WRITE | NYA_OS_FILE_OPEN_APPEND)) != 0;
    s32 open_flags = O_CLOEXEC;
    if (reads && writes) {
        open_flags |= O_RDWR;
    } else if (writes) {
        open_flags |= O_WRONLY;
    } else {
        open_flags |= O_RDONLY;
    }

    if (flags & NYA_OS_FILE_OPEN_APPEND) open_flags |= O_APPEND;
    if (flags & NYA_OS_FILE_OPEN_TRUNCATE) open_flags |= O_TRUNC;
    // O_EXCL without O_CREAT is undefined, so exclusive only means anything on an open that may create.
    if (flags & NYA_OS_FILE_OPEN_EXCLUSIVE) open_flags |= O_EXCL;
    if (flags & NYA_OS_FILE_OPEN_CREATE) open_flags |= O_CREAT;

    s32 descriptor = open(path, open_flags, 0o644);
    if (descriptor < 0) return _nya_os_file_status();

    *out_file = (NYA_OsFile){ .descriptor = descriptor };
    return NYA_OS_FILE_STATUS_OK;
}

void nya_os_file_close(NYA_OsFile* file) {
    if (file == nullptr || file->descriptor < 0) return;

    (void)close(file->descriptor);
    *file = NYA_OS_FILE_NONE;
}

NYA_OsFileStatus nya_os_file_read(NYA_OsFile* file, OUT u8* buffer, u64 size, OUT u64* out_read) {
    if (file == nullptr || buffer == nullptr || out_read == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    ssize_t got = read(file->descriptor, buffer, size);
    if (got < 0) return _nya_os_file_status();

    *out_read = (u64)got;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_write(NYA_OsFile* file, const u8* buffer, u64 size, OUT u64* out_written) {
    if (file == nullptr || buffer == nullptr || out_written == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    ssize_t chunk = write(file->descriptor, buffer, size);
    if (chunk < 0) return _nya_os_file_status();

    *out_written = (u64)chunk;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_seek(NYA_OsFile* file, s64 offset, NYA_OsFileSeek origin) {
    if (file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    s32 whence = SEEK_SET;
    switch (origin) {
        case NYA_OS_FILE_SEEK_SET:     whence = SEEK_SET; break;
        case NYA_OS_FILE_SEEK_CURRENT: whence = SEEK_CUR; break;
        case NYA_OS_FILE_SEEK_END:     whence = SEEK_END; break;
        default:                       return NYA_OS_FILE_STATUS_INVALID;
    }

    return lseek(file->descriptor, (off_t)offset, whence) >= 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_file_tell(NYA_OsFile* file, OUT u64* out_offset) {
    if (file == nullptr || out_offset == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    off_t offset = lseek(file->descriptor, 0, SEEK_CUR);
    if (offset < 0) return _nya_os_file_status();

    *out_offset = (u64)offset;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_truncate(NYA_OsFile* file, u64 length) {
    if (file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return ftruncate(file->descriptor, (off_t)length) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_file_sync(NYA_OsFile* file) {
    if (file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return fsync(file->descriptor) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_directory_sync(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    s32 descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return _nya_os_file_status();

    NYA_OsFileStatus status = fsync(descriptor) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
    (void)close(descriptor);

    return status;
}

// QUERIES

NYA_OsFileStatus nya_os_file_stat(const char* path, b8 follow_links, OUT NYA_OsFileStat* out_stat) {
    if (path == nullptr || out_stat == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    struct stat info;
    if ((follow_links ? stat(path, &info) : lstat(path, &info)) != 0) return _nya_os_file_status();

    *out_stat = (NYA_OsFileStat){
        .kind        = _nya_os_file_kind(info.st_mode),
        .size        = (u64)info.st_size,
        .modified_ms = _nya_os_file_time_ms(info.st_mtim),
        .created_ms  = _nya_os_file_time_ms(info.st_ctim),
        .accessed_ms = _nya_os_file_time_ms(info.st_atim),
        // what this process may do with it, which is the question a caller is actually asking.
        .readonly = access(path, W_OK) != 0,
    };

    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_mode_get(const char* path, OUT u32* out_mode) {
    if (path == nullptr || out_mode == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    struct stat info;
    if (stat(path, &info) != 0) return _nya_os_file_status();

    *out_mode = (u32)(info.st_mode & 0o7777);
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_mode_set(const char* path, u32 mode) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return chmod(path, (mode_t)(mode & 0o7777)) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_path_absolute(const char* path, OUT char* out_path, u64 size) {
    // realpath writes up to PATH_MAX and has no way to be told less.
    if (path == nullptr || out_path == nullptr || size < NYA_OS_PATH_MAX) return NYA_OS_FILE_STATUS_INVALID;

    if (realpath(path, out_path) == nullptr) return _nya_os_file_status();
    return NYA_OS_FILE_STATUS_OK;
}

// MUTATION

NYA_OsFileStatus nya_os_file_rename(const char* source, const char* destination) {
    if (source == nullptr || destination == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return rename(source, destination) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

// rename() is the atomic replace and has no transient refusal, so this never answers BUSY and the caller's retry runs once.
NYA_OsFileStatus nya_os_file_replace(const char* source, const char* destination) {
    if (source == nullptr || destination == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return rename(source, destination) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_file_unlink(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return unlink(path) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_file_link_read(const char* path, OUT char* out_target, u64 size) {
    if (path == nullptr || out_target == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    ssize_t length = readlink(path, out_target, size - 1);
    if (length < 0) return _nya_os_file_status();

    out_target[length] = '\0';
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_link_set(const char* path, const char* target) {
    if (path == nullptr || target == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return symlink(target, path) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_directory_create(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return mkdir(path, 0o755) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_directory_destroy(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return rmdir(path) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

// DIRECTORY ITERATION

NYA_OsFileStatus nya_os_directory_open(const char* path, OUT NYA_OsDirectory* out_directory) {
    if (path == nullptr || out_directory == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DIR* directory = opendir(path);
    if (directory == nullptr) return _nya_os_file_status();

    *out_directory = (NYA_OsDirectory){ .handle = directory };
    return NYA_OS_FILE_STATUS_OK;
}

b8 nya_os_directory_next(NYA_OsDirectory* directory, OUT NYA_OsDirectoryEntry* out_entry) {
    if (directory == nullptr || directory->handle == nullptr || out_entry == nullptr) return false;

    struct dirent* entry = readdir(directory->handle);
    if (entry == nullptr) return false;

    // readdir carries a name and, at best, a kind; the caller stats the entry if it wants the rest.
    *out_entry = (NYA_OsDirectoryEntry){ .kind = NYA_OS_FILE_KIND_UNKNOWN, .has_metadata = false };
    (void)snprintf(out_entry->name, sizeof(out_entry->name), "%s", entry->d_name);

    return true;
}

void nya_os_directory_close(NYA_OsDirectory* directory) {
    if (directory == nullptr || directory->handle == nullptr) return;

    (void)closedir(directory->handle);
    *directory = (NYA_OsDirectory){ 0 };
}

// WELL KNOWN LOCATIONS

NYA_OsFileStatus nya_os_working_directory_get(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    if (getcwd(out_path, size) == nullptr) return _nya_os_file_status();
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_working_directory_set(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return chdir(path) == 0 ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_executable_path(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    ssize_t length = readlink("/proc/self/exe", out_path, size - 1);
    if (length < 0) return _nya_os_file_status();

    out_path[length] = '\0';
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_temp_directory(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    const char* temp = getenv("TMPDIR");
    if (temp == nullptr || temp[0] == '\0') temp = "/tmp";

    if (snprintf(out_path, size, "%s", temp) >= (int)size) return NYA_OS_FILE_STATUS_INVALID;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_user_data_directory(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    // XDG first, falling back to the spec's default of ~/.local/share.
    const char* base = getenv("XDG_DATA_HOME");
    if (base != nullptr && base[0] != '\0') {
        if (snprintf(out_path, size, "%s", base) >= (int)size) return NYA_OS_FILE_STATUS_INVALID;
        return NYA_OS_FILE_STATUS_OK;
    }

    const char* home = getenv("HOME");
    if (home == nullptr || home[0] == '\0') return NYA_OS_FILE_STATUS_NOT_FOUND;

    if (snprintf(out_path, size, "%s/.local/share", home) >= (int)size) return NYA_OS_FILE_STATUS_INVALID;
    return NYA_OS_FILE_STATUS_OK;
}
