#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_COMMAND_MAX_ARGUMENTS 128
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
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_command_run(NYA_Command* command) __attr_no_discard;
NYA_API void      nya_command_destroy(NYA_Command* command);
