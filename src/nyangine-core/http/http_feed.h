/**
 * @file http_feed.h
 *
 * A web feed, in both dialects a reader might ask for: RSS 2.0 as the primary and Atom 1.0 beside it,
 * built from the same set of items.
 *
 * ```
 * nya_http_feed_rss    a channel and its items -> the <rss><channel>… document
 * nya_http_feed_atom   the same items          -> the <feed>… document
 * nya_http_feed_mount  builds both and serves RSS at /feed.xml and Atom at /atom.xml
 * ```
 *
 * ```c
 * const NYA_HttpFeedItem items[] = {
 *     { .title = "First post", .link = "https://example.com/first", .description = "Hello & <welcome>",
 *       .published = nya_instant_now(), .has_published = true },
 * };
 * NYA_HttpFeedConfig feed = {
 *     .title = "Example", .link = "https://example.com/", .description = "An example feed",
 *     .self_link = "https://example.com/feed.xml", .updated = nya_instant_now(), .has_updated = true,
 *     .items = items, .count = nya_carray_length(items),
 * };
 * NYA_EXPECT(nya_http_feed_mount(feed));
 * NYA_EXPECT(nya_http_server_merge(nya_http_doc_router()));
 * ```
 *
 * ── the two formats, one set of data ──
 *
 * RSS is the primary because it is what most readers still take, and Atom is emitted beside it because
 * it is nearly free once the data is in hand and some readers prefer it. The dates differ by format and
 * this file gets each right: RSS uses RFC 822 (the IMF-fixdate nya_instant_to_rfc9110 writes, "Tue, 22
 * Sep 2026 14:03:11 GMT") and Atom uses RFC 3339 ("2026-09-22T14:03:11Z"). A feed dated in the wrong
 * format is one a reader silently drops.
 *
 * Every text value is XML-escaped, and an item `description` goes inside a CDATA section so it may carry
 * markup without each `<` being an entity; a literal `]]>` in it is split so it cannot close the section
 * early (see http_doc.h). Every link — the channel's, the feed's self link, each item's — is gated by
 * nya_http_doc_url_is_web, so a `javascript:` link refuses the build rather than reaching a reader.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_clock_instant.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

#define NYA_HTTP_FEED_PATH      "/feed.xml"
#define NYA_HTTP_FEED_ATOM_PATH "/atom.xml"

/** Items one feed here carries. A loop ceiling under the shared byte bound (NYA_HTTP_DOC_MAX_BYTES), which is the hard backstop. */
#define NYA_HTTP_FEED_MAX_ITEMS 500

// TYPES

typedef struct NYA_HttpFeedItem   NYA_HttpFeedItem;
typedef struct NYA_HttpFeedConfig NYA_HttpFeedConfig;

/** One entry in the feed. `title` and `link` are required; the rest are optional. */
struct NYA_HttpFeedItem {
    /** Required. Escaped into the document. */
    NYA_ConstCString title;

    /** Required, http or https. A non-web link refuses the build. */
    NYA_ConstCString link;

    /** A stable identity for the entry. Any text; when empty, `link` stands in. RSS guid / Atom id. */
    NYA_ConstCString guid;

    /** The body or summary. Optional. Carried in CDATA for RSS and escaped text for Atom's summary. */
    NYA_ConstCString description;

    /** When it was published or last updated. Emitted as RSS pubDate / Atom updated when `has_published`. */
    NYA_Instant published;
    b8          has_published;
};

/** The channel, and the items under it. `title`, `link` and `description` are required of the channel. */
struct NYA_HttpFeedConfig {
    NYA_ConstCString title;
    NYA_ConstCString link;        /**< The site the feed is for, http or https. */
    NYA_ConstCString description;

    /** The feed's own URL, for Atom's `rel="self"` link and its `<id>`. Optional; must be web when set. */
    NYA_ConstCString self_link;

    /** When the feed as a whole last changed. Atom requires one, so when this is unset the newest item's date stands in. */
    NYA_Instant updated;
    b8          has_updated;

    const NYA_HttpFeedItem* items;
    u32                     count;
};

// FUNCTIONS

/**
 * Builds the RSS 2.0 document into `out`, allocated from `arena`.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a missing channel title, link or description, more than
 * NYA_HTTP_FEED_MAX_ITEMS items, an item without a title, or any link that is not an http or https URL;
 * NYA_ERROR_OUT_OF_MEMORY on overflow, with nothing emitted.
 * */
NYA_API NYA_Error nya_http_feed_rss(NYA_Arena* arena, NYA_HttpFeedConfig config, OUT NYA_ConstCString* out) __attr_no_discard;

/**
 * Builds the Atom 1.0 document into `out`, allocated from `arena`.
 *
 * The same refusals as the RSS build, plus NYA_ERROR_INVALID_ARGUMENT when no feed-level date can be
 * determined — neither `has_updated` nor any dated item — since Atom requires a feed `<updated>`.
 * */
NYA_API NYA_Error nya_http_feed_atom(NYA_Arena* arena, NYA_HttpFeedConfig config, OUT NYA_ConstCString* out) __attr_no_discard;

/**
 * Builds both and serves RSS at GET /feed.xml (`application/rss+xml`) and Atom at GET /atom.xml
 * (`application/atom+xml`), through the http_doc registry. Merge nya_http_doc_router() to bring them
 * online. Same refusals as the two builds.
 * */
NYA_API NYA_Error nya_http_feed_mount(NYA_HttpFeedConfig config);
