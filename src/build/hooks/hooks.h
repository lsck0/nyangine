/**
 * @file hooks.h
 *
 * Build rule hooks: work that runs around a rule's command rather than as a command itself.
 *
 * Hooks stay thin. Anything with real logic behind it lives in its own module and is called from
 * here, so that a pipeline is not welded to the build system's calling convention. The asset hooks
 * are the clearest case: they are one line each, forwarding to build/asset/asset.h.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/flags.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * BUILD
 * ─────────────────────────────────────────────────────────
 */

/**
 * Creates the directory a rule configures into, along with any missing parents.
 *
 * Portable stand-in for `mkdir -p`: Windows has no such command. Uses the rule's
 * `command.working_directory`, which for an out of tree build is exactly the directory that has to
 * exist before the command can run.
 * */
void hook_create_build_directory(NYA_BuildRule* rule);

/**
 * Deletes a cmake build directory whose cached toolchain no longer exists.
 *
 * CMakeCache.txt records absolute paths to the compiler and the make program. Reuse that cache on a
 * machine where those paths are wrong and cmake fails at `project()` with a bare "No such file or
 * directory", naming a binary nobody asked for.
 *
 * Which happens more than it sounds: `act` bind mounts the repository into its container, so a
 * build directory configured on the host is handed to a container with an entirely different
 * filesystem layout. It also happens to anyone who changes compilers or moves the checkout.
 *
 * Reads `-B <dir>` out of the rule's own arguments, so it needs no configuration.
 * */
void hook_invalidate_stale_cmake_cache(NYA_BuildRule* rule);

/**
 * Moves input_file to output_file.
 *
 * Exists so rules do not have to shell out to `mv`, which does not exist on Windows. Vendors that
 * build in tree use this to move an archive aside so the other target can build into the same path.
 * */
void hook_move_file(NYA_BuildRule* rule);

/**
 * Rewrites a relative -DCMAKE_PREFIX_PATH= argument into an absolute one.
 *
 * cmake resolves a relative CMAKE_PREFIX_PATH against the build directory rather than the working
 * directory, so a vendor pointing at another vendor's output with "./vendor/..." silently fails to
 * find it.
 * */
void hook_absolutize_cmake_prefix_path(NYA_BuildRule* rule);

/**
 * Expands the token %CWD% in any argument to the absolute working directory.
 *
 * Several tools resolve relative paths against something other than where the build was invoked
 * from, so an argument that has to be absolute can be written with this marker instead.
 * */
void hook_expand_cwd(NYA_BuildRule* rule);

/**
 * Copies input_file to output_file. Use instead of hook_move_file when the artifact may be a
 * relative symlink, which a move would leave dangling.
 * */
void hook_copy_file(NYA_BuildRule* rule);

/** Appends -DVERSION and -DGIT_COMMIT to the rule's compile command. */
void hook_add_version_flag_and_git_hash(NYA_BuildRule* rule);

/** Deletes the rule's output file. Used to clean up after a rule that only ran for its effect. */
void hook_remove_output_file(NYA_BuildRule* rule);

/** Lays out the per platform dist directories and zips them. */
void hook_bundle_project(NYA_BuildRule* rule);

/** Converts perf.data into plain text next to it. */
void hook_convert_perf_data_to_plain(NYA_BuildRule* rule);

/** Patches the tamper detection CRC into the linked binary. Must run after linking. */
void hook_insert_integrity_hash(NYA_BuildRule* rule);

/*
 * ─────────────────────────────────────────────────────────
 * ASSET
 * ─────────────────────────────────────────────────────────
 */

/** Wrapper around nya_asset_compile_shaders. */
void hook_compile_shaders(NYA_BuildRule* rule);

/** Wrapper around nya_asset_index. */
void hook_index_assets(NYA_BuildRule* rule);

/** Wrapper around nya_asset_bundle. */
void hook_bundle_assets(NYA_BuildRule* rule);
