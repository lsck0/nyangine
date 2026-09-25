/**
 * @file base_version.h
 *
 * What this binary is: version, commit, build kind, and when it was built.
 *
 * ```c
 * NYA_BuildInfo info = nya_build_info();
 * nya_log_info("%s %s (%s), built %s", info.version, info.commit, info.kind, info.built);
 *
 * // Or the one line a corner of a menu shows, and a bug report quotes back.
 * u8 line[NYA_BUILD_LINE_MAX];
 * nya_build_line(line, (u32)sizeof(line));
 * ```
 *
 * The values are compiled in rather than read at runtime: `VERSION` and `NYA_BUILD_COMMIT` come from
 * the build system, and the kind is the execution mode the translation unit was compiled under. The
 * build time is the one thing that cannot be a macro, because `__DATE__` makes the output depend on
 * when a file was compiled rather than on what was compiled, which breaks reproducible builds. It is
 * the executable's own modification time instead, read once and cached.
 *
 * This exists because the same four facts are wanted in three places that must not disagree: the
 * crash report a player sends, the corner of the main menu they read it off, and the startup log.
 * Formatting them separately is how a bug report ends up naming a different build than the one that
 * crashed.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * Longest one line summary, including the terminator.
 *
 * Version and commit are bounded by what the build system injects (a semantic version and a short
 * hash with an optional `-dirty`), the kind by NYA_EXECUTION_MODE_NAME_MAP, and the timestamp by
 * NYA_CLOCK_FORMAT_READABLE. 128 is about triple the longest of those, which leaves room for a
 * longer commit description without anyone having to come back here.
 * */
#define NYA_BUILD_LINE_MAX 128

/** Longest formatted build time, including the terminator. */
#define NYA_BUILD_TIME_MAX 32

// TYPES

typedef struct {
    /** From `VERSION` in the build system, or "unknown" outside it. */
    NYA_ConstCString version;

    /** The commit, with `-dirty` appended when the tree had uncommitted changes. */
    NYA_ConstCString commit;

    /** "debug", "dev", "release", "steam" or "test", and " headless" is reported separately. */
    NYA_ConstCString kind;

    /** Whether this translation unit was compiled with the renderer stubbed out. */
    b8 headless;

    /** The executable's modification time, formatted readable, or "unknown" when it cannot be read. */
    NYA_ConstCString built;
} NYA_BuildInfo;

// FUNCTIONS

/**
 * What this binary is. Cheap after the first call: the build time is stat'ed once and kept.
 * */
NYA_API NYA_BuildInfo nya_build_info(void) __attr_no_discard;

/**
 * The same as one line, `"<kind> <version> <commit>, built <time>"`, written into `out`.
 *
 * Truncated rather than cut off mid field if `capacity` is short. Returns the length written, not
 * counting the terminator.
 * */
NYA_API u32 nya_build_line(OUT u8* out, u32 capacity);
