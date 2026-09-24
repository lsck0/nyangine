#include "nyangine/platform/web/web_socket.h"

#if OS_WASM

#include <emscripten/emscripten.h>

// The browser owns the WebSocket, so the C side keeps an id into a registry the JS holds. A slot is
// { ws, phase, inbox }: phase is 0 connecting, 1 open, 2 closed, driven by the socket's events; inbox is
// the queue of received messages, each a Uint8Array, that receive drains oldest-first. A text frame is
// delivered as its bytes so the queue is uniform.
//
// clang-format is off across the EM_JS bodies below: the braces hold JavaScript, which the formatter
// reads as C and breaks. The C around them is formatted as usual.
// clang-format off
EM_JS(int, _nya_web_socket_open_js, (const char* url), {
    if (typeof WebSocket === "undefined") return 0;
    var registry = Module.__nyaSocket || (Module.__nyaSocket = { next: 1, map: {} });
    var id       = registry.next++;
    var slot     = { ws: null, phase: 0, inbox: [] };
    registry.map[id] = slot;
    try {
        var ws        = new WebSocket(UTF8ToString(url));
        ws.binaryType = "arraybuffer";
        ws.onopen     = function() { slot.phase = 1; };
        ws.onmessage  = function(event) {
            if (typeof event.data === "string") {
                var bytes = new Uint8Array(event.data.length);
                for (var i = 0; i < event.data.length; i++) bytes[i] = event.data.charCodeAt(i) & 0xff;
                slot.inbox.push(bytes);
            } else {
                slot.inbox.push(new Uint8Array(event.data));
            }
        };
        ws.onclose = function() { slot.phase = 2; };
        ws.onerror = function() { slot.phase = 2; };
        slot.ws    = ws;
    } catch (error) {
        slot.phase = 2;
    }
    return id;
})

EM_JS(int, _nya_web_socket_phase_js, (int id), {
    var registry = Module.__nyaSocket;
    if (!registry || !registry.map[id]) return 2;
    return registry.map[id].phase;
})

EM_JS(int, _nya_web_socket_send_js, (int id, const unsigned char* bytes, int size), {
    var registry = Module.__nyaSocket;
    if (!registry || !registry.map[id] || registry.map[id].phase !== 1) return 0;
    try {
        registry.map[id].ws.send(HEAPU8.slice(bytes, bytes + size));
        return 1;
    } catch (error) {
        return 0;
    }
})

EM_JS(int, _nya_web_socket_receive_js, (int id, unsigned char* out, int capacity), {
    var registry = Module.__nyaSocket;
    if (!registry || !registry.map[id] || registry.map[id].inbox.length === 0) return -1;
    var message = registry.map[id].inbox[0];
    if (message.length > capacity) return message.length; // left on the queue for a larger read
    HEAPU8.set(message, out);
    registry.map[id].inbox.shift();
    return message.length;
})

EM_JS(void, _nya_web_socket_close_js, (int id), {
    var registry = Module.__nyaSocket;
    if (!registry || !registry.map[id]) return;
    try {
        if (registry.map[id].ws && registry.map[id].phase < 2) registry.map[id].ws.close();
    } catch (error) { /* already closing */ }
    delete registry.map[id];
})
// clang-format on

struct NYA_WebSocketLink {
    int js_id;
};

NYA_WebSocketLink* nya_web_socket_open(NYA_Arena* arena, NYA_ConstCString url) {
    if (arena == nullptr || url == nullptr) return nullptr;

    int js_id = _nya_web_socket_open_js(url);
    if (js_id == 0) return nullptr; // the runtime has no WebSocket at all

    NYA_WebSocketLink* link = nya_arena_alloc(arena, sizeof(NYA_WebSocketLink));
    if (link == nullptr) {
        _nya_web_socket_close_js(js_id);
        return nullptr;
    }

    link->js_id = js_id;
    return link;
}

void nya_web_socket_close(NYA_WebSocketLink* link) {
    if (link == nullptr) return;
    _nya_web_socket_close_js(link->js_id);
}

NYA_WebSocketPhase nya_web_socket_phase(const NYA_WebSocketLink* link) {
    if (link == nullptr) return NYA_WEB_SOCKET_CLOSED;

    switch (_nya_web_socket_phase_js(link->js_id)) {
        case 1:  return NYA_WEB_SOCKET_OPEN;
        case 2:  return NYA_WEB_SOCKET_CLOSED;
        default: return NYA_WEB_SOCKET_CONNECTING;
    }
}

b8 nya_web_socket_send(NYA_WebSocketLink* link, const u8* bytes, u64 size) {
    if (link == nullptr || (bytes == nullptr && size != 0)) return false;
    return _nya_web_socket_send_js(link->js_id, bytes, (int)size) == 1;
}

s64 nya_web_socket_receive(NYA_WebSocketLink* link, OUT u8* buffer, u64 capacity) {
    if (link == nullptr || buffer == nullptr) return -1;
    return (s64)_nya_web_socket_receive_js(link->js_id, buffer, (int)capacity);
}

#else // native fallback: no browser WebSocket off wasm, so the seam refuses and stays browser-only

NYA_WebSocketLink* nya_web_socket_open(NYA_Arena* arena, NYA_ConstCString url) {
    nya_unused(arena);
    nya_unused(url);
    return nullptr;
}

void nya_web_socket_close(NYA_WebSocketLink* link) {
    nya_unused(link);
}

NYA_WebSocketPhase nya_web_socket_phase(const NYA_WebSocketLink* link) {
    nya_unused(link);
    return NYA_WEB_SOCKET_CLOSED;
}

b8 nya_web_socket_send(NYA_WebSocketLink* link, const u8* bytes, u64 size) {
    nya_unused(link);
    nya_unused(bytes);
    nya_unused(size);
    return false;
}

s64 nya_web_socket_receive(NYA_WebSocketLink* link, OUT u8* buffer, u64 capacity) {
    nya_unused(link);
    nya_unused(buffer);
    nya_unused(capacity);
    return -1;
}

#endif // OS_WASM
