#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_url.h"
#include "nyangine-core/http/http_doc.h"
#include "nyangine-core/http/http_message.h"

// PRIVATE TYPES

/** One served document: the bytes, how many, and what to announce them as. Its path is the route's. */
typedef struct {
    NYA_ConstCString  body;
    u64               body_size;
    NYA_HttpMediaType media;
} _NYA_HttpDocEntry;

// PRIVATE API DECLARATION

/** Appends `size` bytes verbatim, or records an overflow and writes nothing. Every append lands here. */
NYA_INTERNAL void _nya_http_doc_bytes(NYA_HttpDoc* doc, const char* data, u64 size);

/** The one handler every served route shares; which document it answers with is the route it was reached by. */
NYA_INTERNAL NYA_HttpStatus _nya_http_doc_serve(NYA_HttpExchange* exchange);

/** Copies `text` into the registry's arena, NUL-terminated, or null when it will not fit. */
NYA_INTERNAL NYA_ConstCString _nya_http_doc_dup(NYA_ConstCString text);

// STATE

// The served documents, filled at mount and read by the routes after; a fixed table since everything here is bounded. `routes` is parallel to `entries`: a matched route is found by its offset, which is why the router points straight at this array.
NYA_INTERNAL struct {
    NYA_Arena*        arena;
    _NYA_HttpDocEntry entries[NYA_HTTP_DOC_MAX_ROUTES];
    NYA_HttpRoute     routes[NYA_HTTP_DOC_MAX_ROUTES];
    u32               count;
} _NYA_HTTP_DOCS = { 0 };

NYA_INTERNAL NYA_HttpRouter _NYA_HTTP_DOC_ROUTER = {
    .name        = "discovery",
    .routes      = _NYA_HTTP_DOCS.routes,
    .route_count = 0,
};

// PUBLIC API IMPLEMENTATION — THE BUILDER

NYA_HttpDoc nya_http_doc_over(NYA_Arena* arena, u64 capacity) {
    nya_assert(arena != nullptr);

    if (capacity > NYA_HTTP_DOC_MAX_BYTES) capacity = NYA_HTTP_DOC_MAX_BYTES;

    // One past the bound for the terminator, so the built bytes are always a C string.
    char* buffer = nya_arena_alloc(arena, capacity + 1);

    NYA_HttpDoc doc = {
        .buffer     = buffer,
        .capacity   = buffer != nullptr ? capacity : 0,
        .used       = 0,
        .overflowed = false,
    };

    if (buffer != nullptr) buffer[0] = '\0';

    return doc;
}

void nya_http_doc_put(NYA_HttpDoc* doc, NYA_ConstCString text) {
    nya_assert(doc != nullptr && text != nullptr);

    _nya_http_doc_bytes(doc, text, strlen(text));
}

void nya_http_doc_putf(NYA_HttpDoc* doc, NYA_ConstCString format, ...) {
    nya_assert(doc != nullptr && format != nullptr);

    // A fixed line, as ui_present_html's builder takes: every format string here is short (a number, a fixed tag, a rendered date), and one that wouldn't fit is a caller bug, caught as an overflow rather than truncated silently.
    char line[1024] = { 0 };

    va_list args;
    va_start(args, format);
    s32 written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (written < 0 || (u64)written >= sizeof(line)) {
        doc->overflowed = true;
        return;
    }

    _nya_http_doc_bytes(doc, line, (u64)written);
}

void nya_http_doc_xml_text(NYA_HttpDoc* doc, NYA_ConstCString text) {
    nya_assert(doc != nullptr && text != nullptr);

    for (u64 i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '&': _nya_http_doc_bytes(doc, "&amp;", 5); break;
            case '<': _nya_http_doc_bytes(doc, "&lt;", 4); break;
            case '>': _nya_http_doc_bytes(doc, "&gt;", 4); break;
            default:  _nya_http_doc_bytes(doc, &text[i], 1); break;
        }
    }
}

void nya_http_doc_xml_attr(NYA_HttpDoc* doc, NYA_ConstCString text) {
    nya_assert(doc != nullptr && text != nullptr);

    for (u64 i = 0; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '&':  _nya_http_doc_bytes(doc, "&amp;", 5); break;
            case '<':  _nya_http_doc_bytes(doc, "&lt;", 4); break;
            case '>':  _nya_http_doc_bytes(doc, "&gt;", 4); break;
            case '"':  _nya_http_doc_bytes(doc, "&quot;", 6); break;
            case '\'': _nya_http_doc_bytes(doc, "&#39;", 5); break;
            default:   _nya_http_doc_bytes(doc, &text[i], 1); break;
        }
    }
}

void nya_http_doc_xml_cdata(NYA_HttpDoc* doc, NYA_ConstCString text) {
    nya_assert(doc != nullptr && text != nullptr);

    nya_http_doc_put(doc, "<![CDATA[");

    // The only sequence a CDATA section can't contain is its terminator "]]>"; where the data has one it's split as "]]" + "]]><![CDATA[" + ">" so no parser sees the closer early. Everything else (< and & included) is literal inside CDATA, so nothing else is escaped.
    for (u64 i = 0; text[i] != '\0'; i++) {
        if (text[i] == ']' && text[i + 1] == ']' && text[i + 2] == '>') {
            nya_http_doc_put(doc, "]]]]><![CDATA[>");
            i += 2;
            continue;
        }

        _nya_http_doc_bytes(doc, &text[i], 1);
    }

    nya_http_doc_put(doc, "]]>");
}

NYA_Error nya_http_doc_finish(const NYA_HttpDoc* doc, OUT NYA_ConstCString* out) {
    nya_assert(doc != nullptr && out != nullptr);

    *out = nullptr;

    if (doc->overflowed || doc->buffer == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the document overflowed its %d byte bound and was refused whole", (s32)NYA_HTTP_DOC_MAX_BYTES);
    }

    *out = doc->buffer;

    return NYA_OK;
}

// PUBLIC API IMPLEMENTATION — THE URL GATE

b8 nya_http_doc_url_is_web(NYA_ConstCString text) {
    if (text == nullptr || text[0] == '\0') return false;

    NYA_Url        url     = { 0 };
    NYA_UrlFailure failure = { 0 };

    // The parser refuses a control byte, a space, a malformed escape and every scheme this engine doesn't speak; the two schemes below are the web's, all a loc or link may be.
    if (!nya_url_parse(text, strlen(text), &url, &failure).ok) return false;

    return url.scheme == NYA_URL_SCHEME_HTTP || url.scheme == NYA_URL_SCHEME_HTTPS;
}

// PUBLIC API IMPLEMENTATION — THE SERVE REGISTRY

NYA_Error nya_http_doc_serve(NYA_ConstCString path, NYA_HttpMediaType media, NYA_ConstCString body, NYA_ConstCString summary) {
    if (path == nullptr || path[0] != '/') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a served document needs an absolute path");
    if (body == nullptr || body[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a served document needs a body");
    if (summary == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a served document needs a summary");

    u64 body_size = strlen(body);
    if (body_size > NYA_HTTP_MAX_RESPONSE_BYTES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a %llu byte document does not fit a response", (unsigned long long)body_size);
    }

    if (_NYA_HTTP_DOCS.count >= NYA_HTTP_DOC_MAX_ROUTES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to serve more than %d documents", NYA_HTTP_DOC_MAX_ROUTES);
    }

    for (u32 i = 0; i < _NYA_HTTP_DOCS.count; i++) {
        if (strcmp(_NYA_HTTP_DOCS.routes[i].path, path) == 0) return nya_error(NYA_ERROR_ALREADY_EXISTS, "'%s' is already served", path);
    }

    // The arena is made on the first serve and freed by clear, so the served bytes outlive the caller's build arena; everything below is copied into it.
    if (_NYA_HTTP_DOCS.arena == nullptr) _NYA_HTTP_DOCS.arena = nya_arena_create(.name = "http_doc");

    NYA_ConstCString path_copy    = _nya_http_doc_dup(path);
    NYA_ConstCString summary_copy = _nya_http_doc_dup(summary);
    NYA_ConstCString body_copy    = _nya_http_doc_dup(body);

    if (path_copy == nullptr || summary_copy == nullptr || body_copy == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no memory to keep the '%s' document", path);
    }

    u32 slot = _NYA_HTTP_DOCS.count;

    _NYA_HTTP_DOCS.entries[slot] = (_NYA_HttpDocEntry){ .body = body_copy, .body_size = body_size, .media = media };

    _NYA_HTTP_DOCS.routes[slot] = (NYA_HttpRoute){
        .method   = NYA_HTTP_METHOD_GET,
        .path     = path_copy,
        .auth     = NYA_HTTP_AUTH_NONE,
        .handler  = _nya_http_doc_serve,
        .summary  = summary_copy,
        .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
    };

    _NYA_HTTP_DOCS.count           = slot + 1;
    _NYA_HTTP_DOC_ROUTER.route_count = _NYA_HTTP_DOCS.count;

    // The table must be one the server would serve; a malformed route is rolled back rather than left half-registered, so a refused serve changes nothing.
    NYA_Error checked = nya_http_router_check(&_NYA_HTTP_DOC_ROUTER);
    if (!checked.ok) {
        _NYA_HTTP_DOCS.entries[slot]   = (_NYA_HttpDocEntry){ 0 };
        _NYA_HTTP_DOCS.routes[slot]    = (NYA_HttpRoute){ 0 };
        _NYA_HTTP_DOCS.count           = slot;
        _NYA_HTTP_DOC_ROUTER.route_count = slot;
        return checked;
    }

    return NYA_OK;
}

const NYA_HttpRouter* nya_http_doc_router(void) {
    return &_NYA_HTTP_DOC_ROUTER;
}

void nya_http_doc_clear(void) {
    if (_NYA_HTTP_DOCS.arena != nullptr) {
        nya_arena_destroy(_NYA_HTTP_DOCS.arena);
        _NYA_HTTP_DOCS.arena = nullptr;
    }

    nya_memset(_NYA_HTTP_DOCS.entries, 0, sizeof(_NYA_HTTP_DOCS.entries));
    nya_memset(_NYA_HTTP_DOCS.routes, 0, sizeof(_NYA_HTTP_DOCS.routes));
    _NYA_HTTP_DOCS.count           = 0;
    _NYA_HTTP_DOC_ROUTER.route_count = 0;
}

// PRIVATE API IMPLEMENTATION

void _nya_http_doc_bytes(NYA_HttpDoc* doc, const char* data, u64 size) {
    // Once overflowed, or with no buffer, every further append is a no-op that keeps the flag set: finish turns it into a refusal, so the builder need not be checked per call.
    if (doc->overflowed || doc->buffer == nullptr) {
        doc->overflowed = true;
        return;
    }

    if (size > doc->capacity - doc->used) {
        doc->overflowed = true;
        return;
    }

    if (size > 0) memcpy(doc->buffer + doc->used, data, size);

    doc->used += size;
    doc->buffer[doc->used] = '\0';
}

NYA_HttpStatus _nya_http_doc_serve(NYA_HttpExchange* exchange) {
    nya_assert(exchange->route != nullptr, "the discovery handler is only reachable through one of its routes");

    // Which document, from the route's offset into the parallel arrays; the router points straight at that array, so the matched route is one of these and the index is exact.
    u64 index = (u64)(exchange->route - _NYA_HTTP_DOCS.routes);
    nya_assert(index < _NYA_HTTP_DOCS.count, "a route outside the registry reached the discovery handler");

    const _NYA_HttpDocEntry* entry = &_NYA_HTTP_DOCS.entries[index];

    if (!nya_http_response_bytes(exchange->response, (const u8*)entry->body, entry->body_size, entry->media).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_ConstCString _nya_http_doc_dup(NYA_ConstCString text) {
    u64   size = strlen(text);
    char* copy = nya_arena_alloc(_NYA_HTTP_DOCS.arena, size + 1);

    if (copy == nullptr) return nullptr;

    if (size > 0) memcpy(copy, text, size);
    copy[size] = '\0';

    return copy;
}
