/**
 * @file http_message.h
 *
 * The wire boundary: bytes a stranger sent, in; bytes this program will send, out. Nothing here
 * knows about sockets, routes or handlers, so all of it is a pure function over a byte range and all
 * of it is reachable from a fuzz target (tests/fuzz/fuzz_http_request.c).
 *
 * ```
 * nya_http_request_parse        bytes -> NYA_HttpRequest, or "not yet", or "answer this and close"
 * nya_http_request_header       one header by name, already lowercased and bounded
 * nya_http_request_query_param  one query parameter, percent-decoded into the caller's buffer
 * nya_http_request_json         the body as a serde document
 * nya_http_request_reflect      the body straight into a DTO, by its reflection
 *
 * nya_http_response_create      binds a response to the buffer it will write its body into
 * nya_http_response_destroy     the pair; forgets the buffer, so a stale response cannot write
 * nya_http_response_reset       empties it without unbinding, for a layer that replaces an answer
 * nya_http_response_bytes       the body, as bytes
 * nya_http_response_text        the body, as a null terminated string
 * nya_http_response_printf      the body, formatted
 * nya_http_response_json        the body, as a serde document rendered to JSON
 * nya_http_response_reflect     the body, as a DTO rendered through its reflection. The common one
 * nya_http_response_header      one extra header
 * nya_http_response_head        the status line and headers, rendered, ready to write
 * ```
 *
 * ```c
 * NYA_HttpRequest request = { 0 };
 * u64             consumed = 0;
 * NYA_HttpStatus  refusal = NYA_HTTP_STATUS_NONE;
 *
 * switch (nya_http_request_parse(buffer, filled, &request, &consumed, &refusal)) {
 *     case NYA_HTTP_PARSE_INCOMPLETE: return;                        // wait for more bytes
 *     case NYA_HTTP_PARSE_REFUSED:    answer(refusal); close(); return;
 *     case NYA_HTTP_PARSE_DONE:       break;
 * }
 * ```
 *
 * ── what the parser promises ──
 *
 * On NYA_HTTP_PARSE_DONE, and only then: `method` names a verb, `target` is an NYA_Url with every
 * promise base_url.h makes, `path` is that target's path decoded, null terminated, starting with '/'
 * and with no "." or ".." segment, `header_count` is at most NYA_HTTP_MAX_HEADERS with every name and
 * value null terminated inside their bounds, `body_size` is at most NYA_HTTP_MAX_BODY_BYTES, and
 * `consumed` is at most `size`.
 * Those are the invariants the fuzz target asserts, and they are what lets everything downstream take
 * the struct rather than the bytes.
 *
 * Nothing a client can send reaches an assertion. A request that cannot be answered is
 * NYA_HTTP_PARSE_REFUSED carrying the status that says why, and the connection is closed after it:
 * a stream this parser has given up on cannot be resynchronised, and guessing where the next request
 * starts is exactly the request smuggling bug. The same reasoning refuses a request that carries both
 * Content-Length and Transfer-Encoding rather than preferring one.
 *
 * ── which verbs may carry a body ──
 *
 * QUERY, POST, PUT, PATCH and DELETE. A body on a GET, a HEAD or an OPTIONS is 400: RFC 9110 gives
 * those bytes no meaning, no route here reads them, and an intermediary that counts them as a body
 * while this parser counts them as the start of the next request is the smuggling bug wearing a
 * different hat. A read that needs a request document is a QUERY, which is safe and idempotent like a
 * GET and carries one; see NYA_HttpMethod. A bodiless `Content-Length: 0` is not a body and is fine on
 * any verb. Either way the body is bounded by NYA_HTTP_MAX_BODY_BYTES and the chunk count by
 * NYA_HTTP_MAX_CHUNKS, which a new verb changes nothing about.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_reflection.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_types.h"
#include "nyangine-std/base/base_clock_instant.h"

// CONSTANTS

/**
 * Bytes the rendered status line and headers may take.
 *
 * Big enough for the fixed headers, the default security headers and every custom one at its full bound:
 * 256 + 512 + 8 × (48 + 512 + 4) = 5280, rounded up. The static assert in http_message.c checks the sum rather
 * than trusting this number.
 * */
#define NYA_HTTP_MAX_RESPONSE_HEAD_BYTES 5376

/**
 * The smallest body worth negotiating a content coding for.
 *
 * A gzip stream carries about twenty bytes of framing before it has said anything, so a body shorter
 * than this either grows or saves a handful of bytes that a round trip through the encoder does not
 * earn. Below it nya_http_response_compress leaves the body alone, which is also why a tiny error
 * document is never touched.
 * */
#define NYA_HTTP_COMPRESS_MIN_BYTES 256

// TYPES

typedef enum NYA_HttpParse NYA_HttpParse;

/** What one call to nya_http_request_parse decided. */
enum NYA_HttpParse {
    /**
     * Not enough bytes yet. Nothing was written to the request and nothing is wrong; call again when
     * more have arrived. A caller still has to bound how long it waits; see NYA_HTTP_IDLE_TIMEOUT_MS.
     * */
    NYA_HTTP_PARSE_INCOMPLETE = 0,

    /** The request is in `out_request` and `out_consumed` says how much of the stream it took. */
    NYA_HTTP_PARSE_DONE,

    /**
     * The bytes are not a request this server will answer. `out_status` carries what to send, and the
     * connection is finished either way: see the note at the top of this file.
     * */
    NYA_HTTP_PARSE_REFUSED,
};

// FUNCTIONS

// REQUESTS

/**
 * Parses one request out of the front of `data`.
 *
 * `out_request` is scratch for the duration and only means anything on NYA_HTTP_PARSE_DONE, where it
 * has been fully overwritten; a caller reuses one struct across requests and never reads it after any
 * other answer. Built in place because the struct is twenty kilobytes of fixed buffers and a copy of
 * it per request would cost more than the parse. `out_consumed` says how many bytes of `data` this
 * request took, which is what a caller shifts off to handle a pipelined second one.
 *
 * Allocates nothing and reads nothing outside `data[0, size)`.
 * */
NYA_API NYA_HttpParse
nya_http_request_parse(const u8* data, u64 size, OUT NYA_HttpRequest* out_request, OUT u64* out_consumed, OUT NYA_HttpStatus* out_status);

/**
 * The value of the header called `name`, or null when there is none. `name` is matched without regard
 * to case, since that is what the grammar says and not a convenience.
 * */
NYA_API NYA_ConstCString nya_http_request_header(const NYA_HttpRequest* request, NYA_ConstCString name) __attr_no_discard;

/**
 * Percent-decodes the query parameter called `name` into `buffer`, null terminated.
 *
 * False when there is no such parameter, when the name is given twice (see nya_url_query_find), or
 * when its decoded value does not fit `capacity`; `buffer` is left holding an empty string in all
 * three, so a caller that ignores the return value gets the empty default rather than the previous
 * request's value.
 * */
NYA_API b8 nya_http_request_query_param(const NYA_HttpRequest* request, NYA_ConstCString name, OUT char* buffer, u64 capacity);

/**
 * Reads the field called `name` out of an `application/x-www-form-urlencoded` body, decoded into `buffer`.
 *
 * What a handler for an HTML form's POST uses, where a browser sends `a=b&c=d` rather than a document.
 * `+` decodes to a space and the percent escapes are undone, in that order. False when the body is not
 * a form, when there is no such field, when a value contains a malformed escape, or when the decoded
 * value does not fit — `buffer` is left empty in every case, so an ignored return is the empty default.
 *
 * A field given twice answers the first: a form that repeats a name is a form doing something a server
 * has no obligation to guess the meaning of, and the first is the one the person filled in.
 * */
NYA_API b8 nya_http_request_form_value(const NYA_HttpRequest* request, NYA_ConstCString name, OUT char* buffer, u64 capacity) __attr_no_discard;

/**
 * The body as a serde document, allocated from `arena`.
 *
 * NYA_ERROR_INVALID_ARGUMENT when the request announced anything but JSON, which is the 415 case, and
 * NYA_ERROR_PARSE when the bytes are not JSON, which is the 400 one. An empty body is
 * NYA_ERROR_INVALID_ARGUMENT rather than an empty document, since "no body" and "{}" are different
 * requests.
 * */
/**
 * The body as a document, whichever of the formats the caller announced.
 *
 * `application/json`, `application/nya` and `application/nya-binary` all parse to an NYA_Object, so a
 * handler asks for a document and never learns which arrived. The native formats are what two
 * nyangine programs use between themselves; JSON is what everything else uses, and none is privileged
 * here. A binary body must be untyped here; a typed one belongs to nya_http_request_reflect.
 *
 * The native format's checksum is not enforced: it guards a file on disk against a torn write, and a
 * request body has TCP underneath it. Enforcing it would refuse every document composed by hand.
 * */
NYA_API NYA_Error nya_http_request_document(const NYA_HttpRequest* request, NYA_Arena* arena, OUT NYA_Object** out_object) __attr_no_discard;

/**
 * Which document format this caller asked to be answered in.
 *
 * NYA_HTTP_MEDIA_NYA_BINARY when Accept names `application/nya-binary`, NYA_HTTP_MEDIA_NYA when it
 * names `application/nya`; JSON for everything else, including no Accept header and the wildcard a
 * browser sends. A wildcard is not a statement that a client can read
 * the native format, and a caller that has never heard of this engine must not be handed one.
 * */
NYA_API NYA_HttpMediaType nya_http_request_accepts(const NYA_HttpRequest* request) __attr_no_discard;

/** The JSON half of nya_http_request_document. Refuses a body that announced either native format. */
NYA_API NYA_Error nya_http_request_json(const NYA_HttpRequest* request, NYA_Arena* arena, OUT NYA_Object** out_object) __attr_no_discard;

/**
 * The body straight into `out_dto`, by the DTO's own reflection. The total conversion in: after this
 * returns OK the handler holds its request type and never the bytes.
 *
 * Any document format. An `application/nya-binary` body must carry this type's layout hash, so a
 * client built from other headers is refused with both hashes named rather than read into the wrong
 * fields; see serde_nya_binary.h.
 *
 * `out_dto` is zeroed first, so a field the document omits reads as zero rather than as whatever the
 * last request left there. Every failure of nya_http_request_json, plus NYA_ERROR_PARSE when the
 * document does not fit the type, which nya_reflect_check reports field by field into the log.
 *
 * Once filled, the DTO is run against its own validation attributes (see base_validate.h): a field that
 * breaks a `@required`, `@min`/`@max`, `@len`, `@email` or `@pattern` rule is NYA_ERROR_INVALID_ARGUMENT,
 * which the router answers 400. A type with no such attributes validates trivially.
 * */
NYA_API NYA_Error nya_http_request_reflect(const NYA_HttpRequest* request, NYA_Arena* arena, const NYA_TypeReflection* type, OUT void* out_dto)
    __attr_no_discard;

// RESPONSES

/**
 * Binds `response` to `buffer`, which is where every later body write lands and which the response
 * never owns, frees or outlives.
 * */
NYA_API void nya_http_response_create(OUT NYA_HttpResponse* response, u8* buffer, u64 capacity);

/**
 * Unbinds it. Writing to a destroyed response is refused rather than scribbling on a buffer whose
 * owner has moved on, which is the whole reason this exists when there is nothing to free.
 * */
NYA_API void nya_http_response_destroy(NYA_HttpResponse* response);

/** Empties the body, the headers and the media type, keeping the buffer. For a layer that replaces an answer. */
NYA_API void nya_http_response_reset(NYA_HttpResponse* response);

/** NYA_ERROR_OUT_OF_MEMORY when the body would not fit, which is a bug in the handler and not in the request. */
NYA_API NYA_Error nya_http_response_bytes(NYA_HttpResponse* response, const u8* data, u64 size, NYA_HttpMediaType media_type) __attr_no_discard;

NYA_API NYA_Error nya_http_response_text(NYA_HttpResponse* response, NYA_ConstCString text, NYA_HttpMediaType media_type) __attr_no_discard;

NYA_API NYA_Error nya_http_response_printf(NYA_HttpResponse* response, NYA_HttpMediaType media_type, NYA_ConstCString format, ...)
    __attr_fmt_printf(3, 4) __attr_no_discard;

/** Renders `object` as JSON into the body. `arena` is scratch and holds nothing once this returns. */
/**
 * The body as a document in `media`: NYA_HTTP_MEDIA_JSON, NYA_HTTP_MEDIA_NYA or, untyped,
 * NYA_HTTP_MEDIA_NYA_BINARY.
 *
 * Pair with nya_http_request_accepts to answer a caller in whatever it asked for.
 * */
NYA_API NYA_Error nya_http_response_document(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_Object* object, NYA_HttpMediaType media)
    __attr_no_discard;

/** nya_http_response_document as JSON. */
NYA_API NYA_Error nya_http_response_json(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_Object* object) __attr_no_discard;

/**
 * Renders `dto` as JSON through its reflection. The total conversion out, and the one every handler
 * here uses: the schema in the OpenAPI document is generated from the same table, so a field added to
 * the DTO appears in both without either being edited.
 * */
/**
 * nya_http_response_reflect in `media`, for a handler that has asked what the caller accepts. As
 * `application/nya-binary` the body carries the type's layout hash, which the client's decode checks.
 * */
NYA_API NYA_Error nya_http_response_reflect_as(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_TypeReflection* type, const void* dto,
                                               NYA_HttpMediaType media) __attr_no_discard;

NYA_API NYA_Error nya_http_response_reflect(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_TypeReflection* type, const void* dto)
    __attr_no_discard;

/**
 * Adds one header. NYA_ERROR_OUT_OF_MEMORY past NYA_HTTP_MAX_RESPONSE_HEADERS, and
 * NYA_ERROR_INVALID_ARGUMENT for a name or value that does not fit its bound or carries a CR or LF,
 * which is the response splitting case and is refused rather than stripped.
 * */
NYA_API NYA_Error nya_http_response_header(NYA_HttpResponse* response, NYA_ConstCString name, NYA_ConstCString value) __attr_no_discard;

/**
 * Whether every response carries `Strict-Transport-Security`.
 *
 * Set by nya_system_http_init from whether the server has a certificate, and not meant to be called
 * by anything else: a browser ignores the header over plaintext, so a server that sent it anyway
 * would be writing a line that says nothing, and one that sent it while serving TLS on some requests
 * and not others would be making a promise it does not keep.
 *
 * The value is a year for this host, without `includeSubDomains` and without `preload`; a program
 * that means either sets its own header, which replaces this one.
 * */
NYA_API void nya_http_hsts_set(b8 enabled);

/** Whether it is on, which is what a test asks and what an operator's status page shows. */
NYA_API b8 nya_http_hsts(void) __attr_no_discard;

/**
 * Compresses the body in place when the client asked for it, the type is worth compressing and it
 * helps, and reports whether it did.
 *
 * `accept_encoding` is the request's Accept-Encoding header, or null when there was none; the
 * negotiation is a pure function of that string, so this stays on the wire boundary with everything
 * else here and never reaches for the request. The coding is picked from the client's list by its own
 * q-values, ties broken br > gzip > deflate, and a coding it marked `;q=0` is refused. `arena` is
 * scratch the encoder writes into and holds nothing once this returns; the compressed bytes are copied
 * back over `response->body`.
 *
 * Nothing happens, and false comes back, when there is no Accept-Encoding, when the body is under
 * NYA_HTTP_COMPRESS_MIN_BYTES, when the media type is one already-compressed or binary (png, woff2,
 * wasm and the native binary document are left alone), when the response already carries a
 * Content-Encoding (a handler that encoded its own body owns it), when there is no header room for the
 * two this adds, or when the coding did not actually shrink the body. On true the body has been
 * replaced with the compressed bytes, `Content-Encoding` names the coding and `Vary: Accept-Encoding`
 * is set so a shared cache keeps the codings apart; `body_size` is the compressed length, which is
 * what nya_http_response_head then renders as Content-Length. The media type, and so Content-Type, is
 * unchanged: a coding is not a type.
 *
 * Built only where zlib is on the link line (Linux today); elsewhere it compiles to a no-op that
 * returns false, so a response simply goes out uncompressed rather than a caller having to guess
 * whether the feature is there.
 * */
NYA_API b8 nya_http_response_compress(NYA_HttpResponse* response, NYA_Arena* arena, NYA_ConstCString accept_encoding);

/**
 * Renders the status line and every header into `buffer`, ending with the blank line. The body is not
 * copied: a caller writes `buffer` and then `response->body`, which is one copy fewer than joining
 * them would be.
 *
 * `status` rather than `response->status` because the status a client is told is the dispatcher's to
 * decide: a layer may turn a handler's 200 into a 304 without the handler's body changing.
 *
 * `date` becomes the Date header, which RFC 9110 6.6.1 has a server with a clock send on every answer.
 * Taken rather than read here so the head stays a pure function of its arguments: the server passes
 * nya_instant_now, and a test or the fuzz target pins it.
 * */
NYA_API NYA_Error nya_http_response_head(const NYA_HttpResponse* response, NYA_HttpStatus status, b8 keep_alive, NYA_Instant date, OUT u8* buffer,
                                         u64 capacity, OUT u64* out_size) __attr_no_discard;
