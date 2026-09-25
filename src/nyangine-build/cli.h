/**
 * @file cli.h
 * */
#pragma once

#include "nyangine-core/nyangine.h"

/* THE PARSER */

/**
 * The whole command tree. Defined in cli.c.
 * */
NYA_INTERNAL NYA_ArgParser parser;

/* WHAT MAIN DISPATCHES ON */

/* These are the commands and flags something outside cli.c has to recognise by identity rather than run. main short circuits to `completions` before anything writes to stdout, prints usage for `help` instead of dispatching, and consults `no-rebuild` before rebuilding itself; the preprocessor passes in pp/ consult `regenerate`. Every other command reaches main only as an opaque NYA_ArgCommand* handed straight to nya_args_run_command. */

/** `./build completions <shell>`. Handled early, see main. */
NYA_INTERNAL NYA_ArgCommand completions;

/** `--help`, on the root command so it applies to every subcommand. */
NYA_INTERNAL NYA_ArgParameter help_flag;

/** `--no-rebuild`, which suppresses the tool recompiling itself before it runs. */
NYA_INTERNAL NYA_ArgParameter skip_self_rebuild_flag;

/**
 * `--regenerate`, which makes every preprocessor pass run whether or not its inputs have moved.
 * */
NYA_INTERNAL NYA_ArgParameter regenerate_flag;
