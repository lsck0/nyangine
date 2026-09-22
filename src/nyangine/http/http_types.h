/**
 * @file http_types.h
 *
 * The vocabulary of one HTTP exchange: what a client may ask, what this program may answer, and the
 * bounds every one of those is held to. No sockets and no routing here; those are http_server.h and
 * http_router.h, and both are written in terms of these types.
 *
 * ```
 * NYA_HttpMethod        the verb, parsed from the request line. Unknown ones do not reach a handler
 * NYA_HttpStatus        the answer, _NONE meaning "no status", never a value a handler may return
 * ```
 *
 * The verbs a resource here is written in are QUERY, POST, PUT and DELETE: read, create, update,
 * remove. GET and HEAD are parsed and routed as before and are what a browser gets to use; they are
 * simply not what a new route is written as. See NYA_HttpMethod for why.
 *
 * ```
 * NYA_HttpMediaType     the handful of bodies this server speaks, as an enum rather than a string
 * NYA_HttpHeader        one bounded name and one bounded value
 * NYA_HttpRequest       a request that parsed. There is no way to make one that did not
 * NYA_HttpResponse      what a handler fills in, writing into a buffer it does not own
 * ```
 *
 * ── the bounds ──
 *
 * Every number below is a fixed capacity with the reason for its size written next to it, because a
 * request comes from a stranger and every part of it is something that stranger chose the length of.
 * Nothing here grows, nothing here is allocated per request, and a request that does not fit is
 * answered with the status that says so rather than making room.
 *
 * The whole cost of a running server is NYA_HTTP_MAX_CONNECTIONS receive buffers plus one response
 * buffer, which is under two hundred kilobytes at the defaults, and nothing at all before
 * nya_system_http_init is called.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"
#include "nyangine/base/base_url.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Connections held at once. A browser opens up to six to one origin for a single page, so six is the
 * floor for the debug interface to load at all and eight leaves two for a `curl` beside it. The
 * connection past the last is accepted and closed immediately rather than queued, so a process that
 * spams connect cannot grow this.
 * */
#ifndef NYA_HTTP_MAX_CONNECTIONS
#define NYA_HTTP_MAX_CONNECTIONS 8
#endif

/**
 * Connections one address may hold at once, unless the config lowers it. Half the table, so one client
 * cannot take every slot and a second one, or a `curl` beside a browser, still gets in. A browser opens up to
 * six per host, and past four it queues rather than fails, which is the right place for it to wait.
 * */
#ifndef NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS
#define NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS 4
#endif

/**
 * Addresses whose request budget is remembered at once. Past it the budget touched longest ago is dropped,
 * so an address that stops sending eventually starts fresh, and one spraying addresses to evict the others
 * gains a fresh burst per address it owns, which a proxy in front is the answer to. Sixty four entries of
 * sixty four bytes, linear to search: cheaper than a hash for a table this small.
 * */
#define NYA_HTTP_MAX_RATE_BUCKETS 64

/**
 * Requests an address may make per second, and how many it may make at once from a full bucket, unless the
 * config says otherwise. A page load of the generated docs is two requests and a polling dashboard one a
 * second, so twenty a second with a burst of forty never refuses a person and does refuse a loop.
 * */
#define NYA_HTTP_DEFAULT_REQUESTS_PER_SECOND 20
#define NYA_HTTP_DEFAULT_REQUEST_BURST       40

/**
 * Bytes of request line and headers together.
 *
 * Four kilobytes is what nginx allows by default and comfortably more than any browser sends; past it
 * the answer is 431 and the connection closes, because a peer that has sent this much without a blank
 * line is not going to send one.
 * */
#define NYA_HTTP_MAX_HEAD_BYTES 4096

/**
 * Bytes of request body, after any chunked encoding is undone.
 *
 * Sized from what the request DTOs here actually are: a handful of fields of JSON. A body that
 * announces more is answered 413 without the bytes ever being read into anything.
 * */
#define NYA_HTTP_MAX_BODY_BYTES 8192

/**
 * Chunks one chunked body may be built from.
 *
 * The body is already bounded by NYA_HTTP_MAX_BODY_BYTES, but a peer can send that as thousands of
 * one-byte chunks: this is the second bound, on the work rather than the size. Sixty four chunks is
 * far past what any client produces for a body this small.
 * */
#define NYA_HTTP_MAX_CHUNKS 64

/** Longest chunk size line, "1fff;ext=value" and its CRLF. A longer one is a malformed body. */
#define NYA_HTTP_MAX_CHUNK_LINE_BYTES 32

/** Trailer lines after the last chunk. They are read past and dropped; nothing here reads a trailer. */
#define NYA_HTTP_MAX_TRAILERS 4

/** Longest trailer line, its CRLF included. */
#define NYA_HTTP_MAX_TRAILER_BYTES 256

/**
 * What chunked framing may add on the wire on top of the decoded body: every chunk's size line and
 * CRLF, every trailer, and the terminating blank line.
 * */
#define NYA_HTTP_MAX_FRAMING_BYTES                                                                                                                   \
    (NYA_HTTP_MAX_CHUNKS * (NYA_HTTP_MAX_CHUNK_LINE_BYTES + 2) + NYA_HTTP_MAX_TRAILERS * NYA_HTTP_MAX_TRAILER_BYTES + 4)

/**
 * What one connection may hold of a request in flight, which is its entire memory cost.
 *
 * The worst legal request rather than a round number: a full head, a full body, and the framing that
 * carries the body at its most wasteful. A connection whose buffer fills without a complete request
 * in it has sent something longer than that, and is answered 413 and closed.
 * */
#define NYA_HTTP_MAX_REQUEST_BYTES (NYA_HTTP_MAX_HEAD_BYTES + NYA_HTTP_MAX_BODY_BYTES + NYA_HTTP_MAX_FRAMING_BYTES)

/**
 * Bytes of response body a handler may write.
 *
 * The largest real body is the generated OpenAPI document, which is a few tens of kilobytes with
 * every schema in it. One buffer, shared: the drain runs one exchange at a time on one thread, so a
 * second is never live. A handler that writes past this gets NYA_ERROR_OUT_OF_MEMORY from the write
 * and the exchange becomes a 500, which is a bug in the handler rather than something a client did.
 * */
#define NYA_HTTP_MAX_RESPONSE_BYTES 65536

/**
 * Headers kept from one request. A browser sends a dozen; the rest are dropped rather than the
 * request being refused, because a header this server never reads is not a reason to fail a page.
 * */
#define NYA_HTTP_MAX_HEADERS 24

/** Longest header name kept, terminator included. "Access-Control-Request-Headers" is 29. */
#define NYA_HTTP_MAX_HEADER_NAME 48

/**
 * Longest header value kept, terminator included. A bearer token with room for a long subject and a
 * signature is the biggest one this server reads.
 * */
#define NYA_HTTP_MAX_HEADER_VALUE 512

/** Headers a handler may add beyond the ones the server always writes. */
#define NYA_HTTP_MAX_RESPONSE_HEADERS 8

/** Longest path after percent-decoding, terminator included. The raw target is bounded by NYA_URL_MAX_BYTES. */
#define NYA_HTTP_MAX_PATH 256

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_HttpMethod     NYA_HttpMethod;
typedef enum NYA_HttpStatus     NYA_HttpStatus;
typedef enum NYA_HttpMediaType  NYA_HttpMediaType;
typedef struct NYA_HttpHeader   NYA_HttpHeader;
typedef struct NYA_HttpRequest  NYA_HttpRequest;
typedef struct NYA_HttpResponse NYA_HttpResponse;

/**
 * The verb. `_NONE` is what a request line that named something else parses to, and it is not a value
 * a route may be registered under; see nya_http_method_is_valid.
 *
 * The four a resource here is written in are QUERY, POST, PUT and DELETE: read, create, update,
 * remove. GET is still parsed, still routed and still what /docs and /openapi.json answer, but it is
 * not the shape a new resource takes, because a read in this codebase takes a reflected DTO as its
 * request and a GET has nowhere to put one. See nya_http_method_allows_body.
 * */
enum NYA_HttpMethod {
    NYA_HTTP_METHOD_NONE = 0,

    NYA_HTTP_METHOD_GET,
    NYA_HTTP_METHOD_HEAD,

    /**
     * A read whose parameters are a document rather than a query string: safe and idempotent like GET,
     * with a body like POST. draft-ietf-httpbis-safe-method-w-body, which is a draft and says so.
     *
     * It is the default read here because the alternative is a query string: a DTO is a reflected type
     * whose schema is generated from it, and there is no encoding of one into `?a=1&b=2` that the same
     * reflection could describe. A server that answered both would have two spellings of one request.
     * */
    NYA_HTTP_METHOD_QUERY,

    NYA_HTTP_METHOD_POST,
    NYA_HTTP_METHOD_PUT,
    NYA_HTTP_METHOD_PATCH,
    NYA_HTTP_METHOD_DELETE,
    NYA_HTTP_METHOD_OPTIONS,

    NYA_HTTP_METHOD_COUNT,
};

/**
 * Every status this server can produce, and the only values a handler may return.
 *
 * `_NONE` is the missing value: it is what an unset entry in a route's documented status list reads
 * as, and returning it from a handler is a programmer error rather than a 0 on the wire.
 * */
enum NYA_HttpStatus {
    NYA_HTTP_STATUS_NONE = 0,

    NYA_HTTP_STATUS_OK         = 200,
    NYA_HTTP_STATUS_CREATED    = 201,
    NYA_HTTP_STATUS_NO_CONTENT = 204,

    NYA_HTTP_STATUS_BAD_REQUEST        = 400,
    NYA_HTTP_STATUS_UNAUTHORIZED       = 401,
    NYA_HTTP_STATUS_FORBIDDEN          = 403,
    NYA_HTTP_STATUS_NOT_FOUND          = 404,
    NYA_HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
    NYA_HTTP_STATUS_REQUEST_TIMEOUT    = 408,
    NYA_HTTP_STATUS_LENGTH_REQUIRED    = 411,
    NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE  = 413,
    NYA_HTTP_STATUS_URI_TOO_LONG       = 414,
    NYA_HTTP_STATUS_UNSUPPORTED_MEDIA  = 415,
    NYA_HTTP_STATUS_UNPROCESSABLE      = 422,
    NYA_HTTP_STATUS_TOO_MANY_REQUESTS  = 429,
    NYA_HTTP_STATUS_HEADERS_TOO_LARGE  = 431,

    NYA_HTTP_STATUS_INTERNAL_ERROR      = 500,
    NYA_HTTP_STATUS_NOT_IMPLEMENTED     = 501,
    NYA_HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
    NYA_HTTP_STATUS_HTTP_VERSION        = 505,
};

/**
 * What a body is, as a closed set rather than a string.
 *
 * A media type arriving on the wire is matched against these and anything else is 415, so no handler
 * is ever handed a content type it has to parse; and going out, the header text comes from
 * nya_http_media_type_text and nowhere else, so there is one place that spells the charset.
 * */
enum NYA_HttpMediaType {
    /** No body. What a 204 and a bodiless GET carry. */
    NYA_HTTP_MEDIA_NONE = 0,

    NYA_HTTP_MEDIA_JSON,

    /**
     * The engine's own document format, `application/nya`.
     *
     * What two nyangine programs talk in. It parses substantially faster than JSON and carries the
     * same NYA_Object, so a server answers either from one document and a client asks for whichever
     * it can read. JSON stays the default for anything that did not ask, because an integration that
     * has never heard of this engine must not have to learn a format to call it.
     * */
    NYA_HTTP_MEDIA_NYA,

    NYA_HTTP_MEDIA_TEXT,
    NYA_HTTP_MEDIA_HTML,

    /** Anything else a client announced. Never produced by this server. */
    NYA_HTTP_MEDIA_OTHER,

    NYA_HTTP_MEDIA_COUNT,
};

/** One header, both halves bounded and null terminated. Names are stored lowercased. */
struct NYA_HttpHeader {
    char name[NYA_HTTP_MAX_HEADER_NAME];
    char value[NYA_HTTP_MAX_HEADER_VALUE];
};

/**
 * A request that parsed.
 *
 * There is no constructor but nya_http_request_parse, which is the point: every field below is
 * already inside its bound and already the type it claims to be, so nothing downstream re-checks a
 * length, re-decodes an escape or is handed a raw byte range it has to interpret.
 * */
struct NYA_HttpRequest {
    NYA_HttpMethod method;

    /**
     * Percent-decoded, always starting with '/', with no "." or ".." segment left in it and no query.
     * A target that tried to climb out of the root was refused by the parser, not repaired.
     * */
    char path[NYA_HTTP_MAX_PATH];

    /**
     * The request target as nya_url_parse_target read it; `path` above is its path decoded. The query
     * is read through nya_http_request_query_param, which is nya_url_query_find, so there is one parser.
     * */
    NYA_Url target;

    NYA_HttpHeader headers[NYA_HTTP_MAX_HEADERS];
    u32            header_count;

    /** What Content-Type announced, NYA_HTTP_MEDIA_NONE when there was no body. */
    NYA_HttpMediaType media_type;

    /** Whether the client asked to keep the connection. HTTP/1.1 defaults to true. */
    b8 keep_alive;

    /**
     * The body, decoded: a chunked request has already been dechunked into this buffer, so a handler
     * never meets the transfer encoding. Always null terminated one past `body_size` so it can be
     * handed to a parser that wants a C string.
     * */
    u8  body[NYA_HTTP_MAX_BODY_BYTES + 1];
    u64 body_size;
};

/**
 * What a handler fills in.
 *
 * `body` points into a buffer the server owns and the response does not: a handler writes through
 * nya_http_response_json and its neighbours, which bound every write against `body_capacity`. The
 * pointer is never freed here and is only valid for the exchange it was handed to.
 * */
struct NYA_HttpResponse {
    NYA_HttpStatus    status;
    NYA_HttpMediaType media_type;

    NYA_HttpHeader headers[NYA_HTTP_MAX_RESPONSE_HEADERS];
    u32            header_count;

    u8* body;
    u64 body_capacity;
    u64 body_size;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** "GET", "POST", ... Asserts on a value outside the enum, which only our own code can produce. */
NYA_API NYA_ConstCString nya_http_method_text(NYA_HttpMethod method) __attr_no_discard;

/**
 * The method `text` names, or NYA_HTTP_METHOD_NONE. Case sensitive, as RFC 9110 requires: "get" is
 * not GET and answering it would be inventing a protocol.
 * */
NYA_API NYA_HttpMethod nya_http_method_parse(const char* text, u64 size) __attr_no_discard;

/** Whether `method` names a verb at all, i.e. is neither _NONE nor past the end. */
NYA_API b8 nya_http_method_is_valid(NYA_HttpMethod method) __attr_no_discard;

/**
 * Whether `method` is safe: it reads and changes nothing, so a repeat of it is the same request.
 *
 * GET, HEAD, QUERY and OPTIONS. Safe implies idempotent, which is why there is one predicate and not
 * two; PUT and DELETE are idempotent without being safe and nothing here asks that question yet.
 *
 * QUERY answers true for the same reason GET does. Having a body does not make a request a write, and
 * anything here that reasons about a read — the HEAD fallback, what a proxy or a cache in front may
 * repeat — has to see the two alike or the body is treated as a change nobody made.
 * */
NYA_API b8 nya_http_method_is_safe(NYA_HttpMethod method) __attr_no_discard;

/**
 * Whether a request with `method` may carry a body.
 *
 * True for QUERY, POST, PUT, PATCH and DELETE. False for GET, HEAD and OPTIONS: RFC 9110 gives content
 * on those no meaning, no route here reads one, and an intermediary that disagrees with us about
 * whether those bytes are a body is the request smuggling case. The parser refuses one rather than
 * dropping it, and QUERY is what a caller reaches for instead.
 * */
NYA_API b8 nya_http_method_allows_body(NYA_HttpMethod method) __attr_no_discard;

/** "OK", "Not Found", ... the reason phrase. Empty for a status this server does not produce. */
NYA_API NYA_ConstCString nya_http_status_text(NYA_HttpStatus status) __attr_no_discard;

/** Whether `status` is one of the listed codes, i.e. something this server may actually answer. */
NYA_API b8 nya_http_status_is_valid(NYA_HttpStatus status) __attr_no_discard;

/** The full Content-Type header value, charset included. Empty for NYA_HTTP_MEDIA_NONE and _OTHER. */
NYA_API NYA_ConstCString nya_http_media_type_text(NYA_HttpMediaType media_type) __attr_no_discard;

/**
 * The media type `text` names, ignoring any parameters after a ';' and ignoring case.
 *
 * NYA_HTTP_MEDIA_OTHER for anything unrecognised, which is what a handler's 415 is keyed off; never
 * NYA_HTTP_MEDIA_NONE, since a Content-Type header was present to be parsed.
 * */
NYA_API NYA_HttpMediaType nya_http_media_type_parse(const char* text, u64 size) __attr_no_discard;
