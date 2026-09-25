/**
 * @file http_files.h
 *
 * Uploads and downloads as a router a server merges: a file goes into the content-addressed blob store,
 * a row records who owns it and what it is, and it comes back out only to the account that owns it — the
 * db_blob.h store wired to HTTP behind accounts, the way http_accounts.h wires the login primitives.
 *
 * ```c
 * const NYA_HttpRouter* routes = nya_http_files_open((NYA_HttpFilesConfig){
 *     .arena    = arena,
 *     .database = database,
 * });
 * if (routes == nullptr) return EXIT_FAILURE;
 * defer nya_http_files_close();
 *
 * NYA_TRY(nya_http_server_merge(routes));
 * ```
 *
 * ── what it mounts ──
 *
 * ```
 * POST /api/files            a multipart/form-data upload: stores the file part, answers its id
 * GET  /api/files?id=<n>     downloads the file with that id, if the caller owns it
 * ```
 *
 * ── an id is not a capability ──
 *
 * The download id is a small integer a row assigned, guessable by design and meant to appear in a URL,
 * so it is never on its own permission to read the bytes. Every download resolves the caller from the
 * `__Host-session` cookie the same way the rest of the application does (nya_http_accounts_caller), finds
 * the row, and refuses with 403 when the row's owner is not that caller — before a single byte of the
 * blob is read. Guessing another account's id gets that 403, not their file. This is the check db_blob.h
 * says the facade above it must make, and this is the facade.
 *
 * ── bounded, and where the bound really is ──
 *
 * An upload is refused past NYA_HTTP_FILES_MAX_UPLOAD_BYTES, checked against the request's Content-Length
 * before the body is parsed so an oversize upload is a cheap 413 rather than work. That per-route ceiling
 * sits under the server's own NYA_HTTP_MAX_BODY_BYTES, which is the true bound: a request body is
 * buffered whole by http_server.h before any handler runs, so an upload cannot be larger than that buffer
 * however this route is configured. The multipart parser (http_multipart.h) hands back each part as a
 * range into that one buffer and copies nothing, so the store's put is the only copy the path makes.
 *
 * ── the filename, and why it is never a path ──
 *
 * The stored name is the uploaded filename reduced to a safe set — letters, digits, dot, dash, underscore
 * and space — with everything else, path separators included, folded away, so a name can be neither a
 * traversal nor a header injection when it is reflected back in a `Content-Disposition`. A download is
 * always served `Content-Disposition: attachment` with `X-Content-Type-Options: nosniff`, so a browser
 * saves the file rather than rendering it, and an uploaded `text/html` cannot run as a page on this origin.
 *
 * ── one process, one mount ──
 *
 * A singleton like http_accounts.h: the route table and the config are module state, so a process mounts
 * it once, and nya_http_files_open refuses a second open while one is live. Every route is
 * NYA_HTTP_AFFINITY_MAIN, because the one database is only ever touched on the ticking thread.
 *
 * nya_accounts_open and the accounts routes must be mounted first: the ownership check reads the caller
 * through nya_http_accounts_caller, which answers nobody when no accounts mount is live.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/db/db_blob.h"
#include "nyangine-core/db/db_sql.h"
#include "nyangine-core/http/http_router.h"

// CONSTANTS

/** The paths this module answers on. Exported so a client, a test or a reverse proxy names the same ones. */
#define NYA_HTTP_FILES_PATH "/api/files"

/**
 * The largest upload this route accepts, checked against Content-Length before the body is parsed.
 *
 * Deliberately under the server's NYA_HTTP_MAX_BODY_BYTES, which buffers the whole request first and is
 * the real ceiling; see the file note. This route's own limit is the one a caller tunes, and it leaves
 * room under that buffer for the multipart framing around the file. A file also goes back out of the one
 * shared response buffer in a single write, so it can never exceed NYA_HTTP_MAX_RESPONSE_BYTES.
 * */
#define NYA_HTTP_FILES_MAX_UPLOAD_BYTES 4096

/** Bytes a stored (sanitised) filename may take, terminator included. Long enough for a name, not a sentence. */
#define NYA_HTTP_FILES_MAX_NAME 128

/** Bytes a stored content type may take, terminator included. "application/vnd.…+json" and its charset fit. */
#define NYA_HTTP_FILES_MAX_CONTENT_TYPE 128

// TYPES

typedef struct NYA_HttpFilesConfig NYA_HttpFilesConfig;
typedef struct NYA_HttpFile        NYA_HttpFile;

/** What a program hands the file routes: where the blob store and the record table live. */
struct NYA_HttpFilesConfig {
    /** The arena the blob store and the record table's prepared statements live in, until nya_http_files_close. Required. */
    NYA_Arena* arena;

    /** The database the blob store and the record table are opened on. Usually the accounts database. Required. */
    NYA_Database* database;
};

/**
 * One uploaded file, as it is stored: the row that binds a blob to its owner and its metadata.
 *
 * Flat on purpose, so the ORM (db_orm.h) derives a column per field. The bytes themselves are not here —
 * they are in the blob store under `blob_id`, deduplicated across every row that names the same content.
 * */
struct NYA_HttpFile {
    /** The row id sqlite assigns, and the id a download names. Zero on a record not yet inserted. */
    s64 id;

    /** The account that owns the file, from the session that uploaded it. A download must match it. */
    s64 owner;

    /** The content address of the bytes in the blob store: the SHA-256, 64 lower case hex digits. */
    char blob_id[NYA_BLOB_ID_LENGTH + 1];

    /** The uploaded filename, reduced to a safe set; never a path. Reflected back in Content-Disposition. */
    char name[NYA_HTTP_FILES_MAX_NAME];

    /** The validated content type, served back as Content-Type. "application/octet-stream" when unknown. */
    char content_type[NYA_HTTP_FILES_MAX_CONTENT_TYPE];

    /** The file's size in bytes, which equals the blob's. */
    s64 size;

    /** Unix seconds when the row was recorded. */
    s64 created;
};

// FUNCTIONS

/**
 * Opens the blob store and the record table, stores `config`, and answers the router to merge — or null
 * on a failure it has already logged.
 *
 * Null when the arena or database is missing, when the accounts tables are not open (the ownership check
 * needs them), or when a table cannot be opened. Refuses a second call while a mount is live, because the
 * route table and the config are one per process; see the file note.
 *
 * The returned router has static storage and outlives the call, so it is merged directly with
 * nya_http_server_merge. Its close is nya_http_files_close.
 * */
NYA_API const NYA_HttpRouter* nya_http_files_open(NYA_HttpFilesConfig config) __attr_no_discard;

/** Closes the record table and forgets the config. The mirror of nya_http_files_open; the database is the caller's. */
NYA_API void nya_http_files_close(void);
