/**
 * @file base_clean.h
 *
 * Linux Kernel inspired automatic cleanup for resources.
 * First define the cleanup function using NYA_DEFINE_CLEANUP_FN,
 * then adding NYA_CLEANUP_WITH before a variable declaration will ensure
 * that the cleanup function is called when the variable goes out of scope.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Used to create the cleanup functions to be used with NYA_CLEANUP_WITH.
 *
 * EXAMPLE USAGE:
 * ```c
 * NYA_DEFINE_CLEANUP_FN(nya_arena_destroy, NYA_Arena, arena, nya_arena_destroy(&arena))
 * ```
 * */
#define NYA_DEFINE_CLEANUP_FN(name, type, var_name, code)                                                                                            \
    __attr_allow_unused void __cleanup_##name(type const* p) {                                                                                       \
        if (!p) return;                                                                                                                              \
        type var_name = *p;                                                                                                                          \
        code;                                                                                                                                        \
    }

/**
 * This is essentially poor mans RAII / defer for C.
 *
 * EXAMPLE USAGE:
 * ```c
 * NYA_CLEANUP_WITH(close) s32 source_fd = open(source, O_RDONLY);
 * if (source_fd < 0) return false;
 *
 * NYA_CLEANUP_WITH(close) s32 destination_fd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0644);
 * if (destination_fd < 0) return false;
 *
 * NYA_CLEANUP_WITH(nya_arena_destroy) NYA_Arena arena  = nya_arena_create();
 * NYA_String                                    buffer = nya_string_create(&arena);
 *
 * NYA_TRY(nya_fd_read(source_fd, &buffer));
 * NYA_TRY(nya_fd_write(destination_fd, &buffer));
 *
 * return NYA_OK;
 * ```
 * */
#define NYA_CLEANUP_WITH(cleanup_fn) __attr_cleanup(__cleanup_##cleanup_fn)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NON NYA* CLEANUP FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_DEFINE_CLEANUP_FN(
    close,
    int,
    fd,
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
)

NYA_DEFINE_CLEANUP_FN(
    fclose,
    FILE*,
    f,
    if (f) {
        (void)fclose(f);
        f = nullptr;
    }
)
