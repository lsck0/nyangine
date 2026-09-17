/**
 * @file hooks.h
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
 * */
void hook_create_build_directory(NYA_BuildRule* rule);

/**
 * Deletes a cmake build directory whose cached toolchain no longer exists.
 * */
void hook_invalidate_stale_cmake_cache(NYA_BuildRule* rule);

/**
 * Moves input_file to output_file.
 * */
void hook_move_file(NYA_BuildRule* rule);

/**
 * Rewrites a relative -DCMAKE_PREFIX_PATH= argument into an absolute one.
 * */
void hook_absolutize_cmake_prefix_path(NYA_BuildRule* rule);

/**
 * Expands the token %CWD% in any argument to the absolute working directory.
 * */
void hook_expand_cwd(NYA_BuildRule* rule);

/**
 * Copies input_file to output_file. Use instead of hook_move_file when the artifact may be a
 * relative symlink, which a move would leave dangling.
 * */
void hook_copy_file(NYA_BuildRule* rule);

/** Creates the directory the rule's output_file goes in. */
void hook_create_output_directory(NYA_BuildRule* rule);

/**
 * Runs the rule's compiler through the compiler cache, when there is one. See COMPILER_CACHE_ENV.
 * */
void hook_use_compiler_cache(NYA_BuildRule* rule);

/** Appends -DVERSION to the rule's compile command. */
void hook_add_version_flag(NYA_BuildRule* rule);

/** Deletes the rule's output file. Used to clean up after a rule that only ran for its effect. */
void hook_remove_output_file(NYA_BuildRule* rule);

/** Deletes the rule's input file. Used to drop an intermediate once the rule has consumed it. */
void hook_remove_input_file(NYA_BuildRule* rule);

/** Converts perf.data into plain text next to it. */
void hook_convert_perf_data_to_plain(NYA_BuildRule* rule);

/** Patches the tamper detection CRC into the linked binary. Must run after linking. */
void hook_insert_integrity_hash(NYA_BuildRule* rule);

/**
 * Authenticode signs the rule's output file. Must run last, after everything that touches the bytes.
 * */
void hook_sign_windows_executable(NYA_BuildRule* rule);

/*
 * ─────────────────────────────────────────────────────────
 * ASSET
 * ─────────────────────────────────────────────────────────
 */

/** Wrapper around nya_asset_compile_shaders. */
void hook_compile_shaders(NYA_BuildRule* rule);

/** Wrapper around nya_asset_index. */
void hook_index_assets(NYA_BuildRule* rule);

/** Generates src/generated/strings.h and validates every locale against the base. See build/pp/i18n.h. */
void hook_generate_strings(NYA_BuildRule* rule);

/** Regenerates src/generated/reflection.{h,c} from the @reflect annotations. See src/build/reflection.h. */
void hook_generate_reflection(NYA_BuildRule* rule);

/** Wrapper around nya_asset_bundle. */
void hook_bundle_assets(NYA_BuildRule* rule);
