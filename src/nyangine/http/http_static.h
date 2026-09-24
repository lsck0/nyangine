/**
 * @file http_static.h
 *
 * The web bundle, served out of the asset system: a page, its stylesheet and its script, each at a
 * name that contains the hash of its own bytes, and each with the caching policy that name earns.
 *
 * ```
 * nya_http_static_mount    takes a listed set of assets and their bytes, hashes them, builds the routes
 * nya_http_static_unmount  the pair. Frees the bytes and empties the table
 * nya_http_static_router   the table, to merge into a running server
 * nya_http_static_url      the hashed URL one asset is served at, for a page that generates links
 * ```
 *
 * ```c
 * u8* data = nullptr;
 * u64 size = 0;
 * NYA_EXPECT(nya_asset_read(scratch, NYA_ASSET_WEB_INDEX_HTML, &data, &size));
 *
 * NYA_HttpStaticFile files[] = {
 *     { .asset = NYA_ASSET_WEB_INDEX_HTML, .path = "/", .data = data, .size = size },
 * };
 *
 * NYA_EXPECT(nya_http_static_mount((NYA_HttpStaticConfig){ .files = files, .count = nya_carray_length(files) }));
 * defer nya_http_static_unmount();
 *
 * NYA_EXPECT(nya_http_server_merge(nya_http_static_router()));
 * ```
 *
 * ── the caller reads, this serves ──
 *
 * The bytes arrive already read, and `nya_asset_read` is what reads them: html, css and js are assets
 * like a texture or a font, so they get the asset system's handles, its files in a debug build and its
 * baked blob in a release, and there is no second way in here to open one. The read is the caller's
 * because `core` sits above `http` in the module order this tree is moving to — a server that reached
 * into the asset system would invert it. It costs a caller three lines and buys a module that depends
 * on `base` and nothing else.
 *
 * Mount copies what it is given, so the caller's arena is free to die straight after.
 *
 * ── one route per file, and no path resolution anywhere ──
 *
 * A mount turns each listed asset into two exact routes and nothing else: `/static/app.<hash>.css`,
 * and whatever `path` says. The router matches a path by comparing it whole (http_router.h), so a
 * request either *is* one of those strings or it is a 404. There is no root to escape from, no segment
 * to join onto a directory and no byte of the request that ever becomes part of a file name — which is
 * the one design decision here that matters, because path resolution is what static serving gets wrong.
 *
 * It follows that there are no listings, no implicit `index.html` for a directory, and no "try the
 * path, then the path plus .html". The entry point is the file whose `path` the program wrote down.
 *
 * The remaining way a traversal could get in is the program itself, building an asset handle out of
 * something a stranger said. So a mount checks every handle before it serves a byte of it: under the
 * root, made only of `A-Za-z0-9._-` and `/`, no `.` or `..` segment, no empty segment, no leading dot
 * on a segment, and a suffix this server has a media type for. That refuses `..`, an absolute path, a
 * backslash, a percent escape, a NUL, and every byte an overlong UTF-8 sequence is made of, at startup
 * and by the same rule. Where the assets are still files rather than a baked blob, a handle naming a
 * symlink or a directory is refused as well: a link is a name for bytes somewhere else, which is the
 * whole thing this is trying not to serve.
 *
 * ── what a name means ──
 *
 * The hash is SHA-256 over the bytes as they are served, taken at mount, and the first
 * NYA_HTTP_STATIC_HASH_DIGITS hex digits of it spell both the name and the ETag. One hash, one
 * spelling: a file cannot be cached under a name that describes different bytes.
 *
 * | Path                       | Cache-Control                            | Why                                     |
 * | :------------------------- | :--------------------------------------- | :-------------------------------------- |
 * | `/static/app.<hash>.css`   | `public, max-age=31536000, immutable`     | new bytes are a new name, so it cannot go stale |
 * | `/`, `/app.css`            | `no-cache`                                | one name, changing bytes: stored, and revalidated every time |
 *
 * Both carry the ETag, and both answer a matching `If-None-Match` with 304 and no body. `no-cache`
 * does not mean "do not store"; it means "store it and ask me first", which is exactly what an entry
 * point wants and is why the second row is not `no-store`.
 *
 * There is no `Last-Modified` and no `If-Modified-Since`. The ETag answers the question exactly, a
 * second validator is a second answer that can disagree with the first, and in a release build the
 * bytes come out of the executable's `.rodata` and have no modification time that is not a fiction.
 *
 * ── no content coding ──
 *
 * Responses go out as they are stored, with no `Content-Encoding` and no `Vary: Accept-Encoding`. The
 * compressor this engine vendors is LZ4, which is not a registered HTTP content coding and which no
 * browser can decode; gzip and brotli are a dependency decision of their own, not a side effect of
 * serving a page. A `Vary` on a response that does not vary only splits every cache entry in two.
 *
 * The build does compress the bundle where compression pays here: `src/build/pp/asset.c` stores each
 * asset LZ4-compressed inside the executable when that is smaller, so a release carries the bundle
 * compressed and the asset system expands it once on the read that feeds a mount.
 *
 * Ranges are not implemented either, and nothing advertises `Accept-Ranges`. A file is at most
 * NYA_HTTP_MAX_STATIC_FILE_BYTES and leaves in one write; a `Range` header is ignored and the whole
 * representation is answered, which is what RFC 9110 says a server that does not support ranges does.
 *
 * ── the policy an HTML response carries ──
 *
 * The server's default `Content-Security-Policy` is `default-src 'none'`, which is right for a JSON
 * body and would stop a page loading its own stylesheet. An HTML file out of the bundle replaces it
 * with NYA_HTTP_STATIC_PAGE_CSP: same origin for scripts, styles, images, fonts and fetches, and
 * nothing else — no inline anything, no `eval`, no frame, no `<base>`, no form target. Every other
 * media type keeps the default, so an SVG opened on its own is still a document that may load nothing.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_router.h"
#include "nyangine/http/http_types.h"

// CONSTANTS

/** Where the hashed names live when a mount does not say. One segment, so a bundle is one branch of the tree. */
#define NYA_HTTP_STATIC_PREFIX "/static"

/** The asset directory a mount serves out of when it does not say. */
#define NYA_HTTP_STATIC_ROOT "./assets/web"

/**
 * The policy an HTML file from the bundle is served under, replacing the server's default.
 *
 * Same origin and nothing more: no inline script or style, so a markup injection cannot execute; no
 * `data:` anywhere a script could come from; and the three that say where the page itself may go —
 * `frame-ancestors`, `base-uri`, `form-action` — stay shut as they are in the default.
 * */
#define NYA_HTTP_STATIC_PAGE_CSP                                                                                                                     \
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; connect-src 'self'; "                                  \
    "frame-ancestors 'none'; base-uri 'none'; form-action 'none'"

/**
 * The same, for a page that submits a form back to its own origin.
 *
 * The one difference is `form-action 'self'`: the default and the bundle policy above both shut form
 * submission off entirely, which is right for a JSON API and a single-page app that never posts a
 * form, and wrong for server-rendered HTML where the form *is* the app. A server that renders forms
 * sets this on those responses itself, the same way it would set any other CSP.
 *
 * Still no inline anything: HTMX works as markup attributes, and Alpine needs its CSP build, so a
 * server-rendered page needs `'unsafe-inline'` nowhere. A page that reaches for an inline `<script>`
 * or an `hx-on:` handler has to widen this, and widening it is the decision the default makes loud.
 * */
#define NYA_HTTP_STATIC_FORM_CSP                                                                                                                     \
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; connect-src 'self'; "                                  \
    "frame-ancestors 'none'; base-uri 'none'; form-action 'self'"

// TYPES

typedef struct NYA_HttpStaticFile   NYA_HttpStaticFile;
typedef struct NYA_HttpStaticConfig NYA_HttpStaticConfig;

/** One file of the bundle: which asset, its bytes, and the unhashed path it also answers at. */
struct NYA_HttpStaticFile {
    /**
     * The asset handle the bytes came out of, which is the generated NYA_ASSET_ constant:
     * "./assets/web/app.css". Required, and it has to be under the mount's root; see the file note for
     * every rule it is held to.
     *
     * Its suffix decides the Content-Type, and a suffix with no media type is refused at mount. The
     * request never has a say in what a file is: a client that announces `text/html` for a stylesheet
     * gets a stylesheet.
     * */
    NYA_ConstCString asset;

    /** The bytes `asset` holds, as nya_asset_read returned them. Copied at mount. */
    const u8* data;
    u64       size;

    /**
     * Where it is served besides its hashed name, revalidating: "/" for the entry point, "/app.css" for
     * anything a hand-written page links by name. Null for a file that is only reachable by its hash,
     * which is what a generated bundle wants.
     * */
    NYA_ConstCString path;
};

struct NYA_HttpStaticConfig {
    const NYA_HttpStaticFile* files;
    u32                       count;

    /** The asset directory every file has to be under, no trailing slash. Null means NYA_HTTP_STATIC_ROOT. */
    NYA_ConstCString root;

    /** Where the hashed names go, absolute and with no trailing slash. Null means NYA_HTTP_STATIC_PREFIX. */
    NYA_ConstCString prefix;
};

// FUNCTIONS

/**
 * Takes a copy of every listed file, hashes it, and builds the route table. Mounts nothing on the
 * server: the program merges nya_http_static_router as it merges any other resource.
 *
 * All or nothing. The first file that fails any check takes the whole mount with it and leaves the
 * table empty, because a bundle serving three of its four files is a page that half loads and a
 * failure nobody notices until a person opens it.
 *
 * NYA_ERROR_ALREADY_EXISTS when a mount is live, NYA_ERROR_INVALID_ARGUMENT for a handle or a path
 * that breaks one of the rules in the file note, and NYA_ERROR_OUT_OF_MEMORY when a file or the whole
 * bundle is past its bound in http_types.h.
 * */
NYA_API NYA_Error nya_http_static_mount(NYA_HttpStaticConfig config) __attr_no_discard;

/** Drops every file and frees the bytes. Idempotent. Unmerge the router first, or it answers 404s. */
NYA_API void nya_http_static_unmount(void);

/** The bundle's routes. Always valid; empty, and so matching nothing, until a mount succeeds. */
NYA_API const NYA_HttpRouter* nya_http_static_router(void) __attr_no_discard;

/**
 * The hashed URL `asset` is served at — "/static/app.1f0a….css" — or null when it is not mounted.
 *
 * What a generated page links to, so the link carries the immutable policy rather than the
 * revalidating one. The string belongs to the mount and dies with it.
 * */
NYA_API NYA_ConstCString nya_http_static_url(NYA_ConstCString asset) __attr_no_discard;

/** How many files the current mount serves. Zero when nothing is mounted. */
NYA_API u32 nya_http_static_file_count(void) __attr_no_discard;

/**
 * One number over every mounted file's content hash, in mount order: the bundle's version, and the whole
 * of what a live-reload watch has to compare between two mounts.
 *
 * A mount is a snapshot, so this changes only when the bundle is unmounted and mounted again with
 * different bytes — a new file, a dropped one, or one whose hash moved — which is exactly a rebuild. Zero
 * when nothing is mounted, a value a real bundle never folds to, so a watcher tells "nothing yet" from
 * "these bytes" without a second flag. It is FNV-1a over the ETags http_static already computed, not a
 * second pass over the bytes, so polling it costs nothing. Not stable across builds or machines and never
 * a cache key or an ETag of its own: the per-file hash is the identity a client sees, this only says
 * whether the set of them moved. See http_livereload.h for the watch that reads it.
 * */
NYA_API u64 nya_http_static_fingerprint(void) __attr_no_discard;
