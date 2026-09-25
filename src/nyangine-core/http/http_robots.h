/**
 * @file http_robots.h
 *
 * A `robots.txt`: the rules a crawler reads before it fetches anything, built from groups a program
 * declares, with a strict deny-all preset for a site that wants no crawling at all.
 *
 * ```
 * nya_http_robots_build         user-agent groups + a Sitemap line -> the robots.txt body
 * nya_http_robots_strict        the deny-all preset: "User-agent: *" then "Disallow: /"
 * nya_http_robots_mount         builds from groups and serves it at GET /robots.txt
 * nya_http_robots_mount_strict  serves the deny-all preset at GET /robots.txt
 * ```
 *
 * ```c
 * const NYA_ConstCString disallow[] = { "/private", "/admin" };
 * const NYA_HttpRobotsGroup groups[] = {
 *     { .user_agent = "*", .disallow = disallow, .disallow_count = nya_carray_length(disallow), .crawl_delay_s = 10 },
 * };
 * NYA_EXPECT(nya_http_robots_mount((NYA_HttpRobotsConfig){
 *     .groups = groups, .count = nya_carray_length(groups), .sitemap = "https://example.com/sitemap.xml" }));
 *
 * // or, for a site that wants no crawling:
 * NYA_EXPECT(nya_http_robots_mount_strict());
 * ```
 *
 * ── the format has no escape, so the rule is refusal ──
 *
 * robots.txt is line-oriented and has no way to escape a value, so the only defence against a newline in
 * a user-agent or a path turning into a directive of its own is to refuse it: any control byte — a CR, an
 * LF, anything below a space — in a group's fields fails the build rather than being written. A Sitemap
 * URL is gated by nya_http_doc_url_is_web like every other URL here. Bounded and refuse-whole; see
 * http_doc.h.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

#define NYA_HTTP_ROBOTS_PATH "/robots.txt"

/**
 * The largest robots.txt this builds, terminator excluded. A robots file is a handful of short lines;
 * 8 KiB is generous and well under the shared response bound, and a rule set larger than it is refused
 * rather than truncated into a file that allows what a cut-off Disallow line meant to forbid.
 * */
#define NYA_HTTP_ROBOTS_MAX_BYTES 8192

/** User-agent groups one robots.txt here declares. Past this a build is refused. */
#define NYA_HTTP_ROBOTS_MAX_GROUPS 32

// TYPES

typedef struct NYA_HttpRobotsGroup  NYA_HttpRobotsGroup;
typedef struct NYA_HttpRobotsConfig NYA_HttpRobotsConfig;

/** One user-agent group: who it is for, and what they may and may not fetch. */
struct NYA_HttpRobotsGroup {
    /** The crawler this group is for, "*" for all. Required, and rejected if it carries a control byte. */
    NYA_ConstCString user_agent;

    /** Paths this crawler may fetch. Each becomes an `Allow:` line. */
    const NYA_ConstCString* allow;
    u32                     allow_count;

    /** Paths this crawler may not fetch. Each becomes a `Disallow:` line; an empty string is `Disallow:` with nothing, which allows all. */
    const NYA_ConstCString* disallow;
    u32                     disallow_count;

    /** Seconds a crawler should wait between requests. Emitted as `Crawl-delay:` when non-zero. */
    u32 crawl_delay_s;
};

/** The groups, and an optional Sitemap line pointing a crawler at the sitemap. */
struct NYA_HttpRobotsConfig {
    const NYA_HttpRobotsGroup* groups;
    u32                        count;

    /** The sitemap's URL, emitted as a `Sitemap:` line. Optional; must be an http or https URL when set. */
    NYA_ConstCString sitemap;
};

// FUNCTIONS

/**
 * Builds the robots.txt body into `out`, allocated from `arena`.
 *
 * NYA_ERROR_INVALID_ARGUMENT for no groups, more than NYA_HTTP_ROBOTS_MAX_GROUPS, a group with no
 * user-agent, a control byte in any field, or a Sitemap that is not an http or https URL;
 * NYA_ERROR_OUT_OF_MEMORY on overflow, with nothing emitted.
 * */
NYA_API NYA_Error nya_http_robots_build(NYA_Arena* arena, NYA_HttpRobotsConfig config, OUT NYA_ConstCString* out) __attr_no_discard;

/**
 * The strict preset: a robots.txt that disallows every path for every crawler — "User-agent: *" then
 * "Disallow: /". Static storage, so it needs no arena and outlives any mount.
 * */
NYA_API NYA_ConstCString nya_http_robots_strict(void) __attr_no_discard;

/** Builds from `config` and serves it at GET /robots.txt as text/plain. Merge nya_http_doc_router() to bring it online. */
NYA_API NYA_Error nya_http_robots_mount(NYA_HttpRobotsConfig config);

/** Serves the deny-all preset at GET /robots.txt as text/plain. */
NYA_API NYA_Error nya_http_robots_mount_strict(void);
