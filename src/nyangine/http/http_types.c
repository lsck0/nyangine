#include <stdio.h>
#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/http/http_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One verb: what it is called on the wire, and the two things this server asks about a verb. */
typedef struct {
    NYA_ConstCString text;

    /** Reads and changes nothing, so repeating it is the same request. RFC 9110 section 9.2.1. */
    b8 safe;

    /** Content means something on this verb, so a body is read rather than refused. */
    b8 allows_body;
} _NYA_HttpMethodRow;

/**
 * The verbs, indexed by NYA_HttpMethod. One table so the text, the parse and what a verb allows cannot
 * disagree, which is the failure mode of writing the switch three times.
 *
 * QUERY is safe and carries a body, which is the whole of what the draft adds and the reason it is the
 * read verb here: every other safe method has nowhere to put a request DTO.
 * */
NYA_INTERNAL const _NYA_HttpMethodRow _NYA_HTTP_METHOD_ROWS[NYA_HTTP_METHOD_COUNT] = {
    [NYA_HTTP_METHOD_NONE]    = { .text = "" },
    [NYA_HTTP_METHOD_GET]     = { .text = "GET", .safe = true },
    [NYA_HTTP_METHOD_HEAD]    = { .text = "HEAD", .safe = true },
    [NYA_HTTP_METHOD_QUERY]   = { .text = "QUERY", .safe = true, .allows_body = true },
    [NYA_HTTP_METHOD_POST]    = { .text = "POST", .allows_body = true },
    [NYA_HTTP_METHOD_PUT]     = { .text = "PUT", .allows_body = true },
    [NYA_HTTP_METHOD_PATCH]   = { .text = "PATCH", .allows_body = true },
    [NYA_HTTP_METHOD_DELETE]  = { .text = "DELETE", .allows_body = true },
    [NYA_HTTP_METHOD_OPTIONS] = { .text = "OPTIONS", .safe = true },
};

/** One row per status this server may answer with. A code absent from here is not a valid status. */
typedef struct {
    NYA_HttpStatus   status;
    NYA_ConstCString reason;
} _NYA_HttpStatusRow;

NYA_INTERNAL const _NYA_HttpStatusRow _NYA_HTTP_STATUS_ROWS[] = {
    { NYA_HTTP_STATUS_OK,                  "OK"                              },
    { NYA_HTTP_STATUS_CREATED,             "Created"                         },
    { NYA_HTTP_STATUS_NO_CONTENT,          "No Content"                      },
    { NYA_HTTP_STATUS_NOT_MODIFIED,        "Not Modified"                    },
    { NYA_HTTP_STATUS_BAD_REQUEST,         "Bad Request"                     },
    { NYA_HTTP_STATUS_UNAUTHORIZED,        "Unauthorized"                    },
    { NYA_HTTP_STATUS_FORBIDDEN,           "Forbidden"                       },
    { NYA_HTTP_STATUS_NOT_FOUND,           "Not Found"                       },
    { NYA_HTTP_STATUS_METHOD_NOT_ALLOWED,  "Method Not Allowed"              },
    { NYA_HTTP_STATUS_REQUEST_TIMEOUT,     "Request Timeout"                 },
    { NYA_HTTP_STATUS_LENGTH_REQUIRED,     "Length Required"                 },
    { NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE,   "Payload Too Large"               },
    { NYA_HTTP_STATUS_URI_TOO_LONG,        "URI Too Long"                    },
    { NYA_HTTP_STATUS_UNSUPPORTED_MEDIA,   "Unsupported Media Type"          },
    { NYA_HTTP_STATUS_UNPROCESSABLE,       "Unprocessable Content"           },
    { NYA_HTTP_STATUS_TOO_MANY_REQUESTS,   "Too Many Requests" },
    { NYA_HTTP_STATUS_HEADERS_TOO_LARGE,   "Request Header Fields Too Large" },
    { NYA_HTTP_STATUS_INTERNAL_ERROR,      "Internal Server Error"           },
    { NYA_HTTP_STATUS_NOT_IMPLEMENTED,     "Not Implemented"                 },
    { NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "Service Unavailable"             },
    { NYA_HTTP_STATUS_HTTP_VERSION,        "HTTP Version Not Supported"      },
};

/**
 * The media types this server speaks, with the charset spelled once. A body without a charset is read
 * as latin-1 by some proxies, which turns a UTF-8 name in a metrics row into mojibake.
 * */
NYA_INTERNAL NYA_ConstCString _NYA_HTTP_MEDIA_TEXT[NYA_HTTP_MEDIA_COUNT] = {
    [NYA_HTTP_MEDIA_NONE]       = "",
    [NYA_HTTP_MEDIA_JSON]       = "application/json; charset=utf-8",
    [NYA_HTTP_MEDIA_NYA]        = "application/nya; charset=utf-8",
    [NYA_HTTP_MEDIA_NYA_BINARY] = "application/nya-binary", // bytes, which a charset would misdescribe
    [NYA_HTTP_MEDIA_TEXT]       = "text/plain; charset=utf-8",
    [NYA_HTTP_MEDIA_HTML]       = "text/html; charset=utf-8",
    [NYA_HTTP_MEDIA_CSS]        = "text/css; charset=utf-8",
    [NYA_HTTP_MEDIA_JAVASCRIPT] = "text/javascript; charset=utf-8",
    [NYA_HTTP_MEDIA_SVG]        = "image/svg+xml; charset=utf-8",
    [NYA_HTTP_MEDIA_PNG]        = "image/png",                  // bytes, as the three below are
    [NYA_HTTP_MEDIA_ICON]       = "image/vnd.microsoft.icon",
    [NYA_HTTP_MEDIA_WOFF2]      = "font/woff2",
    [NYA_HTTP_MEDIA_OTHER]      = "",
};

/** What each media type is called on the wire, without parameters, for matching an incoming header. */
NYA_INTERNAL NYA_ConstCString _NYA_HTTP_MEDIA_ESSENCE[NYA_HTTP_MEDIA_COUNT] = {
    [NYA_HTTP_MEDIA_NONE]       = "",
    [NYA_HTTP_MEDIA_JSON]       = "application/json",
    [NYA_HTTP_MEDIA_NYA]        = "application/nya",
    [NYA_HTTP_MEDIA_NYA_BINARY] = "application/nya-binary",
    [NYA_HTTP_MEDIA_TEXT]       = "text/plain",
    [NYA_HTTP_MEDIA_HTML]       = "text/html",
    [NYA_HTTP_MEDIA_CSS]        = "text/css",
    [NYA_HTTP_MEDIA_JAVASCRIPT] = "text/javascript",
    [NYA_HTTP_MEDIA_SVG]        = "image/svg+xml",
    [NYA_HTTP_MEDIA_PNG]        = "image/png",
    [NYA_HTTP_MEDIA_ICON]       = "image/vnd.microsoft.icon",
    [NYA_HTTP_MEDIA_WOFF2]      = "font/woff2",
    [NYA_HTTP_MEDIA_OTHER]      = "",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Four decimal octets and nothing else. No leading zeros past one digit, since some readers take those as octal. */
NYA_INTERNAL b8 _nya_http_parse_ipv4(const char* text, u64 size, OUT u8* out_octets) __attr_no_discard;

/** Eight groups, `::` at most once, and an IPv4 tail in the last two. What inet_pton accepts, without the platform. */
NYA_INTERNAL b8 _nya_http_parse_ipv6(const char* text, u64 size, OUT u16* out_groups) __attr_no_discard;


/*
 * These two are defined here and used by every other file in the module, which works because http.c
 * includes this one first; see the note there. They are not declared in http_types.h on purpose: a
 * NYA_INTERNAL declaration in a public header is a static function in every translation unit that
 * includes it, and the ones that never call it fail the build on -Wunused-function.
 */

/**
 * ASCII lowercase, locale-independent on purpose: a Turkish locale folds 'I' to a dotless 'ı', which
 * would stop "Content-Length" matching itself.
 * */
NYA_INTERNAL char _nya_http_lower(char character) __attr_no_discard;

/** Whether `size` bytes at `text` are `expected`, ignoring ASCII case. `expected` is null terminated. */
NYA_INTERNAL b8 _nya_http_equals_ignore_case(const char* text, u64 size, NYA_ConstCString expected) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_http_method_text(NYA_HttpMethod method) {
    nya_assert(method >= 0 && method < NYA_HTTP_METHOD_COUNT, "a method outside the enum reached nya_http_method_text");

    return _NYA_HTTP_METHOD_ROWS[method].text;
}

NYA_HttpMethod nya_http_method_parse(const char* text, u64 size) {
    if (text == nullptr || size == 0) return NYA_HTTP_METHOD_NONE;

    for (u32 method = NYA_HTTP_METHOD_NONE + 1; method < NYA_HTTP_METHOD_COUNT; method++) {
        NYA_ConstCString candidate = _NYA_HTTP_METHOD_ROWS[method].text;

        u64 length = 0;
        while (candidate[length] != '\0') length++;

        if (length != size) continue;

        b8 same = true;
        for (u64 index = 0; index < size; index++) same = same && text[index] == candidate[index];

        if (same) return (NYA_HttpMethod)method;
    }

    return NYA_HTTP_METHOD_NONE;
}

b8 nya_http_method_is_valid(NYA_HttpMethod method) {
    return method > NYA_HTTP_METHOD_NONE && method < NYA_HTTP_METHOD_COUNT;
}

b8 nya_http_method_is_safe(NYA_HttpMethod method) {
    nya_assert(method >= 0 && method < NYA_HTTP_METHOD_COUNT, "a method outside the enum reached nya_http_method_is_safe");

    return _NYA_HTTP_METHOD_ROWS[method].safe;
}

b8 nya_http_method_allows_body(NYA_HttpMethod method) {
    nya_assert(method >= 0 && method < NYA_HTTP_METHOD_COUNT, "a method outside the enum reached nya_http_method_allows_body");

    return _NYA_HTTP_METHOD_ROWS[method].allows_body;
}

NYA_ConstCString nya_http_status_text(NYA_HttpStatus status) {
    for (u64 index = 0; index < nya_carray_length(_NYA_HTTP_STATUS_ROWS); index++) {
        if (_NYA_HTTP_STATUS_ROWS[index].status == status) return _NYA_HTTP_STATUS_ROWS[index].reason;
    }

    return "";
}

void nya_http_address_truncate(NYA_ConstCString address, OUT char* out, u64 capacity) {
    nya_assert(address != nullptr && out != nullptr && capacity > 0);

    u64 size = strnlen(address, NYA_HTTP_MAX_ADDRESS);

    // a zone names the interface, not the peer, and it is not part of the network either.
    const char* zone = memchr(address, '%', size);
    if (zone != nullptr) size = (u64)(zone - address);

    u8  octets[4] = { 0 };
    u16 groups[8] = { 0 };

    if (_nya_http_parse_ipv4(address, size, octets)) {
        (void)snprintf(out, capacity, "%u.%u.%u.0/24", octets[0], octets[1], octets[2]);
        return;
    }

    if (_nya_http_parse_ipv6(address, size, groups)) {
        // an IPv4 peer on a dual stack socket arrives as ::ffff:a.b.c.d, and is the IPv4 network it is.
        b8 mapped = groups[0] == 0 && groups[1] == 0 && groups[2] == 0 && groups[3] == 0 && groups[4] == 0 && groups[5] == 0xFFFF;

        if (mapped) {
            (void)snprintf(out, capacity, "%u.%u.%u.0/24", groups[6] >> 8, groups[6] & 0xFF, groups[7] >> 8);
        } else {
            (void)snprintf(out, capacity, "%x:%x:%x::/48", groups[0], groups[1], groups[2]);
        }
        return;
    }

    (void)snprintf(out, capacity, "unknown");
}

b8 nya_http_status_is_valid(NYA_HttpStatus status) {
    for (u64 index = 0; index < nya_carray_length(_NYA_HTTP_STATUS_ROWS); index++) {
        if (_NYA_HTTP_STATUS_ROWS[index].status == status) return true;
    }

    return false;
}

NYA_ConstCString nya_http_media_type_text(NYA_HttpMediaType media_type) {
    nya_assert(media_type >= 0 && media_type < NYA_HTTP_MEDIA_COUNT, "a media type outside the enum reached nya_http_media_type_text");

    return _NYA_HTTP_MEDIA_TEXT[media_type];
}

NYA_HttpMediaType nya_http_media_type_parse(const char* text, u64 size) {
    if (text == nullptr) return NYA_HTTP_MEDIA_OTHER;

    // the essence is everything before the first ';', trimmed. "application/json ; charset=utf-8" is
    // the same type as "application/json", and a peer is free to write either.
    u64 essence = 0;
    while (essence < size && text[essence] != ';') essence++;

    while (essence > 0 && (text[essence - 1] == ' ' || text[essence - 1] == '\t')) essence--;

    for (u32 media = NYA_HTTP_MEDIA_NONE + 1; media < NYA_HTTP_MEDIA_OTHER; media++) {
        if (_nya_http_equals_ignore_case(text, essence, _NYA_HTTP_MEDIA_ESSENCE[media])) return (NYA_HttpMediaType)media;
    }

    return NYA_HTTP_MEDIA_OTHER;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_http_parse_ipv4(const char* text, u64 size, OUT u8* out_octets) {
    u32 octet = 0;
    u32 value = 0;
    u32 digits = 0;

    for (u64 index = 0; index <= size; index++) {
        char character = index < size ? text[index] : '.';

        if (character >= '0' && character <= '9') {
            if (digits > 0 && value == 0) return false;

            value = (value * 10) + (u32)(character - '0');
            digits++;

            if (digits > 3 || value > 255) return false;
            continue;
        }

        if (character != '.' || digits == 0 || octet >= 4) return false;

        out_octets[octet++] = (u8)value;
        value               = 0;
        digits              = 0;
    }

    return octet == 4;
}

b8 _nya_http_parse_ipv6(const char* text, u64 size, OUT u16* out_groups) {
    u16 head[8]    = { 0 };
    u16 tail[8]    = { 0 };
    u32 head_count = 0;
    u32 tail_count = 0;
    b8  elided     = false;

    u64 at = 0;

    if (size >= 2 && text[0] == ':' && text[1] == ':') {
        elided  = true;
        at      = 2;
    } else if (size >= 1 && text[0] == ':') {
        return false;
    }

    while (at < size) {
        u64 end = at;
        while (end < size && text[end] != ':') end++;

        u16* groups = elided ? tail : head;
        u32* count  = elided ? &tail_count : &head_count;

        // the last piece may be an IPv4 address, standing for the last two groups.
        if (end == size && memchr(text + at, '.', end - at) != nullptr) {
            u8 octets[4] = { 0 };
            if (*count + 2 > 8 || !_nya_http_parse_ipv4(text + at, end - at, octets)) return false;

            groups[(*count)++] = (u16)((octets[0] << 8) | octets[1]);
            groups[(*count)++] = (u16)((octets[2] << 8) | octets[3]);
            break;
        }

        if (end == at || end - at > 4 || *count >= 8) return false;

        u32 value = 0;
        for (u64 index = at; index < end; index++) {
            char character = text[index];
            u32  digit     = 0;

            if (character >= '0' && character <= '9') {
                digit = (u32)(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                digit = (u32)(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                digit = (u32)(character - 'A' + 10);
            } else {
                return false;
            }

            value = (value << 4) | digit;
        }

        groups[(*count)++] = (u16)value;
        at                 = end;

        if (at == size) break;

        // one colon separates; two elide, once.
        at++;
        if (at < size && text[at] == ':') {
            if (elided) return false;

            elided = true;
            at++;
        } else if (at == size) {
            return false;
        }
    }

    if (elided ? head_count + tail_count >= 8 : head_count != 8) return false;

    for (u32 index = 0; index < 8; index++) out_groups[index] = 0;
    for (u32 index = 0; index < head_count; index++) out_groups[index] = head[index];
    for (u32 index = 0; index < tail_count; index++) out_groups[8 - tail_count + index] = tail[index];

    return true;
}


char _nya_http_lower(char character) {
    if (character >= 'A' && character <= 'Z') return (char)(character - 'A' + 'a');

    return character;
}

b8 _nya_http_equals_ignore_case(const char* text, u64 size, NYA_ConstCString expected) {
    u64 length = 0;
    while (expected[length] != '\0') length++;

    if (length != size) return false;

    for (u64 index = 0; index < size; index++) {
        if (_nya_http_lower(text[index]) != _nya_http_lower(expected[index])) return false;
    }

    return true;
}
