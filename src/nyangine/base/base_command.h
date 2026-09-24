/**
 * @file base_command.h
 *
 * Running another program: what to start, what it wrote, and what it returned.
 *
 * ```c
 * NYA_Command command = {
 *     .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
 *     .arena     = arena,
 *     .program   = "git",
 *     .arguments = { "rev-parse", "HEAD", nullptr },
 * };
 * NYA_TRY(nya_command_run(&command));
 * defer nya_command_destroy(&command);
 * ```
 *
 * One implementation over os_process.h and no second one: assembling the arguments, capturing the
 * output and draining two pipes without deadlocking a child that fills one of them is the same work on
 * every target, and it used to be written twice. What is genuinely per target — forking, CreateProcess,
 * which handle is readable — is below this, in `os`.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/os/os_process.h"

// TYPES

/* Raised from 128 once the media libraries landed: SDL_image, SDL_ttf and SDL_mixer each vendor codecs, and a static link names every archive, so a line of ~150 arguments is normal now. */
#define NYA_COMMAND_MAX_ARGUMENTS 512
#define NYA_COMMAND_MAX_ENV_VARS  128

/** Most commands one nya_command_wait_ready call watches. The Windows wait takes no more than 64 handles. */
#define NYA_COMMAND_MAX_WAIT_READY NYA_OS_PROCESS_WAIT_MAX

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

    /* Internal, between nya_command_spawn and nya_command_wait: meaningless before a spawn and consumed by the wait, exposed only because NYA_Command is the handle the caller already holds. */

    /** What was started. Zeroed when nothing is running. */
    NYA_OsProcess process;

    /** Read ends of the child's pipes, held open until the wait drains them. */
    NYA_OsPipe stdout_pipe;
    NYA_OsPipe stderr_pipe;

    u64 start_time_ms;
};

// FUNCTIONS AND MACROS

NYA_API NYA_Error nya_command_run(NYA_Command* command) __attr_no_discard;

/**
 * Starts `command` and returns without waiting for it.
 * */
NYA_API NYA_Error nya_command_spawn(NYA_Command* command) __attr_no_discard;

/** Waits for a spawned command, drains its output and fills in its results. */
NYA_API NYA_Error nya_command_wait(NYA_Command* command) __attr_no_discard;

/**
 * nya_command_wait without blocking: drains whatever output is ready and, once the command has exited,
 * fills in its results and sets `out_finished`. Call it until it finishes, since a child writing more
 * than a pipe buffer holds cannot exit until someone reads.
 * */
NYA_API NYA_Error nya_command_try_wait(NYA_Command* command, b8* out_finished) __attr_no_discard;

/**
 * Sleeps until one of the spawned `commands` may have progressed, or `timeout_ms` passes. Only a hint
 * for when to call nya_command_try_wait again, which is what decides whether anything finished.
 * */
NYA_API void nya_command_wait_ready(NYA_Command* const* commands, u32 count, u32 timeout_ms);
NYA_API void      nya_command_destroy(NYA_Command* command);
