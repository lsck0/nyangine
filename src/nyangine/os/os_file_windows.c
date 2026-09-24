#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/os/os_file.h"

static_assert(NYA_OS_PATH_MAX == MAX_PATH, "NYA_OS_PATH_MAX must be what the host's ANSI calls take");
static_assert(sizeof(WIN32_FIND_DATAA) <= sizeof(((NYA_OsDirectory*)nullptr)->storage), "NYA_OsDirectory must hold one find data");

// INTERNAL

/** GetLastError as one of the few kinds both platforms can tell apart, so a missing file reads as missing here too. */
NYA_INTERNAL NYA_OsFileStatus _nya_os_file_status(void) {
    switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:     return NYA_OS_FILE_STATUS_NOT_FOUND;

        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: return NYA_OS_FILE_STATUS_DENIED;

        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:    return NYA_OS_FILE_STATUS_EXISTS;

        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER: return NYA_OS_FILE_STATUS_INVALID;

        case ERROR_OUTOFMEMORY:
        case ERROR_NOT_ENOUGH_MEMORY: return NYA_OS_FILE_STATUS_NO_MEMORY;

        default:                      return NYA_OS_FILE_STATUS_IO;
    }
}

/** Win32 counts 100 ns ticks from 1601; NYA_OsFileStat documents milliseconds since the unix epoch. */
NYA_INTERNAL u64 _nya_os_file_time_ms(FILETIME time) {
    ULARGE_INTEGER ticks;
    ticks.LowPart  = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;

    if (ticks.QuadPart == 0) return 0;

    return (ticks.QuadPart / 10000ULL) - 11644473600000ULL;
}

/**
 * `follow_links` false reports a link as a link. True reports what it points at, which here means the
 * link's own directory bit: Windows cannot stat a target without opening it, and that bit mirrors the
 * target's kind, which is what every caller of this was reading before.
 * */
NYA_INTERNAL NYA_OsFileKind _nya_os_file_kind(DWORD attributes, b8 follow_links) {
    if (attributes == INVALID_FILE_ATTRIBUTES) return NYA_OS_FILE_KIND_UNKNOWN;

    b8 directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // A reparse point is checked first: a directory symlink carries both bits, and reporting it as a plain directory makes a tree walk follow links into a loop.
    if (!follow_links && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return NYA_OS_FILE_KIND_SYMLINK;

    return directory ? NYA_OS_FILE_KIND_DIRECTORY : NYA_OS_FILE_KIND_FILE;
}

/** A request clamped to what one Win32 call takes; the caller loops over the rest either way. */
NYA_INTERNAL DWORD _nya_os_file_chunk(u64 size) {
    return size > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (DWORD)size;
}

// HANDLES

NYA_OsFileStatus nya_os_file_open(const char* path, u32 flags, OUT NYA_OsFile* out_file) {
    if (path == nullptr || out_file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DWORD access = 0;
    if (flags & NYA_OS_FILE_OPEN_READ) access |= GENERIC_READ;
    if (flags & NYA_OS_FILE_OPEN_WRITE) access |= GENERIC_WRITE;
    // FILE_APPEND_DATA rather than GENERIC_WRITE, so writes always land at the end even if something else seeks the handle.
    if (flags & NYA_OS_FILE_OPEN_APPEND) access |= FILE_APPEND_DATA;
    if (access == 0) access = GENERIC_READ;

    DWORD creation = OPEN_EXISTING;
    if (flags & NYA_OS_FILE_OPEN_EXCLUSIVE) {
        creation = CREATE_NEW;
    } else if (flags & NYA_OS_FILE_OPEN_TRUNCATE) {
        creation = CREATE_ALWAYS;
    } else if (flags & NYA_OS_FILE_OPEN_CREATE) {
        creation = OPEN_ALWAYS;
    }

    // FILE_SHARE_DELETE too, so a file held open can still be renamed over (how an atomic write replaces it); without it a reader would block every writer.
    HANDLE handle = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return _nya_os_file_status();

    *out_file = (NYA_OsFile){ .handle = handle };
    return NYA_OS_FILE_STATUS_OK;
}

void nya_os_file_close(NYA_OsFile* file) {
    if (file == nullptr || file->handle == nullptr) return;

    (void)CloseHandle((HANDLE)file->handle);
    *file = NYA_OS_FILE_NONE;
}

NYA_OsFileStatus nya_os_file_read(NYA_OsFile* file, OUT u8* buffer, u64 size, OUT u64* out_read) {
    if (file == nullptr || buffer == nullptr || out_read == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DWORD got = 0;
    if (!ReadFile((HANDLE)file->handle, buffer, _nya_os_file_chunk(size), &got, nullptr)) return _nya_os_file_status();

    *out_read = (u64)got;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_write(NYA_OsFile* file, const u8* buffer, u64 size, OUT u64* out_written) {
    if (file == nullptr || buffer == nullptr || out_written == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DWORD chunk = 0;
    if (!WriteFile((HANDLE)file->handle, buffer, _nya_os_file_chunk(size), &chunk, nullptr)) return _nya_os_file_status();

    *out_written = (u64)chunk;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_seek(NYA_OsFile* file, s64 offset, NYA_OsFileSeek origin) {
    if (file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DWORD method = FILE_BEGIN;
    switch (origin) {
        case NYA_OS_FILE_SEEK_SET:     method = FILE_BEGIN; break;
        case NYA_OS_FILE_SEEK_CURRENT: method = FILE_CURRENT; break;
        case NYA_OS_FILE_SEEK_END:     method = FILE_END; break;
        default:                       return NYA_OS_FILE_STATUS_INVALID;
    }

    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    if (!SetFilePointerEx((HANDLE)file->handle, distance, nullptr, method)) return _nya_os_file_status();

    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_tell(NYA_OsFile* file, OUT u64* out_offset) {
    if (file == nullptr || out_offset == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    LARGE_INTEGER zero     = { 0 };
    LARGE_INTEGER position = { 0 };
    if (!SetFilePointerEx((HANDLE)file->handle, zero, &position, FILE_CURRENT)) return _nya_os_file_status();

    *out_offset = (u64)position.QuadPart;
    return NYA_OS_FILE_STATUS_OK;
}

/** Two calls, because SetEndOfFile truncates wherever the pointer is and there is no call that takes a length. */
NYA_OsFileStatus nya_os_file_truncate(NYA_OsFile* file, u64 length) {
    if (file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    NYA_OsFileStatus status = nya_os_file_seek(file, (s64)length, NYA_OS_FILE_SEEK_SET);
    if (status != NYA_OS_FILE_STATUS_OK) return status;

    return SetEndOfFile((HANDLE)file->handle) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_file_sync(NYA_OsFile* file) {
    if (file == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return FlushFileBuffers((HANDLE)file->handle) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

/** Windows has no handle for a directory to flush; nya_os_file_replace writes through instead. */
NYA_OsFileStatus nya_os_directory_sync(const char* path) {
    (void)path;

    return NYA_OS_FILE_STATUS_UNSUPPORTED;
}

// QUERIES

NYA_OsFileStatus nya_os_file_stat(const char* path, b8 follow_links, OUT NYA_OsFileStat* out_stat) {
    if (path == nullptr || out_stat == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return _nya_os_file_status();

    ULARGE_INTEGER size;
    size.LowPart  = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;

    *out_stat = (NYA_OsFileStat){
        .kind        = _nya_os_file_kind(data.dwFileAttributes, follow_links),
        .size        = (u64)size.QuadPart,
        .modified_ms = _nya_os_file_time_ms(data.ftLastWriteTime),
        .created_ms  = _nya_os_file_time_ms(data.ftCreationTime),
        .accessed_ms = _nya_os_file_time_ms(data.ftLastAccessTime),
        .readonly    = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0,
    };

    return NYA_OS_FILE_STATUS_OK;
}

/** The one permission bit Windows has, answered in POSIX shape so a caller need not know which host it is on. */
NYA_OsFileStatus nya_os_file_mode_get(const char* path, OUT u32* out_mode) {
    if (path == nullptr || out_mode == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return _nya_os_file_status();

    *out_mode = (attributes & FILE_ATTRIBUTE_READONLY) ? 0o444U : 0o666U;
    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_file_mode_set(const char* path, u32 mode) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return _nya_os_file_status();

    // the owner's write bit is the whole story: anything else POSIX says here has no meaning on Windows.
    DWORD wanted = (mode & 0o200U) ? (attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY) : (attributes | FILE_ATTRIBUTE_READONLY);
    if (wanted == attributes) return NYA_OS_FILE_STATUS_OK;

    return SetFileAttributesA(path, wanted) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_path_absolute(const char* path, OUT char* out_path, u64 size) {
    if (path == nullptr || out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    DWORD length = GetFullPathNameA(path, _nya_os_file_chunk(size), out_path, nullptr);
    if (length == 0) return _nya_os_file_status();
    if (length >= size) return NYA_OS_FILE_STATUS_INVALID;

    return NYA_OS_FILE_STATUS_OK;
}

// MUTATION

/** MOVEFILE_REPLACE_EXISTING is what makes this behave like rename() does on POSIX. */
NYA_OsFileStatus nya_os_file_rename(const char* source, const char* destination) {
    if (source == nullptr || destination == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

/**
 * No COPY_ALLOWED: a cross-volume copy is not one step, so it fails instead. WRITE_THROUGH is the whole
 * durability story here, since Windows has no way to fsync a directory. The A variant like every other
 * call in this file, so the name the temp file was created under is the one moved.
 *
 * Two replaces racing onto one name, or a scanner holding the file for a moment, refuse with
 * ACCESS_DENIED or a sharing violation that clears by itself. Those come back as BUSY, and the caller
 * waits and asks again; test_file_atomic's racing writers hit this on every Windows run.
 * */
NYA_OsFileStatus nya_os_file_replace(const char* source, const char* destination) {
    if (source == nullptr || destination == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    if (MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return NYA_OS_FILE_STATUS_OK;

    DWORD reason = GetLastError();
    if (reason == ERROR_ACCESS_DENIED || reason == ERROR_SHARING_VIOLATION) return NYA_OS_FILE_STATUS_BUSY;

    return _nya_os_file_status();
}

NYA_OsFileStatus nya_os_file_unlink(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return DeleteFileA(path) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

/** Reading a link needs the file opened with FILE_FLAG_OPEN_REPARSE_POINT and the reparse buffer parsed by hand. */
NYA_OsFileStatus nya_os_file_link_read(const char* path, OUT char* out_target, u64 size) {
    (void)path;
    (void)out_target;
    (void)size;

    return NYA_OS_FILE_STATUS_UNSUPPORTED;
}

/** Creating one needs developer mode or elevation, which is why the flag is there and why this often refuses. */
NYA_OsFileStatus nya_os_file_link_set(const char* path, const char* target) {
    if (path == nullptr || target == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return CreateSymbolicLinkA(path, target, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_directory_create(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return CreateDirectoryA(path, nullptr) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_directory_destroy(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return RemoveDirectoryA(path) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

// DIRECTORY ITERATION

NYA_OsFileStatus nya_os_directory_open(const char* path, OUT NYA_OsDirectory* out_directory) {
    if (path == nullptr || out_directory == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    // FindFirstFile wants a wildcard rather than a directory, this call's own convention, so it is added here rather than by every caller.
    char pattern[NYA_OS_PATH_MAX];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >= (int)sizeof(pattern)) return NYA_OS_FILE_STATUS_INVALID;

    *out_directory = (NYA_OsDirectory){ 0 };

    // straight into the storage, so the entry it hands back with the handle is already where next() looks.
    HANDLE find = FindFirstFileA(pattern, (WIN32_FIND_DATAA*)out_directory->storage);
    if (find == INVALID_HANDLE_VALUE) return _nya_os_file_status();

    out_directory->handle = find;
    // FindFirstFile already returned the first entry, so it waits here for the first next().
    out_directory->pending = true;

    return NYA_OS_FILE_STATUS_OK;
}

b8 nya_os_directory_next(NYA_OsDirectory* directory, OUT NYA_OsDirectoryEntry* out_entry) {
    if (directory == nullptr || directory->handle == nullptr || out_entry == nullptr) return false;

    WIN32_FIND_DATAA* find_data = (WIN32_FIND_DATAA*)directory->storage;

    if (directory->pending) {
        directory->pending = false;
    } else if (!FindNextFileA((HANDLE)directory->handle, find_data)) {
        return false;
    }

    ULARGE_INTEGER size;
    size.LowPart  = find_data->nFileSizeLow;
    size.HighPart = find_data->nFileSizeHigh;

    // the find data already carries everything, so unlike POSIX no caller has to stat the entry.
    *out_entry = (NYA_OsDirectoryEntry){
        .kind         = _nya_os_file_kind(find_data->dwFileAttributes, false),
        .size         = (u64)size.QuadPart,
        .modified_ms  = _nya_os_file_time_ms(find_data->ftLastWriteTime),
        .has_metadata = true,
    };
    (void)snprintf(out_entry->name, sizeof(out_entry->name), "%s", find_data->cFileName);

    return true;
}

void nya_os_directory_close(NYA_OsDirectory* directory) {
    if (directory == nullptr || directory->handle == nullptr) return;

    (void)FindClose((HANDLE)directory->handle);
    *directory = (NYA_OsDirectory){ 0 };
}

// WELL KNOWN LOCATIONS

NYA_OsFileStatus nya_os_working_directory_get(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    DWORD length = GetCurrentDirectoryA(_nya_os_file_chunk(size), out_path);
    if (length == 0) return _nya_os_file_status();
    if (length >= size) return NYA_OS_FILE_STATUS_INVALID;

    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_working_directory_set(const char* path) {
    if (path == nullptr) return NYA_OS_FILE_STATUS_INVALID;

    return SetCurrentDirectoryA(path) ? NYA_OS_FILE_STATUS_OK : _nya_os_file_status();
}

NYA_OsFileStatus nya_os_executable_path(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    DWORD length = GetModuleFileNameA(nullptr, out_path, _nya_os_file_chunk(size));
    if (length == 0) return _nya_os_file_status();
    if (length >= size) return NYA_OS_FILE_STATUS_INVALID;

    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_temp_directory(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    DWORD length = GetTempPathA(_nya_os_file_chunk(size), out_path);
    if (length == 0) return _nya_os_file_status();
    if (length >= size) return NYA_OS_FILE_STATUS_INVALID;

    return NYA_OS_FILE_STATUS_OK;
}

NYA_OsFileStatus nya_os_user_data_directory(OUT char* out_path, u64 size) {
    if (out_path == nullptr || size == 0) return NYA_OS_FILE_STATUS_INVALID;

    // APPDATA rather than SHGetKnownFolderPath, which would drag in shell32 and ole32 for one string.
    const char* roaming = getenv("APPDATA");
    if (roaming == nullptr || roaming[0] == '\0') return NYA_OS_FILE_STATUS_NOT_FOUND;

    if (snprintf(out_path, size, "%s", roaming) >= (int)size) return NYA_OS_FILE_STATUS_INVALID;
    return NYA_OS_FILE_STATUS_OK;
}
