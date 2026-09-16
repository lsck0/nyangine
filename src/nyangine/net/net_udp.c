#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

#include "SDL3_net/SDL_net.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE WIRE FORMAT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every datagram is one packet header followed by one or more message fragments:
 */

#define _NYA_NET_UDP_PROTOCOL 0x6E796105U /* "nya" + version 5 */

#define _NYA_NET_UDP_HEADER_SIZE   15
#define _NYA_NET_UDP_FRAGMENT_HEADER_SIZE 9

/** How long without hearing anything before a peer is considered gone. */
#define _NYA_NET_UDP_TIMEOUT_MS 10000

/** How often to send a keepalive when nothing else is going out, so a quiet peer does not time out. */
#define _NYA_NET_UDP_KEEPALIVE_MS 1000

/** How long to wait for an unacknowledged reliable message before sending it again. */
#define _NYA_NET_UDP_RESEND_MS 250

/** How long a connect attempt runs before giving up. */
#define _NYA_NET_UDP_CONNECT_TIMEOUT_MS 5000

/** How often a connecting client repeats its request. */
#define _NYA_NET_UDP_CONNECT_RETRY_MS 500

/**
 * How many message ids behind the newest one are remembered per channel, for duplicate suppression. An id
 * further back than this is treated as a duplicate: nothing legitimate arrives that late.
 * */
#define _NYA_NET_UDP_SEEN_WINDOW 1024

/** Bits per word of the seen window. */
#define _NYA_NET_UDP_SEEN_WORD_BITS 64

/** How many packets back the ack bitfield reaches. One per bit of a u32. */
#define _NYA_NET_UDP_ACK_WINDOW 32

/** The most datagrams one poll drains before returning. */
#define _NYA_NET_UDP_MAX_RECEIVE_PER_POLL 512

/*
 * Internal packet kinds, in the channel byte's high bits. Management packets (connect, accept, disconnect,
 * challenge, response) carry no fragments.
 */
#define _NYA_NET_UDP_KIND_DATA       0
#define _NYA_NET_UDP_KIND_CONNECT    1
#define _NYA_NET_UDP_KIND_ACCEPT     2
#define _NYA_NET_UDP_KIND_DISCONNECT 3

/*
 * Connect challenge.
 *
 * A UDP source address is a claim anyone can forge. Allocating a peer on CONNECT would let one attacker fill
 * every slot with spoofed addresses. So the server answers CONNECT with a CHALLENGE carrying a cookie derived
 * from the claimed address, a startup secret and a coarse clock, and only allocates a slot when the cookie
 * comes back. That proves the client receives at that address, and the cookie is recomputed rather than
 * stored, so pending handshakes cost no state. The same idea as TCP SYN cookies.
 */
#define _NYA_NET_UDP_KIND_CHALLENGE 4
#define _NYA_NET_UDP_KIND_RESPONSE  5

/** How long a cookie stays valid, in milliseconds. */
#define _NYA_NET_UDP_COOKIE_WINDOW_MS 20000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A reliable message awaiting acknowledgement. Kept whole; fragmentation happens at send time. */
typedef struct {
    u16 message_id;
    u8* data;
    u64 size;

    /** Monotonic ms of the last transmission, for the resend timer. */
    u64 last_sent_ms;

    /** Times it has been sent. For the stats, and for noticing a peer that never acknowledges. */
    u32 sends;
} _NYA_NetUdpReliable;

/*
 * Retired when the peer's `reliable_ack` passes its id, not by tracking the packet that carried it, which
 * does not work for fragmented messages.
 */

nya_derive_array(_NYA_NetUdpReliable);

/** A message being reassembled from fragments. */
typedef struct {
    b8  active;
    u16 message_id;
    u16 fragment_total;
    u16 fragments_received;

    NYA_NetChannel channel;

    /** Which fragments have arrived, so a duplicate does not count twice. */
    u8 received[64];

    u8* data;
    u64 size;

    /** How large `data` is, as opposed to how much this message uses. */
    u64 capacity;

    /** When the first fragment arrived, so an incomplete reassembly is eventually dropped. */
    u64 started_ms;
} _NYA_NetUdpReassembly;

/** Partial reassemblies a peer may have before the oldest is dropped. */
#define _NYA_NET_UDP_MAX_REASSEMBLY 8

/** The most fragments one message may be split into. See _NYA_NetUdpReassembly.received. */
#define _NYA_NET_UDP_MAX_FRAGMENTS 512

/**
 * The largest message this transport will reassemble, in bytes.
 * */
#define _NYA_NET_UDP_MAX_MESSAGE (256ULL * 1024ULL)

/**
 * The most one peer may have tied up in partial reassemblies at once.
 * */
#define _NYA_NET_UDP_MAX_REASSEMBLY_BYTES (512ULL * 1024ULL)

/** Reliable messages that may wait out of order before a peer is dropped. */
#define _NYA_NET_UDP_MAX_REORDER NYA_NET_MAX_RELIABLE_IN_FLIGHT

typedef struct {
    b8 occupied;
    u32 generation;

    NET_Address* address;
    u16          port;

    /** Printed form of address:port, resolved once. See _nya_net_udp_peer_address. */
    char address_text[128];

    /** A secret shared by these two endpoints, minted when the connection is accepted. */
    u64 session_token;

    /** Our next outgoing packet sequence, and what we have heard from them. */
    u16 local_sequence;
    u16 remote_sequence;
    u32 ack_bits;

    /** Next id to stamp on an outgoing message, per channel. */
    u16 next_message_id[NYA_NET_CHANNEL_COUNT];

    /** Next reliable id to deliver. Later ones are held until it arrives. */
    u16 next_delivery_id;

    /**
     * Ids already received, per channel, as a window behind the newest id: bit k is `newest - k`. Relative to the
     * newest, so an id arriving out of order cannot clear another's mark.
     * */
    u64 seen[NYA_NET_CHANNEL_COUNT][_NYA_NET_UDP_SEEN_WINDOW / _NYA_NET_UDP_SEEN_WORD_BITS];
    u16 seen_newest[NYA_NET_CHANNEL_COUNT];
    b8  seen_any[NYA_NET_CHANNEL_COUNT];

    NYA_Arrayᐸ_NYA_NetUdpReliableᐳ* outgoing_reliable;

    /** Reliable messages that arrived early, held until the gap before them fills. */
    NYA_Arrayᐸ_NYA_NetUdpReliableᐳ* incoming_reliable;

    _NYA_NetUdpReassembly reassembly[_NYA_NET_UDP_MAX_REASSEMBLY];

    /** Bytes this peer has allocated across its reassembly slots. */
    u64 reassembly_bytes;

    u64 last_received_ms;
    u64 last_sent_ms;

    NYA_NetPeerStats stats;

    /** Exponential moving averages behind stats.rtt_ms and stats.jitter_ms. */
    f32 rtt_ms;
    f32 jitter_ms;

    /** Send time of each of the last 32 packets, by sequence, for measuring the round trip. */
    u64 sent_at_ms[_NYA_NET_UDP_ACK_WINDOW];
    u16 sent_at_sequence[_NYA_NET_UDP_ACK_WINDOW];

    /** Rolling counts behind stats.packet_loss. */
    u32 acked_window;
    u32 sent_window;
} _NYA_NetUdpPeer;

/** A completed message waiting to be reported by poll. */
typedef struct {
    NYA_NetTransportEventKind kind;
    NYA_NetPeerId             peer;
    u8*                       data;
    u64                       size;
    NYA_NetChannel            channel;
    NYA_NetDisconnect         reason;
} _NYA_NetUdpEvent;

nya_derive_array(_NYA_NetUdpEvent);

typedef struct {
    NYA_Arena* allocator;

    NET_DatagramSocket* socket;

    b8 listening;

    /**
     * The key the connect cookies are derived from, generated once at startup.
     * */
    u64 cookie_key_low;
    u64 cookie_key_high;

    /** Bumped for every session token minted, so two connections never share one. */
    u64 session_counter;

    /**
     * The cookie the server challenged this client with, while connecting out.
     * */
    u64 pending_cookie;

    /** Set while a client is trying to reach a server, cleared when accepted or timed out. */
    b8           connecting;
    NET_Address* connect_address;
    u16          connect_port;
    u64          connect_started_ms;
    u64          connect_last_sent_ms;

    _NYA_NetUdpPeer peers[NYA_NET_MAX_PEERS];

    NYA_Arrayᐸ_NYA_NetUdpEventᐳ* events;

    /**
     * Bytes handed out by the last poll, freed by the next.
     * */
    NYA_Arena* delivered;

    /** Scratch for building one outgoing datagram. */
    u8 send_buffer[NYA_NET_MAX_DATAGRAM];
} _NYA_NetUdpState;

NYA_INTERNAL NYA_Error _nya_net_udp_listen(NYA_NetTransport* transport, u16 port);
NYA_INTERNAL NYA_Error _nya_net_udp_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port);
NYA_INTERNAL NYA_Error _nya_net_udp_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_INTERNAL b8        _nya_net_udp_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_INTERNAL void      _nya_net_udp_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_INTERNAL NYA_NetPeerStats _nya_net_udp_stats(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL NYA_ConstCString _nya_net_udp_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void      _nya_net_udp_destroy(NYA_NetTransport* transport);
NYA_INTERNAL void      _nya_net_udp_simulate_packet_loss(NYA_NetTransport* transport, u32 percent);

/** Reads whatever datagrams are waiting and turns them into events. */
NYA_INTERNAL void _nya_net_udp_receive(NYA_NetTransport* transport);

/** Retransmits, keepalives and timeouts: everything driven by time rather than by a packet. */
NYA_INTERNAL void _nya_net_udp_update(NYA_NetTransport* transport);

/** One datagram from one peer, already known to carry the right protocol word. */
NYA_INTERNAL void _nya_net_udp_handle_packet(NYA_NetTransport* transport, u32 peer_index, const u8* data, u64 size);

/** Sends whatever is queued for a peer, packing as many fragments into a datagram as fit. */
NYA_INTERNAL void _nya_net_udp_flush(NYA_NetTransport* transport, u32 peer_index);

/** Sends one management packet: no fragments, an optional cookie. */
NYA_INTERNAL void _nya_net_udp_send_control(NYA_NetTransport* transport, NET_Address* address, u16 port, u8 kind, u8 reason, u64 cookie);

/** The cookie an address must echo to get a peer slot. See the connect challenge note. */
NYA_INTERNAL u64 _nya_net_udp_cookie(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 epoch_offset) __attr_no_discard;

/** Whether `cookie` is one this server would recently have issued to this address. */
NYA_INTERNAL b8 _nya_net_udp_cookie_valid(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 cookie) __attr_no_discard;

/** The peer at `address`:`port`, or NYA_NET_MAX_PEERS when there is none. */
NYA_INTERNAL u32 _nya_net_udp_find_peer(_NYA_NetUdpState* state, NET_Address* address, u16 port) __attr_no_discard;

/** Takes a free peer slot for `address`:`port`, or NYA_NET_MAX_PEERS when the table is full. */
NYA_INTERNAL u32 _nya_net_udp_add_peer(_NYA_NetUdpState* state, NET_Address* address, u16 port) __attr_no_discard;

NYA_INTERNAL void _nya_net_udp_remove_peer(NYA_NetTransport* transport, u32 peer_index, NYA_NetDisconnect reason, b8 notify);

/** Resolves a peer id to a slot index, or NYA_NET_MAX_PEERS when it names nothing live. */
NYA_INTERNAL u32 _nya_net_udp_resolve(_NYA_NetUdpState* state, NYA_NetPeerId peer) __attr_no_discard;

/** Records an incoming packet sequence into the ack bitfield. */
NYA_INTERNAL void _nya_net_udp_record_ack(_NYA_NetUdpPeer* peer, u16 sequence);

/** Applies the packet acknowledgements a peer sent: round trip and loss estimate. */
NYA_INTERNAL void _nya_net_udp_apply_acks(_NYA_NetUdpPeer* peer, u16 ack, u32 ack_bits, u64 now_ms);

/** Stops retransmitting every reliable message the peer says it delivered. */
NYA_INTERNAL void _nya_net_udp_retire_reliable(_NYA_NetUdpPeer* peer, NYA_Arena* allocator, u16 reliable_ack);

/** Whether `id` was already received on `channel`. Pure: asking records nothing. */
NYA_INTERNAL b8 _nya_net_udp_is_seen(const _NYA_NetUdpPeer* peer, NYA_NetChannel channel, u16 message_id) __attr_no_discard;

/** Records `id` as received on `channel`, sliding the window if it is the newest. */
NYA_INTERNAL void _nya_net_udp_mark_seen(_NYA_NetUdpPeer* peer, NYA_NetChannel channel, u16 message_id);

/** Whether a reliable id may still be delivered: not yet delivered, and within the reorder window. */
NYA_INTERNAL b8 _nya_net_udp_reliable_acceptable(const _NYA_NetUdpPeer* peer, u16 message_id) __attr_no_discard;

/** Queues a fully assembled message as a MESSAGE event. Copies the bytes. */
NYA_INTERNAL void _nya_net_udp_deliver(NYA_NetTransport* transport, u32 peer_index, NYA_NetChannel channel, const u8* data, u64 size);

/** Feeds one fragment into reassembly, delivering the message when the last one lands. */
NYA_INTERNAL void _nya_net_udp_reassemble(
    NYA_NetTransport* transport, u32 peer_index, NYA_NetChannel channel, u16 message_id, u16 index, u16 total, const u8* data, u16 size
);

/** Hands up every reliable message now in order, starting from next_delivery_id. */
NYA_INTERNAL void _nya_net_udp_drain_ordered(NYA_NetTransport* transport, u32 peer_index);

/*
 * Sequence numbers wrap at 16 bits, so 0 is newer than 65535. a is newer than b when the forward distance is
 * less than half the space.
 */
NYA_INTERNAL b8 _nya_net_udp_sequence_newer(u16 a, u16 b) __attr_no_discard;

/** Little endian readers and writers, so every host produces the same bytes. */
NYA_INTERNAL void _nya_net_udp_write_u16(u8* out, u16 value);
NYA_INTERNAL void _nya_net_udp_write_u32(u8* out, u32 value);
NYA_INTERNAL u16  _nya_net_udp_read_u16(const u8* in) __attr_no_discard;
NYA_INTERNAL u32  _nya_net_udp_read_u32(const u8* in) __attr_no_discard;

/** Transports with SDL_net up. */
NYA_INTERNAL u32 _NYA_NET_UDP_INIT_COUNT = 0;

NYA_INTERNAL const NYA_NetTransportVTable _NYA_NET_UDP_VTABLE = {
    .name = "udp",
    .kind = NYA_NET_TRANSPORT_UDP,

    .listen       = &_nya_net_udp_listen,
    .connect      = &_nya_net_udp_connect,
    .send         = &_nya_net_udp_send,
    .poll         = &_nya_net_udp_poll,
    .disconnect   = &_nya_net_udp_disconnect,
    .stats        = &_nya_net_udp_stats,
    .peer_address = &_nya_net_udp_peer_address,
    .simulate_packet_loss = &_nya_net_udp_simulate_packet_loss,
    .destroy      = &_nya_net_udp_destroy,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_transport_udp_create(NYA_Arena* arena, OUT NYA_NetTransport** out_transport) {
    nya_assert(arena != nullptr);
    nya_assert(out_transport != nullptr);

    *out_transport = nullptr;

    if (!NET_Init()) return nya_error(NYA_ERROR_NOT_OK, "SDL_net could not start: %s", SDL_GetError());
    _NYA_NET_UDP_INIT_COUNT++;

    _NYA_NetUdpState* state = nya_arena_alloc(arena, sizeof(_NYA_NetUdpState));

    *state = (_NYA_NetUdpState){
        .allocator = arena,
        .events    = nya_array_create(arena, _NYA_NetUdpEvent),
        .delivered = nya_arena_create(.name = "net_udp_delivered"),
    };

    /* The cookie key, from an unseeded RNG so it differs every run. */
    NYA_RNG rng = nya_rng_create();

    /* The range is explicit, because a zeroed NYA_RNGDistribution is uniform(0, 0). */
    NYA_RNGDistribution uniform = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = (f64)U64_MAX } };

    /*
     * Sampled through an f64, so about 53 bits of entropy. Enough: an attacker never sees cookies issued to
     * addresses they do not control, so they would have to forge siphash output blind.
     */
    state->cookie_key_low  = nya_rng_sample_u64(&rng, uniform);
    state->cookie_key_high = nya_rng_sample_u64(&rng, uniform);

    // a zero key means the RNG setup is broken.
    nya_assert(state->cookie_key_low != 0 || state->cookie_key_high != 0, "the connect cookie key came out zero");

    NYA_NetTransport* transport = nya_arena_alloc(arena, sizeof(NYA_NetTransport));

    *transport = (NYA_NetTransport){ .vtable = &_NYA_NET_UDP_VTABLE, .allocator = arena, .state = state };

    *out_transport = transport;

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFECYCLE
 * ─────────────────────────────────────────────────────────
 */

NYA_Error _nya_net_udp_listen(NYA_NetTransport* transport, u16 port) {
    _NYA_NetUdpState* state = transport->state;

    if (state->socket != nullptr) return nya_error(NYA_ERROR_NOT_OK, "this transport already has a socket");

    // a null address binds every local interface, so a host on ethernet and wifi is reachable on both.
    state->socket = NET_CreateDatagramSocket(nullptr, port, 0);
    if (state->socket == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not bind UDP port %u: %s", port, SDL_GetError());

    state->listening = true;

    nya_log_info("Listening for players on UDP port %u.", port);

    return NYA_OK;
}

NYA_Error _nya_net_udp_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port) {
    nya_assert(address != nullptr);

    _NYA_NetUdpState* state = transport->state;

    if (state->socket != nullptr) return nya_error(NYA_ERROR_NOT_OK, "this transport already has a socket");

    NET_Address* resolved = NET_ResolveHostname(address);
    if (resolved == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "could not resolve '%s': %s", address, SDL_GetError());

    /* SDL_net resolves asynchronously; this waits. */
    if (NET_WaitUntilResolved(resolved, _NYA_NET_UDP_CONNECT_TIMEOUT_MS) != 1) {
        NET_UnrefAddress(resolved);
        return nya_error(NYA_ERROR_NOT_FOUND, "could not resolve '%s': %s", address, SDL_GetError());
    }

    // port zero lets the system pick, so two copies of a game on one machine can both connect.
    state->socket = NET_CreateDatagramSocket(nullptr, 0, 0);
    if (state->socket == nullptr) {
        NET_UnrefAddress(resolved);
        return nya_error(NYA_ERROR_NOT_OK, "could not open a UDP socket: %s", SDL_GetError());
    }

    state->connecting           = true;
    state->connect_address      = resolved;
    state->connect_port         = port;
    state->connect_started_ms   = nya_clock_get_monotonic_ms();
    state->connect_last_sent_ms = 0;

    nya_log_info("Connecting to %s:%u.", address, port);

    return NYA_OK;
}

void _nya_net_udp_destroy(NYA_NetTransport* transport) {
    _NYA_NetUdpState* state = transport->state;
    if (state == nullptr) return;

    // a courtesy: the datagram may be lost and the peer times out instead. worth it, since a clean exit is the
    // common case and a ten second timeout is poor for everyone else.
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (!state->peers[i].occupied) continue;

        _nya_net_udp_send_control(transport, state->peers[i].address, state->peers[i].port, _NYA_NET_UDP_KIND_DISCONNECT,
                                  (u8)NYA_NET_DISCONNECT_SERVER_CLOSED, state->peers[i].session_token);

        NET_UnrefAddress(state->peers[i].address);
        state->peers[i].address  = nullptr;
        state->peers[i].occupied = false;
    }

    if (state->connect_address != nullptr) {
        NET_UnrefAddress(state->connect_address);
        state->connect_address = nullptr;
    }

    if (state->socket != nullptr) {
        NET_DestroyDatagramSocket(state->socket);
        state->socket = nullptr;
    }

    nya_arena_destroy(state->delivered);

    transport->state = nullptr;

    // balances NET_Init in create. SDL_net reference counts.
    if (_NYA_NET_UDP_INIT_COUNT > 0) {
        _NYA_NET_UDP_INIT_COUNT--;
        NET_Quit();
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * SENDING
 * ─────────────────────────────────────────────────────────
 */

NYA_Error _nya_net_udp_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS) return nya_error(NYA_ERROR_NOT_FOUND, "no such peer");

    _NYA_NetUdpPeer* connection = &state->peers[index];

    u64 usable   = NYA_NET_MAX_DATAGRAM - _NYA_NET_UDP_HEADER_SIZE - _NYA_NET_UDP_FRAGMENT_HEADER_SIZE;
    u64 fragments = (size + usable - 1) / usable;

    if (fragments > _NYA_NET_UDP_MAX_FRAGMENTS) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a %llu byte message needs more than %d fragments", (unsigned long long)size,
                         _NYA_NET_UDP_MAX_FRAGMENTS);
    }

    u16 message_id = connection->next_message_id[channel]++;

    if (channel == NYA_NET_CHANNEL_RELIABLE) {
        /* Queued whole and fragmented at transmit, so a resend re-fragments against the current header size. */
        if (connection->outgoing_reliable->length >= NYA_NET_MAX_RELIABLE_IN_FLIGHT) {
            // the peer stopped acknowledging; it is gone, the timeout just has not fired.
            _nya_net_udp_remove_peer(transport, index, NYA_NET_DISCONNECT_TIMEOUT, true);
            return nya_error(NYA_ERROR_NOT_OK, "peer stopped acknowledging; dropped");
        }

        u8* copy = nya_arena_alloc(state->allocator, size);
        nya_memcpy(copy, data, size);

        nya_array_push_back(connection->outgoing_reliable,
                            ((_NYA_NetUdpReliable){ .message_id = message_id, .data = copy, .size = size, .last_sent_ms = 0, .sends = 0 }));

        _nya_net_udp_flush(transport, index);

        return NYA_OK;
    }

    /* Unreliable goes out immediately and is never kept. */
    u64 offset = 0;

    for (u16 fragment = 0; fragment < (u16)fragments; fragment++) {
        u64 chunk = nya_min(usable, size - offset);

        u8* buffer = state->send_buffer;
        u64 at     = 0;

        _nya_net_udp_write_u32(buffer + at, _NYA_NET_UDP_PROTOCOL);
        at += 4;
        _nya_net_udp_write_u16(buffer + at, connection->local_sequence);
        at += 2;
        _nya_net_udp_write_u16(buffer + at, connection->remote_sequence);
        at += 2;
        _nya_net_udp_write_u32(buffer + at, connection->ack_bits);
        at += 4;
        _nya_net_udp_write_u16(buffer + at, connection->next_delivery_id);
        at += 2;
        buffer[at++] = 1; // one fragment per datagram on this path

        buffer[at++] = (u8)((_NYA_NET_UDP_KIND_DATA << 4) | (u8)channel);
        _nya_net_udp_write_u16(buffer + at, message_id);
        at += 2;
        _nya_net_udp_write_u16(buffer + at, fragment);
        at += 2;
        _nya_net_udp_write_u16(buffer + at, (u16)fragments);
        at += 2;
        _nya_net_udp_write_u16(buffer + at, (u16)chunk);
        at += 2;

        nya_memcpy(buffer + at, data + offset, chunk);
        at += chunk;

        // the send time is kept per sequence to measure the round trip, in a ring as long as the ack window.
        u32 slot                          = connection->local_sequence % _NYA_NET_UDP_ACK_WINDOW;
        connection->sent_at_ms[slot]       = nya_clock_get_monotonic_ms();
        connection->sent_at_sequence[slot] = connection->local_sequence;

        connection->local_sequence++;
        connection->sent_window++;

        if (!NET_SendDatagram(state->socket, connection->address, connection->port, buffer, (int)at)) {
            // a failed send is a full buffer or a blip, not a dead peer. the timeout decides that.
            nya_log_debug("UDP send failed: %s", SDL_GetError());
            return NYA_OK;
        }

        connection->stats.bytes_sent += at;
        connection->stats.packets_sent++;
        connection->last_sent_ms = nya_clock_get_monotonic_ms();

        offset += chunk;
    }

    return NYA_OK;
}

void _nya_net_udp_flush(NYA_NetTransport* transport, u32 peer_index) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    u64 now_ms = nya_clock_get_monotonic_ms();
    u64 usable = NYA_NET_MAX_DATAGRAM - _NYA_NET_UDP_HEADER_SIZE - _NYA_NET_UDP_FRAGMENT_HEADER_SIZE;

    nya_array_foreach (connection->outgoing_reliable, pending) {
        // not due: sent once immediately, then every _NYA_NET_UDP_RESEND_MS until acknowledged.
        if (pending->sends > 0 && _nya_net_elapsed_ms(now_ms, pending->last_sent_ms) < _NYA_NET_UDP_RESEND_MS) continue;

        if (pending->sends > 0) connection->stats.retransmits++;

        u64 fragments = (pending->size + usable - 1) / usable;
        u64 offset    = 0;

        for (u16 fragment = 0; fragment < (u16)fragments; fragment++) {
            u64 chunk = nya_min(usable, pending->size - offset);

            u8* buffer = state->send_buffer;
            u64 at     = 0;

            _nya_net_udp_write_u32(buffer + at, _NYA_NET_UDP_PROTOCOL);
            at += 4;
            _nya_net_udp_write_u16(buffer + at, connection->local_sequence);
            at += 2;
            _nya_net_udp_write_u16(buffer + at, connection->remote_sequence);
            at += 2;
            _nya_net_udp_write_u32(buffer + at, connection->ack_bits);
            at += 4;
            _nya_net_udp_write_u16(buffer + at, connection->next_delivery_id);
            at += 2;
            buffer[at++] = 1;

            buffer[at++] = (u8)((_NYA_NET_UDP_KIND_DATA << 4) | (u8)NYA_NET_CHANNEL_RELIABLE);
            _nya_net_udp_write_u16(buffer + at, pending->message_id);
            at += 2;
            _nya_net_udp_write_u16(buffer + at, fragment);
            at += 2;
            _nya_net_udp_write_u16(buffer + at, (u16)fragments);
            at += 2;
            _nya_net_udp_write_u16(buffer + at, (u16)chunk);
            at += 2;

            nya_memcpy(buffer + at, pending->data + offset, chunk);
            at += chunk;

            u32 slot                           = connection->local_sequence % _NYA_NET_UDP_ACK_WINDOW;
            connection->sent_at_ms[slot]       = now_ms;
            connection->sent_at_sequence[slot] = connection->local_sequence;

            connection->local_sequence++;
            connection->sent_window++;

            if (!NET_SendDatagram(state->socket, connection->address, connection->port, buffer, (int)at)) break;

            connection->stats.bytes_sent += at;
            connection->stats.packets_sent++;

            offset += chunk;
        }

        pending->last_sent_ms = now_ms;
        pending->sends++;

        connection->last_sent_ms = now_ms;
    }
}

void _nya_net_udp_send_control(NYA_NetTransport* transport, NET_Address* address, u16 port, u8 kind, u8 reason, u64 cookie) {
    _NYA_NetUdpState* state = transport->state;

    if (state->socket == nullptr || address == nullptr) return;

    u8  buffer[_NYA_NET_UDP_HEADER_SIZE + 1 + 8];
    u64 at = 0;

    _nya_net_udp_write_u32(buffer + at, _NYA_NET_UDP_PROTOCOL);
    at += 4;
    _nya_net_udp_write_u16(buffer + at, 0);
    at += 2;
    _nya_net_udp_write_u16(buffer + at, 0);
    at += 2;
    _nya_net_udp_write_u32(buffer + at, 0);
    at += 4;
    _nya_net_udp_write_u16(buffer + at, 0);
    at += 2;

    // zero fragments, so a receiver finds none; the kind sits where a fragment's channel would.
    buffer[at++] = 0;
    buffer[at++] = (u8)((kind << 4) | (reason & 0x0F));

    // always present, so the layout is fixed.
    for (u32 i = 0; i < 8; i++) buffer[at++] = (u8)((cookie >> (i * 8)) & 0xFF);

    (void)NET_SendDatagram(state->socket, address, port, buffer, (int)at);
}

u64 _nya_net_udp_cookie(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 epoch_offset) {
    /* Derived, never stored. */
    int         address_size  = 0;
    const void* address_bytes = NET_GetAddressBytes(address, &address_size);

    u64 epoch = (nya_clock_get_monotonic_ms() / _NYA_NET_UDP_COOKIE_WINDOW_MS) - epoch_offset;

    /* Address, port and epoch, keyed with the secret. */
    u8  material[64] = { 0 };
    u64 at           = 0;

    if (address_bytes != nullptr && address_size > 0) {
        u64 copied = (u64)address_size > sizeof(material) - 16 ? sizeof(material) - 16 : (u64)address_size;

        nya_memcpy(material, address_bytes, copied);
        at += copied;
    }

    material[at++] = (u8)(port & 0xFF);
    material[at++] = (u8)((port >> 8) & 0xFF);

    for (u32 i = 0; i < 8; i++) material[at++] = (u8)((epoch >> (i * 8)) & 0xFF);

    u64 cookie = nya_siphash(material, at, state->cookie_key_low, state->cookie_key_high);

    // zero means "no cookie" on the wire, so a zero hash is nudged.
    return cookie == 0 ? 1 : cookie;
}

b8 _nya_net_udp_cookie_valid(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 cookie) {
    if (cookie == 0) return false;

    // current and previous bucket, so a response crossing a boundary is still accepted.
    return cookie == _nya_net_udp_cookie(state, address, port, 0) || cookie == _nya_net_udp_cookie(state, address, port, 1);
}

/*
 * ─────────────────────────────────────────────────────────
 * RECEIVING
 * ─────────────────────────────────────────────────────────
 */

b8 _nya_net_udp_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event) {
    _NYA_NetUdpState* state = transport->state;

    // only once the queue is empty, so the arena is not reset under unread events.
    if (state->events->length == 0) {
        nya_arena_free_all(state->delivered);

        _nya_net_udp_receive(transport);
        _nya_net_udp_update(transport);
    }

    if (state->events->length == 0) return false;

    _NYA_NetUdpEvent event = state->events->items[0];
    nya_array_remove(state->events, 0);

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

void _nya_net_udp_receive(NYA_NetTransport* transport) {
    _NYA_NetUdpState* state = transport->state;

    if (state->socket == nullptr) return;

    u32 drained = 0;

    for (;;) {
        // bounded, so a flood cannot stall the frame loop. the rest waits in the socket buffer.
        if (drained >= _NYA_NET_UDP_MAX_RECEIVE_PER_POLL) return;

        drained++;

        NET_Datagram* datagram = nullptr;

        // false is an error; a null datagram with true is simply nothing waiting.
        if (!NET_ReceiveDatagram(state->socket, &datagram)) {
            nya_log_debug("UDP receive failed: %s", SDL_GetError());
            return;
        }

        if (datagram == nullptr) return;

        /*
         * An oversized datagram is dropped before anything reads it. Short or foreign packets on a shared port are
         * normal and ignored below.
         */
        if (datagram->buflen > NYA_NET_MAX_DATAGRAM) {
            nya_log_debug("Dropping a %d byte datagram; the limit is %d.", datagram->buflen, NYA_NET_MAX_DATAGRAM);
            NET_DestroyDatagram(datagram);
            continue;
        }

        if (datagram->buflen >= _NYA_NET_UDP_HEADER_SIZE && _nya_net_udp_read_u32(datagram->buf) == _NYA_NET_UDP_PROTOCOL) {
            u32 index = _nya_net_udp_find_peer(state, datagram->addr, datagram->port);

            /* The kind byte follows the header, and a packet may end at the header. */
            b8 has_kind = datagram->buflen > _NYA_NET_UDP_HEADER_SIZE;
            u8 kind     = has_kind ? datagram->buf[_NYA_NET_UDP_HEADER_SIZE] >> 4 : (u8)_NYA_NET_UDP_KIND_DATA;

            if (index >= NYA_NET_MAX_PEERS) {
                /*
                 * A stranger. Only a connection request matters, and only while listening; a client never accepts an
                 * unsolicited peer.
                 */
                if (state->listening && has_kind) {
                    /* A CONNECT gets a challenge and no peer slot. */
                    if (kind == _NYA_NET_UDP_KIND_CONNECT) {
                        u64 cookie = _nya_net_udp_cookie(state, datagram->addr, datagram->port, 0);

                        _nya_net_udp_send_control(transport, datagram->addr, datagram->port, _NYA_NET_UDP_KIND_CHALLENGE, 0, cookie);
                    }

                    /*
                     * A RESPONSE echoing a cookie issued to this address proves the client receives there. Only now is a slot
                     * allocated.
                     */
                    if (kind == _NYA_NET_UDP_KIND_RESPONSE) {
                        u64 echoed = 0;

                        // too short for a cookie leaves zero, which the validator rejects.
                        if (datagram->buflen >= _NYA_NET_UDP_HEADER_SIZE + 1 + 8) {
                            for (u32 i = 0; i < 8; i++) echoed |= (u64)datagram->buf[_NYA_NET_UDP_HEADER_SIZE + 1 + i] << (i * 8);
                        }

                        if (_nya_net_udp_cookie_valid(state, datagram->addr, datagram->port, echoed)) {
                            u32 added = _nya_net_udp_add_peer(state, datagram->addr, datagram->port);

                            if (added >= NYA_NET_MAX_PEERS) {
                                _nya_net_udp_send_control(transport, datagram->addr, datagram->port, _NYA_NET_UDP_KIND_DISCONNECT,
                                                          (u8)NYA_NET_DISCONNECT_FULL, 0);
                            } else {
                                // the token goes out with the acceptance, so the client can authenticate its control packets.
                                _nya_net_udp_send_control(transport, state->peers[added].address, state->peers[added].port,
                                                          _NYA_NET_UDP_KIND_ACCEPT, 0, state->peers[added].session_token);

                                nya_array_push_back(state->events, ((_NYA_NetUdpEvent){
                                                                       .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED,
                                                                       .peer = { .index = added, .generation = state->peers[added].generation },
                                                                   }));
                            }
                        }
                    }
                }
            } else {
                _nya_net_udp_handle_packet(transport, index, datagram->buf, (u64)datagram->buflen);
            }
        }

        NET_DestroyDatagram(datagram);
    }
}

void _nya_net_udp_handle_packet(NYA_NetTransport* transport, u32 peer_index, const u8* data, u64 size) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    u64 now_ms = nya_clock_get_monotonic_ms();

    connection->last_received_ms = now_ms;
    connection->stats.bytes_received += size;
    connection->stats.packets_received++;

    u64 at = 4; // past the protocol word, checked by the caller

    u16 sequence = _nya_net_udp_read_u16(data + at);
    at += 2;
    u16 ack = _nya_net_udp_read_u16(data + at);
    at += 2;
    u32 ack_bits = _nya_net_udp_read_u32(data + at);
    at += 4;
    u16 reliable_ack = _nya_net_udp_read_u16(data + at);
    at += 2;
    u8 fragment_count = data[at++];

    _nya_net_udp_record_ack(connection, sequence);
    _nya_net_udp_apply_acks(connection, ack, ack_bits, now_ms);
    _nya_net_udp_retire_reliable(connection, state->allocator, reliable_ack);

    // a management packet: zero fragments, kind in the channel byte.
    if (fragment_count == 0) {
        if (at >= size) return;

        u8 kind   = data[at] >> 4;
        u8 reason = data[at] & 0x0F;

        // the eight byte payload of a control packet, zero (absent) when the packet is too short.
        u64 payload = 0;

        if (size >= (u64)_NYA_NET_UDP_HEADER_SIZE + 1 + 8) {
            for (u32 i = 0; i < 8; i++) payload |= (u64)data[_NYA_NET_UDP_HEADER_SIZE + 1 + i] << (i * 8);
        }

        /* A challenge while connecting out: echoed straight back. */
        if (kind == _NYA_NET_UDP_KIND_CHALLENGE && state->connecting) {
            u64 cookie = payload;

            if (cookie != 0) {
                state->pending_cookie = cookie;

                // immediately, not on the retry timer: handshake latency is what the player sees as "connecting".
                _nya_net_udp_send_control(transport, connection->address, connection->port, _NYA_NET_UDP_KIND_RESPONSE, 0, cookie);
            }
        }

        if (kind == _NYA_NET_UDP_KIND_ACCEPT && state->connecting) {
            state->connecting = false;

            /*
             * The server's token for this connection, used to authenticate our control packets. A server that sends none
             * leaves them unauthenticated rather than rejected, so old builds still connect.
             */
            if (payload != 0) connection->session_token = payload;

            nya_array_push_back(state->events, ((_NYA_NetUdpEvent){
                                                   .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED,
                                                   .peer = { .index = peer_index, .generation = connection->generation },
                                               }));
        }

        if (kind == _NYA_NET_UDP_KIND_DISCONNECT) {
            /* Only from the endpoint that completed this handshake. */
            if (connection->session_token != 0 && payload != connection->session_token) {
                nya_log_debug("Ignoring a disconnect for '%s' with the wrong session token.", connection->address_text);
                return;
            }

            _nya_net_udp_remove_peer(transport, peer_index, (NYA_NetDisconnect)reason, true);
        }

        // a CONNECT from a known peer means our ACCEPT was lost; answering again completes the handshake. the same
        // token goes out, since the client may already use it.
        if (kind == _NYA_NET_UDP_KIND_CONNECT && state->listening) {
            _nya_net_udp_send_control(transport, connection->address, connection->port, _NYA_NET_UDP_KIND_ACCEPT, 0, connection->session_token);
        }

        return;
    }

    for (u8 fragment = 0; fragment < fragment_count; fragment++) {
        // every read below stays inside the datagram. a malformed packet is dropped, not fatal.
        if (at + _NYA_NET_UDP_FRAGMENT_HEADER_SIZE > size) return;

        u8 channel_byte = data[at++];
        u8 channel      = channel_byte & 0x0F;

        u16 message_id = _nya_net_udp_read_u16(data + at);
        at += 2;
        u16 index = _nya_net_udp_read_u16(data + at);
        at += 2;
        u16 total = _nya_net_udp_read_u16(data + at);
        at += 2;
        u16 length = _nya_net_udp_read_u16(data + at);
        at += 2;

        if (channel >= NYA_NET_CHANNEL_COUNT) return;
        if (total == 0 || total > _NYA_NET_UDP_MAX_FRAGMENTS || index >= total) return;
        if (at + length > size) return;

        /* The declared total size is bounded, not just the fragment count. */
        u64 usable_per_fragment = NYA_NET_MAX_DATAGRAM - _NYA_NET_UDP_HEADER_SIZE - _NYA_NET_UDP_FRAGMENT_HEADER_SIZE;

        if ((u64)total * usable_per_fragment > _NYA_NET_UDP_MAX_MESSAGE) return;

        /* A fragment of a multi-fragment message may not be longer than a fragment. */
        if (total > 1 && length > usable_per_fragment) return;

        if (total == 1) {
            // one fragment is the whole message, so reassembly is skipped. refused ids are not marked, so their
            // retransmits are not mistaken for duplicates.
            b8 refused = channel == NYA_NET_CHANNEL_RELIABLE && !_nya_net_udp_reliable_acceptable(connection, message_id);

            if (refused || _nya_net_udp_is_seen(connection, (NYA_NetChannel)channel, message_id)) {
                at += length;
                continue;
            }

            _nya_net_udp_mark_seen(connection, (NYA_NetChannel)channel, message_id);

            if (channel == NYA_NET_CHANNEL_RELIABLE) {
                // the out-of-order queue is bounded; a peer that fills it is dropped.
                if (connection->incoming_reliable->length >= _NYA_NET_UDP_MAX_REORDER) {
                    nya_log_warn("Dropping a peer with %d reliable messages stuck out of order.", _NYA_NET_UDP_MAX_REORDER);
                    _nya_net_udp_remove_peer(transport, peer_index, NYA_NET_DISCONNECT_PROTOCOL, true);
                    return;
                }

                u8* copy = nya_arena_alloc(state->allocator, length);
                nya_memcpy(copy, data + at, length);

                nya_array_push_back(connection->incoming_reliable,
                                    ((_NYA_NetUdpReliable){ .message_id = message_id, .data = copy, .size = length }));

                _nya_net_udp_drain_ordered(transport, peer_index);
            } else {
                _nya_net_udp_deliver(transport, peer_index, (NYA_NetChannel)channel, data + at, length);
            }
        } else {
            _nya_net_udp_reassemble(transport, peer_index, (NYA_NetChannel)channel, message_id, index, total, data + at, length);

            // reassembly may have dropped the peer.
            if (!connection->occupied) return;
        }

        at += length;
    }
}

void _nya_net_udp_reassemble(
    NYA_NetTransport* transport, u32 peer_index, NYA_NetChannel channel, u16 message_id, u16 index, u16 total, const u8* data, u16 size
) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    u64 now_ms = nya_clock_get_monotonic_ms();
    u64 usable = NYA_NET_MAX_DATAGRAM - _NYA_NET_UDP_HEADER_SIZE - _NYA_NET_UDP_FRAGMENT_HEADER_SIZE;

    _NYA_NetUdpReassembly* slot  = nullptr;
    _NYA_NetUdpReassembly* free_slot = nullptr;
    _NYA_NetUdpReassembly* oldest = nullptr;

    for (u32 i = 0; i < _NYA_NET_UDP_MAX_REASSEMBLY; i++) {
        _NYA_NetUdpReassembly* candidate = &connection->reassembly[i];

        if (!candidate->active) {
            if (free_slot == nullptr) free_slot = candidate;
            continue;
        }

        if (candidate->message_id == message_id && candidate->channel == channel) {
            slot = candidate;
            break;
        }

        if (oldest == nullptr || candidate->started_ms < oldest->started_ms) oldest = candidate;
    }

    if (slot == nullptr) {
        // fragments of an already delivered message, resent. is_seen is pure; marking here would suppress every
        // fragmented message at completion.
        if (_nya_net_udp_is_seen(connection, channel, message_id)) return;

        // the same window as single fragment reliable messages, checked before allocating.
        if (channel == NYA_NET_CHANNEL_RELIABLE && !_nya_net_udp_reliable_acceptable(connection, message_id)) return;

        /* No free slot: the oldest partial message is abandoned. */
        slot = free_slot != nullptr ? free_slot : oldest;
        if (slot == nullptr) return;

        u64 needed = (u64)total * usable;

        /* The slot's buffer is reused when big enough. */
        /* The per-peer budget, against what this slot would grow to. */
        u64 would_hold = connection->reassembly_bytes - slot->capacity + needed;

        if (needed > slot->capacity && would_hold > _NYA_NET_UDP_MAX_REASSEMBLY_BYTES) {
            nya_log_debug("Refusing a reassembly that would take a peer to %llu bytes.", (unsigned long long)would_hold);
            return;
        }

        if (slot->capacity < needed) {
            if (slot->data != nullptr) nya_arena_free(state->allocator, slot->data, slot->capacity);

            connection->reassembly_bytes -= slot->capacity;

            slot->data     = nya_arena_alloc(state->allocator, needed);
            slot->capacity = needed;

            connection->reassembly_bytes += needed;
        }

        u8* buffer   = slot->data;
        u64 capacity = slot->capacity;

        *slot = (_NYA_NetUdpReassembly){
            .active         = true,
            .message_id     = message_id,
            .channel        = channel,
            .fragment_total = total,
            .started_ms     = now_ms,
            .size           = 0,
            .data           = buffer,
            .capacity       = capacity,
        };
    }

    if (slot->fragment_total != total) return; // two messages claiming one id; the newcomer is dropped

    // a duplicate fragment would complete the message with a hole.
    if (slot->received[index / 8] & (u8)(1U << (index % 8))) return;

    slot->received[index / 8] |= (u8)(1U << (index % 8));
    slot->fragments_received++;

    /* Checked where it is relied on. */
    if ((u64)index * usable + size > slot->capacity) {
        nya_log_warn("Refusing a fragment that would write %llu bytes past a %llu byte reassembly buffer.",
                 (unsigned long long)(((u64)index * usable + size) - slot->capacity), (unsigned long long)slot->capacity);
        return;
    }

    nya_memcpy(slot->data + ((u64)index * usable), data, size);

    // only the last fragment is short, so this is the exact total length.
    u64 end = ((u64)index * usable) + size;
    if (end > slot->size) slot->size = end;

    if (slot->fragments_received < slot->fragment_total) return;

    if (!_nya_net_udp_is_seen(connection, channel, message_id)) {
        _nya_net_udp_mark_seen(connection, channel, message_id);

        if (channel == NYA_NET_CHANNEL_RELIABLE) {
            if (connection->incoming_reliable->length >= _NYA_NET_UDP_MAX_REORDER) {
                nya_log_warn("Dropping a peer with %d reliable messages stuck out of order.", _NYA_NET_UDP_MAX_REORDER);
                _nya_net_udp_remove_peer(transport, peer_index, NYA_NET_DISCONNECT_PROTOCOL, true);
                return;
            }

            // copied out, since the slot is about to be reused.
            u8* owned = nya_arena_alloc(state->allocator, slot->size);
            nya_memcpy(owned, slot->data, slot->size);

            nya_array_push_back(connection->incoming_reliable,
                                ((_NYA_NetUdpReliable){ .message_id = message_id, .data = owned, .size = slot->size }));

            _nya_net_udp_drain_ordered(transport, peer_index);
        } else {
            // _nya_net_udp_deliver copies, so the slot's buffer is not referenced after this.
            _nya_net_udp_deliver(transport, peer_index, channel, slot->data, slot->size);
        }
    }

    // the buffer and capacity are kept for reuse.
    slot->active             = false;
    slot->fragments_received = 0;
    slot->size               = 0;
    nya_memset(slot->received, 0, sizeof(slot->received));
}

void _nya_net_udp_drain_ordered(NYA_NetTransport* transport, u32 peer_index) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];


    /*
     * Reliable messages are delivered strictly in order, so a lost one stalls those behind it. That is why
     * snapshots do not use this channel.
     */
    for (;;) {
        b8 delivered_any = false;

        for (u64 i = 0; i < connection->incoming_reliable->length; i++) {
            _NYA_NetUdpReliable* message = &connection->incoming_reliable->items[i];

            if (message->message_id != connection->next_delivery_id) continue;

            _nya_net_udp_deliver(transport, peer_index, NYA_NET_CHANNEL_RELIABLE, message->data, message->size);

            /* Freed once delivered; _nya_net_udp_deliver made its own copy. */
            if (message->data != nullptr) nya_arena_free(state->allocator, message->data, message->size);

            connection->next_delivery_id++;
            nya_array_remove(connection->incoming_reliable, i);

            delivered_any = true;
            break;
        }

        if (!delivered_any) return;
    }
}

void _nya_net_udp_deliver(NYA_NetTransport* transport, u32 peer_index, NYA_NetChannel channel, const u8* data, u64 size) {
    _NYA_NetUdpState* state = transport->state;

    // into the delivered arena, reset after the caller drains the queue. the datagram is freed on return.
    u8* copy = nya_arena_alloc(state->delivered, size);
    nya_memcpy(copy, data, size);

    nya_array_push_back(state->events, ((_NYA_NetUdpEvent){
                                           .kind    = NYA_NET_TRANSPORT_EVENT_MESSAGE,
                                           .peer    = { .index = peer_index, .generation = state->peers[peer_index].generation },
                                           .data    = copy,
                                           .size    = size,
                                           .channel = channel,
                                       }));
}

/*
 * ─────────────────────────────────────────────────────────
 * TIME
 * ─────────────────────────────────────────────────────────
 */

void _nya_net_udp_update(NYA_NetTransport* transport) {
    _NYA_NetUdpState* state = transport->state;

    u64 now_ms = nya_clock_get_monotonic_ms();

    if (state->connecting) {
        if (_nya_net_elapsed_ms(now_ms, state->connect_started_ms) > _NYA_NET_UDP_CONNECT_TIMEOUT_MS) {
            state->connecting = false;

            nya_array_push_back(state->events, ((_NYA_NetUdpEvent){
                                                   .kind   = NYA_NET_TRANSPORT_EVENT_DISCONNECTED,
                                                   .peer   = NYA_NET_PEER_NONE,
                                                   .reason = NYA_NET_DISCONNECT_TIMEOUT,
                                               }));
        } else if (_nya_net_elapsed_ms(now_ms, state->connect_last_sent_ms) >= _NYA_NET_UDP_CONNECT_RETRY_MS) {
            /* The request is repeated until answered. */
            if (_nya_net_udp_find_peer(state, state->connect_address, state->connect_port) >= NYA_NET_MAX_PEERS) {
                (void)_nya_net_udp_add_peer(state, state->connect_address, state->connect_port);
            }

            /* Whichever stage the handshake has reached. */
            if (state->pending_cookie != 0) {
                _nya_net_udp_send_control(transport, state->connect_address, state->connect_port, _NYA_NET_UDP_KIND_RESPONSE, 0,
                                          state->pending_cookie);
            } else {
                _nya_net_udp_send_control(transport, state->connect_address, state->connect_port, _NYA_NET_UDP_KIND_CONNECT, 0, 0);
            }

            state->connect_last_sent_ms = now_ms;
        }
    }

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetUdpPeer* connection = &state->peers[i];
        if (!connection->occupied) continue;

        if (_nya_net_elapsed_ms(now_ms, connection->last_received_ms) > _NYA_NET_UDP_TIMEOUT_MS) {
            _nya_net_udp_remove_peer(transport, i, NYA_NET_DISCONNECT_TIMEOUT, true);
            continue;
        }

        // Retransmits.
        _nya_net_udp_flush(transport, i);

        /*
         * A keepalive when there is nothing else to say.
         */
        if (_nya_net_elapsed_ms(now_ms, connection->last_sent_ms) >= _NYA_NET_UDP_KEEPALIVE_MS) {
            u8  buffer[_NYA_NET_UDP_HEADER_SIZE];
            u64 at = 0;

            _nya_net_udp_write_u32(buffer + at, _NYA_NET_UDP_PROTOCOL);
            at += 4;
            _nya_net_udp_write_u16(buffer + at, connection->local_sequence);
            at += 2;
            _nya_net_udp_write_u16(buffer + at, connection->remote_sequence);
            at += 2;
            _nya_net_udp_write_u32(buffer + at, connection->ack_bits);
            at += 4;
            _nya_net_udp_write_u16(buffer + at, connection->next_delivery_id);
            at += 2;
            buffer[at++] = 0;

            // DATA with zero fragments: the receiver applies the acks and delivers nothing.
            connection->local_sequence++;

            (void)NET_SendDatagram(state->socket, connection->address, connection->port, buffer, (int)at);

            /* Counted like every packet. */
            connection->stats.bytes_sent += at;
            connection->stats.packets_sent++;

            connection->last_sent_ms = now_ms;
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * PEERS
 * ─────────────────────────────────────────────────────────
 */

u32 _nya_net_udp_find_peer(_NYA_NetUdpState* state, NET_Address* address, u16 port) {
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (!state->peers[i].occupied) continue;
        if (state->peers[i].port != port) continue;
        if (NET_CompareAddresses(state->peers[i].address, address) != 0) continue;

        return i;
    }

    return NYA_NET_MAX_PEERS;
}

u32 _nya_net_udp_add_peer(_NYA_NetUdpState* state, NET_Address* address, u16 port) {
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (state->peers[i].occupied) continue;

        u32 generation = state->peers[i].generation + 1;

        state->peers[i] = (_NYA_NetUdpPeer){
            .occupied = true,

            // never zero: nya_net_peer_is_set reads the generation.
            .generation = generation == 0 ? 1 : generation,

            // referenced: SDL_net frees the address with the datagram.
            .address = NET_RefAddress(address),
            .port    = port,

            .last_received_ms = nya_clock_get_monotonic_ms(),
            .last_sent_ms     = nya_clock_get_monotonic_ms(),

            .outgoing_reliable = nya_array_create(state->allocator, _NYA_NetUdpReliable),
            .incoming_reliable = nya_array_create(state->allocator, _NYA_NetUdpReliable),
        };

        (void)snprintf(state->peers[i].address_text, sizeof(state->peers[i].address_text), "%s:%u", NET_GetAddressString(address), port);

        /* A fresh token for this connection. */
        state->session_counter++;

        u64 token = nya_siphash(&state->session_counter, sizeof(state->session_counter), state->cookie_key_low, state->cookie_key_high);

        state->peers[i].session_token = token == 0 ? 1 : token;

        return i;
    }

    return NYA_NET_MAX_PEERS;
}

void _nya_net_udp_remove_peer(NYA_NetTransport* transport, u32 peer_index, NYA_NetDisconnect reason, b8 notify) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    if (!connection->occupied) return;

    NYA_NetPeerId id = { .index = peer_index, .generation = connection->generation };

    /* Everything this peer holds goes back to the arena. */
    nya_array_foreach (connection->outgoing_reliable, pending) {
        if (pending->data != nullptr) nya_arena_free(state->allocator, pending->data, pending->size);
    }

    nya_array_foreach (connection->incoming_reliable, pending) {
        if (pending->data != nullptr) nya_arena_free(state->allocator, pending->data, pending->size);
    }

    for (u32 i = 0; i < _NYA_NET_UDP_MAX_REASSEMBLY; i++) {
        _NYA_NetUdpReassembly* slot = &connection->reassembly[i];

        if (slot->data == nullptr) continue;

        nya_arena_free(state->allocator, slot->data, slot->capacity);
        slot->data     = nullptr;
        slot->capacity = 0;
    }

    // the budget goes with the buffers, or the slot's next peer starts over its limit.
    connection->reassembly_bytes = 0;

    nya_array_destroy(connection->outgoing_reliable);
    nya_array_destroy(connection->incoming_reliable);

    if (connection->address != nullptr) {
        NET_UnrefAddress(connection->address);
        connection->address = nullptr;
    }

    connection->occupied = false;

    // the generation is bumped when the slot is reused, so stale handles fail.

    if (notify) {
        nya_array_push_back(state->events, ((_NYA_NetUdpEvent){
                                               .kind   = NYA_NET_TRANSPORT_EVENT_DISCONNECTED,
                                               .peer   = id,
                                               .reason = reason,
                                           }));
    }
}

u32 _nya_net_udp_resolve(_NYA_NetUdpState* state, NYA_NetPeerId peer) {
    if (peer.index >= NYA_NET_MAX_PEERS) return NYA_NET_MAX_PEERS;
    if (!state->peers[peer.index].occupied) return NYA_NET_MAX_PEERS;

    // a stale id must not address whoever took the slot next.
    if (state->peers[peer.index].generation != peer.generation) return NYA_NET_MAX_PEERS;

    return peer.index;
}

void _nya_net_udp_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS) return;

    _nya_net_udp_send_control(transport, state->peers[index].address, state->peers[index].port, _NYA_NET_UDP_KIND_DISCONNECT, (u8)reason,
                              state->peers[index].session_token);

    // no event: the caller asked for this.
    _nya_net_udp_remove_peer(transport, index, reason, false);
}

NYA_NetPeerStats _nya_net_udp_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS) return (NYA_NetPeerStats){ 0 };

    NYA_NetPeerStats stats = state->peers[index].stats;

    stats.rtt_ms    = state->peers[index].rtt_ms;
    stats.jitter_ms = state->peers[index].jitter_ms;

    u32 sent = state->peers[index].sent_window;
    stats.packet_loss = sent == 0 ? 0.0F : 1.0F - ((f32)state->peers[index].acked_window / (f32)sent);
    if (stats.packet_loss < 0.0F) stats.packet_loss = 0.0F;

    return stats;
}

void _nya_net_udp_simulate_packet_loss(NYA_NetTransport* transport, u32 percent) {
    _NYA_NetUdpState* state = transport->state;

    if (state->socket == nullptr) return;

    // SDL_net drops outgoing datagrams below this layer, so everything above sees a real lossy link.
    NET_SimulateDatagramPacketLoss(state->socket, (int)percent);
}

NYA_ConstCString _nya_net_udp_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS) return "(gone)";

    return state->peers[index].address_text;
}

/*
 * ─────────────────────────────────────────────────────────
 * ACKNOWLEDGEMENTS
 * ─────────────────────────────────────────────────────────
 */

void _nya_net_udp_record_ack(_NYA_NetUdpPeer* peer, u16 sequence) {
    if (_nya_net_udp_sequence_newer(sequence, peer->remote_sequence)) {
        u16 shift = (u16)(sequence - peer->remote_sequence);

        // shifting by 32 or more is undefined, and a peer that jumps ahead reaches it. the old window is out of range
        // anyway, so it is cleared.
        peer->ack_bits = shift >= _NYA_NET_UDP_ACK_WINDOW ? 0 : (peer->ack_bits << shift) | (1U << (shift - 1));

        peer->remote_sequence = sequence;
        return;
    }

    // older than the newest: set its bit if still inside the window.
    u16 back = (u16)(peer->remote_sequence - sequence);
    if (back == 0 || back > _NYA_NET_UDP_ACK_WINDOW) return;

    peer->ack_bits |= 1U << (back - 1);
}

void _nya_net_udp_apply_acks(_NYA_NetUdpPeer* peer, u16 ack, u32 ack_bits, u64 now_ms) {
    // the newest acknowledged packet plus the 32 before it.
    for (u32 bit = 0; bit <= _NYA_NET_UDP_ACK_WINDOW; bit++) {
        if (bit > 0 && (ack_bits & (1U << (bit - 1))) == 0) continue;

        u16 sequence = (u16)(ack - bit);
        u32 slot     = sequence % _NYA_NET_UDP_ACK_WINDOW;

        // the ring only holds the last 32 sends; checking the sequence stops a reused slot measuring a wrong round
        // trip.
        if (peer->sent_at_sequence[slot] != sequence) continue;
        if (peer->sent_at_ms[slot] == 0) continue;

        f32 sample = (f32)_nya_net_elapsed_ms(now_ms, peer->sent_at_ms[slot]);

        /* An exponential moving average. */
        peer->jitter_ms = peer->rtt_ms == 0.0F ? 0.0F : (peer->jitter_ms * 0.9F) + (fabsf(sample - peer->rtt_ms) * 0.1F);
        peer->rtt_ms    = peer->rtt_ms == 0.0F ? sample : (peer->rtt_ms * 0.9F) + (sample * 0.1F);

        peer->sent_at_ms[slot] = 0; // acknowledged once; duplicate acks do not count
        peer->acked_window++;
    }

}

void _nya_net_udp_retire_reliable(_NYA_NetUdpPeer* peer, NYA_Arena* allocator, u16 reliable_ack) {
    /* An acknowledgement ahead of anything outstanding is refused. */
    if (peer->outgoing_reliable->length > 0) {
        u16 oldest = peer->outgoing_reliable->items[0].message_id;

        if (_nya_net_udp_sequence_newer(reliable_ack, (u16)(oldest + NYA_NET_MAX_RELIABLE_IN_FLIGHT))) {
            nya_log_debug("Ignoring an implausible reliable acknowledgement (%u against an oldest of %u).", reliable_ack, oldest);
            return;
        }
    }

    /* Everything the peer says it delivered stops being retransmitted. */
    for (u64 i = peer->outgoing_reliable->length; i > 0; i--) {
        _NYA_NetUdpReliable* message = &peer->outgoing_reliable->items[i - 1];

        if (!_nya_net_udp_sequence_newer(reliable_ack, message->message_id)) continue;

        // the retransmit copy is freed.
        if (message->data != nullptr) nya_arena_free(allocator, message->data, message->size);

        nya_array_remove(peer->outgoing_reliable, i - 1);
    }
}

b8 _nya_net_udp_is_seen(const _NYA_NetUdpPeer* peer, NYA_NetChannel channel, u16 message_id) {
    nya_assert(channel < NYA_NET_CHANNEL_COUNT);

    if (!peer->seen_any[channel]) return false;

    u16 newest = peer->seen_newest[channel];
    if (message_id != newest && _nya_net_udp_sequence_newer(message_id, newest)) return false;

    u16 offset = (u16)(newest - message_id);
    if (offset >= _NYA_NET_UDP_SEEN_WINDOW) return true;

    return (peer->seen[channel][offset / _NYA_NET_UDP_SEEN_WORD_BITS] >> (offset % _NYA_NET_UDP_SEEN_WORD_BITS)) & 1U;
}

void _nya_net_udp_mark_seen(_NYA_NetUdpPeer* peer, NYA_NetChannel channel, u16 message_id) {
    nya_assert(channel < NYA_NET_CHANNEL_COUNT);

    u64* window = peer->seen[channel];
    u32  words  = _NYA_NET_UDP_SEEN_WINDOW / _NYA_NET_UDP_SEEN_WORD_BITS;

    if (!peer->seen_any[channel] || _nya_net_udp_sequence_newer(message_id, peer->seen_newest[channel])) {
        u32 shift = peer->seen_any[channel] ? (u16)(message_id - peer->seen_newest[channel]) : _NYA_NET_UDP_SEEN_WINDOW;

        // slides every mark `shift` places into the past.
        u32 word_shift = shift / _NYA_NET_UDP_SEEN_WORD_BITS;
        u32 bit_shift  = shift % _NYA_NET_UDP_SEEN_WORD_BITS;

        for (u32 i = words; i > 0; i--) {
            u32 to   = i - 1;
            u64 bits = 0;

            if (to >= word_shift) {
                u32 from = to - word_shift;
                bits     = window[from] << bit_shift;
                if (bit_shift != 0 && from > 0) bits |= window[from - 1] >> (_NYA_NET_UDP_SEEN_WORD_BITS - bit_shift);
            }

            window[to] = bits;
        }

        peer->seen_newest[channel] = message_id;
        peer->seen_any[channel]    = true;
    }

    u16 offset = (u16)(peer->seen_newest[channel] - message_id);
    if (offset < _NYA_NET_UDP_SEEN_WINDOW) window[offset / _NYA_NET_UDP_SEEN_WORD_BITS] |= 1ULL << (offset % _NYA_NET_UDP_SEEN_WORD_BITS);

    nya_assert(_nya_net_udp_is_seen(peer, channel, message_id));
}

b8 _nya_net_udp_reliable_acceptable(const _NYA_NetUdpPeer* peer, u16 message_id) {
    u16 next = peer->next_delivery_id;

    // behind next_delivery_id: already delivered.
    if (message_id != next && !_nya_net_udp_sequence_newer(message_id, next)) return false;

    // beyond the sender's in-flight limit: a lying peer, not a gap.
    return (u16)(message_id - next) < _NYA_NET_UDP_MAX_REORDER;
}

/*
 * ─────────────────────────────────────────────────────────
 * BYTES
 * ─────────────────────────────────────────────────────────
 */

b8 _nya_net_udp_sequence_newer(u16 a, u16 b) {
    // half the sequence space is the largest unambiguous window, and nothing here has 32768 packets in flight.
    return ((a > b) && (a - b <= 32768)) || ((b > a) && (b - a > 32768));
}

void _nya_net_udp_write_u16(u8* out, u16 value) {
    out[0] = (u8)(value & 0xFF);
    out[1] = (u8)((value >> 8) & 0xFF);
}

void _nya_net_udp_write_u32(u8* out, u32 value) {
    out[0] = (u8)(value & 0xFF);
    out[1] = (u8)((value >> 8) & 0xFF);
    out[2] = (u8)((value >> 16) & 0xFF);
    out[3] = (u8)((value >> 24) & 0xFF);
}

u16 _nya_net_udp_read_u16(const u8* in) {
    return (u16)((u16)in[0] | ((u16)in[1] << 8));
}

u32 _nya_net_udp_read_u32(const u8* in) {
    return (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
}
