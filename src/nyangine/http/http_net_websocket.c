#include "nyangine/nyangine.h"

// PRIVATE API DECLARATION

/** The tightest a framed net message is bounded to: a WebSocket message, less the one tag byte. */
#define _NYA_NET_WS_MAX_PAYLOAD (NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES - 1)

/**
 * One thing that happened on the wire, waiting for nya_net_transport_poll. `data` is owned in the
 * transport's allocator for a MESSAGE and freed once the poll that delivers it moves it into `delivered`.
 * */
typedef struct {
    NYA_NetTransportEventKind kind;
    NYA_NetPeerId             peer;
    NYA_NetChannel            channel;
    NYA_NetDisconnect         reason;

    u8* data;
    u64 size;
} _NYA_NetWsEvent;

nya_derive_array(_NYA_NetWsEvent);

/** One connected, or connecting, browser. Its slot in the table is its NYA_NetPeerId index. */
typedef struct {
    b8 used;

    /** Whether the join was accepted. A slot is used but not connected between on_open and a good join. */
    b8 connected;

    /** Bumped per reuse so a stale id never resolves to a new peer. Never zero. */
    u32 generation;

    /** The HTTP server's connection, this transport's only handle on the socket. Null once it is gone. */
    NYA_HttpWebSocket* socket;

    b8 has_key;
    u8 key[NYA_NET_KEY_SIZE];

    NYA_NetPeerStats stats;
} _NYA_NetWsPeer;

struct _NYA_NetWsState {
    NYA_Arena* allocator;

    u32 version;

    /** Mounted on the HTTP server; the callbacks below reach back through the singleton. Owns the path. */
    NYA_HttpWebSocketRoute route;
    b8                     listening;

    _NYA_NetWsPeer peers[NYA_NET_MAX_PEERS];
    u32            next_generation;

    u8  allowed[NYA_NET_WS_MAX_ALLOWED][NYA_NET_KEY_SIZE];
    b8  allowed_used[NYA_NET_WS_MAX_ALLOWED];
    u32 allowed_count;

    NYA_Arrayᐸ_NYA_NetWsEventᐳ* events;

    /** The last MESSAGE's bytes, handed out by poll and freed by the next MESSAGE poll. */
    NYA_Arena* delivered;

    /** Where a send frames a message whole, the tag byte in front of the payload. */
    u8* scratch;
};

typedef struct _NYA_NetWsState _NYA_NetWsState;

/**
 * The one transport in a process. The HTTP server is a singleton and a route's callbacks carry no user
 * pointer, so the callbacks find their transport here rather than through the socket.
 * */
NYA_INTERNAL _NYA_NetWsState* _NYA_NET_WS = nullptr;

NYA_INTERNAL NYA_Error _nya_net_ws_listen(NYA_NetTransport* transport, u16 port);
NYA_INTERNAL u16       _nya_net_ws_port(NYA_NetTransport* transport);
NYA_INTERNAL NYA_Error _nya_net_ws_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_INTERNAL b8        _nya_net_ws_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_INTERNAL void      _nya_net_ws_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_INTERNAL NYA_NetPeerStats _nya_net_ws_stats(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL NYA_ConstCString _nya_net_ws_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL const u8*        _nya_net_ws_peer_key(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void             _nya_net_ws_destroy(NYA_NetTransport* transport);

NYA_INTERNAL void _nya_net_ws_on_open(NYA_HttpWebSocket* socket);
NYA_INTERNAL void _nya_net_ws_on_message(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size);
NYA_INTERNAL void _nya_net_ws_on_close(NYA_HttpWebSocket* socket, NYA_WebSocketClose code);

NYA_INTERNAL const NYA_NetTransportVTable _NYA_NET_WS_VTABLE = {
    .name = "websocket",
    .kind = NYA_NET_TRANSPORT_WEBSOCKET,

    // Browsers dial in, so this transport listens and never connects out, like a loopback pair; condition and public_key are null — TCP is the wire and wss the encryption, neither this transport's to shape.
    .listen  = &_nya_net_ws_listen,
    .connect = nullptr,

    .port         = &_nya_net_ws_port,
    .send         = &_nya_net_ws_send,
    .poll         = &_nya_net_ws_poll,
    .disconnect   = &_nya_net_ws_disconnect,
    .stats        = &_nya_net_ws_stats,
    .peer_address = &_nya_net_ws_peer_address,
    .peer_key     = &_nya_net_ws_peer_key,
    .destroy      = &_nya_net_ws_destroy,
};

// PRIVATE HELPERS

NYA_INTERNAL u32 _nya_net_ws_read_u32(const u8* at) {
    return (u32)at[0] | ((u32)at[1] << 8) | ((u32)at[2] << 16) | ((u32)at[3] << 24);
}

NYA_INTERNAL void _nya_net_ws_write_u32(OUT u8* at, u32 value) {
    at[0] = (u8)(value & 0xFFU);
    at[1] = (u8)((value >> 8) & 0xFFU);
    at[2] = (u8)((value >> 16) & 0xFFU);
    at[3] = (u8)((value >> 24) & 0xFFU);
}

NYA_INTERNAL b8 _nya_net_ws_allowed_contains(_NYA_NetWsState* state, const u8* key) {
    for (u32 i = 0; i < NYA_NET_WS_MAX_ALLOWED; i++) {
        if (!state->allowed_used[i]) continue;
        if (nya_memcmp(state->allowed[i], key, NYA_NET_KEY_SIZE) == 0) return true;
    }

    return false;
}

/** A NetPeerId for a slot. Generation is never zero, so an unset id never matches an occupied slot. */
NYA_INTERNAL NYA_NetPeerId _nya_net_ws_id(u32 index, u32 generation) {
    return (NYA_NetPeerId){ .index = index, .generation = generation };
}

/** The peer an id names, only while it still names a used slot of that exact generation. */
NYA_INTERNAL _NYA_NetWsPeer* _nya_net_ws_peer_of(_NYA_NetWsState* state, NYA_NetPeerId id) {
    if (id.index >= NYA_NET_MAX_PEERS) return nullptr;

    _NYA_NetWsPeer* peer = &state->peers[id.index];
    if (!peer->used || peer->generation != id.generation) return nullptr;

    return peer;
}

NYA_INTERNAL _NYA_NetWsPeer* _nya_net_ws_peer_by_socket(_NYA_NetWsState* state, const NYA_HttpWebSocket* socket, OUT u32* out_index) {
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (state->peers[i].used && state->peers[i].socket == socket) {
            if (out_index != nullptr) *out_index = i;
            return &state->peers[i];
        }
    }

    return nullptr;
}

/** Claims a free slot for a just-opened socket, in the connecting state. Null when the table is full. */
NYA_INTERNAL _NYA_NetWsPeer* _nya_net_ws_peer_alloc(_NYA_NetWsState* state, NYA_HttpWebSocket* socket, OUT u32* out_index) {
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (state->peers[i].used) continue;

        u32 generation = ++state->next_generation;
        if (generation == 0) generation = ++state->next_generation; // never zero, which means "no peer"

        state->peers[i] = (_NYA_NetWsPeer){
            .used       = true,
            .connected  = false,
            .generation = generation,
            .socket     = socket,
        };

        if (out_index != nullptr) *out_index = i;
        return &state->peers[i];
    }

    return nullptr;
}

NYA_INTERNAL void _nya_net_ws_peer_free(_NYA_NetWsPeer* peer) {
    // Wiped rather than only marked free, so a stale key or socket pointer never outlives the slot.
    *peer = (_NYA_NetWsPeer){ 0 };
}

NYA_INTERNAL void _nya_net_ws_event_push(_NYA_NetWsState* state, _NYA_NetWsEvent event) {
    nya_array_push_back(state->events, event);
}

/** Copies a received net message into the allocator and queues it for poll. */
NYA_INTERNAL void _nya_net_ws_event_push_message(_NYA_NetWsState* state, NYA_NetPeerId peer, const u8* data, u64 size) {
    u8* copy = nya_arena_alloc(state->allocator, size);
    nya_memcpy(copy, data, size);

    _nya_net_ws_event_push(
        state,
        (_NYA_NetWsEvent){
            .kind    = NYA_NET_TRANSPORT_EVENT_MESSAGE,
            .peer    = peer,
            .channel = NYA_NET_CHANNEL_RELIABLE,
            .data    = copy,
            .size    = size,
        }
    );
}

/**
 * Ends a peer this transport had accepted: reports it gone with `reason`, closes the wire with `code`
 * and `text` when it is still open, and frees the slot. For a peer refused at the join, which was never
 * an established peer, pass notify=false so nothing above ever hears of it.
 * */
NYA_INTERNAL void _nya_net_ws_drop(_NYA_NetWsState* state, u32 index, NYA_NetDisconnect reason, NYA_WebSocketClose code, NYA_ConstCString text, b8 notify) {
    _NYA_NetWsPeer* peer = &state->peers[index];
    if (!peer->used) return;

    if (peer->socket != nullptr) {
        NYA_WebSocketProtocol* protocol = nya_http_websocket_protocol(peer->socket);
        if (protocol != nullptr) (void)nya_websocket_protocol_close(protocol, code, text);
    }

    if (notify && peer->connected) {
        _nya_net_ws_event_push(state, (_NYA_NetWsEvent){ .kind = NYA_NET_TRANSPORT_EVENT_DISCONNECTED, .peer = _nya_net_ws_id(index, peer->generation), .reason = reason });
    }

    _nya_net_ws_peer_free(peer);
}

// PUBLIC API IMPLEMENTATION

NYA_Error nya_net_transport_ws_create(NYA_Arena* arena, NYA_NetWsOptions options, OUT NYA_NetTransport** out_transport) {
    nya_assert(arena != nullptr);
    nya_assert(out_transport != nullptr);

    *out_transport = nullptr;

    // One per process: the callbacks find their state through the singleton, and the HTTP server they mount on is itself one per process.
    if (_NYA_NET_WS != nullptr) return nya_error(NYA_ERROR_ALREADY_EXISTS, "a websocket net transport already exists");

    NYA_NetTransport* transport = nya_arena_alloc(arena, sizeof(NYA_NetTransport));
    _NYA_NetWsState*  state     = nya_arena_alloc(arena, sizeof(_NYA_NetWsState));

    *state = (_NYA_NetWsState){
        .allocator       = arena,
        .version         = options.version,
        .next_generation = 0,
        .events          = nya_array_create(arena, _NYA_NetWsEvent),
        .delivered       = nya_arena_create(.name = "net_ws_delivered"),
        .scratch         = nya_arena_alloc(arena, NYA_HTTP_WEBSOCKET_MAX_MESSAGE_BYTES),
        .route =
            {
                .path       = options.path != nullptr ? options.path : NYA_NET_WS_DEFAULT_PATH,
                .summary    = "carries net messages for a browser peer",
                .on_open    = &_nya_net_ws_on_open,
                .on_message = &_nya_net_ws_on_message,
                .on_close   = &_nya_net_ws_on_close,
            },
    };

    *transport = (NYA_NetTransport){
        .vtable    = &_NYA_NET_WS_VTABLE,
        .allocator = arena,
        .state     = state,
    };

    _NYA_NET_WS   = state;
    *out_transport = transport;

    return NYA_OK;
}

NYA_Error nya_net_transport_ws_allow(NYA_NetTransport* transport, const u8 key[NYA_NET_KEY_SIZE]) {
    nya_assert(transport != nullptr && transport->vtable == &_NYA_NET_WS_VTABLE, "not a websocket net transport");
    nya_assert(key != nullptr);

    _NYA_NetWsState* state = transport->state;

    if (!nya_net_key_is_set(key)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an all-zero player key");

    // Idempotent: the same key twice is one entry, not two.
    if (_nya_net_ws_allowed_contains(state, key)) return NYA_OK;

    for (u32 i = 0; i < NYA_NET_WS_MAX_ALLOWED; i++) {
        if (state->allowed_used[i]) continue;

        nya_memcpy(state->allowed[i], key, NYA_NET_KEY_SIZE);
        state->allowed_used[i] = true;
        state->allowed_count++;

        return NYA_OK;
    }

    return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the allowlist holds at most %d keys", NYA_NET_WS_MAX_ALLOWED);
}

void nya_net_transport_ws_disallow(NYA_NetTransport* transport, const u8 key[NYA_NET_KEY_SIZE]) {
    nya_assert(transport != nullptr && transport->vtable == &_NYA_NET_WS_VTABLE, "not a websocket net transport");
    nya_assert(key != nullptr);

    _NYA_NetWsState* state = transport->state;

    for (u32 i = 0; i < NYA_NET_WS_MAX_ALLOWED; i++) {
        if (!state->allowed_used[i]) continue;
        if (nya_memcmp(state->allowed[i], key, NYA_NET_KEY_SIZE) != 0) continue;

        nya_memset(state->allowed[i], 0, NYA_NET_KEY_SIZE);
        state->allowed_used[i] = false;
        state->allowed_count--;

        return;
    }
}

b8 nya_net_transport_ws_is_allowed(NYA_NetTransport* transport, const u8 key[NYA_NET_KEY_SIZE]) {
    nya_assert(transport != nullptr && transport->vtable == &_NYA_NET_WS_VTABLE, "not a websocket net transport");
    nya_assert(key != nullptr);

    return _nya_net_ws_allowed_contains(transport->state, key);
}

void nya_net_ws_join_encode(u32 version, const u8 key[NYA_NET_KEY_SIZE], OUT u8 out_frame[NYA_NET_WS_JOIN_SIZE]) {
    nya_assert(key != nullptr);
    nya_assert(out_frame != nullptr);

    out_frame[0] = (u8)NYA_NET_WS_TAG_JOIN;
    _nya_net_ws_write_u32(out_frame + 1, version);
    nya_memcpy(out_frame + 5, key, NYA_NET_KEY_SIZE);
}

// VTABLE IMPLEMENTATION

NYA_Error _nya_net_ws_listen(NYA_NetTransport* transport, u16 port) {
    _NYA_NetWsState* state = transport->state;

    nya_unused(port); // the HTTP server owns the port; this transport only mounts a route on it.

    if (!nya_http_server_is_running()) {
        return nya_error(NYA_ERROR_NOT_OK, "the HTTP server must be running before a websocket net transport listens");
    }

    if (state->listening) return NYA_OK;

    NYA_Error mounted = nya_http_websocket_route_add(&state->route);
    if (!mounted.ok) return mounted;

    state->listening = true;

    return NYA_OK;
}

u16 _nya_net_ws_port(NYA_NetTransport* transport) {
    nya_unused(transport);

    // The HTTP server's port, since that is the one a browser reaches this transport on.
    return nya_http_server_port();
}

NYA_Error _nya_net_ws_send(NYA_NetTransport* transport, NYA_NetPeerId peer_id, NYA_NetChannel channel, const u8* data, u64 size) {
    _NYA_NetWsState* state = transport->state;

    nya_unused(channel); // one TCP stream, reliable and ordered; both channels ride it.

    _NYA_NetWsPeer* peer = _nya_net_ws_peer_of(state, peer_id);
    if (peer == nullptr || !peer->connected) return nya_error(NYA_ERROR_NOT_FOUND, "no such websocket peer");

    if (size > _NYA_NET_WS_MAX_PAYLOAD) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message of %llu bytes is past the websocket bound", (unsigned long long)size);

    if (peer->socket == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the websocket peer is gone");

    NYA_WebSocketProtocol* protocol = nya_http_websocket_protocol(peer->socket);
    if (protocol == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the websocket peer is gone");

    // Tagged and framed whole: the tag says this is a net message and not the join or the acceptance.
    state->scratch[0] = (u8)NYA_NET_WS_TAG_DATA;
    nya_memcpy(state->scratch + 1, data, size);

    NYA_Error sent = nya_websocket_protocol_send(protocol, NYA_WEBSOCKET_OPCODE_BINARY, state->scratch, size + 1);
    if (!sent.ok) return sent;

    peer->stats.bytes_sent += size;
    peer->stats.packets_sent++;

    return NYA_OK;
}

b8 _nya_net_ws_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event) {
    _NYA_NetWsState* state = transport->state;

    if (state->events->length == 0) return false;

    _NYA_NetWsEvent event = state->events->items[0];
    nya_array_remove(state->events, 0);

    if (event.kind == NYA_NET_TRANSPORT_EVENT_MESSAGE) {
        // The previous poll's bytes are done with; this one's move out of the allocator, where the copy made on receive would otherwise stay for the connection's life.
        nya_arena_free_all(state->delivered);

        u8* delivered = nya_arena_alloc(state->delivered, event.size);
        nya_memcpy(delivered, event.data, event.size);
        nya_arena_free(state->allocator, event.data, event.size);

        event.data = delivered;
    }

    *out_event = (NYA_NetTransportEvent){
        .kind    = event.kind,
        .peer    = event.peer,
        .data    = event.data,
        .size    = event.size,
        .channel = event.channel,
        .reason  = event.reason,
    };

    return true;
}

void _nya_net_ws_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer_id, NYA_NetDisconnect reason) {
    nya_assert((u32)reason < NYA_NET_DISCONNECT_COUNT);

    _NYA_NetWsState* state = transport->state;

    _NYA_NetWsPeer* peer = _nya_net_ws_peer_of(state, peer_id);
    if (peer == nullptr) return;

    // The caller asked to drop this peer, so it isn't told again with a DISCONNECTED event; the wire is closed with a code and reason the peer can read, and the slot is freed.
    NYA_ConstCString text = reason == NYA_NET_DISCONNECT_KICKED   ? "kicked"
                          : reason == NYA_NET_DISCONNECT_CHEATING ? "too many rule violations"
                                                                  : "disconnected";

    NYA_WebSocketClose code = reason == NYA_NET_DISCONNECT_SERVER_CLOSED ? NYA_WEBSOCKET_CLOSE_GOING_AWAY : NYA_WEBSOCKET_CLOSE_NORMAL;

    _nya_net_ws_drop(state, peer_id.index, reason, code, text, /*notify*/ false);
}

NYA_NetPeerStats _nya_net_ws_stats(NYA_NetTransport* transport, NYA_NetPeerId peer_id) {
    _NYA_NetWsState* state = transport->state;

    _NYA_NetWsPeer* peer = _nya_net_ws_peer_of(state, peer_id);
    if (peer == nullptr) return (NYA_NetPeerStats){ 0 };

    return peer->stats;
}

NYA_ConstCString _nya_net_ws_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer_id) {
    _NYA_NetWsState* state = transport->state;

    _NYA_NetWsPeer* peer = _nya_net_ws_peer_of(state, peer_id);
    if (peer == nullptr || peer->socket == nullptr) return "(gone)";

    return nya_http_websocket_address(peer->socket);
}

const u8* _nya_net_ws_peer_key(NYA_NetTransport* transport, NYA_NetPeerId peer_id) {
    _NYA_NetWsState* state = transport->state;

    _NYA_NetWsPeer* peer = _nya_net_ws_peer_of(state, peer_id);
    if (peer == nullptr || !peer->has_key) return nullptr;

    return peer->key;
}

void _nya_net_ws_destroy(NYA_NetTransport* transport) {
    _NYA_NetWsState* state = transport->state;
    if (state == nullptr) return;

    // Unmounting closes whatever is still open on the route with 1001, so no callback fires after this.
    if (state->listening) {
        nya_http_websocket_route_remove(&state->route);
        state->listening = false;
    }

    nya_arena_destroy(state->delivered);

    _NYA_NET_WS      = nullptr;
    transport->state = nullptr;

    // The state, its peers, the event array and the scratch all came from the caller's arena.
}

// THE ROUTE'S CALLBACKS

void _nya_net_ws_on_open(NYA_HttpWebSocket* socket) {
    _NYA_NetWsState* state = _NYA_NET_WS;
    if (state == nullptr) return;

    u32             index = 0;
    _NYA_NetWsPeer* peer  = _nya_net_ws_peer_alloc(state, socket, &index);

    // Full at the transport's own ceiling, below the HTTP server's: refuse before the join, so this is a close the browser reads rather than a socket that opens and goes quiet.
    if (peer == nullptr) {
        NYA_WebSocketProtocol* protocol = nya_http_websocket_protocol(socket);
        if (protocol != nullptr) (void)nya_websocket_protocol_close(protocol, NYA_WEBSOCKET_CLOSE_POLICY, "server full");
        return;
    }

    // No CONNECTED event yet: the peer has a slot but hasn't completed the join, so nothing above hears of it until its version and key check out.
}

void _nya_net_ws_on_message(NYA_HttpWebSocket* socket, b8 is_text, const u8* data, u64 size) {
    _NYA_NetWsState* state = _NYA_NET_WS;
    if (state == nullptr) return;

    u32             index = 0;
    _NYA_NetWsPeer* peer  = _nya_net_ws_peer_by_socket(state, socket, &index);
    if (peer == nullptr) return; // a socket this transport already dropped; the HTTP server reaps it.

    // ── the join, before a peer is connected ──
    if (!peer->connected) {
        // A join is one binary frame, tagged, exactly sized; anything else is a peer not speaking this protocol, refused before it's ever established, so notify is false.
        if (is_text || size != NYA_NET_WS_JOIN_SIZE || data[0] != (u8)NYA_NET_WS_TAG_JOIN) {
            _nya_net_ws_drop(state, index, NYA_NET_DISCONNECT_PROTOCOL, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "expected a binary join frame", false);
            return;
        }

        u32 version = _nya_net_ws_read_u32(data + 1);
        if (version != state->version) {
            // The one refusal that must be unmistakable: the reason names the mismatch so a client shows "update your game" rather than "connection lost".
            _nya_net_ws_drop(state, index, NYA_NET_DISCONNECT_VERSION, NYA_WEBSOCKET_CLOSE_POLICY, "protocol version mismatch", false);
            return;
        }

        const u8* key = data + 5;
        if (!_nya_net_ws_allowed_contains(state, key)) {
            _nya_net_ws_drop(state, index, NYA_NET_DISCONNECT_IDENTITY, NYA_WEBSOCKET_CLOSE_POLICY, "player key not on allowlist", false);
            return;
        }

        // Accepted: the peer is connected, its key remembered for peer_key, and the acceptance goes back so the client knows it's in rather than inferring it from silence.
        peer->connected = true;
        peer->has_key   = true;
        nya_memcpy(peer->key, key, NYA_NET_KEY_SIZE);

        NYA_WebSocketProtocol* protocol = nya_http_websocket_protocol(socket);
        if (protocol != nullptr) {
            u8 accept[NYA_NET_WS_ACCEPT_SIZE];
            accept[0] = (u8)NYA_NET_WS_TAG_ACCEPT;
            _nya_net_ws_write_u32(accept + 1, state->version);

            (void)nya_websocket_protocol_send(protocol, NYA_WEBSOCKET_OPCODE_BINARY, accept, sizeof(accept));
        }

        _nya_net_ws_event_push(state, (_NYA_NetWsEvent){ .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED, .peer = _nya_net_ws_id(index, peer->generation) });

        return;
    }

    // ── a net message, once connected ──

    // Every carried message is a tagged binary frame with a payload behind the tag; a text frame, a tag-only frame, or one tagged as something else is a peer that stopped speaking the protocol — this one *was* established, so it's reported gone.
    if (is_text || size <= 1 || data[0] != (u8)NYA_NET_WS_TAG_DATA) {
        _nya_net_ws_drop(state, index, NYA_NET_DISCONNECT_PROTOCOL, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a malformed net frame", true);
        return;
    }

    peer->stats.bytes_received += size - 1;
    peer->stats.packets_received++;

    _nya_net_ws_event_push_message(state, _nya_net_ws_id(index, peer->generation), data + 1, size - 1);
}

void _nya_net_ws_on_close(NYA_HttpWebSocket* socket, NYA_WebSocketClose code) {
    _NYA_NetWsState* state = _NYA_NET_WS;
    if (state == nullptr) return;

    u32             index = 0;
    _NYA_NetWsPeer* peer  = _nya_net_ws_peer_by_socket(state, socket, &index);
    if (peer == nullptr) return; // already dropped by a refusal or an explicit disconnect.

    // The socket is already gone when this fires, so the wire can't be closed again; only the report and the slot are left. A peer that never connected leaves without an event, like a refused join.
    NYA_NetDisconnect reason = code == NYA_WEBSOCKET_CLOSE_NORMAL     ? NYA_NET_DISCONNECT_REQUESTED
                             : code == NYA_WEBSOCKET_CLOSE_GOING_AWAY ? NYA_NET_DISCONNECT_SERVER_CLOSED
                             : code == NYA_WEBSOCKET_CLOSE_ABNORMAL   ? NYA_NET_DISCONNECT_TIMEOUT
                                                                      : NYA_NET_DISCONNECT_PROTOCOL;

    if (peer->connected) {
        _nya_net_ws_event_push(state, (_NYA_NetWsEvent){ .kind = NYA_NET_TRANSPORT_EVENT_DISCONNECTED, .peer = _nya_net_ws_id(index, peer->generation), .reason = reason });
    }

    _nya_net_ws_peer_free(peer);
}
