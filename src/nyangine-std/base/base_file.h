#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/base/base_filesystem.h"

// FILE DESCRIPTOR FUNCTIONS

/* Whole-content helpers over an open NYA_File. These took a raw s32 fd before, quietly POSIX-only; NYA_File wraps a descriptor or a Windows HANDLE, so the same code works on both. */

NYA_API NYA_Error nya_file_read_string(NYA_File* file, OUT NYA_String* out_content) __attr_no_discard;
NYA_API NYA_Error nya_file_write_string(NYA_File* file, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write_string(NYA_File* file, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append_string(NYA_File* file, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append_string(NYA_File* file, NYA_ConstCString content) __attr_overloaded __attr_no_discard;

// FILE FUNCTIONS

NYA_API NYA_Error nya_file_read(const char* path, OUT NYA_String* out_content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_read(const NYA_String* path, OUT NYA_String* out_content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const char* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const NYA_String* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const char* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const NYA_String* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const char* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const NYA_String* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const char* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const NYA_String* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;

// ATOMIC WRITE

/**
 * Tries before giving up on finding an unused temp name. The name carries the pid and a counter, so a
 * clash means a stale temp from a dead process that had the same pid; eight is past any real run of those.
 * */
#define NYA_FILE_ATOMIC_ATTEMPTS_MAX 8

/** The steps of an atomic write, in order. A test can make any one of them fail; see below. */
typedef enum {
    NYA_FILE_ATOMIC_STEP_OPEN,
    NYA_FILE_ATOMIC_STEP_WRITE,
    NYA_FILE_ATOMIC_STEP_SYNC,
    NYA_FILE_ATOMIC_STEP_REPLACE,

    NYA_FILE_ATOMIC_STEP_COUNT,
} NYA_FileAtomicStep;

/**
 * Replaces all of `path` with `content`, so that after a crash anywhere it holds the old bytes or the new.
 * Never a mix, whether the crash is the process or the power. A file that did not exist before either
 * still does not or holds all of `content`.
 *
 * Writes `<path>.<pid>.<counter>.tmp` beside the target, fsyncs it, renames it over the target and
 * fsyncs the directory (Windows writes the rename through instead). Beside, not in the temp directory,
 * because a rename across volumes is a copy. A failed write removes its temp file and leaves the target
 * alone. A symlink is followed, so the file it points at is what gets replaced, and the target's
 * permission bits are kept.
 *
 * What it cannot promise: a disk that acknowledges a flush it has not done can still lose the new bytes
 * or the old ones, and a crash (as opposed to a failure) leaves the temp file behind.
 *
 * ```c
 * NYA_TRY(nya_file_write_atomic("settings.nya", text));
 * ```
 *
 * Rejected: truncating and rewriting in place, which is nya_file_write, since a crash in the middle
 * leaves half a file and the old contents are already gone.
 * */
NYA_API NYA_Error nya_file_write_atomic(const char* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write_atomic(const char* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;

#ifdef NYA_TESTING

/** What an armed step does when it is reached. */
typedef enum {
    NYA_FILE_ATOMIC_FAULT_NONE,
    /** Returns an error and cleans up, as when the disk fills. */
    NYA_FILE_ATOMIC_FAULT_FAIL,
    /** Returns at once without cleaning up, which is what the disk looks like after the process dies there. */
    NYA_FILE_ATOMIC_FAULT_CRASH,

    NYA_FILE_ATOMIC_FAULT_COUNT,
} NYA_FileAtomicFault;

/**
 * Arms `fault` for the next write on this thread that reaches `step`, just before the step runs. Fires
 * once and disarms. Thread local, so a test arming it cannot hit a write on another thread.
 * */
NYA_API void nya_file_write_atomic_fault_set(NYA_FileAtomicStep step, NYA_FileAtomicFault fault);

#endif // NYA_TESTING
