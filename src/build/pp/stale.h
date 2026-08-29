/**
 * @file pp/stale.h
 *
 * Whether a preprocessor pass has anything to do: the mtime comparison every generator makes
 * before it starts.
 *
 * Each pass reads a tree and writes generated C. Nothing about that is cheap — the reflection
 * lexer alone walks 138 headers — and none of it produces a different answer when no input has
 * moved. The build system already has NYA_BUILD_IF_OUTDATED for this, but it compares exactly one
 * input file against one output file, and a pass whose input is a directory and whose output is
 * two files cannot be expressed that way. So the check moves inside the pass.
 *
 * Comparison is strictly newer, not newer-or-equal, and that matters more than it looks: mtimes are
 * whole seconds on both platforms, and a pass that finishes inside the same second as the edit that
 * triggered it would otherwise mark itself current and never run again. Erring towards one wasted
 * regeneration is the only safe direction.
 *
 * Every pass lists its own generator source among its inputs. Editing the code that emits a file is
 * a reason to re-emit it, and without that the first thing anyone touching a generator notices is
 * that their change does nothing.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Newest modification time across `paths`, or 0 if none of them exist.
 *
 * A path may be a file or a directory; a directory is walked recursively and contributes its own
 * mtime as well as its contents'. The directory itself is what catches a deletion — removing a file
 * leaves nothing behind to compare against, but it does bump the mtime of the directory it was in.
 *
 * `extension` filters which *files* count, ".hlsli" for instance. Directories are always counted,
 * for the reason above. Pass nullptr to count every file.
 *
 * `paths` is nullptr terminated.
 * */
u64 nya_pp_newest(NYA_ConstCString* paths, NYA_ConstCString extension) __attr_no_discard;

/**
 * Whether every file in `outputs` exists and is strictly newer than everything in `inputs`.
 *
 * True means the pass can return without doing anything. `pass` names it in the trace line, which
 * is the only way a skipped pass is visible in a build log.
 *
 * Both arrays are nullptr terminated. Forced to false by `--regenerate`.
 * */
b8 nya_pp_is_current(NYA_ConstCString pass, NYA_ConstCString* inputs, NYA_ConstCString* outputs) __attr_no_discard;
