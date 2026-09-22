/**
 * @file base_build.h
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
 * */
#define NYA_BUILD_MAX_PARALLEL_JOBS 64
#define NYA_BUILD_MAX_VENDORS      24
#define NYA_VENDOR_MAX_PARTS       8
#define NYA_VENDOR_MAX_FLAGS       32

typedef enum NYA_BuildRulePolicy  NYA_BuildRulePolicy;
typedef enum NYA_BuildVendorFlags NYA_BuildVendorFlags;
typedef struct NYA_BuildRule      NYA_BuildRule;
typedef struct NYA_VendorRule     NYA_VendorRule;

enum NYA_BuildRulePolicy {
    NYA_BUILD_ALWAYS,
    NYA_BUILD_ONCE,
    NYA_BUILD_IF_OUTDATED,
    NYA_BUILD_COUNT,
};

/**
 * Which of its vendors' flags a rule's command takes. A compile with `-c` fails on linker inputs under
 * -Werror, so split compile and link rules each take their half.
 * */
enum NYA_BuildVendorFlags {
    /** Includes, cflags and linker flags, for a command that compiles and links at once. */
    NYA_BUILD_VENDOR_FLAGS_ALL,
    /** Includes and cflags. */
    NYA_BUILD_VENDOR_FLAGS_COMPILE,
    /** Linker flags. */
    NYA_BUILD_VENDOR_FLAGS_LINK,
    NYA_BUILD_VENDOR_FLAGS_COUNT,
};

/**
 * NYA_BuildRule
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
     * */
    NYA_VendorRule* vendors[NYA_BUILD_MAX_VENDORS];

    /** Which half of the vendor flags the command gets. */
    NYA_BuildVendorFlags vendor_flags;

    void (*pre_build_hooks[NYA_BUILD_MAX_DEPENDENCIES])(NYA_BuildRule* rule);
    void (*post_build_hooks[NYA_BUILD_MAX_DEPENDENCIES])(NYA_BuildRule* rule);

    /**
     * Where the vendor flag splice began, so nya_build_parallel can undo it after the command has
     * been waited for rather than immediately after it was started.
     * */
    u32 parallel_arguments_before_vendors;

    /** Set between spawn and wait, so an aborted build still reaps exactly the rules it started. */
    b8 parallel_is_running;

    /**
     * Which build this rule last completed in. Bookkeeping; do not set it.
     * */
    u64 last_built_epoch;
};

/**
 * One vendored third party dependency, for one target, described in one place: how to build it and
 * what a consumer needs in order to compile and link against it.
 * */
struct NYA_VendorRule {
    NYA_ConstCString name;

    /** Extra compiler flags a consumer needs. */
    NYA_ConstCString cflags[NYA_VENDOR_MAX_FLAGS];
    /** Include paths a consumer needs, each its own argument, `-I` included. */
    NYA_ConstCString includes[NYA_VENDOR_MAX_FLAGS];
    /** Library paths, archives and `-l` flags a consumer needs, appended after the sources. */
    NYA_ConstCString linker_flags[NYA_VENDOR_MAX_FLAGS];

    /**
     * The file this vendor's build options are written in, usually its own `vendor_*.h`.
     *
     * A vendor's parts are NYA_BUILD_ONCE keyed on the archive they produce, so changing a cmake or make
     * option used to change nothing at all: the archive was still there, every part was skipped, and the
     * option quietly applied to nobody who had built once already. An option that does nothing until a
     * rebuild is not an option. Naming the recipe here is the dependency that was always real and was
     * never written down.
     *
     * When this file is newer than `options_stamp`, every part is built whatever its own policy says,
     * and the stamp is written afterwards. Leave both null for a vendor with no options worth watching.
     * */
    NYA_ConstCString options_file;

    /**
     * Where the stamp for `options_file` is kept, usually beside the artifact.
     *
     * A stamp and not the artifact itself: cmake, ninja and make all leave an archive alone when no
     * source changed, so a no-op rebuild left the archive older than the recipe that triggered it and
     * the vendor was stale again immediately — rebuilding on every invocation for the rest of the
     * checkout's life, `./build stats` included, since the vendors are walked before any subcommand.
     * */
    NYA_ConstCString options_stamp;

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
 * The rule whose command failed in the most recent build, or nullptr when the last one succeeded.
 *
 * Reset when a top level build starts, so it always describes the build that just ran.
 * */
NYA_API const NYA_BuildRule* nya_build_last_failure(void) __attr_no_discard;

/**
 * Reprints what the failing rule was and what its tool wrote, or nothing when nothing failed.
 *
 * For the caller that is about to give up: by then the diagnostic is somewhere above the last few
 * hundred lines of a build log, and the one thing a reader needs is the compiler's own words, last.
 * A rule built in parallel has its output captured and gets it verbatim; a serial rule streamed
 * straight to the terminal, so only the summary line is repeated.
 * */
NYA_API void nya_build_print_last_failure(void);

/**
 * Builds `count` independent rules, running up to `max_jobs` of their commands at once.
 * */
NYA_API NYA_Error nya_build_parallel(NYA_BuildRule** build_rules, u32 count, u32 max_jobs) __attr_no_discard;

/** Builds a vendor's parts in order. */
NYA_API NYA_Error nya_vendor_build(NYA_VendorRule* vendor) __attr_no_discard;

/**
 * Builds every vendor in a nullptr terminated array.
 * */
NYA_API NYA_Error nya_vendor_build_all(NYA_VendorRule** vendors) __attr_no_discard;

NYA_API void nya_rebuild_yourself(s32* argc, NYA_CString* argv, NYA_Command cmd);
