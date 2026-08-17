/**
 * @file base_build.h
 *
 * Rule based build system with integration to the CLI parser.
 *
 * For example usage, see `build.c`.
 * */
#pragma once

#include "nyangine/base/base_string.h"
#include "nyangine/platform/command/command.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_BUILD_MAX_DEPENDENCIES 64

/**
 * Most commands nya_build_parallel will have in flight at once.
 *
 * A ceiling on the slot table rather than a target: the job count is normally the processor count,
 * and this only stops a machine with an implausible core count from asking for a slot array bigger
 * than the batch it is building.
 * */
#define NYA_BUILD_MAX_PARALLEL_JOBS 64
#define NYA_BUILD_MAX_VENDORS      16
#define NYA_VENDOR_MAX_PARTS       8
#define NYA_VENDOR_MAX_FLAGS       32

typedef enum NYA_BuildRulePolicy NYA_BuildRulePolicy;
typedef struct NYA_BuildRule     NYA_BuildRule;
typedef struct NYA_VendorRule    NYA_VendorRule;

enum NYA_BuildRulePolicy {
    NYA_BUILD_ALWAYS,
    NYA_BUILD_ONCE,
    NYA_BUILD_IF_OUTDATED,
    NYA_BUILD_COUNT,
};

/**
 * NYA_BuildRule
 *
 * Build rules have to follow these rules:
 * - If the policy is NYA_BUILD_ONCE, output_file must be specified.
 * - If the policy is NYA_BUILD_IF_OUTDATED, both input_file and output_file must be specified.
 * - Metarules (is_metarule = true) dont have commands, only dependencies and hooks.
 * - Hooks are optional.
 * */
struct NYA_BuildRule {
    NYA_ConstCString    name;
    NYA_BuildRulePolicy policy;
    b8                  is_metarule;

    NYA_ConstCString input_file;
    NYA_ConstCString output_file;
    NYA_Command      command;

    NYA_BuildRule* dependencies[NYA_BUILD_MAX_DEPENDENCIES];

    /**
     * Which vendors this rule compiles and links against. Their includes, cflags and linker flags
     * are appended to the command automatically.
     *
     * This does not build them. Every vendor is built up front by nya_vendor_build_all, so the
     * build dependency is implicit and belongs in neither this list nor `dependencies`. Listing a
     * vendor here only answers "which flags does this rule need".
     * */
    NYA_VendorRule* vendors[NYA_BUILD_MAX_VENDORS];

    void (*pre_build_hooks[NYA_BUILD_MAX_DEPENDENCIES])(NYA_BuildRule* rule);
    void (*post_build_hooks[NYA_BUILD_MAX_DEPENDENCIES])(NYA_BuildRule* rule);

    /**
     * Where the vendor flag splice began, so nya_build_parallel can undo it after the command has
     * been waited for rather than immediately after it was started.
     *
     * A serial build keeps this on the stack; a parallel one cannot, because preparation and
     * completion are separated by every other rule in the batch.
     * */
    u32 parallel_arguments_before_vendors;

    /** Set between spawn and wait, so an aborted batch still reaps exactly the rules it started. */
    b8 parallel_is_running;

    /**
     * Which build this rule last completed in. Bookkeeping; do not set it.
     *
     * A dependency graph is a graph, not a tree: shaders are needed by both the asset index and the
     * asset bundle, and a release target depends on both. Without this the rule runs once per path
     * that reaches it, which is why `./build build release` used to compile shaders twice.
     * */
    u64 last_built_epoch;
};

/**
 * One vendored third party dependency, for one target, described in one place: how to build it and
 * what a consumer needs in order to compile and link against it.
 *
 * A rule names the vendors it uses and the build system splices these flags into its command, so
 * no rule ever spells out a dependency's paths or libraries. That is what keeps a growing
 * dependency list from turning into a growing pile of per-target flag macros.
 *
 * Building is separate: everything in the vendor list is built up front by nya_vendor_build_all,
 * before any rule runs, so a rule's build dependency on a vendor is implicit.
 *
 * Vendors are per target: SDL for Linux and SDL for Windows are two rules, because they build
 * differently and link differently.
 *
 * A vendor that produces a tool rather than a library, a shader compiler for instance, simply
 * leaves the flag arrays empty and only fills in `parts`.
 * */
struct NYA_VendorRule {
    NYA_ConstCString name;

    /** Extra compiler flags a consumer needs. */
    NYA_ConstCString cflags[NYA_VENDOR_MAX_FLAGS];
    /** Include paths a consumer needs, each its own argument, `-I` included. */
    NYA_ConstCString includes[NYA_VENDOR_MAX_FLAGS];
    /** Library paths, archives and `-l` flags a consumer needs, appended after the sources. */
    NYA_ConstCString linker_flags[NYA_VENDOR_MAX_FLAGS];

    /** Rules that produce the artifact, built in order. */
    NYA_BuildRule* parts[NYA_VENDOR_MAX_PARTS];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_build(NYA_BuildRule* build_rule) __attr_no_discard;

/**
 * Builds `count` independent rules, running up to `max_jobs` of their commands at once.
 *
 * For a set of rules that genuinely do not depend on each other. The test suite is the case this
 * exists for: every test binary compiles the whole engine from scratch, so building them one at a
 * time leaves every core but one idle for the length of the run.
 *
 * Each rule's *preparation* still happens sequentially and in order — dependencies, vendors and
 * pre-build hooks — because those are shared between rules and are exactly the part that must not
 * race. Only the rules' own commands overlap. Post-build hooks run as each command is reaped, on
 * this thread, so a hook is no more concurrent than it was before.
 *
 * `max_jobs` of zero means one job per hardware thread. Output is buffered per rule and printed when
 * that rule finishes, so a failure reads as one contiguous block rather than as interleaved lines
 * from a dozen compilers.
 *
 * Stops at the first failure: rules already running are still waited for, since abandoning a child
 * mid-compile leaves a half-written object file that a later build would happily use.
 * */
NYA_API NYA_Error nya_build_parallel(NYA_BuildRule** build_rules, u32 count, u32 max_jobs) __attr_no_discard;

/** Builds a vendor's parts in order. */
NYA_API NYA_Error nya_vendor_build(NYA_VendorRule* vendor) __attr_no_discard;

/**
 * Builds every vendor in a nullptr terminated array.
 *
 * Call this before running any rule. Vendor parts are NYA_BUILD_ONCE, so once the artifacts exist
 * this is just a handful of file existence checks, and in exchange no rule ever has to worry about
 * whether what it links against has been built yet.
 * */
NYA_API NYA_Error nya_vendor_build_all(NYA_VendorRule** vendors) __attr_no_discard;

NYA_API void nya_rebuild_yourself(s32* argc, NYA_CString* argv, NYA_Command cmd);
