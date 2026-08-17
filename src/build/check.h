/**
 * @file check.h
 *
 * The check command: clang-tidy over the tree.
 *
 * A command handler rather than a set of NYA_BuildRules for the same reason the test command is one
 * — the flags have to be assembled at run time, here because the vendor include paths live on
 * NYA_VendorRule and there is no macro that spells them all out. What it produces is still ordinary
 * rules, so a check is reported and parallelised like any other target.
 *
 * ## Why three files and not the whole tree
 *
 * This is a unity build: there are exactly three translation units in the repository, and every
 * other `.c` is `#include`d into one of them. Running clang-tidy per file would compile most headers
 * dozens of times and would analyse the included `.c` files outside the context they are actually
 * compiled in — with the wrong macros, and in some cases as an empty translation unit.
 *
 * So the three real roots are checked, and `HeaderFilterRegex` in `.clang-tidy` is what widens that
 * to every header underneath them. The three do not overlap: main.c is the engine, gnyame.c is the
 * game, build.c is this build system, and each is compiled with a different set of defines.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Runs clang-tidy over the translation units, optionally filtered.
 *
 * With no arguments it checks all three. With arguments it checks the ones whose path contains any
 * of them, so `./build check gnyame` is the game alone.
 *
 * Checks and suppressions come from `.clang-tidy`, which is also what the editor reads through
 * clangd — one list rather than two that drift. The compile flags are assembled here from the same
 * macros the real build rules use, so a check sees the same preprocessor state a build does.
 *
 * Findings are reported and the command succeeds, because clang-tidy exits zero on warnings and this
 * tree has a backlog of them. `--strict` turns every finding into a failure, which is what CI wants
 * and what anyone who has just cleared a file wants to keep it clear.
 * */
void check_runner(NYA_ArgCommand* command);
