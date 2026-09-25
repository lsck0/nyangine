/**
 * @file os_file.h
 *
 * The file system as the operating system offers it: one call per function, paths as `const char*`,
 * results into the caller's buffer, and a status enum instead of an error.
 *
 * ```c
 * NYA_OsFile file;
 * if (nya_os_file_open("save.nya", NYA_OS_FILE_OPEN_READ, &file) != NYA_OS_FILE_STATUS_OK) return;
 * defer nya_os_file_close(&file);
 * ```
 *
 * Everything the engine actually calls — NYA_File, walking a tree, `mkdir -p`, a recursive copy, the
 * atomic replace — is arithmetic and string work over these, and lives once in base/base_filesystem.h
 * rather than twice here. What is left is what genuinely differs between a POSIX descriptor and a
 * Windows HANDLE.
 *
 * Two of these are not literally one call, and for the same reason both times: the platform has no
 * single call for what the other one does in one. They are named where they are defined.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * The longest path the host's calls take, which is what every buffer here is sized to. Windows is
 * MAX_PATH because these are the ANSI entry points, which refuse more whatever the volume supports;
 * Linux is PATH_MAX. Both are static_asserted against the real constant where they are used.
 * */
#if OS_WINDOWS
#define NYA_OS_PATH_MAX 260
#else
#define NYA_OS_PATH_MAX 4096
#endif

/** One directory entry's name. Covers both `struct dirent.d_name` and `WIN32_FIND_DATAA.cFileName`. */
#define NYA_OS_FILE_NAME_MAX 260

/** How an open may be asked for, as flags because a combination is not itself an enumerator. */
enum {
    NYA_OS_FILE_OPEN_READ   = 1 << 0,
    NYA_OS_FILE_OPEN_WRITE  = 1 << 1,
    NYA_OS_FILE_OPEN_APPEND = 1 << 2,
    /** Create if missing. */
    NYA_OS_FILE_OPEN_CREATE = 1 << 3,
    /** Discard existing contents on open. */
    NYA_OS_FILE_OPEN_TRUNCATE = 1 << 4,
    /** Refuse with NYA_OS_FILE_STATUS_EXISTS rather than open a file that is already there. */
    NYA_OS_FILE_OPEN_EXCLUSIVE = 1 << 5,
};

// TYPES

typedef enum NYA_OsFileStatus       NYA_OsFileStatus;
typedef enum NYA_OsFileKind         NYA_OsFileKind;
typedef enum NYA_OsFileSeek         NYA_OsFileSeek;
typedef struct NYA_OsFile           NYA_OsFile;
typedef struct NYA_OsFileStat       NYA_OsFileStat;
typedef struct NYA_OsDirectory      NYA_OsDirectory;
typedef struct NYA_OsDirectoryEntry NYA_OsDirectoryEntry;

/**
 * Why a call did not work, in the few kinds both platforms can tell apart. This is as much as os says:
 * it is below the error machinery, so what a refusal *means* is the caller's to decide.
 * */
enum NYA_OsFileStatus {
    NYA_OS_FILE_STATUS_OK,
    NYA_OS_FILE_STATUS_NOT_FOUND,
    NYA_OS_FILE_STATUS_DENIED,
    NYA_OS_FILE_STATUS_EXISTS,
    /** Refused for a reason that clears by itself; only nya_os_file_replace on Windows reports it. */
    NYA_OS_FILE_STATUS_BUSY,
    NYA_OS_FILE_STATUS_INVALID,
    NYA_OS_FILE_STATUS_NO_MEMORY,
    /** The host has no such call at all, which is a fact about the platform rather than a failure. */
    NYA_OS_FILE_STATUS_UNSUPPORTED,
    NYA_OS_FILE_STATUS_IO,
    NYA_OS_FILE_STATUS_FAILED,

    NYA_OS_FILE_STATUS_COUNT,
};

__attr_allow_unused static NYA_ConstCString NYA_OSFILESTATUS_NAME_MAP[NYA_OS_FILE_STATUS_COUNT] = {
    [NYA_OS_FILE_STATUS_OK]          = "OK",
    [NYA_OS_FILE_STATUS_NOT_FOUND]   = "NOT_FOUND",
    [NYA_OS_FILE_STATUS_DENIED]      = "DENIED",
    [NYA_OS_FILE_STATUS_EXISTS]      = "EXISTS",
    [NYA_OS_FILE_STATUS_BUSY]        = "BUSY",
    [NYA_OS_FILE_STATUS_INVALID]     = "INVALID",
    [NYA_OS_FILE_STATUS_NO_MEMORY]   = "NO_MEMORY",
    [NYA_OS_FILE_STATUS_UNSUPPORTED] = "UNSUPPORTED",
    [NYA_OS_FILE_STATUS_IO]          = "IO",
    [NYA_OS_FILE_STATUS_FAILED]      = "FAILED",
};

/** What is at a path. A stat that follows links never reports a link; see nya_os_file_stat. */
enum NYA_OsFileKind {
    NYA_OS_FILE_KIND_UNKNOWN,
    NYA_OS_FILE_KIND_FILE,
    NYA_OS_FILE_KIND_DIRECTORY,
    NYA_OS_FILE_KIND_SYMLINK,
};

enum NYA_OsFileSeek {
    NYA_OS_FILE_SEEK_SET,
    NYA_OS_FILE_SEEK_CURRENT,
    NYA_OS_FILE_SEEK_END,
};

/** An open file: a descriptor on POSIX, a HANDLE on Windows. Whether it is open is the caller's to track. */
struct NYA_OsFile {
#if OS_WINDOWS
    void* handle;
#else
    s32 descriptor;
#endif
};

/** A handle naming nothing, which is what a closed file holds. */
#if OS_WINDOWS
#define NYA_OS_FILE_NONE ((NYA_OsFile){ .handle = nullptr })
#else
#define NYA_OS_FILE_NONE ((NYA_OsFile){ .descriptor = -1 })
#endif

/** Timestamps are milliseconds since the unix epoch on both platforms. */
struct NYA_OsFileStat {
    NYA_OsFileKind kind;
    u64            size;
    u64            modified_ms;
    u64            created_ms;
    u64            accessed_ms;
    b8             readonly;
};

/**
 * A directory being read. Opaque: only os_file_*.c may name what is in it.
 *
 * The storage is Windows' alone. FindFirstFile hands back the first entry with the handle, so it has
 * to wait somewhere until the first nya_os_directory_next asks for it, and a WIN32_FIND_DATAA cannot
 * be named here without windows.h. os_file_windows.c static_asserts that it fits.
 * */
struct NYA_OsDirectory {
    void* handle;
#if OS_WINDOWS
    b8  pending;
    u64 storage[64];
#endif
};

/**
 * One entry of a listing. `name` is the entry's own name, never a path.
 *
 * `has_metadata` is false where the listing call does not carry it: POSIX `readdir` gives a name and
 * little else, so a caller that wants the rest stats the entry itself, while Windows' find data
 * already holds all of it and a second call would be waste.
 * */
struct NYA_OsDirectoryEntry {
    char           name[NYA_OS_FILE_NAME_MAX];
    NYA_OsFileKind kind;
    u64            size;
    u64            modified_ms;
    b8             has_metadata;
};

// FUNCTIONS

// HANDLES

/** `flags` is NYA_OS_FILE_OPEN_* flags, a u32 because a combination is not itself an enumerator. */
NYA_API NYA_OsFileStatus nya_os_file_open(const char* path, u32 flags, OUT NYA_OsFile* out_file) __attr_no_discard;
NYA_API void             nya_os_file_close(NYA_OsFile* file);

/** Reads up to `size` bytes. `out_read` receives how many came back, 0 at end of file. */
NYA_API NYA_OsFileStatus nya_os_file_read(NYA_OsFile* file, OUT u8* buffer, u64 size, OUT u64* out_read) __attr_no_discard;

/** Writes up to `size` bytes and reports how many landed: both platforms may take fewer than asked. */
NYA_API NYA_OsFileStatus nya_os_file_write(NYA_OsFile* file, const u8* buffer, u64 size, OUT u64* out_written) __attr_no_discard;

NYA_API NYA_OsFileStatus nya_os_file_seek(NYA_OsFile* file, s64 offset, NYA_OsFileSeek origin) __attr_no_discard;
NYA_API NYA_OsFileStatus nya_os_file_tell(NYA_OsFile* file, OUT u64* out_offset) __attr_no_discard;
NYA_API NYA_OsFileStatus nya_os_file_truncate(NYA_OsFile* file, u64 length) __attr_no_discard;

/**
 * Puts what has been written on the device. There is no separate buffered flush because these handles
 * have no buffer of their own: nothing sits between a write here and the kernel, so flushing and
 * syncing are the same call.
 * */
NYA_API NYA_OsFileStatus nya_os_file_sync(NYA_OsFile* file) __attr_no_discard;

/**
 * The same for a directory, which is what makes a rename inside it survive a power cut. Windows has no
 * such call and reports NYA_OS_FILE_STATUS_UNSUPPORTED; its durability comes from the replace itself.
 * */
NYA_API NYA_OsFileStatus nya_os_directory_sync(const char* path) __attr_no_discard;

// QUERIES

/**
 * Everything one stat knows.
 *
 * `follow_links` false reports a symlink as a symlink; true reports what it points at, so the kind is
 * never NYA_OS_FILE_KIND_SYMLINK.
 * */
NYA_API NYA_OsFileStatus nya_os_file_stat(const char* path, b8 follow_links, OUT NYA_OsFileStat* out_stat) __attr_no_discard;

/**
 * The permission bits, POSIX style and masked to 0o7777. Windows has no such thing and answers with the
 * one bit it does have: 0o444 for a read-only file and 0o666 otherwise.
 * */
NYA_API NYA_OsFileStatus nya_os_file_mode_get(const char* path, OUT u32* out_mode) __attr_no_discard;
NYA_API NYA_OsFileStatus nya_os_file_mode_set(const char* path, u32 mode) __attr_no_discard;

/** Resolves links and relative segments. The result is in the host's own spelling, backslashes and all. */
NYA_API NYA_OsFileStatus nya_os_path_absolute(const char* path, OUT char* out_path, u64 size) __attr_no_discard;

// MUTATION

/** Moves `source` onto `destination`, across volumes too, replacing whatever was there. */
NYA_API NYA_OsFileStatus nya_os_file_rename(const char* source, const char* destination) __attr_no_discard;

/**
 * The same in one step, so a reader finds the old file or the new one and never half of either, and as
 * durable as the host will say. One volume only: a copy is not one step.
 *
 * NYA_OS_FILE_STATUS_BUSY means try again in a moment; see os_file_windows.c for what produces it.
 * */
NYA_API NYA_OsFileStatus nya_os_file_replace(const char* source, const char* destination) __attr_no_discard;

/** Removes one name. Refuses a directory; that is nya_os_directory_destroy. */
NYA_API NYA_OsFileStatus nya_os_file_unlink(const char* path) __attr_no_discard;

/** Where a symlink points, null terminated. Windows cannot read one and answers UNSUPPORTED. */
NYA_API NYA_OsFileStatus nya_os_file_link_read(const char* path, OUT char* out_target, u64 size) __attr_no_discard;

/** A new symlink at `path` pointing at `target`. Path first, unlike symlink(2), to match every call here. */
NYA_API NYA_OsFileStatus nya_os_file_link_set(const char* path, const char* target) __attr_no_discard;

/** One directory, whose parent must exist. NYA_OS_FILE_STATUS_EXISTS when something is already there. */
NYA_API NYA_OsFileStatus nya_os_directory_create(const char* path) __attr_no_discard;

/** Removes one empty directory. */
NYA_API NYA_OsFileStatus nya_os_directory_destroy(const char* path) __attr_no_discard;

// DIRECTORY ITERATION

NYA_API NYA_OsFileStatus nya_os_directory_open(const char* path, OUT NYA_OsDirectory* out_directory) __attr_no_discard;

/** The next entry, `.` and `..` included, or false once there are none left. */
NYA_API b8   nya_os_directory_next(NYA_OsDirectory* directory, OUT NYA_OsDirectoryEntry* out_entry) __attr_no_discard;
NYA_API void nya_os_directory_close(NYA_OsDirectory* directory);

// WELL KNOWN LOCATIONS

NYA_API NYA_OsFileStatus nya_os_working_directory_get(OUT char* out_path, u64 size) __attr_no_discard;
NYA_API NYA_OsFileStatus nya_os_working_directory_set(const char* path) __attr_no_discard;

/** Absolute path of the running executable. */
NYA_API NYA_OsFileStatus nya_os_executable_path(OUT char* out_path, u64 size) __attr_no_discard;

NYA_API NYA_OsFileStatus nya_os_temp_directory(OUT char* out_path, u64 size) __attr_no_discard;

/** Where this user's own data goes, without an application name on it: that is the caller's to join. */
NYA_API NYA_OsFileStatus nya_os_user_data_directory(OUT char* out_path, u64 size) __attr_no_discard;
