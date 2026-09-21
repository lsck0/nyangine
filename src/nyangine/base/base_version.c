#include "nyangine/base/base_version.h"

#include "nyangine/base/base_file.h"
#include "nyangine/base/base_string.h"
#include "nyangine/platform/clock/clock.h"
#include "nyangine/platform/filesystem/filesystem.h"

#include <stdio.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Stat'ed once. The executable does not change under a running process. */
NYA_INTERNAL u8 _NYA_BUILD_TIME[NYA_BUILD_TIME_MAX] = { 0 };
NYA_INTERNAL b8 _NYA_BUILD_TIME_RESOLVED            = false;

/**
 * The executable's modification time, readable, or "unknown".
 *
 * Not `__DATE__`: that bakes in when the *translation unit* was compiled, so an incremental build
 * reports whatever file happened to be rebuilt, and two builds of the same source disagree.
 * */
NYA_INTERNAL NYA_ConstCString _nya_build_time(void) {
    if (_NYA_BUILD_TIME_RESOLVED) return (NYA_ConstCString)_NYA_BUILD_TIME;

    _NYA_BUILD_TIME_RESOLVED = true;
    (void)snprintf((char*)_NYA_BUILD_TIME, sizeof(_NYA_BUILD_TIME), "unknown");

    NYA_Arena arena = nya_arena_create_on_stack(.name = "build_time");
    defer     nya_arena_destroy_on_stack(&arena);

    NYA_String* executable = nullptr;
    if (!nya_filesystem_executable_path(&arena, &executable).ok) return (NYA_ConstCString)_NYA_BUILD_TIME;

    u64 modified_ms = 0;
    if (!nya_filesystem_last_modified(nya_string_to_cstring(&arena, executable), &modified_ms).ok) {
        return (NYA_ConstCString)_NYA_BUILD_TIME;
    }

    (void)nya_clock_format_utc(modified_ms / 1'000ULL, NYA_CLOCK_FORMAT_READABLE, _NYA_BUILD_TIME, (u32)sizeof(_NYA_BUILD_TIME));

    return (NYA_ConstCString)_NYA_BUILD_TIME;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_BuildInfo nya_build_info(void) {
    return (NYA_BuildInfo){
        .version  = NYA_VERSION,
        .commit   = NYA_BUILD_COMMIT,
        .kind     = NYA_EXECUTION_MODE_NAME_MAP[NYA_EXECUTION_MODE_CURRENT],
        .headless = NYA_HEADLESS_ENABLED,
        .built    = _nya_build_time(),
    };
}

u32 nya_build_line(OUT u8* out, u32 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    NYA_BuildInfo info = nya_build_info();

    s32 written = snprintf(
        (char*)out, capacity, "%s%s %s %s, built %s", info.kind, info.headless ? " headless" : "", info.version, info.commit, info.built
    );

    // snprintf reports what it *wanted* to write, so a truncated line would otherwise be reported
    // as longer than the buffer and index past it in the caller.
    if (written < 0) {
        out[0] = '\0';
        return 0;
    }

    return (u32)written < capacity ? (u32)written : capacity - 1;
}
