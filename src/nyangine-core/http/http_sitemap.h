/**
 * @file http_sitemap.h
 *
 * A `sitemap.xml`: the list of URLs a site wants a crawler to know about, built from data a program
 * supplies rather than written by hand.
 *
 * ```
 * nya_http_sitemap_build   a set of URLs -> the <urlset> document, XML-escaped and bounded
 * nya_http_sitemap_mount   the same, served at GET /sitemap.xml through the http_doc registry
 * ```
 *
 * ```c
 * const NYA_HttpSitemapUrl urls[] = {
 *     { .loc = "https://example.com/" , .changefreq = NYA_HTTP_SITEMAP_DAILY, .priority = 1.0F, .has_priority = true },
 *     { .loc = "https://example.com/about" },
 * };
 * NYA_EXPECT(nya_http_sitemap_mount((NYA_HttpSitemapConfig){ .urls = urls, .count = nya_carray_length(urls) }));
 * NYA_EXPECT(nya_http_server_merge(nya_http_doc_router()));
 * ```
 *
 * ── what it promises about the document ──
 *
 * The output is the sitemaps.org 0.9 schema — a `<urlset>` with the right xmlns and a `<url>` per entry
 * — and every byte of caller data in it goes through the XML escaper, so a loc with a `&` in its query
 * is valid markup and not a broken document. A `loc` is required and must be an http or https URL, which
 * nya_http_doc_url_is_web is the gate for: a `javascript:` or a `data:` loc, a bare path, or anything a
 * crawler would not fetch, is refused rather than emitted. The build is bounded and refuses whole on
 * overflow; see http_doc.h.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_clock_instant.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

#define NYA_HTTP_SITEMAP_PATH "/sitemap.xml"

/**
 * How many URLs one sitemap here holds. The sitemaps.org limit is 50,000, far past what fits the
 * response bound this document shares (see NYA_HTTP_DOC_MAX_BYTES); this is a loop ceiling under that,
 * and the byte bound is the hard backstop. A site with more URLs than fits splits into a sitemap index.
 * */
#define NYA_HTTP_SITEMAP_MAX_URLS 2000

// TYPES

typedef enum NYA_HttpSitemapChangeFreq NYA_HttpSitemapChangeFreq;
typedef struct NYA_HttpSitemapUrl      NYA_HttpSitemapUrl;
typedef struct NYA_HttpSitemapConfig   NYA_HttpSitemapConfig;

/** How often a URL changes, the sitemaps.org vocabulary. NONE leaves the element out. */
enum NYA_HttpSitemapChangeFreq {
    NYA_HTTP_SITEMAP_CHANGEFREQ_NONE = 0,
    NYA_HTTP_SITEMAP_ALWAYS,
    NYA_HTTP_SITEMAP_HOURLY,
    NYA_HTTP_SITEMAP_DAILY,
    NYA_HTTP_SITEMAP_WEEKLY,
    NYA_HTTP_SITEMAP_MONTHLY,
    NYA_HTTP_SITEMAP_YEARLY,
    NYA_HTTP_SITEMAP_NEVER,

    NYA_HTTP_SITEMAP_CHANGEFREQ_COUNT,
};

/** One URL. Only `loc` is required; the rest are hints a crawler may read, each with a flag that omits it. */
struct NYA_HttpSitemapUrl {
    /** The URL, required, http or https. Escaped into the document; a non-web loc refuses the build. */
    NYA_ConstCString loc;

    /** When it last changed. Emitted as RFC 3339 when `has_lastmod`; ignored otherwise. */
    NYA_Instant lastmod;
    b8          has_lastmod;

    /** How often it changes. NONE omits the element. */
    NYA_HttpSitemapChangeFreq changefreq;

    /** 0.0 to 1.0, its priority relative to the site's other URLs. Emitted when `has_priority`; out of range refuses the build. */
    f32 priority;
    b8  has_priority;
};

/** The URLs a sitemap is built from. */
struct NYA_HttpSitemapConfig {
    const NYA_HttpSitemapUrl* urls;
    u32                       count;
};

// FUNCTIONS

/**
 * Builds the `<urlset>` document into `out`, allocated from `arena`.
 *
 * NYA_ERROR_INVALID_ARGUMENT for more than NYA_HTTP_SITEMAP_MAX_URLS entries, a loc that is not an http
 * or https URL, or a priority outside 0.0..1.0; NYA_ERROR_OUT_OF_MEMORY if the document overflows the
 * bound, in which case nothing is emitted. `out` is a NUL-terminated C string on success.
 * */
NYA_API NYA_Error nya_http_sitemap_build(NYA_Arena* arena, NYA_HttpSitemapConfig config, OUT NYA_ConstCString* out) __attr_no_discard;

/**
 * Builds the document and serves it at GET /sitemap.xml as `application/xml`, through the http_doc
 * registry. Merge nya_http_doc_router() to bring it online; nya_http_doc_clear forgets it. Same refusals
 * as the build.
 * */
NYA_API NYA_Error nya_http_sitemap_mount(NYA_HttpSitemapConfig config);
