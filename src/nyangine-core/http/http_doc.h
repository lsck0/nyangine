/**
 * @file http_doc.h
 *
 * The pieces the four discoverability documents are built and served with, in one place so each of them
 * is a table of data and a call, not a string builder of its own.
 *
 * ```
 * nya_http_doc_over      a bounded builder over `capacity` bytes of an arena
 * nya_http_doc_put       append text, verbatim
 * nya_http_doc_putf      append text, formatted
 * nya_http_doc_xml_text  append text, XML-escaped for an element body:  & < >
 * nya_http_doc_xml_attr  append text, XML-escaped for an attribute value: & < > " '
 * nya_http_doc_xml_cdata append text wrapped in a CDATA section, with any "]]>" split so it cannot close early
 * nya_http_doc_finish    the built string, or NYA_ERROR_OUT_OF_MEMORY if anything overflowed the bound
 *
 * nya_http_doc_url_is_web  whether a string parses as an http or https URL — the one gate on every loc and link
 *
 * nya_http_doc_serve     register a generated document to be served at a path, with its media type
 * nya_http_doc_router    the table of those, to merge into a running server
 * nya_http_doc_clear     forget every served document; the pair of a mount, for a test or a re-mount
 * ```
 *
 * ── why one builder, and why it refuses rather than truncates ──
 *
 * A sitemap, a feed, a robots file and an llms file are all text assembled from data a program supplies,
 * and the one thing that must never happen to any of them is that a `<` or a `&` in that data lands in
 * the output as itself: in XML that is an injection, and in every format it is a document a parser
 * rejects. So the builder is the only way bytes get in, and the two XML escapers are the only way
 * caller data does. `put` is for the markup this file's callers write as literals; caller data goes
 * through `xml_text`, `xml_attr` or `xml_cdata`, or through a format string that has already escaped it.
 *
 * The output is bounded — NYA_HTTP_DOC_MAX_BYTES, which is the server's per-response body buffer, so a
 * document that builds is a document that can be served — and on the first byte that would not fit the
 * builder stops and remembers it. `finish` then refuses the whole thing rather than hand back a document
 * cut off mid-tag, which is the same refuse-whole-on-overflow the rest of this engine's builders take: a
 * truncated sitemap is not a smaller sitemap, it is a parse error at a random offset.
 *
 * ── the serve registry ──
 *
 * Each document is generated once, at mount, and served as those bytes until it is cleared; none of them
 * is rebuilt per request, because the data behind a sitemap or a feed changes far more slowly than it is
 * fetched. `nya_http_doc_serve` copies the finished document into the registry's own arena, so the arena
 * a caller built it in is free to die straight after, and adds one exact GET route for it. The routes
 * are read-only once mounted, so they run on a worker; see http_router.h.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_router.h"
#include "nyangine-core/http/http_types.h"

// CONSTANTS

/**
 * The largest document any of these builders will produce, terminator excluded.
 *
 * Tied to the server's per-response body buffer on purpose: a handler answers from that one buffer
 * (NYA_HTTP_MAX_RESPONSE_BYTES), so a document larger than it is one that builds and then cannot be
 * served. Making the two the same means a build that succeeds is a serve that succeeds, and a program
 * that needs a larger sitemap than this splits it into a sitemap index rather than growing this bound.
 * */
#define NYA_HTTP_DOC_MAX_BYTES NYA_HTTP_MAX_RESPONSE_BYTES

/** How many generated documents one server serves at once. Four presets and a little room; well past what any program mounts. */
#define NYA_HTTP_DOC_MAX_ROUTES 8

// TYPES

typedef struct NYA_HttpDoc NYA_HttpDoc;

/**
 * A bounded, append-only text builder.
 *
 * Transparent, but there is no constructor but nya_http_doc_over and the fields are read, never written,
 * by a caller: `used` is how much is built, `overflowed` is whether anything did not fit, and `finish`
 * is what turns the two into a string or a refusal.
 * */
struct NYA_HttpDoc {
    /** Arena-owned, NYA_HTTP_DOC_MAX_BYTES + 1 at most, always NUL-terminated at `used`. */
    char* buffer;

    /** How many bytes `buffer` may hold before the terminator. */
    u64 capacity;

    /** How many are written. Never past `capacity`. */
    u64 used;

    /** Set the first time an append did not fit, and never unset. `finish` refuses when it is true. */
    b8 overflowed;
};

// FUNCTIONS

/**
 * A builder over `capacity` bytes of `arena`, `capacity` clamped to NYA_HTTP_DOC_MAX_BYTES.
 *
 * The buffer is allocated from `arena` and lives as long as it does; the returned value is small and is
 * held by the caller. An arena that runs out leaves `buffer` null, which every append notices and treats
 * as an overflow, so a caller may build first and check once at `finish`.
 * */
NYA_API NYA_HttpDoc nya_http_doc_over(NYA_Arena* arena, u64 capacity) __attr_no_discard;

/** Appends `text` verbatim. For the markup a builder writes as a literal, never for caller data. */
NYA_API void nya_http_doc_put(NYA_HttpDoc* doc, NYA_ConstCString text);

/** Appends `text` formatted. The format string is the builder's; a caller value in it must already be escaped. */
NYA_API void nya_http_doc_putf(NYA_HttpDoc* doc, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/**
 * Appends `text` with `&`, `<` and `>` turned into entities: the escaping XML element content needs, so
 * a value carrying `<script>` becomes text and not a tag.
 * */
NYA_API void nya_http_doc_xml_text(NYA_HttpDoc* doc, NYA_ConstCString text);

/**
 * Appends `text` with `&`, `<`, `>`, `"` and `'` turned into entities: element escaping plus the two
 * quotes, so the value is safe inside a `"…"` or a `'…'` attribute.
 * */
NYA_API void nya_http_doc_xml_attr(NYA_HttpDoc* doc, NYA_ConstCString text);

/**
 * Appends `text` inside `<![CDATA[ … ]]>`, with any literal `]]>` in it split across two sections so it
 * cannot close the CDATA early. What an RSS description uses to carry markup without entity-escaping it.
 * */
NYA_API void nya_http_doc_xml_cdata(NYA_HttpDoc* doc, NYA_ConstCString text);

/**
 * The finished document in `out`, NUL-terminated, or NYA_ERROR_OUT_OF_MEMORY when any append overflowed
 * the bound — in which case `out` is left null and nothing half-built is handed back.
 * */
NYA_API NYA_Error nya_http_doc_finish(const NYA_HttpDoc* doc, OUT NYA_ConstCString* out) __attr_no_discard;

/**
 * Whether `text` parses as an absolute http or https URL.
 *
 * The one check every loc, link and Sitemap line runs before the URL reaches a document. It parses the
 * URL with nya_url_parse — which refuses a control byte, a space, a malformed escape and every scheme
 * this engine does not speak — and then requires that the scheme is one of the two the web is fetched
 * over. That is what turns away `javascript:`, `data:`, `file:`, a `ws:` socket URL and a bare path: a
 * loc a crawler would not fetch has no business in a sitemap, and a `javascript:` link in a feed is the
 * injection this is here to stop.
 * */
NYA_API b8 nya_http_doc_url_is_web(NYA_ConstCString text) __attr_no_discard;

/**
 * Registers `body` to be served at `path` with `media`, under a route summarised by `summary`.
 *
 * `body` is copied into the registry's arena, so the caller's is free to die after; `path` and `summary`
 * are copied too. NYA_ERROR_INVALID_ARGUMENT for a null or non-absolute path, an empty body or a null
 * summary; NYA_ERROR_ALREADY_EXISTS when `path` is already served; NYA_ERROR_OUT_OF_MEMORY once
 * NYA_HTTP_DOC_MAX_ROUTES are registered or `body` is larger than a response may be. Register before the
 * server serves, the same contract nya_http_server_merge has.
 * */
NYA_API NYA_Error nya_http_doc_serve(NYA_ConstCString path, NYA_HttpMediaType media, NYA_ConstCString body, NYA_ConstCString summary);

/** The table of served documents, to merge into a running server. Static storage; outlives any mount. */
NYA_API const NYA_HttpRouter* nya_http_doc_router(void) __attr_no_discard;

/** Forgets every served document and frees their bytes, so the registry is empty again. */
NYA_API void nya_http_doc_clear(void);
