#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_filesystem.h"
#include "nyangine-std/base/base_hash.h"
#include "nyangine-std/base/base_logging.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/http/http_static.h"

// CONSTANTS

static_assert(NYA_HTTP_MAX_STATIC_FILE_BYTES <= NYA_HTTP_MAX_RESPONSE_BYTES,
              "a served file leaves in one write out of the shared response buffer, so it cannot be bigger than it");

static_assert(NYA_HTTP_STATIC_HASH_DIGITS % 2 == 0 && NYA_HTTP_STATIC_HASH_DIGITS <= NYA_CRYPTO_SHA256_BYTES * 2,
              "the hash is whole bytes of a SHA-256 digest written as hex");

/** Routes a mount builds: the hashed name, and the unhashed one for a file that asked for one. */
#define _NYA_HTTP_STATIC_MAX_ROUTES (NYA_HTTP_MAX_STATIC_FILES * 2)

/** An ETag is the hash in quotes, and the terminator. */
#define _NYA_HTTP_STATIC_ETAG_SIZE (NYA_HTTP_STATIC_HASH_DIGITS + 3)

/** Spelled from the constant rather than beside it, so the header and the number cannot drift apart. */
#define _NYA_HTTP_STATIC_TEXT_(value) #value
#define _NYA_HTTP_STATIC_TEXT(value)  _NYA_HTTP_STATIC_TEXT_(value)

#define _NYA_HTTP_STATIC_CACHE_IMMUTABLE "public, max-age=" _NYA_HTTP_STATIC_TEXT(NYA_HTTP_STATIC_IMMUTABLE_MAX_AGE_S) ", immutable"

/**
 * What an unhashed name is cached under.
 *
 * `no-cache` stores the answer and revalidates it before every reuse, which with an ETag costs one 304
 * and no body. `no-store` would be the other reading of the word and is wrong here: it would make a
 * browser fetch the whole entry point on every navigation for no benefit at all.
 * */
#define _NYA_HTTP_STATIC_CACHE_REVALIDATE "no-cache"

#define _NYA_HTTP_STATIC_SUMMARY_IMMUTABLE "A file of the web bundle, at the hash of its own bytes"
#define _NYA_HTTP_STATIC_SUMMARY_NAMED     "A file of the web bundle, at its name"

// TYPES

typedef struct _NYA_HttpStaticEntry _NYA_HttpStaticEntry;
typedef struct _NYA_HttpStaticState _NYA_HttpStaticState;

/** One file, read once and kept: the bytes that go out, and everything said about them. */
struct _NYA_HttpStaticEntry {
    NYA_ConstCString asset;

    /** Owned by the mount's arena. */
    const u8* data;
    u64       size;

    /** From the asset's own suffix, decided at mount. Nothing a request says can change it. */
    NYA_HttpMediaType media_type;

    /** The content hash in quotes, ready to be a header value. */
    char etag[_NYA_HTTP_STATIC_ETAG_SIZE];

    /** "/static/app.1f0a….css", in the mount's arena. */
    NYA_ConstCString hashed_path;
};

struct _NYA_HttpStaticState {
    NYA_Arena* arena;

    _NYA_HttpStaticEntry files[NYA_HTTP_MAX_STATIC_FILES];
    u32                  file_count;

    NYA_HttpRoute routes[_NYA_HTTP_STATIC_MAX_ROUTES];
    u32           route_count;

    /** Which file each route answers from, and whether it is that file's hashed name. Parallel to `routes`. */
    u32 route_file[_NYA_HTTP_STATIC_MAX_ROUTES];
    b8  route_immutable[_NYA_HTTP_STATIC_MAX_ROUTES];

    /** Every file's bytes together, against NYA_HTTP_MAX_STATIC_BYTES, and reported as a gauge. */
    u64 bytes;
};

/** One suffix and what it is. A file whose suffix is not in here is refused rather than guessed at. */
typedef struct {
    NYA_ConstCString  suffix;
    NYA_HttpMediaType media_type;
} _NYA_HttpStaticSuffix;

// PRIVATE API DECLARATION

NYA_INTERNAL _NYA_HttpStaticState _NYA_HTTP_STATIC = { 0 };

NYA_INTERNAL NYA_HttpRouter _NYA_HTTP_STATIC_ROUTER = {
    .name        = "static",
    .routes      = _NYA_HTTP_STATIC.routes,
    .route_count = 0,
};

/**
 * What a web bundle is made of, and the whole of what this will serve.
 *
 * Deliberately short. Every entry is a type a browser has a renderer for and this program has bytes
 * for; adding one is a decision about what a page here may contain, which is why it is a table and not
 * a fallback to `application/octet-stream`.
 * */
NYA_INTERNAL const _NYA_HttpStaticSuffix _NYA_HTTP_STATIC_SUFFIXES[] = {
    { ".html",  NYA_HTTP_MEDIA_HTML       },
    { ".css",   NYA_HTTP_MEDIA_CSS        },
    { ".js",    NYA_HTTP_MEDIA_JAVASCRIPT },
    { ".wasm",  NYA_HTTP_MEDIA_WASM       },
    { ".json",  NYA_HTTP_MEDIA_JSON       },
    { ".webmanifest", NYA_HTTP_MEDIA_MANIFEST },
    { ".svg",   NYA_HTTP_MEDIA_SVG        },
    { ".png",   NYA_HTTP_MEDIA_PNG        },
    { ".ico",   NYA_HTTP_MEDIA_ICON       },
    { ".woff2", NYA_HTTP_MEDIA_WOFF2      },
    { ".txt",   NYA_HTTP_MEDIA_TEXT       },
};

/** The one handler every route here shares; which file it answers from is the route it was reached by. */
NYA_INTERNAL NYA_HttpStatus _nya_http_static_serve(NYA_HttpExchange* exchange);

/**
 * The bytes a name may be made of: letters, digits, '.', '_' and '-'.
 *
 * The refusal list is what is missing. '/' is handled a segment at a time, and everything else is gone:
 * '\\' so a Windows path separator is not a separator here, '%' so nothing can be escaped into something
 * else, ':' so there is no drive letter and no NTFS stream, and every byte from 0x80 up, which is what
 * an overlong UTF-8 sequence for '/' or '.' is made of.
 * */
NYA_INTERNAL b8 _nya_http_static_is_name_char(char character) __attr_no_discard;

/** Whether `text` ends in `suffix`. nya_string_ends_with wants an NYA_String, and a handle is a C string. */
NYA_INTERNAL b8 _nya_http_static_ends_with(NYA_ConstCString text, NYA_ConstCString suffix) __attr_no_discard;

/**
 * Whether `text` is a series of segments this will serve, and an error saying which rule it broke.
 *
 * Used for the part of an asset handle under the root and for a served path alike, so a URL is exactly
 * as boring as the file behind it. No empty segment, no segment starting with '.' — which is where
 * "." and ".." go, and hidden files with them — and no byte outside _nya_http_static_is_name_char.
 * */
NYA_INTERNAL NYA_Error _nya_http_static_check_segments(NYA_ConstCString text, u64 size, NYA_ConstCString what) __attr_no_discard;

/** The media type `asset`'s suffix names, or NYA_HTTP_MEDIA_OTHER when it names none. */
NYA_INTERNAL NYA_HttpMediaType _nya_http_static_media_of(NYA_ConstCString asset) __attr_no_discard;

/** The first NYA_HTTP_STATIC_HASH_DIGITS hex digits of SHA-256 over `data`, written lower case. */
NYA_INTERNAL void _nya_http_static_hash(const u8* data, u64 size, OUT char* out_hex);

/** `relative` with `hash` inserted before its final suffix, under `prefix`: "/static/app.1f0a….css". */
NYA_INTERNAL NYA_ConstCString _nya_http_static_hashed_path(NYA_ConstCString prefix, NYA_ConstCString relative, NYA_ConstCString hash) __attr_no_discard;

/**
 * Whether `header`, an If-None-Match value, names `etag`.
 *
 * Weak comparison, as RFC 9110 8.8.3.2 requires of If-None-Match: `W/"x"` and `"x"` are the same entity
 * here. `*` matches, since this server does have a representation to send. Anything that is not a list
 * of entity tags matches nothing, so a malformed header costs the caller a body rather than a 304 it
 * cannot use.
 * */
NYA_INTERNAL b8 _nya_http_static_etag_matches(NYA_ConstCString header, NYA_ConstCString etag) __attr_no_discard;

/** Adds one route, refusing a path something else already answers: an unreachable route is a silent 404. */
NYA_INTERNAL NYA_Error _nya_http_static_route_add(NYA_ConstCString path, u32 file, b8 immutable_name) __attr_no_discard;

/** Checks one file, keeps a copy of it, hashes it and builds its routes. Everything a mount does per file. */
NYA_INTERNAL NYA_Error _nya_http_static_add(const NYA_HttpStaticFile* file, NYA_ConstCString root, NYA_ConstCString prefix) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_static_mount(NYA_HttpStaticConfig config) {
    if (_NYA_HTTP_STATIC.arena != nullptr) return nya_error(NYA_ERROR_ALREADY_EXISTS, "the web bundle is already mounted");

    if (config.files == nullptr || config.count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a mount serves at least one file");

    if (config.count > NYA_HTTP_MAX_STATIC_FILES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a mount serves at most %d files, not " FMTu32, NYA_HTTP_MAX_STATIC_FILES, config.count);
    }

    NYA_ConstCString root   = config.root != nullptr ? config.root : NYA_HTTP_STATIC_ROOT;
    NYA_ConstCString prefix = config.prefix != nullptr ? config.prefix : NYA_HTTP_STATIC_PREFIX;

    if (root[0] == '\0' || root[strlen(root) - 1] == '/') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the root '%s' ends in a separator", root);

    if (prefix[0] != '/' || prefix[strlen(prefix) - 1] == '/') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the prefix '%s' is not an absolute path without a trailing separator", prefix);
    }

    NYA_TRY(_nya_http_static_check_segments(prefix + 1, strlen(prefix) - 1, "the prefix"));

    _NYA_HTTP_STATIC.arena = nya_arena_create(.name = "http_static");

    for (u32 index = 0; index < config.count; index++) {
        NYA_Error added = _nya_http_static_add(&config.files[index], root, prefix);

        // all or nothing: a bundle missing one file is a page that half loads, worse than a program that refuses to start and says which file.
        if (!added.ok) {
            nya_http_static_unmount();
            return added;
        }
    }

    _NYA_HTTP_STATIC_ROUTER.route_count = _NYA_HTTP_STATIC.route_count;

    NYA_Error checked = nya_http_router_check(&_NYA_HTTP_STATIC_ROUTER);
    if (!checked.ok) {
        nya_http_static_unmount();
        return checked;
    }

    nya_log_info("Mounted " FMTu32 " files of the web bundle from %s, " FMTu64 " bytes.", _NYA_HTTP_STATIC.file_count, root, _NYA_HTTP_STATIC.bytes);

    return NYA_OK;
}

void nya_http_static_unmount(void) {
    if (_NYA_HTTP_STATIC.arena == nullptr) return;

    nya_arena_destroy(_NYA_HTTP_STATIC.arena);

    _NYA_HTTP_STATIC                    = (_NYA_HttpStaticState){ 0 };
    _NYA_HTTP_STATIC_ROUTER.route_count = 0;
}

const NYA_HttpRouter* nya_http_static_router(void) { return &_NYA_HTTP_STATIC_ROUTER; }

NYA_ConstCString nya_http_static_url(NYA_ConstCString asset) {
    if (asset == nullptr) return nullptr;

    for (u32 index = 0; index < _NYA_HTTP_STATIC.file_count; index++) {
        if (nya_string_equals(_NYA_HTTP_STATIC.files[index].asset, asset)) return _NYA_HTTP_STATIC.files[index].hashed_path;
    }

    return nullptr;
}

u32 nya_http_static_file_count(void) { return _NYA_HTTP_STATIC.file_count; }

u64 nya_http_static_fingerprint(void) {
    // Nothing mounted folds to zero rather than a hash of nothing, so a watch reads "no bundle yet" as a value apart from any real one and never signals a reload onto an empty server.
    if (_NYA_HTTP_STATIC.file_count == 0) return 0;

    // FNV-1a over each file's ETag in mount order, with a NUL between so two files can't pool into one stream; the ETag is the hash http_static already took at mount, so this folds over ~16 bytes a file, not a second walk of the bytes.
    u64 hash = NYA_HASH_FNV1A_OFFSET_BASIS;

    for (u32 index = 0; index < _NYA_HTTP_STATIC.file_count; index++) {
        const char* etag = _NYA_HTTP_STATIC.files[index].etag;

        hash = nya_hash_fnv1a_continue(hash, etag, strlen(etag) + 1);
    }

    return hash;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_static_is_name_char(char character) {
    if (character >= 'a' && character <= 'z') return true;
    if (character >= 'A' && character <= 'Z') return true;
    if (character >= '0' && character <= '9') return true;

    return character == '.' || character == '_' || character == '-';
}

b8 _nya_http_static_ends_with(NYA_ConstCString text, NYA_ConstCString suffix) {
    nya_assert(text != nullptr);
    nya_assert(suffix != nullptr);

    u64 text_size   = strlen(text);
    u64 suffix_size = strlen(suffix);

    if (suffix_size > text_size) return false;

    return nya_memcmp(text + text_size - suffix_size, suffix, suffix_size) == 0;
}

NYA_Error _nya_http_static_check_segments(NYA_ConstCString text, u64 size, NYA_ConstCString what) {
    nya_assert(text != nullptr);
    nya_assert(what != nullptr);

    if (size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s is empty", what);
    if (text[size - 1] == '/') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s ends in a separator", what);

    u64 segment_start = 0;

    for (u64 index = 0; index <= size; index++) {
        if (index < size && text[index] != '/') {
            if (!_nya_http_static_is_name_char(text[index])) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s has a byte at offset " FMTu64 " that a name may not contain", what, index);
            }

            continue;
        }

        u64 length = index - segment_start;

        if (length == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s has an empty segment at offset " FMTu64, what, segment_start);

        // "." and ".." are here, and every dot file: a name starting with a dot is never something this publishes, so one rule refuses the climb and the hidden file together.
        if (text[segment_start] == '.') {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s has a segment starting with a dot at offset " FMTu64, what, segment_start);
        }

        segment_start = index + 1;
    }

    return NYA_OK;
}

NYA_HttpMediaType _nya_http_static_media_of(NYA_ConstCString asset) {
    nya_assert(asset != nullptr);

    for (u64 index = 0; index < nya_carray_length(_NYA_HTTP_STATIC_SUFFIXES); index++) {
        if (_nya_http_static_ends_with(asset, _NYA_HTTP_STATIC_SUFFIXES[index].suffix)) return _NYA_HTTP_STATIC_SUFFIXES[index].media_type;
    }

    return NYA_HTTP_MEDIA_OTHER;
}

void _nya_http_static_hash(const u8* data, u64 size, OUT char* out_hex) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_hex != nullptr);

    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(data, size, &digest);

    NYA_ConstCString hex = "0123456789abcdef";

    for (u64 index = 0; index < NYA_HTTP_STATIC_HASH_DIGITS / 2; index++) {
        out_hex[index * 2]     = hex[digest.bytes[index] >> 4];
        out_hex[index * 2 + 1] = hex[digest.bytes[index] & 0x0F];
    }

    out_hex[NYA_HTTP_STATIC_HASH_DIGITS] = '\0';
}

NYA_ConstCString _nya_http_static_hashed_path(NYA_ConstCString prefix, NYA_ConstCString relative, NYA_ConstCString hash) {
    nya_assert(prefix != nullptr);
    nya_assert(relative != nullptr);
    nya_assert(hash != nullptr);

    // every served file has a suffix from the table, so there's always a dot to insert before, and it's the last one: "app.min.css" becomes "app.min.<hash>.css".
    u64 dot = strlen(relative);
    while (dot > 0 && relative[dot - 1] != '.') dot--;

    nya_assert(dot > 0, "'%s' reached the hashed name with no suffix", relative);

    NYA_String* path = nya_string_sprintf(_NYA_HTTP_STATIC.arena, "%s/%.*s.%s.%s", prefix, (int)(dot - 1), relative, hash, relative + dot);

    return nya_string_to_cstring(_NYA_HTTP_STATIC.arena, path);
}

b8 _nya_http_static_etag_matches(NYA_ConstCString header, NYA_ConstCString etag) {
    nya_assert(header != nullptr);
    nya_assert(etag != nullptr);

    u64 quoted = strlen(etag);
    nya_assert(quoted >= 2, "an ETag is stored quoted");

    const char* wanted      = etag + 1;
    u64         wanted_size = quoted - 2;

    u64 index = 0;

    while (header[index] != '\0') {
        while (header[index] == ' ' || header[index] == '\t' || header[index] == ',') index++;
        if (header[index] == '\0') break;

        // "*" is "whatever you have", and this route always has one.
        if (header[index] == '*') return true;

        if (header[index] == 'W' && header[index + 1] == '/') index += 2;

        // a list this parser can't read is not a list of tags, and guessing at the rest would be guessing at whether a caller's cache is current.
        if (header[index] != '"') return false;
        index++;

        u64 start = index;
        while (header[index] != '\0' && header[index] != '"') index++;
        if (header[index] != '"') return false;

        if (index - start == wanted_size && nya_memcmp(header + start, wanted, wanted_size) == 0) return true;

        index++;
    }

    return false;
}

NYA_Error _nya_http_static_route_add(NYA_ConstCString path, u32 file, b8 immutable_name) {
    nya_assert(path != nullptr);

    if (_NYA_HTTP_STATIC.route_count >= _NYA_HTTP_STATIC_MAX_ROUTES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a mount builds at most %d routes", _NYA_HTTP_STATIC_MAX_ROUTES);
    }

    // two files answering one path would leave the second unreachable, which reads as a missing file long after the mount that caused it.
    for (u32 index = 0; index < _NYA_HTTP_STATIC.route_count; index++) {
        if (nya_string_equals(_NYA_HTTP_STATIC.routes[index].path, path)) {
            return nya_error(NYA_ERROR_ALREADY_EXISTS, "'%s' is already served by another file", path);
        }
    }

    u32 slot = _NYA_HTTP_STATIC.route_count;

    _NYA_HTTP_STATIC.routes[slot] = (NYA_HttpRoute){
        .method   = NYA_HTTP_METHOD_GET,
        .path     = path,
        .auth     = NYA_HTTP_AUTH_NONE,
        .handler  = _nya_http_static_serve,
        .summary  = immutable_name ? _NYA_HTTP_STATIC_SUMMARY_IMMUTABLE : _NYA_HTTP_STATIC_SUMMARY_NAMED,
        .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_NOT_MODIFIED, NYA_HTTP_STATUS_INTERNAL_ERROR },
    };

    _NYA_HTTP_STATIC.route_file[slot]      = file;
    _NYA_HTTP_STATIC.route_immutable[slot] = immutable_name;
    _NYA_HTTP_STATIC.route_count++;

    return NYA_OK;
}

NYA_Error _nya_http_static_add(const NYA_HttpStaticFile* file, NYA_ConstCString root, NYA_ConstCString prefix) {
    nya_assert(file != nullptr);
    nya_assert(root != nullptr);
    nya_assert(prefix != nullptr);

    if (file->asset == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a file with no asset handle");
    // an empty file is a read that went wrong upstream far more often than it's a file, and serving zero bytes under a hash of zero bytes helps nobody find out which.
    if (file->data == nullptr || file->size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' was handed no bytes", file->asset);

    u64 asset_size = strlen(file->asset);
    u64 root_size  = strlen(root);

    if (asset_size >= NYA_HTTP_MAX_STATIC_ASSET) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is longer than %d bytes", file->asset, NYA_HTTP_MAX_STATIC_ASSET);
    }

    // Under the root, spelled exactly: the handle must begin with the root and a separator. This refuses an absolute path, a sibling directory and a climbing handle, done on the text before anything looks at the filesystem.
    if (asset_size <= root_size + 1 || strncmp(file->asset, root, root_size) != 0 || file->asset[root_size] != '/') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not under '%s'", file->asset, root);
    }

    NYA_ConstCString relative = file->asset + root_size + 1;

    NYA_TRY(_nya_http_static_check_segments(relative, asset_size - root_size - 1, "the asset handle"));

    NYA_HttpMediaType media_type = _nya_http_static_media_of(file->asset);

    if (media_type == NYA_HTTP_MEDIA_OTHER) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has no suffix this server has a media type for", file->asset);
    }

    // Where the assets are still files, what the handle names must be a file: a symlink names bytes elsewhere (the traversal this can't otherwise see) and a directory isn't served at all. A release reads from the baked blob with nothing to stat — the build walked the tree then, and the blob's integrity hash is checked on load.
    NYA_FileInfo info = { 0 };

    if (nya_filesystem_info(file->asset, &info).ok && info.type != NYA_FILE_TYPE_FILE) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is a %s, and only a file is served", file->asset, NYA_FILETYPE_NAME_MAP[info.type]);
    }

    if (file->size > NYA_HTTP_MAX_STATIC_FILE_BYTES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' is " FMTu64 " bytes, past the %d a served file may be", file->asset, file->size,
                         NYA_HTTP_MAX_STATIC_FILE_BYTES);
    }

    if (_NYA_HTTP_STATIC.bytes + file->size > NYA_HTTP_MAX_STATIC_BYTES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the bundle is past the " FMTu64 " bytes it may hold", (u64)NYA_HTTP_MAX_STATIC_BYTES);
    }

    // copied, so the caller's arena is free to go: the scratch a read used is not this mount's to hold.
    u8* data = nya_arena_alloc(_NYA_HTTP_STATIC.arena, file->size);

    if (data == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "could not keep " FMTu64 " bytes of '%s'", file->size, file->asset);

    nya_memcpy(data, file->data, file->size);

    u64 size = file->size;

    char hash[NYA_HTTP_STATIC_HASH_DIGITS + 1] = { 0 };
    _nya_http_static_hash(data, size, hash);

    u32                   slot  = _NYA_HTTP_STATIC.file_count;
    _NYA_HttpStaticEntry* entry = &_NYA_HTTP_STATIC.files[slot];

    *entry = (_NYA_HttpStaticEntry){
        .asset       = file->asset,
        .data        = data,
        .size        = size,
        .media_type  = media_type,
        .hashed_path = _nya_http_static_hashed_path(prefix, relative, hash),
    };

    (void)snprintf(entry->etag, sizeof(entry->etag), "\"%s\"", hash);

    if (strlen(entry->hashed_path) >= NYA_HTTP_MAX_PATH) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the hashed name of '%s' is longer than a path may be", file->asset);
    }

    _NYA_HTTP_STATIC.file_count++;
    _NYA_HTTP_STATIC.bytes += size;

    NYA_TRY(_nya_http_static_route_add(entry->hashed_path, slot, true));

    if (file->path == nullptr) return NYA_OK;

    if (file->path[0] != '/' || strlen(file->path) >= NYA_HTTP_MAX_PATH) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not an absolute path that fits one", file->path);
    }

    // "/" is the entry point and the one path with no segments; anything else is held to the same spelling as the handle behind it.
    if (file->path[1] != '\0') NYA_TRY(_nya_http_static_check_segments(file->path + 1, strlen(file->path) - 1, "the served path"));

    return _nya_http_static_route_add(file->path, slot, false);
}

NYA_HttpStatus _nya_http_static_serve(NYA_HttpExchange* exchange) {
    nya_assert(exchange != nullptr);
    nya_assert(exchange->route != nullptr, "the bundle's handler is only reachable through one of its routes");

    u64 index = (u64)(exchange->route - _NYA_HTTP_STATIC.routes);

    nya_assert(index < _NYA_HTTP_STATIC.route_count, "a route outside this mount reached the bundle's handler");

    const _NYA_HttpStaticEntry* file = &_NYA_HTTP_STATIC.files[_NYA_HTTP_STATIC.route_file[index]];

    NYA_ConstCString cache = _NYA_HTTP_STATIC.route_immutable[index] ? _NYA_HTTP_STATIC_CACHE_IMMUTABLE : _NYA_HTTP_STATIC_CACHE_REVALIDATE;

    NYA_ConstCString conditional = nya_http_request_header(exchange->request, "if-none-match");

    // the validator before the body: a hit costs the caller nothing and this program one header.
    if (conditional != nullptr && _nya_http_static_etag_matches(conditional, file->etag)) {
        nya_http_response_reset(exchange->response);

        if (!nya_http_response_header(exchange->response, "ETag", file->etag).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;
        if (!nya_http_response_header(exchange->response, "Cache-Control", cache).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

        return NYA_HTTP_STATUS_NOT_MODIFIED;
    }

    if (!nya_http_response_bytes(exchange->response, file->data, file->size, file->media_type).ok) {
        nya_log_error("'%s' does not fit the response buffer, which the mount was supposed to have refused.", file->asset);
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    if (!nya_http_response_header(exchange->response, "ETag", file->etag).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;
    if (!nya_http_response_header(exchange->response, "Cache-Control", cache).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // a page must be able to load its own stylesheet, which the server's default policy forbids; only a document gets the looser one, everything else keeps default-src 'none'.
    if (file->media_type == NYA_HTTP_MEDIA_HTML &&
        !nya_http_response_header(exchange->response, "Content-Security-Policy", NYA_HTTP_STATIC_PAGE_CSP).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}
