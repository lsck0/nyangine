#include "nyangine-core/nyangine.h"

// STEAM TRANSPORT

// The NYA_NetTransport over SteamNetworkingMessages: no encryption or reliability of its own (Valve's relay provides both), address is a Steam id, no fragmentation, conditions not simulated.

// PRIVATE API DECLARATION

/** The one Steam channel this transport uses: reliability is a per-message flag in this SDK, not a channel. */
#define _NYA_NET_STEAM_CHANNEL 0

/** One message taken off Steam and not yet polled. */
typedef struct {
    u32            peer_index;
    u32            offset;
    u32            size;
    NYA_NetChannel channel;
} _NYA_NetSteamInboxEntry;

/** A connected peer. The slot index plus a generation is what NYA_NetPeerId names. */
typedef struct {
    NYA_SteamId user;
    u32         generation;
    b8          connected;

    NYA_NetPeerStats stats;

    /** Rendered on demand by peer_address, which has to return a string that outlives the call. */
    char address[24];
} _NYA_NetSteamPeer;

/** A transport level event waiting to be polled: a peer arriving or leaving. */
typedef struct {
    NYA_NetTransportEventKind kind;
    NYA_NetPeerId             peer;
    NYA_NetDisconnect         reason;
} _NYA_NetSteamEvent;

typedef struct {
    NYA_Arena* allocator;

    /** Whether sessions other peers open are accepted. False on a client, which only talks to its server. */
    b8 accepting;

    _NYA_NetSteamPeer peers[NYA_NET_MAX_PEERS];

    /** Connects and disconnects waiting to be reported, oldest first. */
    _NYA_NetSteamEvent events[NYA_NET_MAX_PEERS * 2];
    u32                event_count;

    /** One receive round's messages, refilled only once every entry has been polled, so a MESSAGE event's bytes stay valid until the next poll. */
    _NYA_NetSteamInboxEntry inbox[NYA_STEAM_MAX_RECEIVE];
    u32                     inbox_count;
    u32                     inbox_read;
    u32                     inbox_used;
    u8                      inbox_bytes[NYA_STEAM_MAX_RECEIVE * NYA_STEAM_MAX_MESSAGE];
} _NYA_NetSteamEndpoint;

NYA_INTERNAL NYA_Error        _nya_net_steam_listen(NYA_NetTransport* transport, u16 port);
NYA_INTERNAL NYA_Error        _nya_net_steam_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port);
NYA_INTERNAL NYA_Error        _nya_net_steam_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_INTERNAL b8               _nya_net_steam_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_INTERNAL void             _nya_net_steam_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_INTERNAL NYA_NetPeerStats _nya_net_steam_stats(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL NYA_ConstCString _nya_net_steam_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void             _nya_net_steam_destroy(NYA_NetTransport* transport);

/** Finds the slot holding `user`, or NYA_NET_MAX_PEERS. */
NYA_INTERNAL u32 _nya_net_steam_find(const _NYA_NetSteamEndpoint* endpoint, NYA_SteamId user) __attr_no_discard;

/** Takes a free slot for `user` and reports it as connected, or NYA_NET_MAX_PEERS when the table is full. */
NYA_INTERNAL u32 _nya_net_steam_add(_NYA_NetSteamEndpoint* endpoint, NYA_SteamId user) __attr_no_discard;

/** Frees a slot and reports the peer as gone. */
NYA_INTERNAL void _nya_net_steam_remove(_NYA_NetSteamEndpoint* endpoint, u32 index, NYA_NetDisconnect reason);

/** Turns a slot index into the id the layers above carry. */
NYA_INTERNAL NYA_NetPeerId _nya_net_steam_peer_id(const _NYA_NetSteamEndpoint* endpoint, u32 index) __attr_no_discard;

/** Resolves a peer id back to a slot, or NYA_NET_MAX_PEERS when it is stale or was never ours. */
NYA_INTERNAL u32 _nya_net_steam_resolve(const _NYA_NetSteamEndpoint* endpoint, NYA_NetPeerId peer) __attr_no_discard;

/** Queues a transport event, dropping it with a warning if the queue is somehow full. */
NYA_INTERNAL void _nya_net_steam_event_push(_NYA_NetSteamEndpoint* endpoint, _NYA_NetSteamEvent event);

/** Answers every session request and failure Steam reported since the last poll. */
NYA_INTERNAL void _nya_net_steam_drain_sessions(_NYA_NetSteamEndpoint* endpoint);

/** Takes one round of messages off Steam into the inbox. Only called with the inbox empty. */
NYA_INTERNAL void _nya_net_steam_receive(_NYA_NetSteamEndpoint* endpoint);

/** Parses an address into a Steam id: decimal digits and nothing else. Zero means it was not one. */
NYA_INTERNAL NYA_SteamId _nya_net_steam_id_from_address(NYA_ConstCString address) __attr_no_discard;

NYA_INTERNAL const NYA_NetTransportVTable _NYA_NET_STEAM_VTABLE = {
    .name = "steam",
    .kind = NYA_NET_TRANSPORT_STEAM,

    .listen       = &_nya_net_steam_listen,
    .connect      = &_nya_net_steam_connect,
    .send         = &_nya_net_steam_send,
    .poll         = &_nya_net_steam_poll,
    .disconnect   = &_nya_net_steam_disconnect,
    .stats        = &_nya_net_steam_stats,
    .peer_address = &_nya_net_steam_peer_address,
    .destroy      = &_nya_net_steam_destroy,

    // no condition, no public_key and no peer_key: see the note at the top of this file.
    .condition  = nullptr,
    .public_key = nullptr,
    .peer_key   = nullptr,
};

// PUBLIC API IMPLEMENTATION

NYA_Error nya_net_transport_steam_create(NYA_Arena* arena, OUT NYA_NetTransport** out_transport) {
    nya_assert(arena != nullptr);
    nya_assert(out_transport != nullptr);

    *out_transport = nullptr;

    // reported, not asserted: a player who closed Steam and pressed "host" gets a message, as does a build with no Steamworks.
    if (!nya_steam_is_connected()) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the Steam transport needs a running Steam client; use the UDP transport");
    }

    NYA_NetTransport*      transport = nya_arena_alloc(arena, sizeof(NYA_NetTransport));
    _NYA_NetSteamEndpoint* endpoint  = nya_arena_alloc(arena, sizeof(_NYA_NetSteamEndpoint));

    *endpoint = (_NYA_NetSteamEndpoint){ .allocator = arena };

    *transport = (NYA_NetTransport){
        .vtable    = &_NYA_NET_STEAM_VTABLE,
        .allocator = arena,
        .state     = endpoint,
    };

    *out_transport = transport;

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

NYA_Error _nya_net_steam_listen(NYA_NetTransport* transport, u16 port) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    // no socket to bind (peers reach this by account through Valve's relay); the port is accepted and ignored.
    if (port != 0) nya_log_debug("The Steam transport has no port to bind; ignoring %u.", port);

    endpoint->accepting = true;

    nya_log_info("Accepting Steam sessions as %llu.", (unsigned long long)nya_steam_user_id().value);

    return NYA_OK;
}

NYA_Error _nya_net_steam_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    nya_unused(port);

    NYA_SteamId host = _nya_net_steam_id_from_address(address);

    if (!nya_steam_id_is_set(host)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a Steam id; the Steam transport connects to an account, not a host name",
                         address == nullptr ? "" : address);
    }

    if (nya_steam_id_equals(host, nya_steam_user_id())) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "connecting to this account's own Steam id");

    u32 index = _nya_net_steam_add(endpoint, host);
    if (index == NYA_NET_MAX_PEERS) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no free peer slot");

    // pre-accepted here too (the SDK is symmetric), else the host's first reply would raise a session request to answer.
    NYA_Error accepted = nya_steam_p2p_accept(host);
    if (!accepted.ok) nya_log_debug("Steam did not pre-accept the session with %llu; the first message will open it.", (unsigned long long)host.value);

    return NYA_OK;
}

NYA_Error _nya_net_steam_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    u32 index = _nya_net_steam_resolve(endpoint, peer);
    if (index == NYA_NET_MAX_PEERS) return nya_error(NYA_ERROR_NOT_FOUND, "no such Steam peer");

    NYA_Error sent = nya_steam_p2p_send(endpoint->peers[index].user, data, size, channel == NYA_NET_CHANNEL_RELIABLE, _NYA_NET_STEAM_CHANNEL);

    if (!sent.ok) return sent;

    endpoint->peers[index].stats.bytes_sent += size;
    endpoint->peers[index].stats.packets_sent++;

    return NYA_OK;
}

b8 _nya_net_steam_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    // sessions before messages: a peer's CONNECTED must precede its first message, or the layer above sees an unknown peer.
    if (endpoint->event_count == 0) _nya_net_steam_drain_sessions(endpoint);

    if (endpoint->event_count > 0) {
        _NYA_NetSteamEvent event = endpoint->events[0];

        endpoint->event_count--;
        nya_memmove(&endpoint->events[0], &endpoint->events[1], endpoint->event_count * sizeof(_NYA_NetSteamEvent));

        *out_event = (NYA_NetTransportEvent){ .kind = event.kind, .peer = event.peer, .reason = event.reason };

        return true;
    }

    // only with the inbox drained: the previous poll's bytes live in the same buffer a receive overwrites.
    if (endpoint->inbox_read == endpoint->inbox_count) _nya_net_steam_receive(endpoint);

    if (endpoint->inbox_read == endpoint->inbox_count) return false;

    _NYA_NetSteamInboxEntry entry = endpoint->inbox[endpoint->inbox_read];
    endpoint->inbox_read++;

    nya_assert(entry.peer_index < NYA_NET_MAX_PEERS);
    nya_assert(entry.offset + entry.size <= sizeof(endpoint->inbox_bytes));

    endpoint->peers[entry.peer_index].stats.bytes_received += entry.size;
    endpoint->peers[entry.peer_index].stats.packets_received++;

    *out_event = (NYA_NetTransportEvent){
        .kind    = NYA_NET_TRANSPORT_EVENT_MESSAGE,
        .peer    = _nya_net_steam_peer_id(endpoint, entry.peer_index),
        .data    = endpoint->inbox_bytes + entry.offset,
        .size    = entry.size,
        .channel = entry.channel,
    };

    return true;
}

void _nya_net_steam_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    u32 index = _nya_net_steam_resolve(endpoint, peer);
    if (index == NYA_NET_MAX_PEERS) return;

    // Steam is told so the far end's session ends now rather than on its own timeout.
    nya_steam_p2p_close(endpoint->peers[index].user);

    // no event: this side asked for it; _nya_net_steam_remove is for the far end going away.
    endpoint->peers[index] = (_NYA_NetSteamPeer){ .generation = endpoint->peers[index].generation };

    nya_unused(reason);
}

NYA_NetPeerStats _nya_net_steam_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    u32 index = _nya_net_steam_resolve(endpoint, peer);
    if (index == NYA_NET_MAX_PEERS) return (NYA_NetPeerStats){ 0 };

    // round trip, jitter and loss stay zero: they are Valve's relay's numbers, and this transport never measures a packet.
    return endpoint->peers[index].stats;
}

NYA_ConstCString _nya_net_steam_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;

    u32 index = _nya_net_steam_resolve(endpoint, peer);
    if (index == NYA_NET_MAX_PEERS) return "(gone)";

    return endpoint->peers[index].address;
}

void _nya_net_steam_destroy(NYA_NetTransport* transport) {
    _NYA_NetSteamEndpoint* endpoint = transport->state;
    if (endpoint == nullptr) return;

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (!endpoint->peers[i].connected) continue;

        nya_steam_p2p_close(endpoint->peers[i].user);
    }

    // the endpoint itself came from the arena the caller passed in, which is the caller's to destroy.
    transport->state = nullptr;
}

u32 _nya_net_steam_find(const _NYA_NetSteamEndpoint* endpoint, NYA_SteamId user) {
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (!endpoint->peers[i].connected) continue;
        if (nya_steam_id_equals(endpoint->peers[i].user, user)) return i;
    }

    return NYA_NET_MAX_PEERS;
}

u32 _nya_net_steam_add(_NYA_NetSteamEndpoint* endpoint, NYA_SteamId user) {
    nya_assert(nya_steam_id_is_set(user));

    u32 existing = _nya_net_steam_find(endpoint, user);
    if (existing != NYA_NET_MAX_PEERS) return existing;

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (endpoint->peers[i].connected) continue;

        // bumped on every reuse, so a peer id held across a reconnect resolves to nothing, not the slot's new owner.
        endpoint->peers[i].generation++;
        endpoint->peers[i].user      = user;
        endpoint->peers[i].connected = true;
        endpoint->peers[i].stats     = (NYA_NetPeerStats){ 0 };

        (void)snprintf(endpoint->peers[i].address, sizeof(endpoint->peers[i].address), "%llu", (unsigned long long)user.value);

        _nya_net_steam_event_push(endpoint, (_NYA_NetSteamEvent){
                                                .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED,
                                                .peer = _nya_net_steam_peer_id(endpoint, i),
                                            });

        return i;
    }

    return NYA_NET_MAX_PEERS;
}

void _nya_net_steam_remove(_NYA_NetSteamEndpoint* endpoint, u32 index, NYA_NetDisconnect reason) {
    nya_assert(index < NYA_NET_MAX_PEERS);

    if (!endpoint->peers[index].connected) return;

    NYA_NetPeerId peer = _nya_net_steam_peer_id(endpoint, index);

    nya_steam_p2p_close(endpoint->peers[index].user);

    endpoint->peers[index] = (_NYA_NetSteamPeer){ .generation = endpoint->peers[index].generation };

    _nya_net_steam_event_push(endpoint, (_NYA_NetSteamEvent){ .kind = NYA_NET_TRANSPORT_EVENT_DISCONNECTED, .peer = peer, .reason = reason });
}

NYA_NetPeerId _nya_net_steam_peer_id(const _NYA_NetSteamEndpoint* endpoint, u32 index) {
    nya_assert(index < NYA_NET_MAX_PEERS);

    return (NYA_NetPeerId){ .index = index, .generation = endpoint->peers[index].generation };
}

u32 _nya_net_steam_resolve(const _NYA_NetSteamEndpoint* endpoint, NYA_NetPeerId peer) {
    if (peer.index >= NYA_NET_MAX_PEERS) return NYA_NET_MAX_PEERS;
    if (!endpoint->peers[peer.index].connected) return NYA_NET_MAX_PEERS;
    if (endpoint->peers[peer.index].generation != peer.generation) return NYA_NET_MAX_PEERS;

    return peer.index;
}

void _nya_net_steam_event_push(_NYA_NetSteamEndpoint* endpoint, _NYA_NetSteamEvent event) {
    // two per peer (a connect and a disconnect) is the most outstanding: no second connect before the slot is freed.
    if (endpoint->event_count >= nya_carray_length(endpoint->events)) {
        nya_log_warn("The Steam transport's event queue is full; dropping a %s.",
                     event.kind == NYA_NET_TRANSPORT_EVENT_CONNECTED ? "connect" : "disconnect");
        return;
    }

    endpoint->events[endpoint->event_count] = event;
    endpoint->event_count++;
}

void _nya_net_steam_drain_sessions(_NYA_NetSteamEndpoint* endpoint) {
    NYA_SteamEvent event;

    while (nya_steam_p2p_poll(&event)) {
        if (!nya_steam_id_is_set(event.user)) continue;

        if (event.kind == NYA_STEAM_EVENT_SESSION_REQUEST) {
            // a client refuses everyone: anything but the one account it connected to is not the game.
            if (!endpoint->accepting) {
                if (_nya_net_steam_find(endpoint, event.user) == NYA_NET_MAX_PEERS) {
                    nya_steam_p2p_close(event.user);
                    continue;
                }
            }

            NYA_Error accepted = nya_steam_p2p_accept(event.user);

            if (!accepted.ok) {
                nya_log_warn("Could not accept a Steam session from %llu: %s", (unsigned long long)event.user.value,
                             (NYA_ConstCString)accepted.message);
                continue;
            }

            if (_nya_net_steam_add(endpoint, event.user) == NYA_NET_MAX_PEERS) {
                // the table is full (the player limit); closed rather than left half open, so the far end learns now.
                nya_log_warn("Refusing a Steam session from %llu: the peer table is full.", (unsigned long long)event.user.value);
                nya_steam_p2p_close(event.user);
            }

            continue;
        }

        if (event.kind == NYA_STEAM_EVENT_SESSION_FAILED) {
            u32 index = _nya_net_steam_find(endpoint, event.user);
            if (index == NYA_NET_MAX_PEERS) continue;

            // Steam reports only sessions that broke, never a polite close, so this is always "the link went away".
            _nya_net_steam_remove(endpoint, index, NYA_NET_DISCONNECT_TIMEOUT);
            continue;
        }
    }
}

void _nya_net_steam_receive(_NYA_NetSteamEndpoint* endpoint) {
    nya_assert(endpoint->inbox_read == endpoint->inbox_count);

    endpoint->inbox_count = 0;
    endpoint->inbox_read  = 0;
    endpoint->inbox_used  = 0;

    NYA_SteamMessage messages[NYA_STEAM_MAX_RECEIVE] = { 0 };

    u32 taken = nya_steam_p2p_receive(_NYA_NET_STEAM_CHANNEL, messages, NYA_STEAM_MAX_RECEIVE);
    if (taken == 0) return;

    nya_assert(taken <= NYA_STEAM_MAX_RECEIVE);

    for (u32 i = 0; i < taken; i++) {
        const NYA_SteamMessage* message = &messages[i];

        if (message->data == nullptr || message->size == 0) continue;
        if (message->size > NYA_STEAM_MAX_MESSAGE) continue;
        if (endpoint->inbox_used + message->size > sizeof(endpoint->inbox_bytes)) break;

        u32 index = _nya_net_steam_find(endpoint, message->sender);

        if (index == NYA_NET_MAX_PEERS) {
            // a first message from a peer Steam accepted before this transport existed (an invite taken at the menu).
            if (!endpoint->accepting) continue;

            index = _nya_net_steam_add(endpoint, message->sender);
            if (index == NYA_NET_MAX_PEERS) continue;
        }

        nya_memcpy(endpoint->inbox_bytes + endpoint->inbox_used, message->data, message->size);

        endpoint->inbox[endpoint->inbox_count] = (_NYA_NetSteamInboxEntry){
            .peer_index = index,
            .offset     = endpoint->inbox_used,
            .size       = message->size,
            .channel    = message->reliable ? NYA_NET_CHANNEL_RELIABLE : NYA_NET_CHANNEL_UNRELIABLE,
        };

        endpoint->inbox_count++;
        endpoint->inbox_used += message->size;
    }
}

NYA_SteamId _nya_net_steam_id_from_address(NYA_ConstCString address) {
    if (address == nullptr) return NYA_STEAM_ID_NONE;

    // twenty digits is the most a 64 bit account id can be written in.
    u64 length = strnlen(address, 21);

    if (length == 0 || length > 20) return NYA_STEAM_ID_NONE;

    u64 value = 0;

    for (u64 i = 0; i < length; i++) {
        if (address[i] < '0' || address[i] > '9') return NYA_STEAM_ID_NONE;

        // the address came off a join secret, so a number past 2^64 is refused rather than wrapped into another account.
        if (value > (UINT64_MAX - (u64)(address[i] - '0')) / 10) return NYA_STEAM_ID_NONE;

        value = (value * 10) + (u64)(address[i] - '0');
    }

    return (NYA_SteamId){ .value = value };
}
