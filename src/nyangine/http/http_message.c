#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_string.h"
#include "nyangine/http/http_message.h"
#include "nyangine/base/base_clock_format.h"
#include "nyangine/serde/serde.h"

// The content-coding libraries, on the platform whose link line carries them (FLAGS_MODULE_COMPRESSION): zlib for gzip and `deflate`, libbrotlienc for `br`. Where the macros are undefined (Windows here, and the build tool), nya_http_response_compress is a no-op and none of this is reached.
#ifdef NYA_HTTP_COMPRESSION
#include <zlib.h>
#ifdef NYA_HTTP_COMPRESSION_BROTLI
#include <brotli/encode.h>
#endif
#endif

// CONSTANTS

/** The fixed part of a rendered head: the status line, Content-Length, Content-Type, Connection, Date and X-Request-Id. */
#define _NYA_HTTP_FIXED_HEAD_BYTES 256

// Written on every response so no route, layer or error path can forget one; a response setting a header of the same name replaces the default (how /docs names its one inline style). The CSP is the strictest there is (nothing loads, frames it, or points a form/<base> anywhere), JSON and .nya need no more, COEP/COOP are what a wasm client's threads need, and HSTS is left for TLS since a browser ignores it over plain HTTP.
#define _NYA_HTTP_DEFAULT_CSP "default-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"

// The three a page is most often asked for by something it embedded rather than by the reader; a route that wants one says so itself, like its own CSP.
#define _NYA_HTTP_DEFAULT_PERMISSIONS "geolocation=(), camera=(), microphone=()"

NYA_INTERNAL const NYA_ConstCString _NYA_HTTP_SECURITY_HEADERS[][2] = {
    { "Content-Security-Policy",      _NYA_HTTP_DEFAULT_CSP },
    { "X-Content-Type-Options",       "nosniff" },
    { "Referrer-Policy",              "no-referrer" },
    { "Cross-Origin-Opener-Policy",   "same-origin" },
    { "Cross-Origin-Embedder-Policy", "require-corp" },
    { "Cross-Origin-Resource-Policy", "same-origin" },
    { "Permissions-Policy",           _NYA_HTTP_DEFAULT_PERMISSIONS },
    // Belt and suspenders for the CSP's frame-ancestors above: a pre-CSP browser honours only this one, so twenty-four bytes buys framing protection for the browsers the CSP can't reach.
    { "X-Frame-Options",              "DENY" },
};

/**
 * What HSTS says when it is on: a year, and this host only.
 *
 * Not `includeSubDomains`, and not `preload`. Both are promises about names this server does not
 * serve and cannot withdraw for two years, and a program that means them says so itself with its own
 * header — which replaces this one, like every other default here.
 * */
#define _NYA_HTTP_HSTS "max-age=31536000"

/**
 * Whether the server this is rendering for is serving TLS.
 *
 * A header that only means anything over TLS, and which a browser ignores over plaintext — so it is
 * sent only when it is true rather than always. One flag rather than a field on every response,
 * because a server is TLS or it is not for the whole of its life; nya_http_hsts_set is what moves it.
 * */
NYA_INTERNAL b8 _NYA_HTTP_HSTS_ENABLED = false;

/** Room for the defaults above, rendered: each name and value plus ": " and CRLF, with slack. Checked below. */
#define _NYA_HTTP_SECURITY_HEAD_BYTES 512

static_assert(
    sizeof("Content-Security-Policy" _NYA_HTTP_DEFAULT_CSP "X-Content-Type-Options" "nosniff" "Referrer-Policy" "no-referrer"
           "Cross-Origin-Opener-Policy" "same-origin" "Cross-Origin-Embedder-Policy" "require-corp" "Cross-Origin-Resource-Policy" "same-origin"
           "Permissions-Policy" _NYA_HTTP_DEFAULT_PERMISSIONS "X-Frame-Options" "DENY" "Strict-Transport-Security" _NYA_HTTP_HSTS) +
            (sizeof(": \r\n") - 1) * (nya_carray_length(_NYA_HTTP_SECURITY_HEADERS) + 1) <= _NYA_HTTP_SECURITY_HEAD_BYTES,
    "the default security headers outgrew the room kept for them"
);

static_assert(
    NYA_HTTP_MAX_RESPONSE_HEAD_BYTES >=
        _NYA_HTTP_FIXED_HEAD_BYTES + _NYA_HTTP_SECURITY_HEAD_BYTES + NYA_HTTP_MAX_RESPONSE_HEADERS * (NYA_HTTP_MAX_HEADER_NAME + NYA_HTTP_MAX_HEADER_VALUE + 4),
    "the rendered head buffer has to fit every custom header at its full bound, or a handler could be refused for a header it was allowed to set"
);

// PRIVATE API DECLARATION

/** Whether `character` may appear in a header name or a method. RFC 9110's `tchar`, spelled out. */
NYA_INTERNAL b8 _nya_http_is_token_char(char character) __attr_no_discard;

/** Whether `character` may appear in a header value: visible ASCII, space and tab, nothing else. */
NYA_INTERNAL b8 _nya_http_is_field_char(char character) __attr_no_discard;

/** The value of one hex digit. False for anything else, so a half-written escape cannot decode to zero. */
NYA_INTERNAL b8 _nya_http_hex_digit(char character, OUT u8* out_value);

/**
 * Decodes one `application/x-www-form-urlencoded` field into `out`, NUL terminated.
 *
 * `+` becomes a space and then the percent escapes are undone, which is the order the form encoding is
 * defined in — a literal `+` in a value is sent as `%2B`, so undoing the escapes first would turn it
 * into a space. False when a malformed escape or the capacity says the value is not one this can hold.
 * */
NYA_INTERNAL b8 _nya_http_form_decode(const char* text, u64 size, OUT char* out, u64 capacity) __attr_no_discard;

/**
 * The next CRLF-terminated line at `*cursor`, within at most `line_max` bytes of it.
 *
 * Three answers rather than two: the line, "not here yet", and "this is longer than a line may be".
 * A caller that could not tell the last two apart would wait forever for a peer that is never going
 * to send a newline.
 * */
typedef enum {
    _NYA_HTTP_LINE_INCOMPLETE = 0,
    _NYA_HTTP_LINE_FOUND,
    _NYA_HTTP_LINE_TOO_LONG,
} _NYA_HttpLine;

NYA_INTERNAL _NYA_HttpLine
_nya_http_next_line(const u8* data, u64 size, u64 line_max, OUT u64* cursor, OUT const char** out_line, OUT u64* out_length);

/** Decimal, refusing an empty run, a non-digit, and anything over `limit`. No overflow is possible past the limit check. */
NYA_INTERNAL b8 _nya_http_parse_decimal(const char* text, u64 size, u64 limit, OUT u64* out_value);

/** Hexadecimal, same contract. Used for a chunk size and nothing else. */
NYA_INTERNAL b8 _nya_http_parse_hex(const char* text, u64 size, u64 limit, OUT u64* out_value);

/** Copies `size` bytes and null terminates, or fails when they do not fit. Never truncates silently. */
NYA_INTERNAL b8 _nya_http_copy_bounded(OUT char* destination, u64 capacity, const char* source, u64 size);

/** The request line: method, target and version, or the status that refuses it. */
NYA_INTERNAL NYA_HttpStatus _nya_http_parse_request_line(const char* line, u64 length, OUT NYA_HttpRequest* out_request, OUT b8* out_http_1_1);

/** Everything after the head: Content-Length or chunked, decoded into `out_request->body`. */
NYA_INTERNAL NYA_HttpParse _nya_http_parse_body(
    const u8*            data,
    u64                  size,
    u64                  head_end,
    b8                   chunked,
    b8                   has_length,
    u64                  content_length,
    OUT NYA_HttpRequest* out_request,
    OUT u64*             out_consumed,
    OUT NYA_HttpStatus*  out_status
);

/** The chunked decoder. Split out because it is the part with a loop a stranger controls. */
NYA_INTERNAL NYA_HttpParse _nya_http_decode_chunked(
    const u8*            data,
    u64                  size,
    u64                  head_end,
    OUT NYA_HttpRequest* out_request,
    OUT u64*             out_consumed,
    OUT NYA_HttpStatus*  out_status
);

/** Appends to a rendered head, refusing to write past `capacity`. */
NYA_INTERNAL b8 _nya_http_head_append(OUT u8* buffer, u64 capacity, OUT u64* size, NYA_ConstCString text);

#ifdef NYA_HTTP_COMPRESSION

/** Whether a body of this type is worth compressing: text and the document formats, never the already-compressed binaries. */
NYA_INTERNAL b8 _nya_http_media_type_compressible(NYA_HttpMediaType media_type) __attr_no_discard;


/** The content codings this server can produce, in no order; the negotiation picks between them. */
typedef enum {
    _NYA_HTTP_ENCODING_NONE = 0,
    _NYA_HTTP_ENCODING_GZIP,
    _NYA_HTTP_ENCODING_DEFLATE,
    _NYA_HTTP_ENCODING_BROTLI,
} _NYA_HttpEncoding;

/**
 * The quality a client gave `token` in its Accept-Encoding, on a 0..1000 scale, or -1 when the header
 * names neither the token nor a `*` that would stand in for it.
 *
 * A `;q=0` is a real answer — the client saying "not this one" — and comes back as 0, which the
 * negotiation reads as unacceptable. A malformed weight is read as no weight, so 1000.
 * */
NYA_INTERNAL s32 _nya_http_encoding_quality(NYA_ConstCString accept, NYA_ConstCString token) __attr_no_discard;

/** The one qvalue between `text` and `end`, milli-scaled: "1" -> 1000, "0.5" -> 500, malformed -> 1000. */
NYA_INTERNAL s32 _nya_http_qvalue(const char* text, const char* end) __attr_no_discard;

/** The coding to answer this Accept-Encoding with, by the client's own weights, ties broken br > gzip > deflate. */
NYA_INTERNAL _NYA_HttpEncoding _nya_http_negotiate_encoding(NYA_ConstCString accept) __attr_no_discard;

#endif

/**
 * The body as a document. `type` is what a binary body must have been encoded against, or null for an
 * untyped one; the text forms carry no layout and ignore it.
 * */
NYA_INTERNAL NYA_Error
_nya_http_request_document_as(const NYA_HttpRequest* request, NYA_Arena* arena, const NYA_TypeReflection* type, OUT NYA_Object** out_object)
    __attr_no_discard;

/** The response half of the same: `object` into the body in `media`, a binary one tied to `type` when given. */
NYA_INTERNAL NYA_Error _nya_http_response_document_as(
    NYA_HttpResponse*         response,
    NYA_Arena*                arena,
    const NYA_Object*         object,
    NYA_HttpMediaType         media,
    const NYA_TypeReflection* type
) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

// REQUESTS

NYA_HttpParse nya_http_request_parse(const u8* data, u64 size, NYA_HttpRequest* out_request, u64* out_consumed, NYA_HttpStatus* out_status) {
    // the caller's own contract, which is our code and not the peer's.
    nya_assert(out_request != nullptr);
    nya_assert(out_consumed != nullptr);
    nya_assert(out_status != nullptr);

    *out_consumed = 0;
    *out_status   = NYA_HTTP_STATUS_NONE;

    if (data == nullptr || size == 0) return NYA_HTTP_PARSE_INCOMPLETE;

    // The head ends at the first blank line, and must do so within NYA_HTTP_MAX_HEAD_BYTES: a peer that sent that much without one isn't going to, so it's refused here rather than held open.
    u64 scan_limit = size < NYA_HTTP_MAX_HEAD_BYTES ? size : NYA_HTTP_MAX_HEAD_BYTES;
    u64 head_end   = 0;

    for (u64 index = 3; index < scan_limit; index++) {
        if (data[index - 3] == '\r' && data[index - 2] == '\n' && data[index - 1] == '\r' && data[index] == '\n') {
            head_end = index + 1;
            break;
        }
    }

    if (head_end == 0) {
        if (size >= NYA_HTTP_MAX_HEAD_BYTES) {
            *out_status = NYA_HTTP_STATUS_HEADERS_TOO_LARGE;
            return NYA_HTTP_PARSE_REFUSED;
        }

        return NYA_HTTP_PARSE_INCOMPLETE;
    }

    u64 cursor = 0;

    const char* line   = nullptr;
    u64         length = 0;

    if (_nya_http_next_line(data, head_end, NYA_HTTP_MAX_HEAD_BYTES, &cursor, &line, &length) != _NYA_HTTP_LINE_FOUND) {
        *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
        return NYA_HTTP_PARSE_REFUSED;
    }

    // Built in place rather than a local: NYA_HttpRequest is twenty kilobytes of fixed buffers, and a stack copy per request would be most of what parsing costs; the price is the struct holds scratch until DONE, as the header says.
    NYA_HttpRequest* request = out_request;
    *request                 = (NYA_HttpRequest){ 0 };

    b8 http_1_1 = false;

    NYA_HttpStatus refusal = _nya_http_parse_request_line(line, length, request, &http_1_1);
    if (refusal != NYA_HTTP_STATUS_NONE) {
        *out_status = refusal;
        return NYA_HTTP_PARSE_REFUSED;
    }

    // Framing is decided here and nowhere else: both Content-Length and Transfer-Encoding on one request is request smuggling (two intermediaries pick different ones and disagree where the next request starts), and there's no safe preference, so it's refused.
    b8  chunked        = false;
    b8  has_length     = false;
    u64 content_length = 0;
    u32 host_count     = 0;

    request->keep_alive = http_1_1;

    while (cursor < head_end) {
        _NYA_HttpLine found = _nya_http_next_line(data, head_end, NYA_HTTP_MAX_HEAD_BYTES, &cursor, &line, &length);

        if (found != _NYA_HTTP_LINE_FOUND) {
            *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        // the blank line that ended the head. Everything after it is body.
        if (length == 0) break;

        // a line starting with whitespace is an obsolete folded header; RFC 9112 lets a server refuse one, and refusing is the only answer that doesn't have to guess what it folds into.
        if (line[0] == ' ' || line[0] == '\t') {
            *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        u64 colon = 0;
        while (colon < length && line[colon] != ':') colon++;

        if (colon == 0 || colon == length) {
            *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        for (u64 index = 0; index < colon; index++) {
            if (_nya_http_is_token_char(line[index])) continue;

            *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        u64 value_start = colon + 1;
        u64 value_end   = length;

        while (value_start < value_end && (line[value_start] == ' ' || line[value_start] == '\t')) value_start++;
        while (value_end > value_start && (line[value_end - 1] == ' ' || line[value_end - 1] == '\t')) value_end--;

        for (u64 index = value_start; index < value_end; index++) {
            if (_nya_http_is_field_char(line[index])) continue;

            *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        const char* value      = line + value_start;
        u64         value_size = value_end - value_start;

        // The four headers that change how the request is read are handled whether or not there's room to store them, so a peer can't hide a Content-Length behind twenty-five padding headers.
        char name[NYA_HTTP_MAX_HEADER_NAME] = { 0 };

        b8 name_fits = _nya_http_copy_bounded(name, sizeof(name), line, colon);
        if (name_fits) {
            for (u64 index = 0; index < colon; index++) name[index] = _nya_http_lower(name[index]);
        }

        if (name_fits && nya_string_equals(name, "content-length")) {
            if (has_length) {
                *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
                return NYA_HTTP_PARSE_REFUSED;
            }

            if (!_nya_http_parse_decimal(value, value_size, U64_MAX / 2, &content_length)) {
                *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
                return NYA_HTTP_PARSE_REFUSED;
            }

            if (content_length > NYA_HTTP_MAX_BODY_BYTES) {
                *out_status = NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE;
                return NYA_HTTP_PARSE_REFUSED;
            }

            has_length = true;
        } else if (name_fits && nya_string_equals(name, "transfer-encoding")) {
            // only "chunked", and only on its own: a coding this server can't undo is 501 rather than a body read as if it weren't encoded at all.
            if (value_size != 7 || _nya_http_lower(value[0]) != 'c' || _nya_http_lower(value[1]) != 'h' || _nya_http_lower(value[2]) != 'u' ||
                _nya_http_lower(value[3]) != 'n' || _nya_http_lower(value[4]) != 'k' || _nya_http_lower(value[5]) != 'e' ||
                _nya_http_lower(value[6]) != 'd') {
                *out_status = NYA_HTTP_STATUS_NOT_IMPLEMENTED;
                return NYA_HTTP_PARSE_REFUSED;
            }

            chunked = true;
        } else if (name_fits && nya_string_equals(name, "content-type")) {
            request->media_type = nya_http_media_type_parse(value, value_size);
        } else if (name_fits && nya_string_equals(name, "host")) {
            // Counted whether or not it fits the table, like the four above: how many Host headers there are decides whether the request is one request.
            host_count++;

            if (value_size == 0) {
                *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
                return NYA_HTTP_PARSE_REFUSED;
            }
        } else if (name_fits && nya_string_equals(name, "connection")) {
            if (value_size == 5 && _nya_http_lower(value[0]) == 'c') request->keep_alive = false;
            if (value_size == 10 && _nya_http_lower(value[0]) == 'k') request->keep_alive = true;
        }

        if (!name_fits || request->header_count >= NYA_HTTP_MAX_HEADERS) continue;

        NYA_HttpHeader* header = &request->headers[request->header_count];

        if (!_nya_http_copy_bounded(header->value, sizeof(header->value), value, value_size)) continue;

        (void)_nya_http_copy_bounded(header->name, sizeof(header->name), name, colon);

        request->header_count++;
    }

    // RFC 9112 makes Host a must on HTTP/1.1 and more than one invalid at any version — both smuggling cases, not pedantry: a front end routing on the second Host and a back end on the first are two servers that disagree about who the request was for.
    if (host_count > 1 || (http_1_1 && host_count == 0)) {
        *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
        return NYA_HTTP_PARSE_REFUSED;
    }

    if (chunked && has_length) {
        *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
        return NYA_HTTP_PARSE_REFUSED;
    }

    // A body on a verb that gives one no meaning: RFC 9110 leaves those bytes undefined, no route reads them, and an intermediary counting them as a body while we count them as the next request is smuggling again. Refused rather than skipped, and QUERY is the verb a read with a document is sent as.
    if (!nya_http_method_allows_body(request->method) && (chunked || content_length > 0)) {
        *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
        return NYA_HTTP_PARSE_REFUSED;
    }

    NYA_HttpParse parsed = _nya_http_parse_body(data, size, head_end, chunked, has_length, content_length, request, out_consumed, out_status);

    if (parsed != NYA_HTTP_PARSE_DONE) return parsed;

    // a body with no Content-Type is not JSON, whatever it looks like: guessing is how a parser ends up deciding what a request means from its first byte.
    if (request->body_size == 0) request->media_type = NYA_HTTP_MEDIA_NONE;

    return NYA_HTTP_PARSE_DONE;
}

void nya_http_hsts_set(b8 enabled) {
    _NYA_HTTP_HSTS_ENABLED = enabled;
}

b8 nya_http_hsts(void) {
    return _NYA_HTTP_HSTS_ENABLED;
}

NYA_ConstCString nya_http_request_header(const NYA_HttpRequest* request, NYA_ConstCString name) {
    nya_assert(request != nullptr);
    nya_assert(name != nullptr);

    for (u32 index = 0; index < request->header_count && index < NYA_HTTP_MAX_HEADERS; index++) {
        const NYA_HttpHeader* header = &request->headers[index];

        u64 position = 0;
        while (name[position] != '\0' && header->name[position] != '\0' && _nya_http_lower(name[position]) == header->name[position]) position++;

        if (name[position] == '\0' && header->name[position] == '\0') return header->value;
    }

    return nullptr;
}

b8 nya_http_request_query_param(const NYA_HttpRequest* request, NYA_ConstCString name, char* buffer, u64 capacity) {
    nya_assert(request != nullptr);
    nya_assert(name != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    b8 found = false;

    // a name given twice, a value too long and an absent name all read as no value; the url module says which, and a handler needing to tell them apart calls it on request->target itself.
    if (!nya_url_query_find(&request->target, name, buffer, capacity, &found).ok) return false;

    return found;
}

b8 nya_http_request_form_value(const NYA_HttpRequest* request, NYA_ConstCString name, char* buffer, u64 capacity) {
    nya_assert(request != nullptr && name != nullptr && buffer != nullptr && capacity > 0);

    buffer[0] = '\0';

    // Only a body that announced itself as a form is read as one: guessing at bytes whose type nobody stated is how a parser comes to decide what a request means from its first character.
    if (request->media_type != NYA_HTTP_MEDIA_FORM || request->body_size == 0) return false;

    const char* body = (const char*)request->body;
    u64         size = request->body_size;

    u64 cursor = 0;

    while (cursor < size) {
        // one `key=value`, up to the next `&`.
        u64 pair_end = cursor;
        while (pair_end < size && body[pair_end] != '&') pair_end++;

        u64 equals = cursor;
        while (equals < pair_end && body[equals] != '=') equals++;

        // a pair with no `=` is a key with an empty value, which is a thing a form sends.
        const char* key_text   = body + cursor;
        u64         key_size   = equals - cursor;
        const char* value_text = equals < pair_end ? body + equals + 1 : body + pair_end;
        u64         value_size = equals < pair_end ? pair_end - equals - 1 : 0;

        // The key is compared decoded, because a form may percent-encode it; a small stack buffer holds it, and a key longer than a header value is not one this server has a name to match.
        char key[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };

        if (_nya_http_form_decode(key_text, key_size, key, sizeof(key)) && strcmp(key, name) == 0) {
            return _nya_http_form_decode(value_text, value_size, buffer, capacity);
        }

        cursor = pair_end + 1;
    }

    return false;
}

NYA_Error nya_http_request_document(const NYA_HttpRequest* request, NYA_Arena* arena, NYA_Object** out_object) {
    return _nya_http_request_document_as(request, arena, nullptr, out_object);
}

NYA_Error nya_http_request_json(const NYA_HttpRequest* request, NYA_Arena* arena, NYA_Object** out_object) {
    nya_assert(request != nullptr);

    if (request->media_type == NYA_HTTP_MEDIA_NYA || request->media_type == NYA_HTTP_MEDIA_NYA_BINARY) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "the request body is %s; use nya_http_request_document",
            nya_http_media_type_text(request->media_type)
        );
    }

    return nya_http_request_document(request, arena, out_object);
}

NYA_HttpMediaType nya_http_request_accepts(const NYA_HttpRequest* request) {
    nya_assert(request != nullptr);

    // The native format only when the caller named it; anything else answers JSON, including no Accept header and the wildcard a browser sends — a client that never heard of this engine must not be handed a body it can't read, and a wildcard isn't a statement that it can.
    NYA_ConstCString accepted = nya_http_request_header(request, "accept");

    // the binary form first: its name contains the text form's, so the other order would never see it.
    if (accepted != nullptr && strstr(accepted, "application/nya-binary") != nullptr) return NYA_HTTP_MEDIA_NYA_BINARY;
    if (accepted != nullptr && strstr(accepted, "application/nya") != nullptr) return NYA_HTTP_MEDIA_NYA;

    return NYA_HTTP_MEDIA_JSON;
}

NYA_Error nya_http_request_reflect(const NYA_HttpRequest* request, NYA_Arena* arena, const NYA_TypeReflection* type, void* out_dto) {
    nya_assert(type != nullptr);
    nya_assert(out_dto != nullptr);

    // Any document format, since a DTO is filled from the NYA_Object not the bytes; a binary body must have been encoded against this very layout, which is the check the hash exists for.
    NYA_Object* document = nullptr;
    NYA_TRY(_nya_http_request_document_as(request, arena, type, &document));

    // zeroed rather than left alone: nya_reflect_from_object skips a field the document omits, and the DTO the caller handed us may be a stack struct holding the last request's values.
    memset(out_dto, 0, type->size);

    NYA_TRY(nya_reflect_from_object(type, out_dto, document));

    return NYA_OK;
}

// RESPONSES

void nya_http_response_create(NYA_HttpResponse* response, u8* buffer, u64 capacity) {
    nya_assert(response != nullptr);
    nya_assert(buffer != nullptr || capacity == 0);

    *response = (NYA_HttpResponse){
        .status        = NYA_HTTP_STATUS_NONE,
        .media_type    = NYA_HTTP_MEDIA_NONE,
        .body          = buffer,
        .body_capacity = capacity,
    };
}

void nya_http_response_destroy(NYA_HttpResponse* response) {
    if (response == nullptr) return;

    *response = (NYA_HttpResponse){ 0 };
}

void nya_http_response_reset(NYA_HttpResponse* response) {
    nya_assert(response != nullptr);

    response->status       = NYA_HTTP_STATUS_NONE;
    response->media_type   = NYA_HTTP_MEDIA_NONE;
    response->header_count = 0;
    response->body_size    = 0;
}

NYA_Error nya_http_response_bytes(NYA_HttpResponse* response, const u8* data, u64 size, NYA_HttpMediaType media_type) {
    nya_assert(response != nullptr);

    if (response->body == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the response is not bound to a buffer");

    if (size > response->body_capacity) {
        return nya_error(
            NYA_ERROR_OUT_OF_MEMORY,
            "a %llu byte body does not fit the %llu byte response buffer",
            (unsigned long long)size,
            (unsigned long long)response->body_capacity
        );
    }

    if (size > 0) {
        nya_assert(data != nullptr);
        memcpy(response->body, data, size);
    }

    response->body_size  = size;
    response->media_type = media_type;

    return NYA_OK;
}

NYA_Error nya_http_response_text(NYA_HttpResponse* response, NYA_ConstCString text, NYA_HttpMediaType media_type) {
    nya_assert(text != nullptr);

    return nya_http_response_bytes(response, (const u8*)text, strlen(text), media_type);
}

NYA_Error nya_http_response_printf(NYA_HttpResponse* response, NYA_HttpMediaType media_type, NYA_ConstCString format, ...) {
    nya_assert(response != nullptr);
    nya_assert(format != nullptr);

    if (response->body == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the response is not bound to a buffer");

    va_list arguments;
    va_start(arguments, format);
    s32 written = vsnprintf((char*)response->body, response->body_capacity, format, arguments);
    va_end(arguments);

    if (written < 0) return nya_error(NYA_ERROR_NOT_OK, "the response body could not be formatted");

    if ((u64)written >= response->body_capacity) {
        response->body_size = 0;
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the formatted body does not fit the response buffer");
    }

    response->body_size  = (u64)written;
    response->media_type = media_type;

    return NYA_OK;
}

NYA_Error nya_http_response_document(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_Object* object, NYA_HttpMediaType media) {
    return _nya_http_response_document_as(response, arena, object, media, nullptr);
}

NYA_Error nya_http_response_json(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_Object* object) {
    return nya_http_response_document(response, arena, object, NYA_HTTP_MEDIA_JSON);
}

NYA_Error nya_http_response_reflect_as(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_TypeReflection* type, const void* dto,
                                       NYA_HttpMediaType media) {
    nya_assert(arena != nullptr);
    nya_assert(type != nullptr);
    nya_assert(dto != nullptr);

    NYA_Object* document = nya_reflect_to_object(arena, type, dto);
    if (document == nullptr) return nya_error(NYA_ERROR_NOT_OK, "%s could not be described as a document", type->name);

    return _nya_http_response_document_as(response, arena, document, media, type);
}

NYA_Error nya_http_response_reflect(NYA_HttpResponse* response, NYA_Arena* arena, const NYA_TypeReflection* type, const void* dto) {
    return nya_http_response_reflect_as(response, arena, type, dto, NYA_HTTP_MEDIA_JSON);
}

NYA_Error nya_http_response_header(NYA_HttpResponse* response, NYA_ConstCString name, NYA_ConstCString value) {
    nya_assert(response != nullptr);
    nya_assert(name != nullptr);
    nya_assert(value != nullptr);

    if (response->header_count >= NYA_HTTP_MAX_RESPONSE_HEADERS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a response carries at most %d headers", NYA_HTTP_MAX_RESPONSE_HEADERS);
    }

    // a CR or LF in either half would end the header early and let what follows be read as another header or the body. Refused rather than stripped: a caller that wanted a newline here is wrong about something and should find out.
    for (u64 index = 0; name[index] != '\0'; index++) {
        if (!_nya_http_is_token_char(name[index])) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a header name", name);
    }

    for (u64 index = 0; value[index] != '\0'; index++) {
        if (!_nya_http_is_field_char(value[index])) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the value of '%s' is not a header value", name);
    }

    NYA_HttpHeader* header = &response->headers[response->header_count];

    if (!_nya_http_copy_bounded(header->name, sizeof(header->name), name, strlen(name)) ||
        !_nya_http_copy_bounded(header->value, sizeof(header->value), value, strlen(value))) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' does not fit a header", name);
    }

    response->header_count++;

    return NYA_OK;
}

b8 nya_http_response_compress(NYA_HttpResponse* response, NYA_Arena* arena, NYA_ConstCString accept_encoding) {
    nya_assert(response != nullptr);
    nya_assert(arena != nullptr);

#ifndef NYA_HTTP_COMPRESSION
    // No zlib on this link line: the response goes out as is. The parameters are named so a caller reads the same signature everywhere; nothing here touches them.
    (void)response;
    (void)arena;
    (void)accept_encoding;

    return false;
#else
    // A client that didn't ask, a body too small to earn the framing, or a type whose bytes are already packed — each is a reason to send the body untouched rather than spend a compressor on it.
    if (response->body == nullptr || accept_encoding == nullptr) return false;
    if (response->body_size < NYA_HTTP_COMPRESS_MIN_BYTES) return false;
    if (!_nya_http_media_type_compressible(response->media_type)) return false;

    // Never a second coding on top of a handler's own: a body that already carries Content-Encoding was encoded on purpose, and layering gzip over it would leave a client unable to undo either.
    for (u32 index = 0; index < response->header_count && index < NYA_HTTP_MAX_RESPONSE_HEADERS; index++) {
        if (_nya_http_equals_ignore_case(response->headers[index].name, strlen(response->headers[index].name), "Content-Encoding")) return false;
    }

    // Room for the two headers this adds, checked before anything is encoded: a body compressed but unannounced, or announced but with the Vary missing, is worse than one left alone.
    if (response->header_count + 2 > NYA_HTTP_MAX_RESPONSE_HEADERS) return false;

    _NYA_HttpEncoding encoding = _nya_http_negotiate_encoding(accept_encoding);
    if (encoding == _NYA_HTTP_ENCODING_NONE) return false;

    NYA_ConstCString coding_name = nullptr;
    u64              compressed  = 0;

    switch (encoding) {
        case _NYA_HTTP_ENCODING_GZIP:
        case _NYA_HTTP_ENCODING_DEFLATE: {
            // gzip and the `deflate` coding are the same deflate stream in two wrappers: windowBits 15 is the zlib wrapper RFC 9110 means by `deflate`, and +16 swaps it for the gzip one.
            int window_bits = encoding == _NYA_HTTP_ENCODING_GZIP ? 15 + 16 : 15;

            z_stream stream = { 0 };
            if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;

            // The bound is the most the coding can produce, so one Z_FINISH always completes; the scratch lives in the exchange arena and is gone the moment this returns.
            u64 bound   = deflateBound(&stream, response->body_size);
            u8* scratch = nya_arena_alloc(arena, bound);
            if (scratch == nullptr) {
                (void)deflateEnd(&stream);
                return false;
            }

            stream.next_in   = response->body;
            stream.avail_in  = (uInt)response->body_size;
            stream.next_out  = scratch;
            stream.avail_out = (uInt)bound;

            int result = deflate(&stream, Z_FINISH);
            compressed  = (u64)stream.total_out;
            (void)deflateEnd(&stream);

            if (result != Z_STREAM_END) return false;

            coding_name = encoding == _NYA_HTTP_ENCODING_GZIP ? "gzip" : "deflate";

            if (compressed == 0 || compressed >= response->body_size) return false;

            memcpy(response->body, scratch, compressed);
            break;
        }

        case _NYA_HTTP_ENCODING_BROTLI: {
#ifdef NYA_HTTP_COMPRESSION_BROTLI
            size_t bound   = BrotliEncoderMaxCompressedSize(response->body_size);
            u8*    scratch = nya_arena_alloc(arena, bound > 0 ? bound : 1);
            if (scratch == nullptr) return false;

            // Quality 5 rather than the default 11: a server answers in real time, and 11 spends many times the CPU for a few percent on bodies this size. GENERIC, since the type isn't always text.
            size_t encoded = bound;
            if (!BrotliEncoderCompress(5, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, response->body_size, response->body, &encoded, scratch)) {
                return false;
            }

            compressed  = (u64)encoded;
            coding_name = "br";

            if (compressed == 0 || compressed >= response->body_size) return false;

            memcpy(response->body, scratch, compressed);
            break;
#else
            return false;
#endif
        }

        case _NYA_HTTP_ENCODING_NONE:
        default:                       return false;
    }

    // The room was checked above, so neither add fails; the body has already been replaced, and these are what let a client and a cache read it back.
    (void)nya_http_response_header(response, "Content-Encoding", coding_name);
    (void)nya_http_response_header(response, "Vary", "Accept-Encoding");

    response->body_size = compressed;

    return true;
#endif
}

NYA_Error nya_http_response_head(const NYA_HttpResponse* response, NYA_HttpStatus status, b8 keep_alive, NYA_Instant date, u8* buffer, u64 capacity,
                                 u64* out_size) {
    nya_assert(response != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(out_size != nullptr);
    nya_assert(nya_http_status_is_valid(status), "a handler answered with a status this server does not know");

    *out_size = 0;

    u64 size = 0;

    char line[_NYA_HTTP_FIXED_HEAD_BYTES] = { 0 };

    (void)snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", (s32)status, nya_http_status_text(status));
    if (!_nya_http_head_append(buffer, capacity, &size, line)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");

    (void)snprintf(line, sizeof(line), "Content-Length: %llu\r\n", (unsigned long long)response->body_size);
    if (!_nya_http_head_append(buffer, capacity, &size, line)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");

    if (response->media_type != NYA_HTTP_MEDIA_NONE && response->media_type != NYA_HTTP_MEDIA_OTHER) {
        (void)snprintf(line, sizeof(line), "Content-Type: %s\r\n", nya_http_media_type_text(response->media_type));
        if (!_nya_http_head_append(buffer, capacity, &size, line)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");
    }

    if (!_nya_http_head_append(buffer, capacity, &size, keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n")) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");
    }

    u8 stamp[NYA_RFC9110_LENGTH + 1] = { 0 };
    (void)nya_instant_to_rfc9110(date, stamp, sizeof(stamp));

    (void)snprintf(line, sizeof(line), "Date: %s\r\n", (const char*)stamp);
    if (!_nya_http_head_append(buffer, capacity, &size, line)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");

    if (response->request_id[0] != '\0') {
        (void)snprintf(line, sizeof(line), "X-Request-Id: %s\r\n", response->request_id);
        if (!_nya_http_head_append(buffer, capacity, &size, line)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");
    }

    // Over TLS only: a browser ignores this header on a plaintext connection (deliberately, since honouring it there would let anybody who can answer one request lock a host out of HTTP), so sending it anyway would say nothing.
    if (_NYA_HTTP_HSTS_ENABLED) {
        b8 replaced = false;
        for (u32 custom = 0; custom < response->header_count && custom < NYA_HTTP_MAX_RESPONSE_HEADERS; custom++) {
            replaced |= _nya_http_equals_ignore_case(
                response->headers[custom].name, strlen(response->headers[custom].name), "Strict-Transport-Security"
            );
        }

        if (!replaced && (!_nya_http_head_append(buffer, capacity, &size, "Strict-Transport-Security: ") ||
                          !_nya_http_head_append(buffer, capacity, &size, _NYA_HTTP_HSTS) ||
                          !_nya_http_head_append(buffer, capacity, &size, "\r\n"))) {
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");
        }
    }

    for (u32 index = 0; index < nya_carray_length(_NYA_HTTP_SECURITY_HEADERS); index++) {
        NYA_ConstCString name = _NYA_HTTP_SECURITY_HEADERS[index][0];

        b8 replaced = false;
        for (u32 custom = 0; custom < response->header_count && custom < NYA_HTTP_MAX_RESPONSE_HEADERS; custom++) {
            replaced |= _nya_http_equals_ignore_case(response->headers[custom].name, strlen(response->headers[custom].name), name);
        }
        if (replaced) continue;

        if (!_nya_http_head_append(buffer, capacity, &size, name) || !_nya_http_head_append(buffer, capacity, &size, ": ") ||
            !_nya_http_head_append(buffer, capacity, &size, _NYA_HTTP_SECURITY_HEADERS[index][1]) || !_nya_http_head_append(buffer, capacity, &size, "\r\n")) {
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");
        }
    }

    for (u32 index = 0; index < response->header_count && index < NYA_HTTP_MAX_RESPONSE_HEADERS; index++) {
        if (!_nya_http_head_append(buffer, capacity, &size, response->headers[index].name) || !_nya_http_head_append(buffer, capacity, &size, ": ") ||
            !_nya_http_head_append(buffer, capacity, &size, response->headers[index].value) ||
            !_nya_http_head_append(buffer, capacity, &size, "\r\n")) {
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");
        }
    }

    if (!_nya_http_head_append(buffer, capacity, &size, "\r\n")) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the response head does not fit");

    *out_size = size;

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_is_token_char(char character) {
    if (character >= 'a' && character <= 'z') return true;
    if (character >= 'A' && character <= 'Z') return true;
    if (character >= '0' && character <= '9') return true;

    switch (character) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':  return true;
        default:   return false;
    }
}

b8 _nya_http_is_field_char(char character) {
    if (character == '\t') return true;

    return (u8)character >= 0x20 && (u8)character != 0x7F;
}

b8 _nya_http_hex_digit(char character, u8* out_value) {
    if (character >= '0' && character <= '9') {
        *out_value = (u8)(character - '0');
        return true;
    }

    if (character >= 'a' && character <= 'f') {
        *out_value = (u8)(character - 'a' + 10);
        return true;
    }

    if (character >= 'A' && character <= 'F') {
        *out_value = (u8)(character - 'A' + 10);
        return true;
    }

    return false;
}

b8 _nya_http_form_decode(const char* text, u64 size, char* out, u64 capacity) {
    u64 written = 0;

    for (u64 index = 0; index < size; index++) {
        // Room for the byte and the terminator: a value that would not fit is not one this can hand back.
        if (written + 1 >= capacity) return false;

        char character = text[index];

        if (character == '+') {
            out[written++] = ' ';
            continue;
        }

        if (character == '%') {
            u8 high = 0;
            u8 low  = 0;

            // A '%' with fewer than two hex digits after it is a malformed escape, and a form value is attacker-controlled, so it's refused rather than passed through as the literal bytes.
            if (index + 2 >= size) return false;
            if (!_nya_http_hex_digit(text[index + 1], &high) || !_nya_http_hex_digit(text[index + 2], &low)) return false;

            out[written++] = (char)((high << 4) | low);
            index += 2;

            continue;
        }

        out[written++] = character;
    }

    out[written] = '\0';

    return true;
}

_NYA_HttpLine _nya_http_next_line(const u8* data, u64 size, u64 line_max, u64* cursor, const char** out_line, u64* out_length) {
    u64 start = *cursor;

    if (start >= size) return _NYA_HTTP_LINE_INCOMPLETE;

    u64 limit = size - start < line_max ? size : start + line_max;

    for (u64 index = start; index + 1 < limit; index++) {
        if (data[index] != '\r' || data[index + 1] != '\n') continue;

        *out_line   = (const char*)(data + start);
        *out_length = index - start;
        *cursor     = index + 2;

        return _NYA_HTTP_LINE_FOUND;
    }

    if (size - start >= line_max) return _NYA_HTTP_LINE_TOO_LONG;

    return _NYA_HTTP_LINE_INCOMPLETE;
}

b8 _nya_http_parse_decimal(const char* text, u64 size, u64 limit, u64* out_value) {
    if (size == 0) return false;

    u64 value = 0;

    for (u64 index = 0; index < size; index++) {
        if (text[index] < '0' || text[index] > '9') return false;

        u64 digit = (u64)(text[index] - '0');

        // checked before the multiply, so nothing here can wrap whatever a peer sends.
        if (value > (limit - digit) / 10) return false;

        value = value * 10 + digit;
    }

    *out_value = value;

    return true;
}

b8 _nya_http_parse_hex(const char* text, u64 size, u64 limit, u64* out_value) {
    if (size == 0) return false;

    u64 value = 0;

    for (u64 index = 0; index < size; index++) {
        u8 digit = 0;
        if (!_nya_http_hex_digit(text[index], &digit)) return false;

        if (value > (limit - digit) / 16) return false;

        value = value * 16 + digit;
    }

    *out_value = value;

    return true;
}

b8 _nya_http_copy_bounded(char* destination, u64 capacity, const char* source, u64 size) {
    nya_assert(capacity > 0);

    destination[0] = '\0';

    if (size + 1 > capacity) return false;

    if (size > 0) memcpy(destination, source, size);

    destination[size] = '\0';

    return true;
}

NYA_HttpStatus _nya_http_parse_request_line(const char* line, u64 length, NYA_HttpRequest* out_request, b8* out_http_1_1) {
    u64 first = 0;
    while (first < length && line[first] != ' ') first++;

    if (first == 0 || first >= length) return NYA_HTTP_STATUS_BAD_REQUEST;

    u64 second = first + 1;
    while (second < length && line[second] != ' ') second++;

    if (second >= length || second == first + 1) return NYA_HTTP_STATUS_BAD_REQUEST;

    const char* target      = line + first + 1;
    u64         target_size = second - first - 1;

    const char* version      = line + second + 1;
    u64         version_size = length - second - 1;

    // exactly three parts. A fourth would mean a space inside the target, which is not a target.
    for (u64 index = 0; index < version_size; index++) {
        if (version[index] == ' ') return NYA_HTTP_STATUS_BAD_REQUEST;
    }

    out_request->method = nya_http_method_parse(line, first);
    if (!nya_http_method_is_valid(out_request->method)) return NYA_HTTP_STATUS_NOT_IMPLEMENTED;

    if (version_size == 8 && memcmp(version, "HTTP/1.1", 8) == 0) {
        *out_http_1_1 = true;
    } else if (version_size == 8 && memcmp(version, "HTTP/1.0", 8) == 0) {
        *out_http_1_1 = false;
    } else {
        return NYA_HTTP_STATUS_HTTP_VERSION;
    }

    // Origin form only ("/path?query"): the absolute form is for proxies (this isn't one) and the authority form is for CONNECT (not implemented); accepting either would mean deciding what host a request was for, which a proxy in front has already done.
    NYA_UrlFailure failure = { 0 };

    // parsed in place: the request outlives the receive buffer the target was read from.
    if (!nya_url_parse_target(target, target_size, &out_request->target, &failure).ok) {
        return failure.rule == NYA_URL_RULE_TOO_LONG ? NYA_HTTP_STATUS_URI_TOO_LONG : NYA_HTTP_STATUS_BAD_REQUEST;
    }

    // the target parsed, so the only way its path cannot be decoded is not fitting the buffer.
    u64 path_length = 0;
    if (!nya_url_path_decode(&out_request->target, out_request->path, sizeof(out_request->path), &path_length).ok) {
        return NYA_HTTP_STATUS_URI_TOO_LONG;
    }

    nya_assert(path_length > 0 && out_request->path[0] == '/');

    return NYA_HTTP_STATUS_NONE;
}

NYA_HttpParse _nya_http_parse_body(
    const u8*        data,
    u64              size,
    u64              head_end,
    b8               chunked,
    b8               has_length,
    u64              content_length,
    NYA_HttpRequest* out_request,
    u64*             out_consumed,
    NYA_HttpStatus*  out_status
) {
    if (chunked) return _nya_http_decode_chunked(data, size, head_end, out_request, out_consumed, out_status);

    if (!has_length) {
        out_request->body_size = 0;
        out_request->body[0]   = '\0';
        *out_consumed          = head_end;

        return NYA_HTTP_PARSE_DONE;
    }

    // checked when Content-Length was read, and again here because this function is also the one a future caller would reach for directly.
    if (content_length > NYA_HTTP_MAX_BODY_BYTES) {
        *out_status = NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE;
        return NYA_HTTP_PARSE_REFUSED;
    }

    if (size - head_end < content_length) return NYA_HTTP_PARSE_INCOMPLETE;

    if (content_length > 0) memcpy(out_request->body, data + head_end, content_length);

    out_request->body[content_length] = '\0';
    out_request->body_size            = content_length;
    *out_consumed                     = head_end + content_length;

    return NYA_HTTP_PARSE_DONE;
}

NYA_HttpParse
_nya_http_decode_chunked(const u8* data, u64 size, u64 head_end, NYA_HttpRequest* out_request, u64* out_consumed, NYA_HttpStatus* out_status) {
    u64 cursor  = head_end;
    u64 decoded = 0;

    for (u32 chunk = 0; chunk <= NYA_HTTP_MAX_CHUNKS; chunk++) {
        if (chunk == NYA_HTTP_MAX_CHUNKS) {
            *out_status = NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE;
            return NYA_HTTP_PARSE_REFUSED;
        }

        const char* line   = nullptr;
        u64         length = 0;

        switch (_nya_http_next_line(data, size, NYA_HTTP_MAX_CHUNK_LINE_BYTES, &cursor, &line, &length)) {
            case _NYA_HTTP_LINE_INCOMPLETE: return NYA_HTTP_PARSE_INCOMPLETE;

            case _NYA_HTTP_LINE_TOO_LONG:   *out_status = NYA_HTTP_STATUS_BAD_REQUEST; return NYA_HTTP_PARSE_REFUSED;

            case _NYA_HTTP_LINE_FOUND:      break;

            default:                        nya_unreachable();
        }

        // the chunk size ends at the first ';', which starts extensions nothing here reads.
        u64 digits = 0;
        while (digits < length && line[digits] != ';') digits++;

        u64 chunk_size = 0;
        if (!_nya_http_parse_hex(line, digits, NYA_HTTP_MAX_BODY_BYTES, &chunk_size)) {
            // a size that parsed but is over the bound and a size that isn't hex are the same refusal from here; both mean this body is not one we'll assemble.
            *out_status = digits > 0 ? NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE : NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        if (chunk_size == 0) break;

        if (decoded + chunk_size > NYA_HTTP_MAX_BODY_BYTES) {
            *out_status = NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE;
            return NYA_HTTP_PARSE_REFUSED;
        }

        // chunk_size is at most NYA_HTTP_MAX_BODY_BYTES by now, so the + 2 cannot wrap.
        if (size - cursor < chunk_size + 2) return NYA_HTTP_PARSE_INCOMPLETE;

        if (data[cursor + chunk_size] != '\r' || data[cursor + chunk_size + 1] != '\n') {
            *out_status = NYA_HTTP_STATUS_BAD_REQUEST;
            return NYA_HTTP_PARSE_REFUSED;
        }

        memcpy(out_request->body + decoded, data + cursor, chunk_size);

        decoded += chunk_size;
        cursor  += chunk_size + 2;
    }

    // Trailers, read past and dropped: nothing here reads one, and a trailer a server ignores is not allowed to become a header a later layer trusts.
    for (u32 trailer = 0; trailer <= NYA_HTTP_MAX_TRAILERS; trailer++) {
        if (trailer == NYA_HTTP_MAX_TRAILERS) {
            *out_status = NYA_HTTP_STATUS_HEADERS_TOO_LARGE;
            return NYA_HTTP_PARSE_REFUSED;
        }

        const char* line   = nullptr;
        u64         length = 0;

        switch (_nya_http_next_line(data, size, NYA_HTTP_MAX_TRAILER_BYTES, &cursor, &line, &length)) {
            case _NYA_HTTP_LINE_INCOMPLETE: return NYA_HTTP_PARSE_INCOMPLETE;

            case _NYA_HTTP_LINE_TOO_LONG:   *out_status = NYA_HTTP_STATUS_HEADERS_TOO_LARGE; return NYA_HTTP_PARSE_REFUSED;

            case _NYA_HTTP_LINE_FOUND:      break;

            default:                        nya_unreachable();
        }

        if (length == 0) break;
    }

    out_request->body[decoded] = '\0';
    out_request->body_size     = decoded;
    *out_consumed              = cursor;

    return NYA_HTTP_PARSE_DONE;
}

b8 _nya_http_head_append(u8* buffer, u64 capacity, u64* size, NYA_ConstCString text) {
    u64 length = strlen(text);

    if (*size + length > capacity) return false;

    nya_memcpy(buffer + *size, text, length);
    *size += length;

    return true;
}

#ifdef NYA_HTTP_COMPRESSION
b8 _nya_http_media_type_compressible(NYA_HttpMediaType media_type) {
    switch (media_type) {
        // Text and the document formats: every one is markup or characters where deflate finds the repetition it lives on; `application/nya` is text too and rides along.
        case NYA_HTTP_MEDIA_JSON:
        case NYA_HTTP_MEDIA_NYA:
        case NYA_HTTP_MEDIA_TEXT:
        case NYA_HTTP_MEDIA_HTML:
        case NYA_HTTP_MEDIA_CSS:
        case NYA_HTTP_MEDIA_JAVASCRIPT:
        case NYA_HTTP_MEDIA_SVG:
        case NYA_HTTP_MEDIA_XML:
        case NYA_HTTP_MEDIA_RSS:
        case NYA_HTTP_MEDIA_ATOM:
        case NYA_HTTP_MEDIA_MARKDOWN: return true;

        // Everything else is bytes already packed (png, woff2, wasm carry their own compression, and the native binary document is compact) or has no body: a compressor there costs CPU only to make the response larger, and a media type added later stays uncompressed until it's judged here rather than compressed by accident.
        default: return false;
    }
}

s32 _nya_http_qvalue(const char* text, const char* end) {
    // Leading OWS, then "0"/"1" and an optional "." with up to three digits; anything the grammar disallows is a weight this can't read, treated as none, so 1000.
    while (text < end && (*text == ' ' || *text == '\t')) text++;

    if (text >= end || (*text != '0' && *text != '1')) return 1000;

    s32 whole = *text - '0';
    text++;

    s32 milli = whole * 1000;

    if (text >= end || *text != '.') return milli;
    text++;

    // Up to three fractional digits, each ten times finer than the last; a fourth is ignored, as the grammar caps a qvalue at three places anyway.
    s32 scale = 100;
    for (u32 place = 0; place < 3 && text < end; place++) {
        if (*text < '0' || *text > '9') break;

        milli += (*text - '0') * scale;
        scale /= 10;
        text++;
    }

    // A qvalue is at most 1.000; a client that wrote 1.5 gets read as the ceiling rather than believed.
    return milli > 1000 ? 1000 : milli;
}

s32 _nya_http_encoding_quality(NYA_ConstCString accept, NYA_ConstCString token) {
    u64 length   = strlen(accept);
    s32 wildcard = -1;

    u64 cursor = 0;
    while (cursor < length) {
        // Skip the separators and the whitespace between entries.
        while (cursor < length && (accept[cursor] == ',' || accept[cursor] == ' ' || accept[cursor] == '\t')) cursor++;
        if (cursor >= length) break;

        // The coding name runs to the ';' that begins its weight or the ',' that ends the entry.
        u64 name_start = cursor;
        while (cursor < length && accept[cursor] != ';' && accept[cursor] != ',') cursor++;

        u64 name_end = cursor;
        while (name_end > name_start && (accept[name_end - 1] == ' ' || accept[name_end - 1] == '\t')) name_end--;

        // The weight, when the entry carries one: from just past the ';' to the ',' that ends the entry.
        s32 quality = 1000;
        if (cursor < length && accept[cursor] == ';') {
            u64 params_start = cursor + 1;

            u64 params_end = params_start;
            while (params_end < length && accept[params_end] != ',') params_end++;

            // Find the "q=" inside the params: only a 'q' that opens the parameter counts, so the search is for "q=" run against each position rather than a bare 'q'.
            for (u64 scan = params_start; scan + 1 < params_end; scan++) {
                if ((accept[scan] == 'q' || accept[scan] == 'Q') && accept[scan + 1] == '=') {
                    quality = _nya_http_qvalue(accept + scan + 2, accept + params_end);
                    break;
                }
            }

            cursor = params_end;
        }

        u64 name_length = name_end - name_start;

        if (name_length == 1 && accept[name_start] == '*') {
            wildcard = quality;
        } else if (_nya_http_equals_ignore_case(accept + name_start, name_length, token)) {
            return quality;
        }
    }

    return wildcard;
}

_NYA_HttpEncoding _nya_http_negotiate_encoding(NYA_ConstCString accept) {
    s32 gzip    = _nya_http_encoding_quality(accept, "gzip");
    s32 deflate = _nya_http_encoding_quality(accept, "deflate");

#ifdef NYA_HTTP_COMPRESSION_BROTLI
    s32 brotli = _nya_http_encoding_quality(accept, "br");
#else
    s32 brotli = -1;
#endif

    // The highest weight wins, and a weight must be above zero to count (a `;q=0` rules a coding out); a tie falls to brotli over gzip over deflate, this server's order when the client states none.
    if (brotli > 0 && brotli >= gzip && brotli >= deflate) return _NYA_HTTP_ENCODING_BROTLI;
    if (gzip > 0 && gzip >= deflate) return _NYA_HTTP_ENCODING_GZIP;
    if (deflate > 0) return _NYA_HTTP_ENCODING_DEFLATE;

    return _NYA_HTTP_ENCODING_NONE;
}

#endif

// DOCUMENTS

NYA_Error
_nya_http_request_document_as(const NYA_HttpRequest* request, NYA_Arena* arena, const NYA_TypeReflection* type, OUT NYA_Object** out_object) {
    nya_assert(request != nullptr);
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    *out_object = nullptr;

    if (request->body_size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the request has no body");

    // Whichever of the three the caller announced: a handler asks for a document and doesn't care which arrived, since all parse to the same NYA_Object — the point of one vocabulary type.
    switch (request->media_type) {
        case NYA_HTTP_MEDIA_JSON:
            NYA_TRY(nya_deserialize(arena, request->body, request->body_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, out_object));
            break;

        // No checksum: the native format carries one for a file on disk (guarding a torn write), but a request body has TCP underneath and is signed or not at a different layer, and enforcing it here would refuse every hand-composed document.
        case NYA_HTTP_MEDIA_NYA:
            NYA_TRY(nya_deserialize(arena, request->body, request->body_size, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, out_object));
            break;

        case NYA_HTTP_MEDIA_NYA_BINARY: NYA_TRY(nya_serde_nya_binary_decode(arena, request->body, request->body_size, type, out_object)); break;

        default:                        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the request body is not application/json, application/nya or application/nya-binary");
    }

    if (*out_object == nullptr) return nya_error(NYA_ERROR_PARSE, "the request body is not an object");

    return NYA_OK;
}

NYA_Error _nya_http_response_document_as(
    NYA_HttpResponse*         response,
    NYA_Arena*                arena,
    const NYA_Object*         object,
    NYA_HttpMediaType         media,
    const NYA_TypeReflection* type
) {
    nya_assert(arena != nullptr);
    nya_assert(object != nullptr);

    NYA_String* body = nullptr;

    // Anything other than the document types is a caller's mistake, not a negotiation outcome, and answers JSON.
    switch (media) {
        case NYA_HTTP_MEDIA_NYA:        body = nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM); break;
        case NYA_HTTP_MEDIA_NYA_BINARY: NYA_TRY(nya_serde_nya_binary_encode(arena, object, type, &body)); break;

        default:
            media = NYA_HTTP_MEDIA_JSON;
            body  = nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NO_CHECKSUM);
            break;
    }

    if (body == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the response document could not be serialized");

    return nya_http_response_bytes(response, (const u8*)body->items, body->length, media);
}
