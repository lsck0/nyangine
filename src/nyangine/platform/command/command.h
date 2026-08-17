#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Raised from 128 once the media libraries landed: SDL_image, SDL_ttf and SDL_mixer each vendor a
 * handful of codecs, and a static link names every archive individually. A link line of ~150
 * arguments is normal now.
 */
#define NYA_COMMAND_MAX_ARGUMENTS 512
#define NYA_COMMAND_MAX_ENV_VARS  128

typedef enum NYA_CommandFlags NYA_CommandFlags;
typedef struct NYA_Command    NYA_Command;

enum NYA_CommandFlags {
    NYA_COMMAND_FLAG_NONE            = 0,
    NYA_COMMAND_FLAG_OUTPUT_SUPPRESS = (1 << 0),
    NYA_COMMAND_FLAG_OUTPUT_SHOW     = (1 << 1),
    NYA_COMMAND_FLAG_OUTPUT_CAPTURE  = (1 << 2),
    NYA_COMMAND_FLAG_DEFAULT         = NYA_COMMAND_FLAG_OUTPUT_SHOW,
};

/**
 * NYA_Command
 *
 * Commands have to follow these rules:
 * - If NYA_COMMAND_FLAG_OUTPUT_CAPTURE is set, an arena must be provided and stdout_content and stderr_content
 *   will be set after execution.
 * */
struct NYA_Command {
    NYA_CommandFlags flags;

    NYA_ConstCString working_directory;
    NYA_ConstCString program;
    NYA_ConstCString arguments[NYA_COMMAND_MAX_ARGUMENTS];
    NYA_CString      environment[NYA_COMMAND_MAX_ENV_VARS];

    NYA_Arena* arena;

    /* will be filled after execution */

    s32         exit_code;
    NYA_String* stdout_content;
    NYA_String* stderr_content;
    u64         execution_time_ms;

    /*
     * ── Internal, between nya_command_spawn and nya_command_wait ──
     *
     * Meaningless before a spawn and consumed by the wait. Exposed only because NYA_Command is the
     * handle a caller already holds, and a separate process handle type would be one more thing to
     * keep alive alongside it.
     */

    /** Process id on Linux, a HANDLE cast to this on Windows. Zero when nothing is running. */
    u64 process_handle;

    /** Windows keeps a second handle for the thread; unused elsewhere. */
    u64 thread_handle;

    /** Read ends of the child's pipes, held open until the wait drains them. */
    s32 stdout_pipe;
    s32 stderr_pipe;

    u64 start_time_ms;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_command_run(NYA_Command* command) __attr_no_discard;

/**
 * Starts `command` and returns without waiting for it.
 *
 * The half of nya_command_run that does not block. Pair every spawn with exactly one
 * nya_command_wait — the child stays a zombie until something reaps it, and its pipes stay open.
 *
 * For running independent commands at once. Compiling the test suite is the case that motivated it:
 * every test binary rebuilds the whole engine, and doing that one at a time leaves every core but
 * one idle.
 *
 * `exit_code`, `execution_time_ms` and the captured output are only valid after the wait.
 * */
NYA_API NYA_Error nya_command_spawn(NYA_Command* command) __attr_no_discard;

/** Waits for a spawned command, drains its output and fills in its results. */
NYA_API NYA_Error nya_command_wait(NYA_Command* command) __attr_no_discard;
NYA_API void      nya_command_destroy(NYA_Command* command);
