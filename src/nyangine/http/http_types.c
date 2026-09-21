#include "nyangine/base/base_assert.h"
#include "nyangine/http/http_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The verbs, indexed by NYA_HttpMethod. One table so the text and the parse cannot disagree, which is
 * the failure mode of writing the switch twice.
 * */
NYA_INTERNAL NYA_ConstCString _NYA_HTTP_METHOD_TEXT[NYA_HTTP_METHOD_COUNT] = {
    [NYA_HTTP_METHOD_NONE] = "",   [NYA_HTTP_METHOD_GET] = "GET",     [NYA_HTTP_METHOD_HEAD] = "HEAD",     [NYA_HTTP_METHOD_POST] = "POST",
    [NYA_HTTP_METHOD_PUT] = "PUT", [NYA_HTTP_METHOD_PATCH] = "PATCH", [NYA_HTTP_METHOD_DELETE] = "DELETE", [NYA_HTTP_METHOD_OPTIONS] = "OPTIONS",
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
    [NYA_HTTP_MEDIA_NONE]  = "",
    [NYA_HTTP_MEDIA_JSON]  = "application/json; charset=utf-8",
    [NYA_HTTP_MEDIA_TEXT]  = "text/plain; charset=utf-8",
    [NYA_HTTP_MEDIA_HTML]  = "text/html; charset=utf-8",
    [NYA_HTTP_MEDIA_OTHER] = "",
};

/** What each media type is called on the wire, without parameters, for matching an incoming header. */
NYA_INTERNAL NYA_ConstCString _NYA_HTTP_MEDIA_ESSENCE[NYA_HTTP_MEDIA_COUNT] = {
    [NYA_HTTP_MEDIA_NONE] = "",           [NYA_HTTP_MEDIA_JSON] = "application/json",
    [NYA_HTTP_MEDIA_TEXT] = "text/plain", [NYA_HTTP_MEDIA_HTML] = "text/html",
    [NYA_HTTP_MEDIA_OTHER] = "",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    return _NYA_HTTP_METHOD_TEXT[method];
}

NYA_HttpMethod nya_http_method_parse(const char* text, u64 size) {
    if (text == nullptr || size == 0) return NYA_HTTP_METHOD_NONE;

    for (u32 method = NYA_HTTP_METHOD_NONE + 1; method < NYA_HTTP_METHOD_COUNT; method++) {
        NYA_ConstCString candidate = _NYA_HTTP_METHOD_TEXT[method];

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

NYA_ConstCString nya_http_status_text(NYA_HttpStatus status) {
    for (u64 index = 0; index < nya_carray_length(_NYA_HTTP_STATUS_ROWS); index++) {
        if (_NYA_HTTP_STATUS_ROWS[index].status == status) return _NYA_HTTP_STATUS_ROWS[index].reason;
    }

    return "";
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
