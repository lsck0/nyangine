#include "nyangine/platform/web/web_input.h"

#if OS_WASM

#include <emscripten/emscripten.h>

// The browser owns the DOM events, so the C side keeps nothing but a poll that drains a JS queue the
// listeners push onto. The registry Module.__nyaInput is { canvas, queue, handlers }: `queue` is the
// FIFO of events, each a plain object { kind, x, y, dx, dy, button, key, repeat }; `handlers` holds the
// listener functions so detach can remove exactly the ones attach added. The queue is capped so a module
// that stops polling cannot make the tab grow without bound — the oldest events are dropped, as an input
// backend drops stale motion.
//
// The `kind` integers below MUST match NYA_WebInputKind's order in web_input.h: 0 MOUSE_MOVED, 1 MOUSE_DOWN,
// 2 MOUSE_UP, 3 WHEEL, 4 KEY_DOWN, 5 KEY_UP. The key sentinels match the NYA_WEB_KEY_* macros.
//
// clang-format is off across the EM_JS bodies: the braces hold JavaScript, which the formatter reads as C
// and breaks (splitting `===`, reflowing the object and arrow-function literals). The C around them is
// formatted as usual.
// clang-format off
EM_JS(int, _nya_web_input_attach_js, (const char* selector), {
    if (typeof document === "undefined") return 0;
    var canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) return 0;

    // Re-point: tear down any previous attachment so the listeners never double up on a second attach.
    if (Module.__nyaInput) { Module.__nyaInputDetach(); }

    var reg = { canvas: canvas, queue: [], handlers: {} };
    Module.__nyaInput = reg;

    var CAP = 256; // the queue ceiling; past it the oldest event is dropped so the tab cannot grow unbounded.
    var push = function(event) {
        if (reg.queue.length >= CAP) reg.queue.shift();
        reg.queue.push(event);
    };

    // CSS pixels → drawing-buffer pixels, the space the shim's viewport and render2d's projection agree on.
    var at = function(clientX, clientY) {
        var rect  = canvas.getBoundingClientRect();
        var scale_x = rect.width  > 0 ? canvas.width  / rect.width  : 1;
        var scale_y = rect.height > 0 ? canvas.height / rect.height : 1;
        return { x: (clientX - rect.left) * scale_x, y: (clientY - rect.top) * scale_y };
    };

    // event.key → the seam's key code: a printable key is its lowercase code point; the arrows are sentinels.
    var keycode = function(event) {
        if (event.key === "ArrowLeft")  return 0x01000001;
        if (event.key === "ArrowRight") return 0x01000002;
        if (event.key === "ArrowUp")    return 0x01000003;
        if (event.key === "ArrowDown")  return 0x01000004;
        if (event.key.length === 1)     return event.key.toLowerCase().charCodeAt(0);
        return 0;
    };
    // The keys whose default page action (scrolling) must be suppressed while the module has focus.
    var swallow = function(event) {
        return event.key === " " || event.key === "ArrowLeft" || event.key === "ArrowRight"
            || event.key === "ArrowUp" || event.key === "ArrowDown";
    };

    reg.handlers.move = function(event) {
        var p = at(event.clientX, event.clientY);
        push({ kind: 0, x: p.x, y: p.y, dx: event.movementX || 0, dy: event.movementY || 0, button: 0, key: 0, repeat: 0 });
    };
    reg.handlers.down = function(event) {
        var p = at(event.clientX, event.clientY);
        push({ kind: 1, x: p.x, y: p.y, dx: 0, dy: 0, button: event.button + 1, key: 0, repeat: 0 });
    };
    reg.handlers.up = function(event) {
        var p = at(event.clientX, event.clientY);
        push({ kind: 2, x: p.x, y: p.y, dx: 0, dy: 0, button: event.button + 1, key: 0, repeat: 0 });
    };
    reg.handlers.wheel = function(event) {
        var p = at(event.clientX, event.clientY);
        push({ kind: 3, x: p.x, y: p.y, dx: event.deltaX, dy: event.deltaY, button: 0, key: 0, repeat: 0 });
        event.preventDefault();
    };
    reg.handlers.keydown = function(event) {
        push({ kind: 4, x: 0, y: 0, dx: 0, dy: 0, button: 0, key: keycode(event), repeat: event.repeat ? 1 : 0 });
        if (swallow(event)) event.preventDefault();
    };
    reg.handlers.keyup = function(event) {
        push({ kind: 5, x: 0, y: 0, dx: 0, dy: 0, button: 0, key: keycode(event), repeat: 0 });
        if (swallow(event)) event.preventDefault();
    };

    canvas.addEventListener("mousemove", reg.handlers.move);
    canvas.addEventListener("mousedown", reg.handlers.down);
    canvas.addEventListener("mouseup",   reg.handlers.up);
    canvas.addEventListener("wheel",     reg.handlers.wheel, { passive: false });
    window.addEventListener("keydown",   reg.handlers.keydown);
    window.addEventListener("keyup",     reg.handlers.keyup);

    // The teardown, kept on Module so both a re-point above and nya_web_input_detach reach the same removal.
    Module.__nyaInputDetach = function() {
        var r = Module.__nyaInput;
        if (!r) return;
        r.canvas.removeEventListener("mousemove", r.handlers.move);
        r.canvas.removeEventListener("mousedown", r.handlers.down);
        r.canvas.removeEventListener("mouseup",   r.handlers.up);
        r.canvas.removeEventListener("wheel",     r.handlers.wheel);
        window.removeEventListener("keydown",     r.handlers.keydown);
        window.removeEventListener("keyup",       r.handlers.keyup);
        Module.__nyaInput = null;
    };

    return 1;
})

EM_JS(int, _nya_web_input_poll_js, (int* out_i, float* out_f), {
    var reg = Module.__nyaInput;
    if (!reg || reg.queue.length === 0) return 0;
    var e = reg.queue.shift();
    HEAP32[(out_i >> 2) + 0]  = e.kind;
    HEAP32[(out_i >> 2) + 1]  = e.button;
    HEAP32[(out_i >> 2) + 2]  = e.key;
    HEAP32[(out_i >> 2) + 3]  = e.repeat;
    HEAPF32[(out_f >> 2) + 0] = e.x;
    HEAPF32[(out_f >> 2) + 1] = e.y;
    HEAPF32[(out_f >> 2) + 2] = e.dx;
    HEAPF32[(out_f >> 2) + 3] = e.dy;
    return 1;
})

EM_JS(void, _nya_web_input_detach_js, (void), {
    if (Module.__nyaInputDetach) Module.__nyaInputDetach();
})
// clang-format on

b8 nya_web_input_attach(NYA_ConstCString selector) {
    if (selector == nullptr) return false;
    return _nya_web_input_attach_js(selector) == 1;
}

void nya_web_input_detach(void) {
    _nya_web_input_detach_js();
}

b8 nya_web_input_poll(OUT NYA_WebInputEvent* out_event) {
    if (out_event == nullptr) return false;

    // The JS side writes four ints and four floats; unpack them into the flat event the header declares.
    s32 fields_i[4] = { 0 };
    f32 fields_f[4] = { 0 };
    if (_nya_web_input_poll_js(fields_i, fields_f) != 1) return false;

    *out_event = (NYA_WebInputEvent){
        .kind    = (NYA_WebInputKind)fields_i[0],
        .button  = fields_i[1],
        .key     = (u32)fields_i[2],
        .repeat  = fields_i[3] != 0,
        .x       = fields_f[0],
        .y       = fields_f[1],
        .delta_x = fields_f[2],
        .delta_y = fields_f[3],
    };
    return true;
}

#else // native fallback: no DOM off wasm, so the seam refuses and stays browser-only, like fetch and the socket

b8 nya_web_input_attach(NYA_ConstCString selector) {
    nya_unused(selector);
    return false;
}

void nya_web_input_detach(void) {
}

b8 nya_web_input_poll(OUT NYA_WebInputEvent* out_event) {
    nya_unused(out_event);
    return false;
}

#endif // OS_WASM
