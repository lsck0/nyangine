#include "nyangine-std/platform/web/web_fetch.h"

#if OS_WASM

#include <emscripten/emscripten.h>

// A fetch is a browser-side promise, so the C side keeps only an id into a registry the JS holds and
// polls it. The registry maps id → { status, code, body }, status being 0 pending, 1 done, 2 failed; the
// promise's callbacks fill the slot and C reads it. HEAPU8.slice copies the request body at the call, so
// a memory growth later cannot detach the view under the transfer.
//
// clang-format is off across the EM_JS bodies below: the braces hold JavaScript, which the formatter
// reads as C and breaks. The C around them is formatted as usual.
// clang-format off
EM_JS(int, _nya_web_fetch_begin_js, (const char* method, const char* url, const unsigned char* body, int body_len, const char* content_type), {
    var registry = Module.__nyaFetch || (Module.__nyaFetch = { next: 1, map: {} });
    var id       = registry.next++;
    var slot     = { status: 0, code: 0, body: null };
    registry.map[id] = slot;
    try {
        var options = { method: UTF8ToString(method) };
        if (body_len > 0) options.body = HEAPU8.slice(body, body + body_len);
        var contentType = UTF8ToString(content_type);
        if (contentType && contentType.length > 0) options.headers = { "Content-Type": contentType };
        fetch(UTF8ToString(url), options)
            .then(function(response) { slot.code = response.status; return response.arrayBuffer(); })
            .then(function(buffer) { slot.body = new Uint8Array(buffer); slot.status = 1; })
            .catch(function(error) { slot.status = 2; });
    } catch (error) {
        slot.status = 2;
    }
    return id;
})

EM_JS(int, _nya_web_fetch_status_js, (int id), {
    var registry = Module.__nyaFetch;
    if (!registry || !registry.map[id]) return 2;
    return registry.map[id].status;
})

EM_JS(int, _nya_web_fetch_code_js, (int id), {
    var registry = Module.__nyaFetch;
    if (!registry || !registry.map[id]) return 0;
    return registry.map[id].code;
})

EM_JS(int, _nya_web_fetch_body_js, (int id, unsigned char* out, int capacity), {
    var registry = Module.__nyaFetch;
    if (!registry || !registry.map[id] || registry.map[id].body === null) return -1;
    var body    = registry.map[id].body;
    var written = (body.length < capacity) ? body.length : capacity;
    HEAPU8.set(body.subarray(0, written), out);
    return body.length;
})

EM_JS(void, _nya_web_fetch_release_js, (int id), {
    var registry = Module.__nyaFetch;
    if (registry && registry.map[id]) delete registry.map[id];
})
// clang-format on

struct NYA_WebFetch {
    int js_id;
};

NYA_WebFetch*
nya_web_fetch_create(NYA_Arena* arena, NYA_ConstCString method, NYA_ConstCString url, const u8* body, u64 body_size, NYA_ConstCString content_type) {
    if (arena == nullptr || method == nullptr || url == nullptr) return nullptr;
    if (body != nullptr && body_size != 0 && content_type == nullptr) return nullptr;

    NYA_WebFetch* fetch = nya_arena_alloc(arena, sizeof(NYA_WebFetch));
    if (fetch == nullptr) return nullptr;

    fetch->js_id = _nya_web_fetch_begin_js(method, url, body, (int)body_size, content_type != nullptr ? content_type : "");
    return fetch;
}

void nya_web_fetch_destroy(NYA_WebFetch* fetch) {
    if (fetch == nullptr) return;
    _nya_web_fetch_release_js(fetch->js_id);
}

NYA_WebFetchStatus nya_web_fetch_poll(NYA_WebFetch* fetch) {
    if (fetch == nullptr) return NYA_WEB_FETCH_FAILED;

    switch (_nya_web_fetch_status_js(fetch->js_id)) {
        case 1:  return NYA_WEB_FETCH_DONE;
        case 2:  return NYA_WEB_FETCH_FAILED;
        default: return NYA_WEB_FETCH_PENDING;
    }
}

u32 nya_web_fetch_status_code(const NYA_WebFetch* fetch) {
    if (fetch == nullptr) return 0;
    return (u32)_nya_web_fetch_code_js(fetch->js_id);
}

s64 nya_web_fetch_body(const NYA_WebFetch* fetch, OUT u8* buffer, u64 capacity) {
    if (fetch == nullptr || buffer == nullptr) return -1;
    return (s64)_nya_web_fetch_body_js(fetch->js_id, buffer, (int)capacity);
}

#else // native fallback: no fetch off the browser, so the seam refuses and stays browser-only

NYA_WebFetch*
nya_web_fetch_create(NYA_Arena* arena, NYA_ConstCString method, NYA_ConstCString url, const u8* body, u64 body_size, NYA_ConstCString content_type) {
    nya_unused(arena);
    nya_unused(method);
    nya_unused(url);
    nya_unused(body);
    nya_unused(body_size);
    nya_unused(content_type);
    return nullptr;
}

void nya_web_fetch_destroy(NYA_WebFetch* fetch) {
    nya_unused(fetch);
}

NYA_WebFetchStatus nya_web_fetch_poll(NYA_WebFetch* fetch) {
    nya_unused(fetch);
    return NYA_WEB_FETCH_FAILED;
}

u32 nya_web_fetch_status_code(const NYA_WebFetch* fetch) {
    nya_unused(fetch);
    return 0;
}

s64 nya_web_fetch_body(const NYA_WebFetch* fetch, OUT u8* buffer, u64 capacity) {
    nya_unused(fetch);
    nya_unused(buffer);
    nya_unused(capacity);
    return -1;
}

#endif // OS_WASM
