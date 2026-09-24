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
 * Deletes a cmake build directory whose cache no longer describes this build.
 *
 * Two reasons to throw one away: a cached compiler or make program that is not on this machine, and a
 * `-D` in the recipe that disagrees with what the cache was configured from. The second is the one that
 * bites, because cmake does not re-derive everything a CMakeLists only reads on the first pass, and an
 * option that does nothing until a rebuild is not an option.
 *
 * **Register it last.** It compares the rule's arguments against the cache, so every hook that rewrites
 * an argument — `hook_expand_cwd`, `hook_absolutize_cmake_prefix_path` — has to have run first, or it
 * reads a `%CWD%` or a relative path the cache could never have recorded and wipes the directory on
 * every build.
 * */
void hook_invalidate_stale_cmake_cache(NYA_BuildRule* rule);

/**
 * Stamps a rule's `output_file` with the current time, after the rule has run.
 *
 * For an NYA_BUILD_IF_OUTDATED rule whose tool may legitimately do nothing: cmake and ninja leave an
 * archive alone when no source changed, so the archive stays older than the recipe that triggered
 * the run and the rule is outdated again immediately. Without this, editing a vendor recipe makes
 * its rule fire on every invocation of `./build` for the rest of the checkout's life, `./build
 * stats` included, because main walks the vendor rules before it dispatches any subcommand.
 *
 * A no-op build is still a build: the rule ran, the output is current with its input, and saying so
 * is what lets the next invocation skip it.
 * */
void hook_stamp_output_file(NYA_BuildRule* rule);

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

/**
 * Appends -DNYA_BUILD_COMMIT to the rule's compile command, so a shipped binary can say which revision
 * it is. Pairs with hook_add_version_flag on every rule that compiles project sources.
 * */
void hook_add_build_info_flag(NYA_BuildRule* rule);

/** Appends VERSION as -DNYA_RC_VERSION_MAJOR, _MINOR and _PATCH, for the numeric fields of the Windows version resource. */
void hook_add_version_resource_flags(NYA_BuildRule* rule);

/**
 * Deletes the rule's output file, and says nothing when there is none to delete. Used to clean up after a
 * rule that only ran for its effect, and before a rule whose command adds to its output instead of
 * replacing it.
 * */
void hook_remove_output_file(NYA_BuildRule* rule);

/** Deletes the rule's input file. Used to drop an intermediate once the rule has consumed it. */
void hook_remove_input_file(NYA_BuildRule* rule);

#if !OS_WINDOWS
/**
 * Rewrites every absolute symlink under the rule's working directory to point inside it. An unpacked sysroot links
 * into / and would otherwise resolve against the host.
 * */
void hook_relativize_symlinks(NYA_BuildRule* rule);

/** Derives and builds the Steam Runtime vendors. See vendor_steamrt.h. */
void hook_build_steamrt_vendors(NYA_BuildRule* rule);
#endif

/** Converts perf.data into plain text next to it. */
void hook_convert_perf_data_to_plain(NYA_BuildRule* rule);

/** Patches the tamper detection CRC into the linked binary. Must run after linking. */
void hook_insert_integrity_hash(NYA_BuildRule* rule);

#if !OS_WINDOWS
/**
 * Asserts, on the produced Linux release binary, that the shipping hardening actually took, rather than
 * trusting the flag list. Parses `readelf` over the ELF and fails the build (a hard assert) if any of the
 * mitigations is missing: full RELRO (a GNU_RELRO segment) plus immediate binding (DT_FLAGS BIND_NOW /
 * DT_FLAGS_1 NOW) from -Wl,-z,relro -Wl,-z,now; a non-executable stack (a GNU_STACK segment, no RWE
 * segment anywhere) from -Wl,-z,noexecstack; the stack-protector runtime helper __stack_chk_fail from
 * -fstack-protector-strong; and at least one __*_chk fortify wrapper from _FORTIFY_SOURCE. ELF only, so
 * it rides only the Linux release/Steam link rules; the mingw PE build is verified by nothing here.
 * */
void hook_verify_hardening(NYA_BuildRule* rule);
#endif

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

/** Generates src/genyarated/strings.h and validates every locale against the base. See build/pp/i18n.h. */
void hook_generate_strings(NYA_BuildRule* rule);

/** Regenerates src/genyarated/reflection.{h,c} from the @reflect annotations. See src/build/reflection.h. */
void hook_generate_reflection(NYA_BuildRule* rule);

/** Regenerates docs/CHEATSHEET.md from the public headers. See build/pp/cheatsheet.h. */
void hook_generate_cheatsheet(NYA_BuildRule* rule);

/** Regenerates the Lua bindings and their definitions file from the @lua annotations. See build/pp/luabind.h. */
void hook_generate_lua_bindings(NYA_BuildRule* rule);

/** Regenerates one companion header per source file that writes a nya_lambda. See build/pp/lambda.h. */
void hook_generate_lambdas(NYA_BuildRule* rule);

/** Regenerates one companion header per source file with a watched function. See build/pp/watch.h. */
void hook_generate_watches(NYA_BuildRule* rule);

/** Wrapper around nya_asset_bundle. */
void hook_bundle_assets(NYA_BuildRule* rule);

/**
 * Stages the deployable docs site under ./site: the hand-written GitBook prose and SUMMARY.md, the
 * generated cheatsheet, and a .gitbook.yaml rooted at the staged tree. Runs before doxygen writes its
 * HTML into ./site/doxygen, so all three tiers end up under one directory with relative links intact.
 * */
void hook_assemble_docs(NYA_BuildRule* rule);
