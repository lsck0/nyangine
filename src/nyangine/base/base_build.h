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

    void (*pre_build_hooks[NYA_BUILD_MAX_DEPENDENCIES])(NYA_BuildRule* rule);
    void (*post_build_hooks[NYA_BUILD_MAX_DEPENDENCIES])(NYA_BuildRule* rule);

    /**
     * Where the vendor flag splice began, so nya_build_parallel can undo it after the command has
     * been waited for rather than immediately after it was started.
     * */
    u32 parallel_arguments_before_vendors;

    /** Set between spawn and wait, so an aborted batch still reaps exactly the rules it started. */
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
 * */
NYA_API NYA_Error nya_build_parallel(NYA_BuildRule** build_rules, u32 count, u32 max_jobs) __attr_no_discard;

/** Builds a vendor's parts in order. */
NYA_API NYA_Error nya_vendor_build(NYA_VendorRule* vendor) __attr_no_discard;

/**
 * Builds every vendor in a nullptr terminated array.
 * */
NYA_API NYA_Error nya_vendor_build_all(NYA_VendorRule** vendors) __attr_no_discard;

NYA_API void nya_rebuild_yourself(s32* argc, NYA_CString* argv, NYA_Command cmd);
