#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_compare.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/http/http_server.h"
#include "nyangine/http/http_websocket_server.h"

// TYPES

typedef struct _NYA_HttpWebSocketState _NYA_HttpWebSocketState;

/**
 * One connection that upgraded.
 *
 * The socket is the HTTP connection's and is only borrowed: http_server.c accepted it, counts it and
 * closes it, which is what keeps a WebSocket inside the listener's bounds instead of beside them.
 * */
struct NYA_HttpWebSocket {
    NYA_OsSocket socket;

    const NYA_HttpWebSocketRoute* route;

    char address[NYA_HTTP_MAX_ADDRESS];

    NYA_WebSocketProtocol protocol;

    /** What has arrived and not yet been read as frames. */
    u8  receive[NYA_HTTP_WEBSOCKET_RECEIVE_BYTES];
    u64 receive_size;

    /** The two buffers the protocol was opened over. */
    u8 send[NYA_HTTP_WEBSOCKET_SEND_BYTES];
    u8 message[NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES + 1];

    /** Monotonic nanoseconds of the last frame received; the ping and the timeout both read it. */
    u64 heard_at_ns;

    /** Monotonic nanoseconds of the last ping sent, so one goes out per interval and not per tick. */
    u64 pinged_at_ns;
};

struct _NYA_HttpWebSocketState {
    NYA_Arena* allocator;

    const NYA_HttpWebSocketRoute* routes[NYA_HTTP_MAX_WEBSOCKET_ROUTES];
    u32                           route_count;

    NYA_HttpWebSocket connections[NYA_HTTP_MAX_WEBSOCKETS];
};

// STATE

/** Null until a program mounts its first route, which is when a server pays for any of this. */
NYA_INTERNAL _NYA_HttpWebSocketState* _NYA_HTTP_WEBSOCKET = nullptr;

/**
 * How many connections are open, outside the state on purpose: http_server.c registers it as a ceiling
 * and nya_ceiling_register keeps the pointer forever, so what it points at has to outlive the arena.
 * The registration is over there because a ceiling is core's and this file does not reach into core.
 * */
NYA_INTERNAL u32 _NYA_HTTP_WEBSOCKET_COUNT = 0;

// PRIVATE API DECLARATION

// What http_server.c calls, which is why this file is included before it: a connection becomes a WebSocket where a request would otherwise be answered, and is drained and closed where one would be.

/** Whether `request` is asking for the upgrade at all, by its two headers and nothing else. */
NYA_INTERNAL b8 _nya_http_websocket_is_upgrade(const NYA_HttpRequest* request) __attr_no_discard;

/**
 * Answers the upgrade: writes the 101 and takes the socket over, or says which status to refuse with
 * and leaves the connection an HTTP one. `trailing` is what the peer sent after the request.
 * */
NYA_INTERNAL NYA_HttpStatus _nya_http_websocket_upgrade(
    NYA_OsSocket           socket,
    NYA_ConstCString       address,
    const NYA_HttpRequest* request,
    u64                    trailing,
    OUT NYA_ConstCString*  out_detail
) __attr_no_discard;

/** Reads, dispatches, answers and flushes one connection. False when it is finished with. */
NYA_INTERNAL b8 _nya_http_websocket_tick(NYA_OsSocket socket) __attr_no_discard;

/** Gives the slot back and reports the close. Idempotent, and a no-op for a socket that never upgraded. */
NYA_INTERNAL void _nya_http_websocket_detach(NYA_OsSocket socket);

/** Closes every connection and frees the table. What nya_system_http_deinit calls. */
NYA_INTERNAL void _nya_http_websocket_shutdown(void);

/* ── inside this file ── */

/** The open connection on `socket`, or null. Linear over a table of four. */
NYA_INTERNAL NYA_HttpWebSocket* _nya_http_websocket_find(NYA_OsSocket socket) __attr_no_discard;

/** Whether the comma separated header value `text` carries `token`, ignoring case. */
NYA_INTERNAL b8 _nya_http_websocket_has_token(NYA_ConstCString text, NYA_ConstCString token) __attr_no_discard;

/** Pushes whatever the protocol has queued into the socket. False when the socket has failed. */
NYA_INTERNAL b8 _nya_http_websocket_flush(NYA_HttpWebSocket* connection) __attr_no_discard;

/** Takes `count` bytes off the front of the receive buffer. */
NYA_INTERNAL void _nya_http_websocket_consume(NYA_HttpWebSocket* connection, u64 count);

// PUBLIC API IMPLEMENTATION

// ROUTES

NYA_Error nya_http_websocket_route_add(const NYA_HttpWebSocketRoute* route) {
    if (!nya_http_server_is_running()) return nya_error(NYA_ERROR_NOT_FOUND, "the HTTP server is not running");

    if (route == nullptr || route->path == nullptr || route->path[0] != '/') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket route needs an absolute path");
    }

    if (route->summary == nullptr || route->summary[0] == '\0') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' needs a summary, for the line that says a peer connected", route->path);
    }

    // The table is made on the first mount and not at init, so a server with no stream costs nothing.
    if (_NYA_HTTP_WEBSOCKET == nullptr) {
        NYA_Arena* arena = nya_arena_create(.name = "http_websocket");
        if (arena == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the websocket table");

        _NYA_HttpWebSocketState* state = nya_arena_alloc(arena, sizeof(_NYA_HttpWebSocketState));

        if (state == nullptr) {
            nya_arena_destroy(arena);
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the websocket table");
        }

        // memset rather than a compound literal: the table is a hundred kilobytes of buffers, and an unoptimized build would build all of it on the stack first.
        nya_memset(state, 0, sizeof(*state));
        state->allocator = arena;

        _NYA_HTTP_WEBSOCKET       = state;
        _NYA_HTTP_WEBSOCKET_COUNT = 0;
    }

    for (u32 index = 0; index < _NYA_HTTP_WEBSOCKET->route_count; index++) {
        if (strcmp(_NYA_HTTP_WEBSOCKET->routes[index]->path, route->path) == 0) {
            return nya_error(NYA_ERROR_ALREADY_EXISTS, "'%s' is already mounted", route->path);
        }
    }

    if (_NYA_HTTP_WEBSOCKET->route_count >= NYA_HTTP_MAX_WEBSOCKET_ROUTES) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "at most %d websocket routes can be mounted", NYA_HTTP_MAX_WEBSOCKET_ROUTES);
    }

    _NYA_HTTP_WEBSOCKET->routes[_NYA_HTTP_WEBSOCKET->route_count++] = route;

    return NYA_OK;
}

void nya_http_websocket_route_remove(const NYA_HttpWebSocketRoute* route) {
    if (_NYA_HTTP_WEBSOCKET == nullptr || route == nullptr) return;

    b8 mounted = false;

    for (u32 index = 0; index < _NYA_HTTP_WEBSOCKET->route_count; index++) {
        if (_NYA_HTTP_WEBSOCKET->routes[index] != route) continue;

        for (u32 shift = index; shift + 1 < _NYA_HTTP_WEBSOCKET->route_count; shift++) {
            _NYA_HTTP_WEBSOCKET->routes[shift] = _NYA_HTTP_WEBSOCKET->routes[shift + 1];
        }

        _NYA_HTTP_WEBSOCKET->route_count--;
        _NYA_HTTP_WEBSOCKET->routes[_NYA_HTTP_WEBSOCKET->route_count] = nullptr;

        mounted = true;
        break;
    }

    if (!mounted) return;

    // Whoever is still on it is told the stream is going away rather than left calling back into a route the program has already forgotten.
    for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS; index++) {
        NYA_HttpWebSocket* connection = &_NYA_HTTP_WEBSOCKET->connections[index];

        if (connection->socket.handle == 0 || connection->route != route) continue;

        (void)nya_websocket_protocol_close(&connection->protocol, NYA_WEBSOCKET_CLOSE_GOING_AWAY, "the stream was unmounted");
        (void)_nya_http_websocket_flush(connection);

        nya_websocket_protocol_fail(&connection->protocol, NYA_WEBSOCKET_CLOSE_GOING_AWAY, "the stream was unmounted");
    }
}

// MESSAGES

NYA_Error nya_http_websocket_send_text(NYA_HttpWebSocket* socket, NYA_ConstCString text) {
    nya_assert(socket != nullptr);

    if (text == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no text to send");

    return nya_websocket_protocol_send(&socket->protocol, NYA_WEBSOCKET_OPCODE_TEXT, (const u8*)text, strlen(text));
}

u32 nya_http_websocket_broadcast_text(NYA_ConstCString path, NYA_ConstCString text) {
    if (_NYA_HTTP_WEBSOCKET == nullptr || path == nullptr || text == nullptr) return 0;

    u32 sent = 0;

    for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS; index++) {
        NYA_HttpWebSocket* connection = &_NYA_HTTP_WEBSOCKET->connections[index];

        if (connection->socket.handle == 0 || strcmp(connection->route->path, path) != 0) continue;

        // Skipped rather than failing the whole push: one peer that stopped reading is the pending-write bound's to deal with, not the other peers' problem.
        if (nya_http_websocket_send_text(connection, text).ok) sent++;
    }

    return sent;
}

NYA_WebSocketProtocol* nya_http_websocket_protocol(NYA_HttpWebSocket* socket) {
    nya_assert(socket != nullptr);

    return socket->socket.handle != 0 ? &socket->protocol : nullptr;
}

// INTROSPECTION

u32 nya_http_websocket_count(void) {
    return _NYA_HTTP_WEBSOCKET_COUNT;
}

NYA_HttpWebSocket* nya_http_websocket_at(u32 index) {
    if (_NYA_HTTP_WEBSOCKET == nullptr) return nullptr;

    u32 seen = 0;

    for (u32 slot = 0; slot < NYA_HTTP_MAX_WEBSOCKETS; slot++) {
        NYA_HttpWebSocket* connection = &_NYA_HTTP_WEBSOCKET->connections[slot];

        if (connection->socket.handle == 0) continue;
        if (seen++ == index) return connection;
    }

    return nullptr;
}

NYA_ConstCString nya_http_websocket_path(const NYA_HttpWebSocket* socket) {
    nya_assert(socket != nullptr);

    return socket->route != nullptr ? socket->route->path : "";
}

NYA_ConstCString nya_http_websocket_address(const NYA_HttpWebSocket* socket) {
    nya_assert(socket != nullptr);

    return socket->address;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_websocket_is_upgrade(const NYA_HttpRequest* request) {
    nya_assert(request != nullptr);

    if (_NYA_HTTP_WEBSOCKET == nullptr || _NYA_HTTP_WEBSOCKET->route_count == 0) return false;

    NYA_ConstCString upgrade    = nya_http_request_header(request, "upgrade");
    NYA_ConstCString connection = nya_http_request_header(request, "connection");

    if (upgrade == nullptr || connection == nullptr) return false;

    // Both headers are lists, so both are read as lists: "Connection: keep-alive, Upgrade" is what every browser actually sends.
    return _nya_http_websocket_has_token(upgrade, "websocket") && _nya_http_websocket_has_token(connection, "upgrade");
}

NYA_HttpStatus _nya_http_websocket_upgrade(
    NYA_OsSocket           socket,
    NYA_ConstCString       address,
    const NYA_HttpRequest* request,
    u64                    trailing,
    OUT NYA_ConstCString*  out_detail
) {
    nya_assert(socket.handle != 0);
    nya_assert(address != nullptr);
    nya_assert(request != nullptr);
    nya_assert(out_detail != nullptr);
    nya_assert(_NYA_HTTP_WEBSOCKET != nullptr, "an upgrade was offered before any route was mounted");

    *out_detail = "";

    // RFC 6455 section 4.1: the handshake is a GET; anything else with these headers is a client that invented something, and this server doesn't guess what.
    if (request->method != NYA_HTTP_METHOD_GET) {
        *out_detail = "a websocket handshake is a GET";
        return NYA_HTTP_STATUS_BAD_REQUEST;
    }

    const NYA_HttpWebSocketRoute* route = nullptr;
    for (u32 index = 0; index < _NYA_HTTP_WEBSOCKET->route_count; index++) {
        if (strcmp(_NYA_HTTP_WEBSOCKET->routes[index]->path, request->path) == 0) route = _NYA_HTTP_WEBSOCKET->routes[index];
    }

    if (route == nullptr) {
        *out_detail = "no stream on that path";
        return NYA_HTTP_STATUS_NOT_FOUND;
    }

    // The same origin check every request that changes something goes through: an upgrade is a GET, so nothing about the method would have brought it here, and a socket a page on another site opened would read everything this program pushes for as long as it stayed open.
    if (_nya_http_request_is_cross_site(request)) {
        *out_detail = "a page on another site may not open a socket here";
        return NYA_HTTP_STATUS_FORBIDDEN;
    }

    NYA_ConstCString version = nya_http_request_header(request, "sec-websocket-version");

    // 426 is what the RFC names for this and this server's status set lacks it; the client gets the version it must use in the problem body instead of in a header it would have to parse.
    if (version == nullptr || strcmp(version, "13") != 0) {
        *out_detail = "this server speaks websocket version 13 and no other";
        return NYA_HTTP_STATUS_BAD_REQUEST;
    }

    NYA_ConstCString key = nya_http_request_header(request, "sec-websocket-key");

    char accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1] = { 0 };

    // The key is hashed rather than trusted, and a key that isn't a key is refused by the same function the client checks the answer with, so neither end has a second idea of what one is.
    if (key == nullptr || !nya_websocket_accept_from_key(key, accept).ok) {
        *out_detail = "the handshake carries no usable Sec-WebSocket-Key";
        return NYA_HTTP_STATUS_BAD_REQUEST;
    }

    // RFC 6455 section 4.1: a client sends nothing after the handshake until the 101 comes back, so bytes already in the buffer are either a client that doesn't follow the protocol or someone smuggling a second request through whatever is in front of this server — neither worth deciding between.
    if (trailing > 0) {
        *out_detail = "a client may not send anything before the handshake is answered";
        return NYA_HTTP_STATUS_BAD_REQUEST;
    }

    NYA_HttpWebSocket* slot = nullptr;
    u32                held = 0;

    for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS; index++) {
        NYA_HttpWebSocket* candidate = &_NYA_HTTP_WEBSOCKET->connections[index];

        if (candidate->socket.handle == 0) {
            if (slot == nullptr) slot = candidate;
            continue;
        }

        if (strcmp(candidate->address, address) == 0) held++;
    }

    if (slot == nullptr) {
        *out_detail = "this server holds as many sockets as it can";
        return NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;
    }

    if (held >= NYA_HTTP_MAX_WEBSOCKETS_PER_ADDRESS) {
        *out_detail = "this address holds as many sockets as it may";
        return NYA_HTTP_STATUS_SERVICE_UNAVAILABLE;
    }

    // A 101 and nothing else: no Content-Length, no body, none of the response headers the rest of the server adds — what follows the blank line is frames, and a stray byte would be read as the first frame.
    char answer[256] = { 0 };

    s32 written = snprintf(
        answer,
        sizeof(answer),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept
    );

    nya_assert(written > 0 && (u64)written < sizeof(answer), "the 101 is a fixed set of headers and a 28 character accept");

    u64 answered = 0;

    // The 101 is under two hundred bytes into a just-accepted socket that has written nothing, so a host that takes only part of it has a full buffer for another reason entirely: a connection worth refusing rather than queueing behind.
    if (nya_os_socket_send(socket, (const u8*)answer, (u64)written, &answered) != NYA_OS_SOCKET_OK || answered != (u64)written) {
        *out_detail = "the handshake could not be answered";
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    nya_memset(slot, 0, sizeof(*slot));

    NYA_Error opened = nya_websocket_protocol_open(
        &slot->protocol,
        (NYA_WebSocketProtocolConfig){
            .role             = NYA_WEBSOCKET_ROLE_SERVER,
            .message          = slot->message,
            .message_capacity = NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES,
            .send             = slot->send,
            .send_capacity    = sizeof(slot->send),
            .max_frame_bytes  = NYA_HTTP_WEBSOCKET_MAX_FRAME_BYTES,
        }
    );

    nya_assert(opened.ok, "the server's own buffers do not satisfy the protocol");
    nya_unused(opened);

    slot->socket      = socket;
    slot->route       = route;
    slot->heard_at_ns = nya_clock_get_monotonic_ns();
    (void)snprintf(slot->address, sizeof(slot->address), "%s", address);

    _NYA_HTTP_WEBSOCKET_COUNT++;

    nya_log_info("A websocket opened on %s (%s).", route->path, route->summary);

    if (route->on_open != nullptr) route->on_open(slot);

    return NYA_HTTP_STATUS_NONE;
}

b8 _nya_http_websocket_tick(NYA_OsSocket socket) {
    NYA_HttpWebSocket* connection = _nya_http_websocket_find(socket);

    if (connection == nullptr) return false;

    // Whatever has arrived, into whatever room is left: a buffer with no room is a peer that sent more than a header without finishing a frame, which the frame bound already refused.
    u64 room = sizeof(connection->receive) - connection->receive_size;

    if (room > 0) {
        u64 read = 0;

        NYA_OsSocketStatus status = nya_os_socket_receive(connection->socket, connection->receive + connection->receive_size, room, &read);

        if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) {
            nya_websocket_protocol_fail(&connection->protocol, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped");
            return false;
        }

        if (read > 0) {
            connection->receive_size += read;
            connection->heard_at_ns   = nya_clock_get_monotonic_ns();
            connection->pinged_at_ns  = 0;
        }
    }

    // One tick's worth of messages, so a peer that pipelines can't take the drain; the rest of what it sent stays in the buffer for the next one.
    for (u32 step = 0; step < NYA_HTTP_WEBSOCKET_MAX_MESSAGES_PER_TICK; step++) {
        NYA_WebSocketEvent event    = { 0 };
        u64                consumed = 0;

        b8 produced = nya_websocket_protocol_receive(&connection->protocol, connection->receive, connection->receive_size, &consumed, &event);

        _nya_http_websocket_consume(connection, consumed);

        if (!produced) break;

        if (event.kind == NYA_WEBSOCKET_EVENT_CLOSED) {
            // The goodbye the protocol queued goes out before the socket does; the close is reported from the detach, so it's reported exactly once however the connection ended.
            (void)_nya_http_websocket_flush(connection);
            return false;
        }

        if (event.kind != NYA_WEBSOCKET_EVENT_TEXT && event.kind != NYA_WEBSOCKET_EVENT_BINARY) continue;

        if (connection->route->on_message != nullptr) {
            connection->route->on_message(connection, event.kind == NYA_WEBSOCKET_EVENT_TEXT, event.data, event.size);
        }
    }

    if (!_nya_http_websocket_flush(connection)) return false;

    // A peer that stopped reading, the same bound an HTTP answer is held to: what the host wouldn't take stays in the protocol's queue, so this is where that stops growing.
    u64 pending = 0;
    (void)nya_websocket_protocol_pending(&connection->protocol, &pending);

    if (pending > NYA_HTTP_MAX_PENDING_WRITE_BYTES) return false;

    // The goodbye is on the wire and there is nothing left to wait for.
    if (nya_websocket_protocol_is_closed(&connection->protocol) && pending == 0) return false;

    u64 now_ns   = nya_clock_get_monotonic_ns();
    u64 quiet_ns = now_ns > connection->heard_at_ns ? now_ns - connection->heard_at_ns : 0;

    if (quiet_ns > (u64)NYA_HTTP_WEBSOCKET_IDLE_TIMEOUT_MS * 1000000ULL) return false;

    // One ping per interval, not one per tick: the pong resets `heard_at_ns`, and a peer that's gone never answers, so the timeout above is what it runs into.
    if (quiet_ns > (u64)NYA_HTTP_WEBSOCKET_PING_INTERVAL_MS * 1000000ULL && connection->pinged_at_ns < connection->heard_at_ns) {
        connection->pinged_at_ns = now_ns;

        (void)nya_websocket_protocol_send(&connection->protocol, NYA_WEBSOCKET_OPCODE_PING, nullptr, 0);

        return _nya_http_websocket_flush(connection);
    }

    return true;
}

void _nya_http_websocket_detach(NYA_OsSocket socket) {
    NYA_HttpWebSocket* connection = _nya_http_websocket_find(socket);

    if (connection == nullptr) return;

    NYA_WebSocketClose code = nya_websocket_protocol_close_code(&connection->protocol);

    // A socket that goes away without a close frame is 1006, the one code that means exactly "nobody said goodbye".
    if (!nya_websocket_protocol_is_closed(&connection->protocol)) code = NYA_WEBSOCKET_CLOSE_ABNORMAL;

    const NYA_HttpWebSocketRoute* route = connection->route;

    // Cleared before the callback: a handler asking what's connected must not be told about an already-gone socket, and one that tries to send into it gets nothing rather than bytes queued for a closed connection.
    connection->socket = NYA_OS_SOCKET_NONE;

    nya_assert(_NYA_HTTP_WEBSOCKET_COUNT > 0, "a websocket was detached that was never counted");
    _NYA_HTTP_WEBSOCKET_COUNT--;

    nya_log_info("A websocket on %s closed: %s.", route->path, nya_websocket_close_name(code));

    if (route->on_close != nullptr) route->on_close(connection, code);
}

void _nya_http_websocket_shutdown(void) {
    if (_NYA_HTTP_WEBSOCKET == nullptr) return;

    for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS; index++) {
        NYA_HttpWebSocket* connection = &_NYA_HTTP_WEBSOCKET->connections[index];

        if (connection->socket.handle == 0) continue;

        // The socket itself belongs to the HTTP connection, which is closing it in the same breath; this is only the report and the slot.
        _nya_http_websocket_detach(connection->socket);
    }

    NYA_Arena* arena    = _NYA_HTTP_WEBSOCKET->allocator;
    _NYA_HTTP_WEBSOCKET = nullptr;

    nya_arena_destroy(arena);
}

NYA_HttpWebSocket* _nya_http_websocket_find(NYA_OsSocket socket) {
    if (_NYA_HTTP_WEBSOCKET == nullptr || socket.handle == 0) return nullptr;

    for (u32 index = 0; index < NYA_HTTP_MAX_WEBSOCKETS; index++) {
        NYA_HttpWebSocket* connection = &_NYA_HTTP_WEBSOCKET->connections[index];

        if (connection->socket.handle == socket.handle) return connection;
    }

    return nullptr;
}

b8 _nya_http_websocket_has_token(NYA_ConstCString text, NYA_ConstCString token) {
    nya_assert(text != nullptr);
    nya_assert(token != nullptr);

    u64 length = strlen(token);

    for (NYA_ConstCString at = text; *at != '\0'; at++) {
        if (!_nya_http_equals_ignore_case(at, length, token)) continue;

        // A token and not a prefix: "websockets" is not "websocket", and neither is "x-websocket".
        b8 before = at == text || at[-1] == ',' || at[-1] == ' ' || at[-1] == '\t';
        b8 after  = at[length] == '\0' || at[length] == ',' || at[length] == ' ' || at[length] == '\t' || at[length] == ';';

        if (before && after) return true;
    }

    return false;
}

b8 _nya_http_websocket_flush(NYA_HttpWebSocket* connection) {
    nya_assert(connection != nullptr);

    u64       size   = 0;
    const u8* queued = nya_websocket_protocol_pending(&connection->protocol, &size);

    if (size == 0) return true;

    u64 wrote = 0;

    NYA_OsSocketStatus status = nya_os_socket_send(connection->socket, queued, size, &wrote);

    // A full host buffer is a peer reading slowly: what it wouldn't take stays queued, and the bound in the tick decides when that stops being fine.
    if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) {
        nya_websocket_protocol_fail(&connection->protocol, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped");
        return false;
    }

    if (wrote > 0) nya_websocket_protocol_flushed(&connection->protocol, wrote);

    return true;
}

void _nya_http_websocket_consume(NYA_HttpWebSocket* connection, u64 count) {
    nya_assert(connection != nullptr);
    nya_assert(count <= connection->receive_size);

    connection->receive_size -= count;

    if (connection->receive_size > 0) nya_memmove(connection->receive, connection->receive + count, connection->receive_size);
}
