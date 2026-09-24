#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/http/http_livereload.h"
#include "nyangine/http/http_message.h"
#include "nyangine/http/http_static.h"
#include "nyangine/http/http_websocket_server.h"

#if !NYA_SHIPPING_BUILD

/*
 * The client snippet, spelled once and served verbatim at /livereload.js. It opens the socket, reloads
 * on the one message the server pushes — NYA_HTTP_LIVERELOAD_MESSAGE, "reload", kept in step with the
 * literal below — and reconnects with a doubling, capped backoff when the socket drops, so a server that
 * restarts is picked up again without a tight loop hammering a port that is not answering yet. It reloads
 * on the message and never merely on reconnecting, so it cannot fall into a loop of its own.
 */
NYA_INTERNAL const NYA_ConstCString _NYA_HTTP_LIVERELOAD_CLIENT_JS =
    "(function () {\n"
    "  var url = (location.protocol === \"https:\" ? \"wss://\" : \"ws://\") + location.host + \"/livereload\";\n"
    "  var backoff = 500;\n"
    "  function connect() {\n"
    "    var socket = new WebSocket(url);\n"
    "    socket.onmessage = function (event) {\n"
    "      if (event.data === \"reload\") location.reload();\n"
    "    };\n"
    "    socket.onopen = function () { backoff = 500; };\n"
    "    socket.onclose = function () {\n"
    "      setTimeout(connect, backoff);\n"
    "      backoff = Math.min(backoff * 2, 10000);\n"
    "    };\n"
    "    socket.onerror = function () { socket.close(); };\n"
    "  }\n"
    "  connect();\n"
    "})();\n";

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The one number the watch remembers, and whether it has ever seen one. The flag is what makes a first
 * sight record-and-stay-quiet rather than reload: a zero fingerprint is what an unmounted bundle folds
 * to, so zero alone cannot stand in for "nothing recorded yet".
 */
static u64 _FINGERPRINT     = 0;
static b8  _HAS_FINGERPRINT = false;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ROUTES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The push-only stream. No callbacks: the server never hears from the page, it only pushes to it. */
NYA_INTERNAL const NYA_HttpWebSocketRoute _NYA_HTTP_LIVERELOAD_STREAM = {
    .path    = NYA_HTTP_LIVERELOAD_PATH,
    .summary = "development live reload: pushes 'reload' when the web bundle changes",
};

/** Serves the client snippet. Read-only, so it runs on a worker; see http_router.h. */
NYA_INTERNAL NYA_HttpStatus _nya_http_livereload_script(NYA_HttpExchange* exchange) {
    NYA_ConstCString body = _NYA_HTTP_LIVERELOAD_CLIENT_JS;

    if (!nya_http_response_bytes(exchange->response, (const u8*)body, strlen(body), NYA_HTTP_MEDIA_JAVASCRIPT).ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_OK;
}

NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_LIVERELOAD_ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_GET,
     .path     = NYA_HTTP_LIVERELOAD_SCRIPT_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = _nya_http_livereload_script,
     .summary  = "The development live-reload client snippet",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_LIVERELOAD_ROUTER = {
    .name        = "livereload",
    .routes      = _NYA_HTTP_LIVERELOAD_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_LIVERELOAD_ROUTES),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_http_livereload_available(void) { return true; }

NYA_Error nya_http_livereload_route_add(void) { return nya_http_websocket_route_add(&_NYA_HTTP_LIVERELOAD_STREAM); }

void nya_http_livereload_route_remove(void) { nya_http_websocket_route_remove(&_NYA_HTTP_LIVERELOAD_STREAM); }

const NYA_HttpRouter* nya_http_livereload_router(void) { return &_NYA_HTTP_LIVERELOAD_ROUTER; }

b8 nya_http_livereload_signal(u64 fingerprint) {
    // A first sight records the baseline and stays quiet, so a page is not reloaded the moment it connects.
    if (!_HAS_FINGERPRINT) {
        _FINGERPRINT     = fingerprint;
        _HAS_FINGERPRINT = true;
        return false;
    }

    // Unchanged: two comparisons and no push, which is what makes polling every tick free.
    if (fingerprint == _FINGERPRINT) return false;

    // The bundle moved. Record the new version first, so a broadcast that runs re-entrantly cannot see the
    // old one, then push exactly one reload to everyone listening.
    _FINGERPRINT = fingerprint;

    (void)nya_http_websocket_broadcast_text(NYA_HTTP_LIVERELOAD_PATH, NYA_HTTP_LIVERELOAD_MESSAGE);

    return true;
}

b8 nya_http_livereload_poll(void) { return nya_http_livereload_signal(nya_http_static_fingerprint()); }

void nya_http_livereload_reset(void) {
    _FINGERPRINT     = 0;
    _HAS_FINGERPRINT = false;
}

NYA_ConstCString nya_http_livereload_client_js(void) { return _NYA_HTTP_LIVERELOAD_CLIENT_JS; }

#else // NYA_SHIPPING_BUILD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHIPPING: COMPILED OUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A page that reloads itself on a message from the port is a development tool, not something a release
 * ships. So the socket, the script and the watch are gone here, and the entry points stay only so a
 * program links whether or not it guarded them: available() is false, the mount refuses, the router is
 * empty, and a signal pushes nothing.
 */

/** An empty router, so merging it in a shipping build is harmless and serves nothing. */
NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_LIVERELOAD_ROUTER = {
    .name        = "livereload",
    .routes      = nullptr,
    .route_count = 0,
};

b8 nya_http_livereload_available(void) { return false; }

NYA_Error nya_http_livereload_route_add(void) { return nya_error(NYA_ERROR_NOT_SUPPORTED, "live reload is compiled out of a shipping build"); }

void nya_http_livereload_route_remove(void) {}

const NYA_HttpRouter* nya_http_livereload_router(void) { return &_NYA_HTTP_LIVERELOAD_ROUTER; }

b8 nya_http_livereload_signal(u64 fingerprint) {
    nya_unused(fingerprint);
    return false;
}

b8 nya_http_livereload_poll(void) { return false; }

void nya_http_livereload_reset(void) {}

NYA_ConstCString nya_http_livereload_client_js(void) { return ""; }

#endif // !NYA_SHIPPING_BUILD
