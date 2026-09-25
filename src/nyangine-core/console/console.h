/**
 * @file console.h
 *
 * A command system: named commands a program registers once and runs by typing a line, the backend a
 * dev console, a keybind, an IPC message or a script all dispatch through.
 *
 * ```c
 * static void cmd_spawn(NYA_ConsoleInvocation* call) {
 *     if (call->argument_count < 1) { nya_console_printf(call, "usage: spawn <kind>"); return; }
 *     spawn_entity(call->arguments[0]);
 *     nya_console_printf(call, "spawned %s", call->arguments[0]);
 * }
 *
 * nya_console_register("spawn", "spawn <kind> — add an entity", cmd_spawn);
 * nya_console_run("spawn goblin", print_to_overlay, overlay);   // finds spawn, splits args, dispatches
 * ```
 *
 * The registry is fixed and lives in `base` (`nya_console_*`, distinct from `base_command`'s
 * `nya_command_*` subprocess API): register is one-time at startup, run tokenises a line and dispatches,
 * and the output goes to a sink the caller gives, so the same command runs the same whether a person
 * typed it into an overlay, a socket sent it, or a test drives it. There is no allocation per run.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

enum {
    /** Commands the registry holds. A choice, so an enum rather than a #define; it bounds a fixed array. */
    NYA_CONSOLE_MAX_COMMANDS = 128,

    /** Arguments one command line is split into, past the command name itself. */
    NYA_CONSOLE_MAX_ARGUMENTS = 16,

    /** Longest command line run at once, terminator included. */
    NYA_CONSOLE_MAX_LINE = 512,

    /** Longest command name, terminator included. */
    NYA_CONSOLE_MAX_NAME = 48,
};

// TYPES

typedef struct NYA_ConsoleInvocation NYA_ConsoleInvocation;

/** Where a command's output goes: the caller's sink, one line at a time. `user` is whatever it registered with. */
typedef void (*NYA_ConsoleOutput)(void* user, NYA_ConstCString line);

/** One command's work: read `arguments`, write results with nya_console_printf. */
typedef void (*NYA_ConsoleFn)(NYA_ConsoleInvocation* invocation);

/** One dispatch: the split arguments and the sink the command answers through. Valid only for the call. */
struct NYA_ConsoleInvocation {
    NYA_ConstCString  name;
    NYA_ConstCString  arguments[NYA_CONSOLE_MAX_ARGUMENTS];
    u32               argument_count;
    NYA_ConsoleOutput output;
    void*             user;
};

// FUNCTIONS

/**
 * Registers `name` (copied), with a one-line `description` for help and completion, run by `handler`.
 * A duplicate name replaces the earlier one; a full registry or an over-long/empty name is refused.
 * */
NYA_API NYA_Error nya_console_register(NYA_ConstCString name, NYA_ConstCString description, NYA_ConsoleFn handler) __attr_no_discard;

/** Removes a command. Absent is not an error. */
NYA_API void nya_console_unregister(NYA_ConstCString name);

/** Empties the registry. */
NYA_API void nya_console_reset(void);

/**
 * Splits `line` into a command name and its arguments and dispatches to the matching handler, whose
 * output goes to `output(user, ...)`. A blank line does nothing; an unknown command writes one line
 * saying so and returns NYA_ERROR_NOT_FOUND; a line past NYA_CONSOLE_MAX_LINE is refused.
 * */
NYA_API NYA_Error nya_console_run(NYA_ConstCString line, NYA_ConsoleOutput output, void* user) __attr_no_discard;

/** Formats one output line to an invocation's sink. What a handler answers with. */
NYA_API void nya_console_printf(NYA_ConsoleInvocation* invocation, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/** Runs the command at `index` (from a search result) with `arguments`, answering through `output`. */
NYA_API NYA_Error nya_console_run_at(u32 index, const NYA_ConstCString* arguments, u32 argument_count, NYA_ConsoleOutput output, void* user)
    __attr_no_discard;

/** How many commands are registered, and each one's name and description, for help and a listing. */
NYA_API u32              nya_console_count(void) __attr_no_discard;
NYA_API NYA_ConstCString nya_console_name_at(u32 index) __attr_no_discard;
NYA_API NYA_ConstCString nya_console_description_at(u32 index) __attr_no_discard;

/** One search hit: which command, and how well it matched, best (highest) first. */
typedef struct NYA_ConsoleMatch {
    u32 index;
    s32 score;
} NYA_ConsoleMatch;

/**
 * Fuzzy-searches the command names and descriptions for `query` and writes up to `capacity` hits into
 * `out_matches`, ranked best first; returns how many. The match is a case-insensitive subsequence —
 * "spgb" finds "spawn goblin" — scored so a contiguous, word-start, name-over-description hit ranks
 * higher. An empty query returns every command in registration order. This is what a command palette
 * filters its list through as the person types.
 * */
NYA_API u32 nya_console_search(NYA_ConstCString query, OUT NYA_ConsoleMatch* out_matches, u32 capacity) __attr_no_discard;
