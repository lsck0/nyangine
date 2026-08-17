/**
 * @file cli.h
 *
 * The command line: every command the build tool offers, and the parser that dispatches them.
 *
 * What a command *is* lives here; what it *does* lives in the rule or handler it names. That split
 * is the reason `./build --help` and the generated completions describe the whole tool without this
 * file knowing how any of it is built — a command is a name, a description, and a pointer.
 *
 * Only the four things main reaches for are declared. Everything else is internal to cli.c, because
 * nothing outside it has any business naming a subcommand.
 *
 * Declarations only, so this needs nothing but the argument parser's types. What the commands are
 * actually wired to — the rules in misc.h, rebuild.h, host.h and asset.h — is cli.c's problem.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PARSER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The whole command tree. Defined in cli.c.
 *
 * Tentative definition rather than an extern: NYA_INTERNAL is static, and this is a unity build, so
 * naming it here reserves it and cli.c's initialiser is what fills it in.
 * */
NYA_INTERNAL NYA_ArgParser parser;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WHAT MAIN DISPATCHES ON
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * These three are the commands and flags main has to recognise by identity rather than run: it
 * short circuits to `completions` before anything writes to stdout, prints usage for `help` instead
 * of dispatching, and consults `no-rebuild` before rebuilding itself. Every other command reaches
 * main only as an opaque NYA_ArgCommand* handed straight to nya_args_run_command.
 */

/** `./build completions <shell>`. Handled early, see main. */
NYA_INTERNAL NYA_ArgCommand completions;

/** `--help`, on the root command so it applies to every subcommand. */
NYA_INTERNAL NYA_ArgParameter help_flag;

/** `--no-rebuild`, which suppresses the tool recompiling itself before it runs. */
NYA_INTERNAL NYA_ArgParameter skip_self_rebuild_flag;
