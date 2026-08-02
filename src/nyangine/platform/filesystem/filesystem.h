/**
 * @file filesystem.h
 *
 * Filesystem access: metadata, directory traversal, and the mutating operations a file manager
 * needs (copy, move, delete, including recursively).
 *
 * Listing returns metadata per entry rather than just names, because the alternative is a stat
 * syscall per row, which is what makes naive directory listings slow.
 *
 * Path *manipulation* is deliberately not here. Joining, splitting and normalising paths is pure
 * string work with no syscalls behind it, so it lives in base_path.h where it can be tested
 * without touching a disk.
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bounds the recursion in walk and copy, so a symlink loop fails loudly instead of blowing the stack. */
#define NYA_FILESYSTEM_WALK_DEPTH_MAX 256

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_FileType         NYA_FileType;
typedef struct NYA_FileInfo       NYA_FileInfo;
typedef struct NYA_DirectoryEntry NYA_DirectoryEntry;

enum NYA_FileType {
    NYA_FILE_TYPE_UNKNOWN,
    NYA_FILE_TYPE_FILE,
    NYA_FILE_TYPE_DIRECTORY,
    NYA_FILE_TYPE_SYMLINK,

    NYA_FILE_TYPE_COUNT,
};

__attr_allow_unused static NYA_ConstCString NYA_FILETYPE_NAME_MAP[NYA_FILE_TYPE_COUNT] = {
    [NYA_FILE_TYPE_UNKNOWN]   = "UNKNOWN",
    [NYA_FILE_TYPE_FILE]      = "FILE",
    [NYA_FILE_TYPE_DIRECTORY] = "DIRECTORY",
    [NYA_FILE_TYPE_SYMLINK]   = "SYMLINK",
};

/** Timestamps are seconds since the unix epoch on both platforms. */
struct NYA_FileInfo {
    NYA_FileType type;
    u64          size;
    u64          modified_at;
    u64          created_at;
    u64          accessed_at;
    b8           readonly;
};

/** One entry of a directory listing. `name` is the entry name, not a full path. */
struct NYA_DirectoryEntry {
    NYA_String*  name;
    NYA_FileType type;
    u64          size;
    u64          modified_at;
};

nya_derive_array(NYA_DirectoryEntry);

/**
 * Called once per entry while walking. Return false to stop the walk early, which is what makes it
 * usable for "find the first match" without listing everything.
 *
 * `path` is the full path of the entry, `entry` its metadata.
 * */
typedef b8 (*NYA_WalkCallback)(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

/** True if anything exists at `path`, file or directory. */
NYA_API b8 nya_filesystem_exists(NYA_ConstCString path) __attr_no_discard;

NYA_API b8 nya_filesystem_is_file(NYA_ConstCString path) __attr_no_discard;
NYA_API b8 nya_filesystem_is_directory(NYA_ConstCString path) __attr_no_discard;

/** Everything known about one entry, in a single stat. */
NYA_API NYA_Error nya_filesystem_info(NYA_ConstCString path, OUT NYA_FileInfo* out_info) __attr_no_discard;

NYA_API NYA_Error nya_filesystem_size(NYA_ConstCString path, OUT u64* out_size) __attr_no_discard;
NYA_API NYA_Error nya_filesystem_last_modified(NYA_ConstCString path, OUT u64* out_timestamp) __attr_no_discard;

/** Resolves symlinks and relative segments into an absolute path. */
NYA_API NYA_Error nya_filesystem_absolute(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_String** out_path) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * MUTATION
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_filesystem_move(NYA_ConstCString source, NYA_ConstCString destination) __attr_no_discard;
NYA_API NYA_Error nya_filesystem_copy(NYA_ConstCString source, NYA_ConstCString destination) __attr_no_discard;
NYA_API NYA_Error nya_filesystem_delete(NYA_ConstCString path) __attr_no_discard;

/** Creates a directory and every missing parent, like `mkdir -p`. Succeeds if it already exists. */
NYA_API NYA_Error nya_filesystem_create_directory(NYA_ConstCString path) __attr_no_discard;

/** Deletes a directory and everything under it. Deleting a plain file works too. */
NYA_API NYA_Error nya_filesystem_delete_recursive(NYA_ConstCString path) __attr_no_discard;

/** Copies a directory tree. Copying a plain file works too. Destination parents are created. */
NYA_API NYA_Error nya_filesystem_copy_recursive(NYA_ConstCString source, NYA_ConstCString destination) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DIRECTORIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Lists one directory level, metadata included. `.` and `..` are omitted.
 *
 * Order is whatever the filesystem hands back, which is not sorted; sorting is the caller's
 * business since a browser wants to choose the key.
 * */
NYA_API NYA_Error nya_filesystem_list(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_ArrayᐸNYA_DirectoryEntryᐳ** out_entries) __attr_no_discard;

/** Depth first walk of everything under `path`. Children are visited before their parent. */
NYA_API NYA_Error nya_filesystem_walk(NYA_Arena* arena, NYA_ConstCString path, NYA_WalkCallback callback, void* user_data) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * FILE HANDLES
 * ─────────────────────────────────────────────────────────
 */

/**
 * An open file.
 *
 * Opaque and by value rather than a raw descriptor: POSIX hands back an int and Windows a HANDLE,
 * and the old nya_fd_* functions took an `s32 fd` directly, which quietly made them Linux only.
 * */
typedef struct NYA_File NYA_File;

typedef enum {
    NYA_FILE_MODE_READ   = 1 << 0,
    NYA_FILE_MODE_WRITE  = 1 << 1,
    NYA_FILE_MODE_APPEND = 1 << 2,
    /** Create if missing. Implied by WRITE and APPEND. */
    NYA_FILE_MODE_CREATE = 1 << 3,
    /** Discard existing contents on open. */
    NYA_FILE_MODE_TRUNCATE = 1 << 4,
} NYA_FileMode;

typedef enum {
    NYA_FILE_SEEK_SET,
    NYA_FILE_SEEK_CURRENT,
    NYA_FILE_SEEK_END,
} NYA_FileSeek;

struct NYA_File {
#if OS_WINDOWS
    void* handle;
#else
    s32 descriptor;
#endif
    b8 is_open;
};

NYA_API NYA_Error nya_file_open(NYA_ConstCString path, NYA_FileMode mode, OUT NYA_File* out_file) __attr_no_discard;
NYA_API void      nya_file_close(NYA_File* file);
NYA_API b8        nya_file_is_open(const NYA_File* file) __attr_no_discard;

/** Reads up to `length` bytes. `out_read` receives how many actually came back, 0 at end of file. */
NYA_API NYA_Error nya_file_read_bytes(NYA_File* file, OUT u8* buffer, u64 length, OUT u64* out_read) __attr_no_discard;
NYA_API NYA_Error nya_file_write_bytes(NYA_File* file, const u8* buffer, u64 length) __attr_no_discard;

NYA_API NYA_Error nya_file_seek(NYA_File* file, s64 offset, NYA_FileSeek origin) __attr_no_discard;
NYA_API NYA_Error nya_file_tell(NYA_File* file, OUT u64* out_offset) __attr_no_discard;
NYA_API NYA_Error nya_file_truncate(NYA_File* file, u64 length) __attr_no_discard;
NYA_API NYA_Error nya_file_flush(NYA_File* file) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * WELL KNOWN LOCATIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_filesystem_working_directory(NYA_Arena* arena, OUT NYA_String** out_path) __attr_no_discard;
NYA_API NYA_Error nya_filesystem_working_directory_set(NYA_ConstCString path) __attr_no_discard;

/** Absolute path of the running executable. */
NYA_API NYA_Error nya_filesystem_executable_path(NYA_Arena* arena, OUT NYA_String** out_path) __attr_no_discard;

NYA_API NYA_Error nya_filesystem_temp_directory(NYA_Arena* arena, OUT NYA_String** out_path) __attr_no_discard;

/** Per user writable location for saves and logs. `application` names the subdirectory. */
NYA_API NYA_Error nya_filesystem_user_data_directory(NYA_Arena* arena, NYA_ConstCString application, OUT NYA_String** out_path) __attr_no_discard;
