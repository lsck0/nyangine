#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

#include "SDL3_net/SDL_net.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE WIRE FORMAT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Handshake packets start with the protocol word and a kind, and are the only thing a stranger may send:
 *
 * ```
 * CONNECT    client  protocol u32, kind u8, zeros to 64 bytes
 * CHALLENGE  server  protocol, kind, cookie u64, server key [32]
 * RESPONSE   client  protocol, kind, cookie u64, ephemeral key [32], client key [32] or zeros, tag [16]
 * ACCEPT     server  protocol, kind, ephemeral key [32], tag [16]
 * REFUSED    server  protocol, kind, reason u8
 * ```
 *
 * A server answers CONNECT without keeping anything: the cookie is a keyed hash of the address and a coarse clock,
 * recomputed when it comes back, and CONNECT is padded so a challenge is never larger than what asked for it. Only a
 * RESPONSE with a valid cookie and a valid tag takes a peer slot. See net_crypto.h for the keys.
 *
 * Everything after the handshake is a sealed packet from an established peer's address:
 *
 * ```
 * kind u8, sequence u16, ack u16, ack bits u32, reliable ack u16, fragment count u8     the header, authenticated
 * fragments: channel u8, message id u16, index u16, total u16, length u16, bytes        encrypted
 * tag [16]
 * ```
 *
 * The sequence on the wire is the low 16 bits of a 64 bit counter; the receiver rebuilds the rest from the newest
 * sequence it has, and the whole counter is the nonce. A packet older than the ack window, or already inside it, is a
 * replay and is dropped before it is decrypted.
 */

#define _NYA_NET_UDP_PROTOCOL 0x6E796106U /* "nya" + version 6 */

/**
 * How long destroying a transport waits for a hostname it is still resolving. A name answers in milliseconds, and
 * NXDOMAIN for a made up one is as quick; the wait exists for the resolver thread that has not started yet on a
 * loaded machine. Bounded, since a transport closed mid lookup must not stall the program that closed it.
 * */
#define _NYA_NET_UDP_RESOLVE_WAIT_MS 1000

#define _NYA_NET_UDP_KIND_DATA       0
#define _NYA_NET_UDP_KIND_CONNECT    1
#define _NYA_NET_UDP_KIND_ACCEPT     2
#define _NYA_NET_UDP_KIND_DISCONNECT 3
#define _NYA_NET_UDP_KIND_CHALLENGE  4
#define _NYA_NET_UDP_KIND_RESPONSE   5
#define _NYA_NET_UDP_KIND_REFUSED    6

#define _NYA_NET_UDP_PREFIX_SIZE    5

/** How much of a RESPONSE its tag covers: everything before the tag. */
#define _NYA_NET_UDP_RESPONSE_TAGGED (_NYA_NET_UDP_PREFIX_SIZE + 8 + NYA_NET_KEY_SIZE + NYA_NET_KEY_SIZE)
#define _NYA_NET_UDP_CONNECT_SIZE   64
#define _NYA_NET_UDP_CHALLENGE_SIZE (_NYA_NET_UDP_PREFIX_SIZE + 8 + NYA_NET_KEY_SIZE)
#define _NYA_NET_UDP_RESPONSE_SIZE  (_NYA_NET_UDP_RESPONSE_TAGGED + _NYA_NET_MAC_SIZE)
#define _NYA_NET_UDP_ACCEPT_SIZE    (_NYA_NET_UDP_PREFIX_SIZE + NYA_NET_KEY_SIZE + _NYA_NET_MAC_SIZE)
#define _NYA_NET_UDP_REFUSED_SIZE   (_NYA_NET_UDP_PREFIX_SIZE + 1)

#define _NYA_NET_UDP_HEADER_SIZE          12
#define _NYA_NET_UDP_FRAGMENT_HEADER_SIZE 9

/** Payload bytes one fragment may carry in a full datagram. */
#define _NYA_NET_UDP_FRAGMENT_PAYLOAD (NYA_NET_MAX_DATAGRAM - _NYA_NET_UDP_HEADER_SIZE - _NYA_NET_MAC_SIZE - _NYA_NET_UDP_FRAGMENT_HEADER_SIZE)

/** The nonce ACCEPT is sealed with. Data counters start at one and never get near it. */
#define _NYA_NET_UDP_ACCEPT_COUNTER U64_MAX

/** How long without an authenticated packet before a peer is considered gone. */
#define _NYA_NET_UDP_TIMEOUT_MS 10000

/** How often to send a keepalive when nothing else is going out, so a quiet peer does not time out. */
#define _NYA_NET_UDP_KEEPALIVE_MS 1000

/** How long a received message may wait for something outgoing to carry its acknowledgement before one is sent alone. */
#define _NYA_NET_UDP_ACK_DELAY_MS 20

/** Resend delay for an unacknowledged reliable message: this before any round trip is measured, then clamped to the range. */
#define _NYA_NET_UDP_RESEND_INITIAL_MS 250
#define _NYA_NET_UDP_RESEND_MIN_MS     40
#define _NYA_NET_UDP_RESEND_MAX_MS     1000

/** How long a connect attempt runs before giving up, and how often it repeats itself. */
#define _NYA_NET_UDP_CONNECT_TIMEOUT_MS 5000
#define _NYA_NET_UDP_CONNECT_RETRY_MS   250

/** How long a cookie stays valid, in milliseconds. The previous window is accepted too. */
#define _NYA_NET_UDP_COOKIE_WINDOW_MS 20000

/**
 * How many message ids behind the newest one are remembered per channel, for duplicate suppression. An id further
 * back than this is treated as a duplicate: nothing legitimate arrives that late.
 * */
#define _NYA_NET_UDP_SEEN_WINDOW    1024
#define _NYA_NET_UDP_SEEN_WORD_BITS 64

/** How many packets back the ack bitfield, and so the replay window, reaches. One per bit of a u32. */
#define _NYA_NET_UDP_ACK_WINDOW 32

/** How many sent packets are remembered for the round trip and the loss estimate. A power of two dividing 65536. */
#define _NYA_NET_UDP_SENT_WINDOW 64

/** The most datagrams one poll drains before returning. */
#define _NYA_NET_UDP_MAX_RECEIVE_PER_POLL 512

/** Partial reassemblies a peer may have before the oldest is dropped, and how long one may wait for its last piece. */
#define _NYA_NET_UDP_MAX_REASSEMBLY        8
#define _NYA_NET_UDP_REASSEMBLY_TIMEOUT_MS 5000

/** The most fragments one message may be split into. See _NYA_NetUdpReassembly.received. */
#define _NYA_NET_UDP_MAX_FRAGMENTS 512

/** The largest message this transport will reassemble, and the most one peer may tie up in partial ones. */
#define _NYA_NET_UDP_MAX_MESSAGE          (256ULL * 1024ULL)
#define _NYA_NET_UDP_MAX_REASSEMBLY_BYTES (512ULL * 1024ULL)

/** Reliable messages that may wait out of order before a peer is dropped. */
#define _NYA_NET_UDP_MAX_REORDER NYA_NET_MAX_RELIABLE_IN_FLIGHT

/** Handshake packets one address may send per second, and how many it may send at once. Buckets are shared by hash. */
#define _NYA_NET_UDP_HANDSHAKE_RATE    8.0F
#define _NYA_NET_UDP_HANDSHAKE_BURST   16.0F
#define _NYA_NET_UDP_HANDSHAKE_BUCKETS 256

/** Connections one IP address may hold, so one machine cycling source ports cannot fill the server. */
#define _NYA_NET_UDP_MAX_PEERS_PER_ADDRESS 4

/** Datagrams the conditioner can hold back at once. Past it they are dropped, as a full router would. */
#define _NYA_NET_UDP_CONDITIONER_QUEUE 1024

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

    u32 sends;
} _NYA_NetUdpReliable;

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

    u64 started_ms;
} _NYA_NetUdpReassembly;

typedef struct {
    b8  occupied;
    u32 generation;

    NET_Address* address;
    u16          port;

    /** Printed form of address:port, resolved once. */
    char address_text[128];

    /** Whether both sides hold the session keys. Nothing but the handshake is sent or accepted before. */
    b8 established;

    u8 send_key[NYA_NET_KEY_SIZE];
    u8 receive_key[NYA_NET_KEY_SIZE];

    /** The long term key the peer proved it holds. Zero for an anonymous client. */
    u8 remote_key[NYA_NET_KEY_SIZE];

    /** Server side: the client ephemeral key this session answers, and the ACCEPT to repeat when asked again. */
    u8 client_ephemeral[NYA_NET_KEY_SIZE];
    u8 accept[_NYA_NET_UDP_ACCEPT_SIZE];

    /** The next outgoing packet's number. Starts at one, so zero never names a packet. */
    u64 local_sequence;

    /** The newest authenticated packet heard, and which of the 32 before it arrived. */
    u64 remote_sequence;
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

    /** A packet with fragments arrived and nothing has gone out since to acknowledge it. Empty packets never set it, so two peers cannot ping acks back and forth. */
    b8  ack_pending;
    u64 ack_requested_ms;

    NYA_NetPeerStats stats;

    /** Moving averages behind stats.rtt_ms, stats.jitter_ms and stats.packet_loss. */
    f32 rtt_ms;
    f32 jitter_ms;
    f32 loss;

    /** Send time of each recent packet by sequence, zeroed when acknowledged. */
    u64 sent_at_ms[_NYA_NET_UDP_SENT_WINDOW];
    u64 sent_at_sequence[_NYA_NET_UDP_SENT_WINDOW];

    /** Where the current one second rate window started, and the byte counters then. */
    u64 rate_started_ms;
    u64 rate_sent_bytes;
    u64 rate_received_bytes;
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

/** One address's handshake allowance. */
typedef struct {
    u64 address_hash;
    f32 tokens;
    u64 refilled_ms;
} _NYA_NetUdpBucket;

/** A datagram the conditioner is holding back. */
typedef struct {
    NET_Address* address;
    u16          port;
    u16          size;
    u64          due_ms;
    u8           bytes[NYA_NET_MAX_DATAGRAM];
} _NYA_NetUdpDelayed;

typedef struct {
    NYA_Arena* allocator;

    NET_DatagramSocket* socket;

    b8 listening;

    /** This endpoint's long term key: always set on a listening server, optional on a client. */
    NYA_NetKeyPair identity;

    /** The server key a client insists on. Zero accepts the first one presented. */
    u8 server_key[NYA_NET_KEY_SIZE];

    /** The key the connect cookies and the handshake buckets are hashed with, from the system random source. */
    u64 cookie_key_low;
    u64 cookie_key_high;

    /* connecting out */

    b8           connecting;
    NET_Address* connect_address;
    u16          connect_port;
    u64          connect_started_ms;
    u64          connect_last_sent_ms;

    /**
     * The hostname has not come back yet, so there is no socket and no peer.
     *
     * SDL_net resolves on its own thread and NET_GetAddressStatus asks without blocking, so this is
     * polled from the update instead of waited on. Connecting used to wait here for up to the whole
     * connect timeout, five seconds, inside the caller's call — which for a game is five seconds of a
     * frozen frame on a hostname that was misspelled.
     * */
    b8 resolving;

    /** A challenge came back with a key other than the pinned one, so a timeout is reported as an identity failure. */
    b8 identity_refused;

    /** This connection attempt's ephemeral key, wiped when the handshake ends. */
    NYA_NetKeyPair ephemeral;

    /** What the answered challenge produced: the premaster, the server key it named and the RESPONSE to repeat. */
    u8 premaster[NYA_NET_KEY_SIZE];
    u8 presented_key[NYA_NET_KEY_SIZE];
    u8 response[_NYA_NET_UDP_RESPONSE_SIZE];
    b8 has_response;

    _NYA_NetUdpPeer peers[NYA_NET_MAX_PEERS];

    NYA_Arrayᐸ_NYA_NetUdpEventᐳ* events;

    /** How many of `events` poll has handed out. */
    u64 events_read;

    /** Bytes handed out by the last poll, freed by the next. */
    NYA_Arena* delivered;

    _NYA_NetUdpBucket buckets[_NYA_NET_UDP_HANDSHAKE_BUCKETS];

    /* the conditioner */

    NYA_NetConditions   conditions;
    _NYA_NetUdpDelayed* delayed;
    u32                 delayed_count;

    /** The latest due time handed out, so jitter alone delays packets without reordering them. */
    u64 delayed_last_due_ms;

    /** xorshift state for the conditioner's dice, never zero. */
    u64 dice;

    /** Scratch for building one outgoing datagram. */
    u8 send_buffer[NYA_NET_MAX_DATAGRAM];
} _NYA_NetUdpState;

NYA_INTERNAL NYA_Error        _nya_net_udp_listen(NYA_NetTransport* transport, u16 port);
NYA_INTERNAL NYA_Error        _nya_net_udp_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port);
NYA_INTERNAL NYA_Error        _nya_net_udp_send(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetChannel channel, const u8* data, u64 size);
NYA_INTERNAL b8               _nya_net_udp_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event);
NYA_INTERNAL void             _nya_net_udp_disconnect(NYA_NetTransport* transport, NYA_NetPeerId peer, NYA_NetDisconnect reason);
NYA_INTERNAL NYA_NetPeerStats _nya_net_udp_stats(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL NYA_ConstCString _nya_net_udp_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void             _nya_net_udp_condition(NYA_NetTransport* transport, NYA_NetConditions conditions);
NYA_INTERNAL const u8*        _nya_net_udp_public_key(NYA_NetTransport* transport);
NYA_INTERNAL const u8*        _nya_net_udp_peer_key(NYA_NetTransport* transport, NYA_NetPeerId peer);
NYA_INTERNAL void             _nya_net_udp_destroy(NYA_NetTransport* transport);

/** Reads whatever datagrams are waiting and turns them into events. */
NYA_INTERNAL void _nya_net_udp_receive(NYA_NetTransport* transport);

/** Retransmits, keepalives, timeouts and held back datagrams: everything driven by time rather than by a packet. */
NYA_INTERNAL void _nya_net_udp_update(NYA_NetTransport* transport);

/** One handshake packet, from a known peer's address (`peer_index`) or a stranger's (NYA_NET_MAX_PEERS). */
NYA_INTERNAL void _nya_net_udp_handle_handshake(NYA_NetTransport* transport, u32 peer_index, NET_Address* address, u16 port, const u8* data, u64 size);

/** A RESPONSE from a stranger: the only way a peer slot is ever taken on a server. */
NYA_INTERNAL void _nya_net_udp_handle_response(NYA_NetTransport* transport, NET_Address* address, u16 port, const u8* data);

/** A client's side of CHALLENGE and ACCEPT. */
NYA_INTERNAL void _nya_net_udp_handle_challenge(NYA_NetTransport* transport, u32 peer_index, const u8* data);
NYA_INTERNAL void _nya_net_udp_handle_accept(NYA_NetTransport* transport, u32 peer_index, const u8* data);

/** One sealed packet from an established peer. Decrypted in place. */
NYA_INTERNAL void _nya_net_udp_handle_packet(NYA_NetTransport* transport, u32 peer_index, u8* data, u64 size);

/** Resends whatever reliable messages are due. */
NYA_INTERNAL void _nya_net_udp_flush(NYA_NetTransport* transport, u32 peer_index);

/**
 * Seals and sends the packet whose body is already at send_buffer + _NYA_NET_UDP_HEADER_SIZE.
 * */
NYA_INTERNAL void _nya_net_udp_send_packet(NYA_NetTransport* transport, u32 peer_index, u8 kind, u8 fragment_count, u64 body_size);

/** Appends one fragment's header and bytes to the body at send_buffer + _NYA_NET_UDP_HEADER_SIZE + `at`, returning the new end. */
NYA_INTERNAL u64 _nya_net_udp_write_fragment(
    _NYA_NetUdpState* state, u64 at, NYA_NetChannel channel, u16 message_id, u16 index, u16 total, const u8* data, u16 size
);

/** Sends one datagram, or hands it to the conditioner. */
NYA_INTERNAL void _nya_net_udp_transmit(_NYA_NetUdpState* state, NET_Address* address, u16 port, const u8* data, u64 size);

/** A roll in [0, 1) for the conditioner. */
NYA_INTERNAL f32 _nya_net_udp_roll(_NYA_NetUdpState* state) __attr_no_discard;

/** Sends whatever the conditioner holds that is due, or everything when `all`. */
NYA_INTERNAL void _nya_net_udp_release_delayed(_NYA_NetUdpState* state, b8 all);

/** The cookie an address must echo to get a peer slot. */
NYA_INTERNAL u64 _nya_net_udp_cookie(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 epoch_offset) __attr_no_discard;

/** Whether `cookie` is one this server would recently have issued to this address. */
NYA_INTERNAL b8 _nya_net_udp_cookie_valid(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 cookie) __attr_no_discard;

/** Whether this address may send another handshake packet now, spending one if so. */
NYA_INTERNAL b8 _nya_net_udp_handshake_allowed(_NYA_NetUdpState* state, NET_Address* address) __attr_no_discard;

/** The peer at `address`:`port`, or NYA_NET_MAX_PEERS when there is none. */
NYA_INTERNAL u32 _nya_net_udp_find_peer(_NYA_NetUdpState* state, NET_Address* address, u16 port) __attr_no_discard;

/** Takes a free peer slot for `address`:`port`, or NYA_NET_MAX_PEERS when the table is full. */
NYA_INTERNAL u32 _nya_net_udp_add_peer(_NYA_NetUdpState* state, NET_Address* address, u16 port) __attr_no_discard;

NYA_INTERNAL void _nya_net_udp_remove_peer(NYA_NetTransport* transport, u32 peer_index, NYA_NetDisconnect reason, b8 notify);

/** Resolves a peer id to a slot index, or NYA_NET_MAX_PEERS when it names nothing live. */
NYA_INTERNAL u32 _nya_net_udp_resolve(_NYA_NetUdpState* state, NYA_NetPeerId peer) __attr_no_discard;

/** Queues an event for poll. */
NYA_INTERNAL void _nya_net_udp_event(_NYA_NetUdpState* state, _NYA_NetUdpEvent event);

/** The whole sequence a 16 bit wire sequence stands for, taken as the candidate closest to `newest`. */
NYA_INTERNAL u64 _nya_net_udp_sequence_expand(u64 newest, u16 wire) __attr_no_discard;

/** Whether `sequence` was already received or is older than the window. Pure. */
NYA_INTERNAL b8 _nya_net_udp_is_replay(const _NYA_NetUdpPeer* peer, u64 sequence) __attr_no_discard;

/** Records an authenticated packet's sequence into the ack bitfield. */
NYA_INTERNAL void _nya_net_udp_record_ack(_NYA_NetUdpPeer* peer, u64 sequence);

/** Applies the packet acknowledgements a peer sent: round trip and loss estimate. */
NYA_INTERNAL void _nya_net_udp_apply_acks(_NYA_NetUdpPeer* peer, u16 ack, u32 ack_bits, u64 now_ms);

/** How long a reliable message waits for its acknowledgement before it is sent again. */
NYA_INTERNAL u64 _nya_net_udp_resend_ms(const _NYA_NetUdpPeer* peer) __attr_no_discard;

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
 * Message ids wrap at 16 bits, so 0 is newer than 65535. a is newer than b when the forward distance is less than
 * half the space.
 */
NYA_INTERNAL b8 _nya_net_udp_sequence_newer(u16 a, u16 b) __attr_no_discard;

/** Little endian readers and writers, so every host produces the same bytes. */
NYA_INTERNAL void _nya_net_udp_write_u16(u8* out, u16 value);
NYA_INTERNAL void _nya_net_udp_write_u32(u8* out, u32 value);
NYA_INTERNAL void _nya_net_udp_write_u64(u8* out, u64 value);
NYA_INTERNAL u16  _nya_net_udp_read_u16(const u8* in) __attr_no_discard;
NYA_INTERNAL u32  _nya_net_udp_read_u32(const u8* in) __attr_no_discard;
NYA_INTERNAL u64  _nya_net_udp_read_u64(const u8* in) __attr_no_discard;

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
    .condition    = &_nya_net_udp_condition,
    .public_key   = &_nya_net_udp_public_key,
    .peer_key     = &_nya_net_udp_peer_key,
    .destroy      = &_nya_net_udp_destroy,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_transport_udp_create(NYA_Arena* arena, NYA_NetUdpOptions options, OUT NYA_NetTransport** out_transport) {
    nya_assert(arena != nullptr);
    nya_assert(out_transport != nullptr);

    *out_transport = nullptr;

    // the cookie key, then the dice seed, which has to be separate: the loss pattern is visible from outside.
    u8 cookie_key[24] = { 0 };
    if (!nya_os_random_bytes(cookie_key, sizeof(cookie_key))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

    if (!NET_Init()) return nya_error(NYA_ERROR_NOT_OK, "SDL_net could not start: %s", SDL_GetError());
    _NYA_NET_UDP_INIT_COUNT++;

    _NYA_NetUdpState* state = nya_arena_alloc(arena, sizeof(_NYA_NetUdpState));

    *state = (_NYA_NetUdpState){
        .allocator       = arena,
        .events          = nya_array_create(arena, _NYA_NetUdpEvent),
        .delivered       = nya_arena_create(.name = "net_udp_delivered"),
        .cookie_key_low  = _nya_net_udp_read_u64(cookie_key),
        .cookie_key_high = _nya_net_udp_read_u64(cookie_key + 8),
        .dice            = _nya_net_udp_read_u64(cookie_key + 16) | 1,
    };

    nya_crypto_wipe(cookie_key, sizeof(cookie_key));

    // the public half is derived rather than trusted, so a caller cannot pair a secret with the wrong public key.
    if (nya_net_key_is_set(options.identity.secret_key)) state->identity = nya_net_key_pair_from_secret(options.identity.secret_key);

    nya_memcpy(state->server_key, options.server_key, NYA_NET_KEY_SIZE);

    NYA_NetTransport* transport = nya_arena_alloc(arena, sizeof(NYA_NetTransport));

    *transport = (NYA_NetTransport){ .vtable = &_NYA_NET_UDP_VTABLE, .allocator = arena, .state = state };

    if (nya_net_conditions_active(options.conditions)) _nya_net_udp_condition(transport, options.conditions);

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

    if (!nya_net_key_is_set(state->identity.secret_key)) NYA_TRY(nya_net_key_pair_create(&state->identity));

    // a null address binds every local interface, so a host on ethernet and wifi is reachable on both.
    state->socket = NET_CreateDatagramSocket(nullptr, port, 0);
    if (state->socket == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not bind UDP port %u: %s", port, SDL_GetError());

    state->listening = true;

    char key[NYA_NET_KEY_HEX_SIZE];
    nya_net_key_to_hex(state->identity.public_key, key);

    nya_log_info("Listening for players on UDP port %u, server key %s.", port, key);

    return NYA_OK;
}

/**
 * Opens the socket and adds the server's peer, once the hostname has resolved.
 *
 * Split out of _nya_net_udp_connect because it can only run after the name is known, and the name is
 * now known from the update rather than from a wait inside connect. False when the socket could not be
 * opened, which the caller turns into a failed connection.
 * */
NYA_INTERNAL b8 _nya_net_udp_resolve_finish(NYA_NetTransport* transport, u64 now_ms) {
    _NYA_NetUdpState* state = transport->state;

    // port zero lets the system pick, so two copies of a game on one machine can both connect.
    state->socket = NET_CreateDatagramSocket(nullptr, 0, 0);
    if (state->socket == nullptr) {
        nya_log_warn("Could not open a UDP socket: %s", SDL_GetError());

        return false;
    }

    // the server's slot exists from here, so its handshake packets are recognised by address.
    const u32 slot = _nya_net_udp_add_peer(state, state->connect_address, state->connect_port);
    nya_assert(slot < NYA_NET_MAX_PEERS, "a client's first peer slot is always free");

    state->resolving = false;

    /*
     * The clock restarts here rather than at connect. Otherwise a slow lookup spends the connection's
     * whole budget before a single packet has been sent, and a player on a slow resolver would see a
     * timeout without the game ever having tried to reach the server.
     */
    state->connect_started_ms   = now_ms;
    state->connect_last_sent_ms = 0;

    return true;
}

NYA_Error _nya_net_udp_connect(NYA_NetTransport* transport, NYA_ConstCString address, u16 port) {
    nya_assert(address != nullptr);

    _NYA_NetUdpState* state = transport->state;

    if (state->socket != nullptr) return nya_error(NYA_ERROR_NOT_OK, "this transport already has a socket");

    /*
     * Returns at once with an address that may still be resolving; NET_GetAddressStatus is how it is
     * asked later. Nothing here blocks, so a caller may connect from a frame without dropping one.
     */
    NET_Address* resolved = NET_ResolveHostname(address);
    if (resolved == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "could not resolve '%s': %s", address, SDL_GetError());

    NYA_Error keyed = nya_net_key_pair_create(&state->ephemeral);
    if (!keyed.ok) {
        NET_UnrefAddress(resolved);
        return keyed;
    }

    /*
     * The socket and the peer wait for the name. A peer is found by address, and an address that has
     * not resolved is not one yet; see _nya_net_udp_resolve_finish, which does both once it has.
     */
    state->connecting           = true;
    state->resolving            = true;
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

    // a courtesy: the datagram may be lost and the peer times out instead. worth it, since a clean exit is the common
    // case and a ten second timeout is poor for everyone else.
    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (!state->peers[i].occupied) continue;

        if (state->peers[i].established) {
            state->send_buffer[_NYA_NET_UDP_HEADER_SIZE] = (u8)NYA_NET_DISCONNECT_SERVER_CLOSED;
            _nya_net_udp_send_packet(transport, i, _NYA_NET_UDP_KIND_DISCONNECT, 0, 1);
        }

        _nya_net_udp_remove_peer(transport, i, NYA_NET_DISCONNECT_REQUESTED, false);
    }

    _nya_net_udp_release_delayed(state, true);

    if (state->connect_address != nullptr) {
        /*
         * SDL_net's NET_Quit drops addresses still queued for its resolver without releasing them (resolver_queue
         * is set to NULL in SDL_net.c at 4dd9d84), which LeakSanitizer reported as 81 bytes from test_transport
         * under load. Waiting out this transport's own lookup first keeps it off that queue. Delete once
         * SDL_net releases the queue itself.
         */
        if (state->resolving) (void)NET_WaitUntilResolved(state->connect_address, _NYA_NET_UDP_RESOLVE_WAIT_MS);

        NET_UnrefAddress(state->connect_address);
        state->connect_address = nullptr;
    }

    if (state->socket != nullptr) {
        NET_DestroyDatagramSocket(state->socket);
        state->socket = nullptr;
    }

    nya_arena_destroy(state->delivered);

    nya_crypto_wipe(&state->identity, sizeof(state->identity));
    nya_crypto_wipe(&state->ephemeral, sizeof(state->ephemeral));
    nya_crypto_wipe(state->premaster, sizeof(state->premaster));

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

    if (!connection->established) return nya_error(NYA_ERROR_NOT_OK, "the handshake with this peer has not finished");

    u64 fragments = (size + _NYA_NET_UDP_FRAGMENT_PAYLOAD - 1) / _NYA_NET_UDP_FRAGMENT_PAYLOAD;

    if (fragments > _NYA_NET_UDP_MAX_FRAGMENTS || size > _NYA_NET_UDP_MAX_MESSAGE) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a %llu byte message is past the %llu byte limit", (unsigned long long)size,
                         (unsigned long long)_NYA_NET_UDP_MAX_MESSAGE);
    }

    u16 message_id = connection->next_message_id[channel]++;

    if (channel == NYA_NET_CHANNEL_RELIABLE) {
        // queued whole and fragmented at transmit.
        if (connection->outgoing_reliable->length >= NYA_NET_MAX_RELIABLE_IN_FLIGHT) {
            // the peer stopped acknowledging; it is gone, the timeout just has not fired.
            _nya_net_udp_remove_peer(transport, index, NYA_NET_DISCONNECT_TIMEOUT, true);
            return nya_error(NYA_ERROR_NOT_OK, "peer stopped acknowledging; dropped");
        }

        u8* copy = nya_arena_alloc(state->allocator, size);
        nya_memcpy(copy, data, size);

        nya_array_push_back(connection->outgoing_reliable, ((_NYA_NetUdpReliable){ .message_id = message_id, .data = copy, .size = size }));

        _nya_net_udp_flush(transport, index);

        return NYA_OK;
    }

    // unreliable goes out immediately and is never kept.
    u64 offset = 0;

    for (u16 fragment = 0; fragment < (u16)fragments; fragment++) {
        u16 chunk = (u16)nya_min((u64)_NYA_NET_UDP_FRAGMENT_PAYLOAD, size - offset);

        u64 body = _nya_net_udp_write_fragment(state, 0, channel, message_id, fragment, (u16)fragments, data + offset, chunk);
        _nya_net_udp_send_packet(transport, index, _NYA_NET_UDP_KIND_DATA, 1, body);

        offset += chunk;
    }

    return NYA_OK;
}

void _nya_net_udp_flush(NYA_NetTransport* transport, u32 peer_index) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    u64 now_ms    = nya_clock_get_monotonic_ms();
    u64 resend_ms = _nya_net_udp_resend_ms(connection);

    nya_array_foreach (connection->outgoing_reliable, pending) {
        if (pending->sends > 0 && _nya_net_elapsed_ms(now_ms, pending->last_sent_ms) < resend_ms) continue;

        if (pending->sends > 0) connection->stats.retransmits++;

        u64 fragments = (pending->size + _NYA_NET_UDP_FRAGMENT_PAYLOAD - 1) / _NYA_NET_UDP_FRAGMENT_PAYLOAD;
        u64 offset    = 0;

        for (u16 fragment = 0; fragment < (u16)fragments; fragment++) {
            u16 chunk = (u16)nya_min((u64)_NYA_NET_UDP_FRAGMENT_PAYLOAD, pending->size - offset);

            u64 body = _nya_net_udp_write_fragment(state, 0, NYA_NET_CHANNEL_RELIABLE, pending->message_id, fragment, (u16)fragments,
                                                   pending->data + offset, chunk);
            _nya_net_udp_send_packet(transport, peer_index, _NYA_NET_UDP_KIND_DATA, 1, body);

            offset += chunk;
        }

        pending->last_sent_ms = now_ms;
        pending->sends++;
    }
}

u64 _nya_net_udp_write_fragment(_NYA_NetUdpState* state, u64 at, NYA_NetChannel channel, u16 message_id, u16 index, u16 total, const u8* data, u16 size) {
    nya_assert(_NYA_NET_UDP_HEADER_SIZE + at + _NYA_NET_UDP_FRAGMENT_HEADER_SIZE + size + _NYA_NET_MAC_SIZE <= NYA_NET_MAX_DATAGRAM);

    u8* out = state->send_buffer + _NYA_NET_UDP_HEADER_SIZE + at;

    out[0] = (u8)channel;
    _nya_net_udp_write_u16(out + 1, message_id);
    _nya_net_udp_write_u16(out + 3, index);
    _nya_net_udp_write_u16(out + 5, total);
    _nya_net_udp_write_u16(out + 7, size);

    nya_memcpy(out + _NYA_NET_UDP_FRAGMENT_HEADER_SIZE, data, size);

    return at + _NYA_NET_UDP_FRAGMENT_HEADER_SIZE + size;
}

void _nya_net_udp_send_packet(NYA_NetTransport* transport, u32 peer_index, u8 kind, u8 fragment_count, u64 body_size) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    nya_assert(connection->established);
    nya_assert(_NYA_NET_UDP_HEADER_SIZE + body_size + _NYA_NET_MAC_SIZE <= NYA_NET_MAX_DATAGRAM);

    u64 now_ms   = nya_clock_get_monotonic_ms();
    u64 sequence = connection->local_sequence++;

    u8* buffer = state->send_buffer;

    buffer[0] = kind;
    _nya_net_udp_write_u16(buffer + 1, (u16)sequence);
    _nya_net_udp_write_u16(buffer + 3, (u16)connection->remote_sequence);
    _nya_net_udp_write_u32(buffer + 5, connection->ack_bits);
    _nya_net_udp_write_u16(buffer + 9, connection->next_delivery_id);
    buffer[11] = fragment_count;

    u8* body = buffer + _NYA_NET_UDP_HEADER_SIZE;
    _nya_net_crypto_seal(connection->send_key, sequence, buffer, _NYA_NET_UDP_HEADER_SIZE, body, body_size, body + body_size);

    // the slot's previous packet is judged as it leaves the window: acknowledged by now, or lost.
    u32 slot = (u32)(sequence % _NYA_NET_UDP_SENT_WINDOW);

    if (connection->sent_at_sequence[slot] != 0) {
        f32 lost         = connection->sent_at_ms[slot] != 0 ? 1.0F : 0.0F;
        connection->loss = (connection->loss * 0.98F) + (lost * 0.02F);
    }

    connection->sent_at_ms[slot]       = now_ms;
    connection->sent_at_sequence[slot] = sequence;

    u64 size = _NYA_NET_UDP_HEADER_SIZE + body_size + _NYA_NET_MAC_SIZE;

    _nya_net_udp_transmit(state, connection->address, connection->port, buffer, size);

    connection->stats.bytes_sent += size;
    connection->stats.packets_sent++;
    connection->last_sent_ms = now_ms;
    connection->ack_pending  = false;
}

void _nya_net_udp_transmit(_NYA_NetUdpState* state, NET_Address* address, u16 port, const u8* data, u64 size) {
    nya_assert(size <= NYA_NET_MAX_DATAGRAM);

    if (state->socket == nullptr || address == nullptr) return;

    if (state->delayed == nullptr || !nya_net_conditions_active(state->conditions)) {
        // a failed send is a full buffer or a blip, not a dead peer. the timeout decides that.
        if (!NET_SendDatagram(state->socket, address, port, data, (int)size)) nya_log_debug("UDP send failed: %s", SDL_GetError());
        return;
    }

    const NYA_NetConditions* conditions = &state->conditions;

    if (_nya_net_udp_roll(state) * 100.0F < conditions->loss_percent) return;

    u32 copies = _nya_net_udp_roll(state) * 100.0F < conditions->duplicate_percent ? 2 : 1;
    u64 now_ms = nya_clock_get_monotonic_ms();

    for (u32 copy = 0; copy < copies; copy++) {
        if (state->delayed_count >= _NYA_NET_UDP_CONDITIONER_QUEUE) return;

        f32 delay_ms = (f32)conditions->latency_ms + (((_nya_net_udp_roll(state) * 2.0F) - 1.0F) * (f32)conditions->jitter_ms);

        u64 due_ms = now_ms + (u64)nya_max(delay_ms, 0.0F);

        // jitter alone stretches the gaps between packets without swapping them. a reordered one is held past the next.
        if (_nya_net_udp_roll(state) * 100.0F < conditions->reorder_percent) {
            due_ms += (u64)conditions->jitter_ms + (u64)(conditions->latency_ms / 2) + 20;
        } else {
            due_ms                     = nya_max(due_ms, state->delayed_last_due_ms);
            state->delayed_last_due_ms = due_ms;
        }

        _NYA_NetUdpDelayed* held = &state->delayed[state->delayed_count++];

        held->address = NET_RefAddress(address);
        held->port    = port;
        held->size    = (u16)size;
        held->due_ms  = due_ms;
        nya_memcpy(held->bytes, data, size);
    }
}

f32 _nya_net_udp_roll(_NYA_NetUdpState* state) {
    state->dice ^= state->dice << 13;
    state->dice ^= state->dice >> 7;
    state->dice ^= state->dice << 17;

    return (f32)(state->dice >> 40) / (f32)(1ULL << 24);
}

void _nya_net_udp_release_delayed(_NYA_NetUdpState* state, b8 all) {
    if (state->delayed_count == 0) return;

    u64 now_ms = nya_clock_get_monotonic_ms();
    u32 kept   = 0;

    for (u32 i = 0; i < state->delayed_count; i++) {
        _NYA_NetUdpDelayed* held = &state->delayed[i];

        if (!all && held->due_ms > now_ms) {
            // compacted in order, so packets due at the same moment leave in the order they were sent.
            if (kept != i) state->delayed[kept] = *held;
            kept++;
            continue;
        }

        if (state->socket != nullptr) (void)NET_SendDatagram(state->socket, held->address, held->port, held->bytes, (int)held->size);

        NET_UnrefAddress(held->address);
    }

    state->delayed_count = kept;
}

void _nya_net_udp_condition(NYA_NetTransport* transport, NYA_NetConditions conditions) {
    _NYA_NetUdpState* state = transport->state;

    state->conditions = conditions;

    // allocated the first time a condition is set, since a clean wire should cost no memory.
    if (nya_net_conditions_active(conditions) && state->delayed == nullptr) {
        state->delayed = nya_arena_alloc(state->allocator, _NYA_NET_UDP_CONDITIONER_QUEUE * sizeof(_NYA_NetUdpDelayed));
    }

    if (!nya_net_conditions_active(conditions)) _nya_net_udp_release_delayed(state, true);
}

u64 _nya_net_udp_cookie(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 epoch_offset) {
    int         address_size  = 0;
    const void* address_bytes = NET_GetAddressBytes(address, &address_size);

    u64 epoch = (nya_clock_get_monotonic_ms() / _NYA_NET_UDP_COOKIE_WINDOW_MS) - epoch_offset;

    u8  material[64] = { 0 };
    u64 at           = 0;

    if (address_bytes != nullptr && address_size > 0) {
        u64 copied = nya_min((u64)address_size, sizeof(material) - 16);

        nya_memcpy(material, address_bytes, copied);
        at += copied;
    }

    _nya_net_udp_write_u16(material + at, port);
    at += 2;
    _nya_net_udp_write_u64(material + at, epoch);
    at += 8;

    u64 cookie = nya_siphash(material, at, state->cookie_key_low, state->cookie_key_high);

    // zero means "no cookie", so a zero hash is nudged.
    return cookie == 0 ? 1 : cookie;
}

b8 _nya_net_udp_cookie_valid(_NYA_NetUdpState* state, NET_Address* address, u16 port, u64 cookie) {
    if (cookie == 0) return false;

    // current and previous window, so a response crossing a boundary is still accepted.
    return cookie == _nya_net_udp_cookie(state, address, port, 0) || cookie == _nya_net_udp_cookie(state, address, port, 1);
}

b8 _nya_net_udp_handshake_allowed(_NYA_NetUdpState* state, NET_Address* address) {
    int         address_size  = 0;
    const void* address_bytes = NET_GetAddressBytes(address, &address_size);

    // the address without the port: a port is free to change, an address is what an attacker has few of.
    u64 hash = address_bytes != nullptr && address_size > 0 ? nya_siphash(address_bytes, (u64)address_size, state->cookie_key_high, state->cookie_key_low) : 0;

    _NYA_NetUdpBucket* bucket = &state->buckets[hash % _NYA_NET_UDP_HANDSHAKE_BUCKETS];

    u64 now_ms = nya_clock_get_monotonic_ms();

    if (bucket->address_hash != hash || bucket->refilled_ms == 0) {
        *bucket = (_NYA_NetUdpBucket){ .address_hash = hash, .tokens = _NYA_NET_UDP_HANDSHAKE_BURST, .refilled_ms = now_ms };
    }

    bucket->tokens      = nya_min(_NYA_NET_UDP_HANDSHAKE_BURST, bucket->tokens + ((f32)_nya_net_elapsed_ms(now_ms, bucket->refilled_ms) * _NYA_NET_UDP_HANDSHAKE_RATE / 1000.0F));
    bucket->refilled_ms = now_ms;

    if (bucket->tokens < 1.0F) return false;

    bucket->tokens -= 1.0F;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────
 * RECEIVING
 * ─────────────────────────────────────────────────────────
 */

b8 _nya_net_udp_poll(NYA_NetTransport* transport, OUT NYA_NetTransportEvent* out_event) {
    _NYA_NetUdpState* state = transport->state;

    // only once every event is handed out, so the arena is not reset under unread ones.
    if (state->events_read == state->events->length) {
        state->events->length = 0;
        state->events_read    = 0;

        nya_arena_free_all(state->delivered);

        _nya_net_udp_receive(transport);
        _nya_net_udp_update(transport);
    }

    if (state->events_read == state->events->length) return false;

    _NYA_NetUdpEvent event = state->events->items[state->events_read++];

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

    // bounded, so a flood cannot stall the frame loop. the rest waits in the socket buffer.
    for (u32 drained = 0; drained < _NYA_NET_UDP_MAX_RECEIVE_PER_POLL; drained++) {
        NET_Datagram* datagram = nullptr;

        // false is an error; a null datagram with true is simply nothing waiting.
        if (!NET_ReceiveDatagram(state->socket, &datagram)) {
            nya_log_debug("UDP receive failed: %s", SDL_GetError());
            return;
        }

        if (datagram == nullptr) return;

        u64 size  = datagram->buflen > 0 ? (u64)datagram->buflen : 0;
        u32 index = _nya_net_udp_find_peer(state, datagram->addr, datagram->port);

        // oversized is dropped before anything reads it. short or foreign packets on a shared port are normal.
        if (size > NYA_NET_MAX_DATAGRAM || size == 0) {
            nya_log_debug("Dropping a %llu byte datagram.", (unsigned long long)size);
        } else if (size >= _NYA_NET_UDP_PREFIX_SIZE && _nya_net_udp_read_u32(datagram->buf) == _NYA_NET_UDP_PROTOCOL) {
            _nya_net_udp_handle_handshake(transport, index, datagram->addr, datagram->port, datagram->buf, size);
        } else if (index < NYA_NET_MAX_PEERS) {
            _nya_net_udp_handle_packet(transport, index, datagram->buf, size);
        }

        NET_DestroyDatagram(datagram);
    }
}

void _nya_net_udp_handle_handshake(NYA_NetTransport* transport, u32 peer_index, NET_Address* address, u16 port, const u8* data, u64 size) {
    _NYA_NetUdpState* state = transport->state;

    u8 kind = data[4];

    if (state->listening) {
        // every handshake packet a server answers is paid for from the address's allowance, before any work.
        if (kind != _NYA_NET_UDP_KIND_CONNECT && kind != _NYA_NET_UDP_KIND_RESPONSE) return;
        if (!_nya_net_udp_handshake_allowed(state, address)) return;

        // padded, so the challenge never amplifies what asked for it.
        if (kind == _NYA_NET_UDP_KIND_CONNECT && size >= _NYA_NET_UDP_CONNECT_SIZE) {
            u8 challenge[_NYA_NET_UDP_CHALLENGE_SIZE] = { 0 };

            _nya_net_udp_write_u32(challenge, _NYA_NET_UDP_PROTOCOL);
            challenge[4] = _NYA_NET_UDP_KIND_CHALLENGE;
            _nya_net_udp_write_u64(challenge + 5, _nya_net_udp_cookie(state, address, port, 0));
            nya_memcpy(challenge + 13, state->identity.public_key, NYA_NET_KEY_SIZE);

            _nya_net_udp_transmit(state, address, port, challenge, sizeof(challenge));
            return;
        }

        if (kind != _NYA_NET_UDP_KIND_RESPONSE || size != _NYA_NET_UDP_RESPONSE_SIZE) return;

        if (peer_index < NYA_NET_MAX_PEERS) {
            // a repeat of the RESPONSE this session answered means the ACCEPT was lost. anything else waits for the timeout.
            _NYA_NetUdpPeer* connection = &state->peers[peer_index];

            if (nya_memcmp(connection->client_ephemeral, data + 13, NYA_NET_KEY_SIZE) == 0) {
                _nya_net_udp_transmit(state, address, port, connection->accept, _NYA_NET_UDP_ACCEPT_SIZE);
            }

            return;
        }

        _nya_net_udp_handle_response(transport, address, port, data);
        return;
    }

    // a client only listens to the server it is connecting to.
    if (!state->connecting || peer_index >= NYA_NET_MAX_PEERS) return;

    if (kind == _NYA_NET_UDP_KIND_CHALLENGE && size == _NYA_NET_UDP_CHALLENGE_SIZE) _nya_net_udp_handle_challenge(transport, peer_index, data);
    if (kind == _NYA_NET_UDP_KIND_ACCEPT && size == _NYA_NET_UDP_ACCEPT_SIZE) _nya_net_udp_handle_accept(transport, peer_index, data);

    if (kind == _NYA_NET_UDP_KIND_REFUSED && size == _NYA_NET_UDP_REFUSED_SIZE && data[5] < NYA_NET_DISCONNECT_COUNT) {
        // unauthenticated, so only ever taken as a reason to stop trying, never as the end of an established session.
        state->connecting = false;

        _nya_net_udp_remove_peer(transport, peer_index, NYA_NET_DISCONNECT_REQUESTED, false);
        _nya_net_udp_event(state, (_NYA_NetUdpEvent){ .kind = NYA_NET_TRANSPORT_EVENT_DISCONNECTED, .reason = (NYA_NetDisconnect)data[5] });
    }
}

void _nya_net_udp_handle_response(NYA_NetTransport* transport, NET_Address* address, u16 port, const u8* data) {
    _NYA_NetUdpState* state = transport->state;

    if (!_nya_net_udp_cookie_valid(state, address, port, _nya_net_udp_read_u64(data + 5))) return;

    u32 same_address = 0;
    b8  full         = true;

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        if (!state->peers[i].occupied) {
            full = false;
            continue;
        }

        if (NET_CompareAddresses(state->peers[i].address, address) == 0) same_address++;
    }

    if (full || same_address >= _NYA_NET_UDP_MAX_PEERS_PER_ADDRESS) {
        u8 refused[_NYA_NET_UDP_REFUSED_SIZE] = { 0 };

        _nya_net_udp_write_u32(refused, _NYA_NET_UDP_PROTOCOL);
        refused[4] = _NYA_NET_UDP_KIND_REFUSED;
        refused[5] = (u8)NYA_NET_DISCONNECT_FULL;

        _nya_net_udp_transmit(state, address, port, refused, sizeof(refused));
        return;
    }

    const u8* client_ephemeral = data + 13;
    const u8* client_static    = data + 13 + NYA_NET_KEY_SIZE;
    const u8* mac              = data + _NYA_NET_UDP_RESPONSE_TAGGED;

    u8 dh_ephemeral_static[NYA_NET_KEY_SIZE] = { 0 };
    u8 dh_static_static[NYA_NET_KEY_SIZE]    = { 0 };
    u8 premaster[NYA_NET_KEY_SIZE]           = { 0 };
    u8 response_key[NYA_NET_KEY_SIZE]        = { 0 };
    u8 dh_ephemeral[NYA_NET_KEY_SIZE]        = { 0 };

    defer {
        nya_crypto_wipe(dh_ephemeral_static, sizeof(dh_ephemeral_static));
        nya_crypto_wipe(dh_static_static, sizeof(dh_static_static));
        nya_crypto_wipe(premaster, sizeof(premaster));
        nya_crypto_wipe(response_key, sizeof(response_key));
        nya_crypto_wipe(dh_ephemeral, sizeof(dh_ephemeral));
    }

    if (!_nya_net_crypto_exchange(dh_ephemeral_static, state->identity.secret_key, client_ephemeral)) return;

    b8 has_client_key = nya_net_key_is_set(client_static);
    if (has_client_key && !_nya_net_crypto_exchange(dh_static_static, state->identity.secret_key, client_static)) return;

    _nya_net_crypto_premaster(premaster, dh_ephemeral_static, dh_static_static, state->identity.public_key, client_ephemeral, client_static);
    _nya_net_crypto_response_key(response_key, premaster);

    if (!_nya_net_crypto_open(response_key, 0, data, _NYA_NET_UDP_RESPONSE_TAGGED, nullptr, 0, mac)) return;

    NYA_NetKeyPair server_ephemeral = { 0 };
    defer nya_crypto_wipe(&server_ephemeral, sizeof(server_ephemeral));

    if (!nya_net_key_pair_create(&server_ephemeral).ok) return;
    if (!_nya_net_crypto_exchange(dh_ephemeral, server_ephemeral.secret_key, client_ephemeral)) return;

    u32 added = _nya_net_udp_add_peer(state, address, port);
    nya_assert(added < NYA_NET_MAX_PEERS, "a free slot was counted above");

    _NYA_NetUdpPeer* connection = &state->peers[added];

    _nya_net_crypto_session(connection->receive_key, connection->send_key, premaster, dh_ephemeral, server_ephemeral.public_key);

    nya_memcpy(connection->client_ephemeral, client_ephemeral, NYA_NET_KEY_SIZE);
    nya_memcpy(connection->remote_key, client_static, NYA_NET_KEY_SIZE);

    u8* accept = connection->accept;

    _nya_net_udp_write_u32(accept, _NYA_NET_UDP_PROTOCOL);
    accept[4] = _NYA_NET_UDP_KIND_ACCEPT;
    nya_memcpy(accept + 5, server_ephemeral.public_key, NYA_NET_KEY_SIZE);

    _nya_net_crypto_seal(connection->send_key, _NYA_NET_UDP_ACCEPT_COUNTER, accept, 5 + NYA_NET_KEY_SIZE, nullptr, 0, accept + 5 + NYA_NET_KEY_SIZE);

    connection->established = true;

    _nya_net_udp_transmit(state, address, port, accept, _NYA_NET_UDP_ACCEPT_SIZE);

    _nya_net_udp_event(state, (_NYA_NetUdpEvent){
        .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED,
        .peer = { .index = added, .generation = connection->generation },
    });
}

void _nya_net_udp_handle_challenge(NYA_NetTransport* transport, u32 peer_index, const u8* data) {
    _NYA_NetUdpState* state = transport->state;

    const u8* server_key = data + 13;

    if (nya_net_key_is_set(state->server_key) && nya_memcmp(state->server_key, server_key, NYA_NET_KEY_SIZE) != 0) {
        // ignored rather than fatal, since a stranger can forge a challenge. the timeout reports it.
        if (!state->identity_refused) nya_log_warn("The server at %s presented a key other than the one expected.", state->peers[peer_index].address_text);

        state->identity_refused = true;
        return;
    }

    u8 dh_ephemeral_static[NYA_NET_KEY_SIZE] = { 0 };
    u8 dh_static_static[NYA_NET_KEY_SIZE]    = { 0 };
    u8 response_key[NYA_NET_KEY_SIZE]        = { 0 };

    defer {
        nya_crypto_wipe(dh_ephemeral_static, sizeof(dh_ephemeral_static));
        nya_crypto_wipe(dh_static_static, sizeof(dh_static_static));
        nya_crypto_wipe(response_key, sizeof(response_key));
    }

    if (!_nya_net_crypto_exchange(dh_ephemeral_static, state->ephemeral.secret_key, server_key)) return;

    b8 has_identity = nya_net_key_is_set(state->identity.secret_key);
    if (has_identity && !_nya_net_crypto_exchange(dh_static_static, state->identity.secret_key, server_key)) return;

    u8* response = state->response;

    _nya_net_udp_write_u32(response, _NYA_NET_UDP_PROTOCOL);
    response[4] = _NYA_NET_UDP_KIND_RESPONSE;
    nya_memcpy(response + 5, data + 5, 8);
    nya_memcpy(response + 13, state->ephemeral.public_key, NYA_NET_KEY_SIZE);
    nya_memset(response + 13 + NYA_NET_KEY_SIZE, 0, NYA_NET_KEY_SIZE);
    if (has_identity) nya_memcpy(response + 13 + NYA_NET_KEY_SIZE, state->identity.public_key, NYA_NET_KEY_SIZE);

    _nya_net_crypto_premaster(state->premaster, dh_ephemeral_static, dh_static_static, server_key, state->ephemeral.public_key, response + 13 + NYA_NET_KEY_SIZE);
    _nya_net_crypto_response_key(response_key, state->premaster);
    _nya_net_crypto_seal(response_key, 0, response, _NYA_NET_UDP_RESPONSE_TAGGED, nullptr, 0, response + _NYA_NET_UDP_RESPONSE_TAGGED);

    nya_memcpy(state->presented_key, server_key, NYA_NET_KEY_SIZE);
    state->has_response = true;

    // immediately, not on the retry timer: handshake latency is what the player sees as "connecting".
    _nya_net_udp_transmit(state, state->peers[peer_index].address, state->peers[peer_index].port, response, _NYA_NET_UDP_RESPONSE_SIZE);
    state->connect_last_sent_ms = nya_clock_get_monotonic_ms();
}

void _nya_net_udp_handle_accept(NYA_NetTransport* transport, u32 peer_index, const u8* data) {
    _NYA_NetUdpState* state = transport->state;

    if (!state->has_response) return;

    _NYA_NetUdpPeer* connection = &state->peers[peer_index];

    const u8* server_ephemeral = data + 5;

    u8 dh_ephemeral[NYA_NET_KEY_SIZE] = { 0 };
    u8 send_key[NYA_NET_KEY_SIZE]     = { 0 };
    u8 receive_key[NYA_NET_KEY_SIZE]  = { 0 };

    defer {
        nya_crypto_wipe(dh_ephemeral, sizeof(dh_ephemeral));
        nya_crypto_wipe(send_key, sizeof(send_key));
        nya_crypto_wipe(receive_key, sizeof(receive_key));
    }

    if (!_nya_net_crypto_exchange(dh_ephemeral, state->ephemeral.secret_key, server_ephemeral)) return;

    _nya_net_crypto_session(send_key, receive_key, state->premaster, dh_ephemeral, server_ephemeral);

    // the tag proves the server holds its identity's secret, since the premaster needs it.
    if (!_nya_net_crypto_open(receive_key, _NYA_NET_UDP_ACCEPT_COUNTER, data, 5 + NYA_NET_KEY_SIZE, nullptr, 0, data + 5 + NYA_NET_KEY_SIZE)) return;

    nya_memcpy(connection->send_key, send_key, NYA_NET_KEY_SIZE);
    nya_memcpy(connection->receive_key, receive_key, NYA_NET_KEY_SIZE);
    nya_memcpy(connection->remote_key, state->presented_key, NYA_NET_KEY_SIZE);

    connection->established      = true;
    connection->last_received_ms = nya_clock_get_monotonic_ms();

    state->connecting   = false;
    state->has_response = false;

    nya_crypto_wipe(&state->ephemeral, sizeof(state->ephemeral));
    nya_crypto_wipe(state->premaster, sizeof(state->premaster));

    _nya_net_udp_event(state, (_NYA_NetUdpEvent){
        .kind = NYA_NET_TRANSPORT_EVENT_CONNECTED,
        .peer = { .index = peer_index, .generation = connection->generation },
    });
}

void _nya_net_udp_handle_packet(NYA_NetTransport* transport, u32 peer_index, u8* data, u64 size) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    if (!connection->established) return;

    if (size < _NYA_NET_UDP_HEADER_SIZE + _NYA_NET_MAC_SIZE) {
        connection->stats.packets_rejected++;
        return;
    }

    u64 sequence = _nya_net_udp_sequence_expand(connection->remote_sequence, _nya_net_udp_read_u16(data + 1));

    u8* body      = data + _NYA_NET_UDP_HEADER_SIZE;
    u64 body_size = size - _NYA_NET_UDP_HEADER_SIZE - _NYA_NET_MAC_SIZE;

    // nothing from an unauthenticated packet is looked at, not even its acks: that is what makes an address unspoofable.
    if (_nya_net_udp_is_replay(connection, sequence)
        || !_nya_net_crypto_open(connection->receive_key, sequence, data, _NYA_NET_UDP_HEADER_SIZE, body, body_size, body + body_size)) {
        connection->stats.packets_rejected++;
        return;
    }

    u64 now_ms = nya_clock_get_monotonic_ms();

    _nya_net_udp_record_ack(connection, sequence);

    connection->last_received_ms = now_ms;
    connection->stats.bytes_received += size;
    connection->stats.packets_received++;

    u8  kind           = data[0];
    u16 ack            = _nya_net_udp_read_u16(data + 3);
    u32 ack_bits       = _nya_net_udp_read_u32(data + 5);
    u16 reliable_ack   = _nya_net_udp_read_u16(data + 9);
    u8  fragment_count = data[11];

    _nya_net_udp_apply_acks(connection, ack, ack_bits, now_ms);
    _nya_net_udp_retire_reliable(connection, state->allocator, reliable_ack);

    if (kind == _NYA_NET_UDP_KIND_DISCONNECT) {
        u8 reason = body_size >= 1 ? body[0] : (u8)NYA_NET_DISCONNECT_REQUESTED;

        _nya_net_udp_remove_peer(transport, peer_index, reason < NYA_NET_DISCONNECT_COUNT ? (NYA_NetDisconnect)reason : NYA_NET_DISCONNECT_REQUESTED, true);
        return;
    }

    if (kind != _NYA_NET_UDP_KIND_DATA) return;

    if (fragment_count > 0 && !connection->ack_pending) {
        connection->ack_pending      = true;
        connection->ack_requested_ms = now_ms;
    }

    u64 at = 0;

    for (u8 fragment = 0; fragment < fragment_count; fragment++) {
        // authenticated is not the same as well formed: every read below stays inside the body.
        if (at + _NYA_NET_UDP_FRAGMENT_HEADER_SIZE > body_size) return;

        u8  channel    = body[at] & 0x0F;
        u16 message_id = _nya_net_udp_read_u16(body + at + 1);
        u16 index      = _nya_net_udp_read_u16(body + at + 3);
        u16 total      = _nya_net_udp_read_u16(body + at + 5);
        u16 length     = _nya_net_udp_read_u16(body + at + 7);

        at += _NYA_NET_UDP_FRAGMENT_HEADER_SIZE;

        if (channel >= NYA_NET_CHANNEL_COUNT) return;
        if (total == 0 || total > _NYA_NET_UDP_MAX_FRAGMENTS || index >= total) return;
        if (length > body_size - at) return;

        // the declared total size is bounded, not just the fragment count, and no piece of a split message is longer than a piece.
        if ((u64)total * _NYA_NET_UDP_FRAGMENT_PAYLOAD > _NYA_NET_UDP_MAX_MESSAGE && total > 1) return;
        if (total > 1 && length > _NYA_NET_UDP_FRAGMENT_PAYLOAD) return;

        if (total == 1) {
            // one fragment is the whole message. refused ids are not marked, so their retransmits are not taken for duplicates.
            b8 refused = channel == NYA_NET_CHANNEL_RELIABLE && !_nya_net_udp_reliable_acceptable(connection, message_id);

            if (!refused && !_nya_net_udp_is_seen(connection, (NYA_NetChannel)channel, message_id)) {
                _nya_net_udp_mark_seen(connection, (NYA_NetChannel)channel, message_id);

                if (channel == NYA_NET_CHANNEL_RELIABLE) {
                    if (connection->incoming_reliable->length >= _NYA_NET_UDP_MAX_REORDER) {
                        nya_log_warn("Dropping a peer with %d reliable messages stuck out of order.", _NYA_NET_UDP_MAX_REORDER);
                        _nya_net_udp_remove_peer(transport, peer_index, NYA_NET_DISCONNECT_PROTOCOL, true);
                        return;
                    }

                    u8* copy = nya_arena_alloc(state->allocator, length);
                    nya_memcpy(copy, body + at, length);

                    nya_array_push_back(connection->incoming_reliable, ((_NYA_NetUdpReliable){ .message_id = message_id, .data = copy, .size = length }));

                    _nya_net_udp_drain_ordered(transport, peer_index);
                } else if (length > 0) {
                    _nya_net_udp_deliver(transport, peer_index, (NYA_NetChannel)channel, body + at, length);
                }
            }
        } else {
            _nya_net_udp_reassemble(transport, peer_index, (NYA_NetChannel)channel, message_id, index, total, body + at, length);

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
    u64 usable = _NYA_NET_UDP_FRAGMENT_PAYLOAD;

    _NYA_NetUdpReassembly* slot      = nullptr;
    _NYA_NetUdpReassembly* free_slot = nullptr;
    _NYA_NetUdpReassembly* oldest    = nullptr;

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
        // fragments of an already delivered message, resent.
        if (_nya_net_udp_is_seen(connection, channel, message_id)) return;
        if (channel == NYA_NET_CHANNEL_RELIABLE && !_nya_net_udp_reliable_acceptable(connection, message_id)) return;

        // no free slot: the oldest partial message is abandoned.
        slot = free_slot != nullptr ? free_slot : oldest;
        if (slot == nullptr) return;

        u64 needed     = (u64)total * usable;
        u64 would_hold = connection->reassembly_bytes - slot->capacity + needed;

        if (needed > slot->capacity && would_hold > _NYA_NET_UDP_MAX_REASSEMBLY_BYTES) {
            nya_log_debug("Refusing a reassembly that would take a peer to %llu bytes.", (unsigned long long)would_hold);
            return;
        }

        // the slot's buffer is reused when big enough.
        if (slot->capacity < needed) {
            if (slot->data != nullptr) nya_arena_free(state->allocator, slot->data, slot->capacity);

            connection->reassembly_bytes -= slot->capacity;

            slot->data     = nya_arena_alloc(state->allocator, needed);
            slot->capacity = needed;

            connection->reassembly_bytes += needed;
        }

        *slot = (_NYA_NetUdpReassembly){
            .active         = true,
            .message_id     = message_id,
            .channel        = channel,
            .fragment_total = total,
            .started_ms     = now_ms,
            .data           = slot->data,
            .capacity       = slot->capacity,
        };
    }

    // two messages claiming one id; the newcomer is dropped.
    if (slot->fragment_total != total) return;

    // checked before the fragment is counted, so a refused one cannot complete the message with a hole.
    if (((u64)index * usable) + size > slot->capacity) return;

    // a duplicate fragment would complete the message with a hole.
    if (slot->received[index / 8] & (u8)(1U << (index % 8))) return;

    slot->received[index / 8] |= (u8)(1U << (index % 8));
    slot->fragments_received++;

    nya_memcpy(slot->data + ((u64)index * usable), data, size);

    // only the last fragment is short, so the furthest end is the exact total length.
    slot->size = nya_max(slot->size, ((u64)index * usable) + size);

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

            nya_array_push_back(connection->incoming_reliable, ((_NYA_NetUdpReliable){ .message_id = message_id, .data = owned, .size = slot->size }));

            _nya_net_udp_drain_ordered(transport, peer_index);
        } else {
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

    // strictly in order, so a lost message stalls those behind it. that is why snapshots do not use this channel.
    for (;;) {
        b8 delivered_any = false;

        for (u64 i = 0; i < connection->incoming_reliable->length; i++) {
            _NYA_NetUdpReliable* message = &connection->incoming_reliable->items[i];

            if (message->message_id != connection->next_delivery_id) continue;

            if (message->size > 0) _nya_net_udp_deliver(transport, peer_index, NYA_NET_CHANNEL_RELIABLE, message->data, message->size);
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

    _nya_net_udp_event(state, (_NYA_NetUdpEvent){
        .kind    = NYA_NET_TRANSPORT_EVENT_MESSAGE,
        .peer    = { .index = peer_index, .generation = state->peers[peer_index].generation },
        .data    = copy,
        .size    = size,
        .channel = channel,
    });
}

void _nya_net_udp_event(_NYA_NetUdpState* state, _NYA_NetUdpEvent event) {
    nya_array_push_back(state->events, event);
}

/*
 * ─────────────────────────────────────────────────────────
 * TIME
 * ─────────────────────────────────────────────────────────
 */

void _nya_net_udp_update(NYA_NetTransport* transport) {
    _NYA_NetUdpState* state = transport->state;

    _nya_net_udp_release_delayed(state, false);

    u64 now_ms = nya_clock_get_monotonic_ms();

    /*
     * The name, if it has not come back yet. Asked, never waited on: this runs inside the caller's
     * frame and a DNS lookup that takes a second must cost a second of connecting rather than a
     * second of a stopped game.
     */
    if (state->connecting && state->resolving) {
        const NET_Status status = NET_GetAddressStatus(state->connect_address);

        if (status == 1) {
            if (!_nya_net_udp_resolve_finish(transport, now_ms)) {
                state->connecting = false;
                state->resolving  = false;

                _nya_net_udp_event(state, (_NYA_NetUdpEvent){
                    .kind   = NYA_NET_TRANSPORT_EVENT_DISCONNECTED,
                    .reason = NYA_NET_DISCONNECT_TIMEOUT,
                });
            }
        } else if (status < 0 || _nya_net_elapsed_ms(now_ms, state->connect_started_ms) > _NYA_NET_UDP_CONNECT_TIMEOUT_MS) {
            /*
             * A name that will not resolve and a name that is taking too long end the same way. It is
             * reported as an event rather than returned, because by now the caller's connect has long
             * since returned OK; that is what asking instead of waiting costs.
             */
            nya_log_warn("Could not resolve the server's hostname: %s", SDL_GetError());

            state->connecting = false;
            state->resolving  = false;

            _nya_net_udp_event(state, (_NYA_NetUdpEvent){
                .kind   = NYA_NET_TRANSPORT_EVENT_DISCONNECTED,
                .reason = NYA_NET_DISCONNECT_TIMEOUT,
            });
        }
    }

    if (state->connecting && !state->resolving) {
        u32 server = _nya_net_udp_find_peer(state, state->connect_address, state->connect_port);

        if (_nya_net_elapsed_ms(now_ms, state->connect_started_ms) > _NYA_NET_UDP_CONNECT_TIMEOUT_MS) {
            state->connecting = false;

            if (server < NYA_NET_MAX_PEERS) _nya_net_udp_remove_peer(transport, server, NYA_NET_DISCONNECT_TIMEOUT, false);

            _nya_net_udp_event(state, (_NYA_NetUdpEvent){
                .kind   = NYA_NET_TRANSPORT_EVENT_DISCONNECTED,
                .reason = state->identity_refused ? NYA_NET_DISCONNECT_IDENTITY : NYA_NET_DISCONNECT_TIMEOUT,
            });
        } else if (_nya_net_elapsed_ms(now_ms, state->connect_last_sent_ms) >= _NYA_NET_UDP_CONNECT_RETRY_MS) {
            // whichever stage the handshake has reached, repeated until answered.
            if (state->has_response) {
                _nya_net_udp_transmit(state, state->connect_address, state->connect_port, state->response, _NYA_NET_UDP_RESPONSE_SIZE);
            } else {
                u8 connect[_NYA_NET_UDP_CONNECT_SIZE] = { 0 };

                _nya_net_udp_write_u32(connect, _NYA_NET_UDP_PROTOCOL);
                connect[4] = _NYA_NET_UDP_KIND_CONNECT;

                _nya_net_udp_transmit(state, state->connect_address, state->connect_port, connect, sizeof(connect));
            }

            state->connect_last_sent_ms = now_ms;
        }
    }

    for (u32 i = 0; i < NYA_NET_MAX_PEERS; i++) {
        _NYA_NetUdpPeer* connection = &state->peers[i];
        if (!connection->occupied || !connection->established) continue;

        if (_nya_net_elapsed_ms(now_ms, connection->last_received_ms) > _NYA_NET_UDP_TIMEOUT_MS) {
            _nya_net_udp_remove_peer(transport, i, NYA_NET_DISCONNECT_TIMEOUT, true);
            continue;
        }

        for (u32 r = 0; r < _NYA_NET_UDP_MAX_REASSEMBLY; r++) {
            _NYA_NetUdpReassembly* slot = &connection->reassembly[r];

            if (slot->active && _nya_net_elapsed_ms(now_ms, slot->started_ms) > _NYA_NET_UDP_REASSEMBLY_TIMEOUT_MS) {
                slot->active             = false;
                slot->fragments_received = 0;
                slot->size               = 0;
                nya_memset(slot->received, 0, sizeof(slot->received));
            }
        }

        _nya_net_udp_flush(transport, i);

        // a sealed packet with no fragments carries the acks alone: when nothing else has for a while, or to keep a quiet peer alive.
        b8 ack_due = connection->ack_pending && _nya_net_elapsed_ms(now_ms, connection->ack_requested_ms) >= _NYA_NET_UDP_ACK_DELAY_MS;

        if (ack_due || _nya_net_elapsed_ms(now_ms, connection->last_sent_ms) >= _NYA_NET_UDP_KEEPALIVE_MS) {
            _nya_net_udp_send_packet(transport, i, _NYA_NET_UDP_KIND_DATA, 0, 0);
        }

        u64 window_ms = _nya_net_elapsed_ms(now_ms, connection->rate_started_ms);

        if (window_ms >= 1000) {
            connection->stats.bytes_sent_per_second     = (u32)((connection->stats.bytes_sent - connection->rate_sent_bytes) * 1000 / window_ms);
            connection->stats.bytes_received_per_second = (u32)((connection->stats.bytes_received - connection->rate_received_bytes) * 1000 / window_ms);

            connection->rate_started_ms     = now_ms;
            connection->rate_sent_bytes     = connection->stats.bytes_sent;
            connection->rate_received_bytes = connection->stats.bytes_received;
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
        u64 now_ms     = nya_clock_get_monotonic_ms();

        state->peers[i] = (_NYA_NetUdpPeer){
            .occupied = true,

            // never zero: nya_net_peer_is_set reads the generation.
            .generation = generation == 0 ? 1 : generation,

            // referenced: SDL_net frees the address with the datagram.
            .address = NET_RefAddress(address),
            .port    = port,

            .local_sequence = 1,

            .last_received_ms = now_ms,
            .last_sent_ms     = now_ms,
            .rate_started_ms  = now_ms,

            .outgoing_reliable = nya_array_create(state->allocator, _NYA_NetUdpReliable),
            .incoming_reliable = nya_array_create(state->allocator, _NYA_NetUdpReliable),
        };

        (void)snprintf(state->peers[i].address_text, sizeof(state->peers[i].address_text), "%s:%u", NET_GetAddressString(address), port);

        return i;
    }

    return NYA_NET_MAX_PEERS;
}

void _nya_net_udp_remove_peer(NYA_NetTransport* transport, u32 peer_index, NYA_NetDisconnect reason, b8 notify) {
    _NYA_NetUdpState* state      = transport->state;
    _NYA_NetUdpPeer*  connection = &state->peers[peer_index];

    if (!connection->occupied) return;

    NYA_NetPeerId id = { .index = peer_index, .generation = connection->generation };

    nya_array_foreach (connection->outgoing_reliable, pending) {
        if (pending->data != nullptr) nya_arena_free(state->allocator, pending->data, pending->size);
    }

    nya_array_foreach (connection->incoming_reliable, pending) {
        if (pending->data != nullptr) nya_arena_free(state->allocator, pending->data, pending->size);
    }

    for (u32 i = 0; i < _NYA_NET_UDP_MAX_REASSEMBLY; i++) {
        _NYA_NetUdpReassembly* slot = &connection->reassembly[i];

        if (slot->data != nullptr) nya_arena_free(state->allocator, slot->data, slot->capacity);
    }

    nya_array_destroy(connection->outgoing_reliable);
    nya_array_destroy(connection->incoming_reliable);

    if (connection->address != nullptr) NET_UnrefAddress(connection->address);

    // everything but the generation, which is bumped when the slot is reused so stale handles fail. the keys go too.
    u32 generation = connection->generation;
    nya_crypto_wipe(connection, sizeof(*connection));
    connection->generation = generation;

    if (notify) _nya_net_udp_event(state, (_NYA_NetUdpEvent){ .kind = NYA_NET_TRANSPORT_EVENT_DISCONNECTED, .peer = id, .reason = reason });
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

    if (state->peers[index].established) {
        state->send_buffer[_NYA_NET_UDP_HEADER_SIZE] = (u8)reason;
        _nya_net_udp_send_packet(transport, index, _NYA_NET_UDP_KIND_DISCONNECT, 0, 1);
    }

    // no event: the caller asked for this.
    _nya_net_udp_remove_peer(transport, index, reason, false);
}

NYA_NetPeerStats _nya_net_udp_stats(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS) return (NYA_NetPeerStats){ 0 };

    NYA_NetPeerStats stats = state->peers[index].stats;

    stats.rtt_ms      = state->peers[index].rtt_ms;
    stats.jitter_ms   = state->peers[index].jitter_ms;
    stats.packet_loss = nya_clamp(state->peers[index].loss, 0.0F, 1.0F);

    return stats;
}

NYA_ConstCString _nya_net_udp_peer_address(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS) return "(gone)";

    return state->peers[index].address_text;
}

const u8* _nya_net_udp_public_key(NYA_NetTransport* transport) {
    _NYA_NetUdpState* state = transport->state;

    return nya_net_key_is_set(state->identity.public_key) ? state->identity.public_key : nullptr;
}

const u8* _nya_net_udp_peer_key(NYA_NetTransport* transport, NYA_NetPeerId peer) {
    _NYA_NetUdpState* state = transport->state;

    u32 index = _nya_net_udp_resolve(state, peer);
    if (index >= NYA_NET_MAX_PEERS || !state->peers[index].established) return nullptr;

    return nya_net_key_is_set(state->peers[index].remote_key) ? state->peers[index].remote_key : nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * ACKNOWLEDGEMENTS
 * ─────────────────────────────────────────────────────────
 */

u64 _nya_net_udp_sequence_expand(u64 newest, u16 wire) {
    u64 candidate = (newest & ~0xFFFFULL) | wire;

    // of the three candidates around `newest`, the one within half the 16 bit space of it.
    if (candidate + 32768 <= newest) return candidate + 65536;
    if (candidate > newest + 32768 && candidate >= 65536) return candidate - 65536;

    return candidate;
}

b8 _nya_net_udp_is_replay(const _NYA_NetUdpPeer* peer, u64 sequence) {
    if (sequence == 0) return true;
    if (sequence > peer->remote_sequence) return false;

    u64 back = peer->remote_sequence - sequence;
    if (back == 0 || back > _NYA_NET_UDP_ACK_WINDOW) return true;

    return (peer->ack_bits & (1U << (back - 1))) != 0;
}

void _nya_net_udp_record_ack(_NYA_NetUdpPeer* peer, u64 sequence) {
    if (sequence > peer->remote_sequence) {
        u64 shift = sequence - peer->remote_sequence;

        // shifting by 32 or more is undefined, and a jump reaches it. the old window is out of range anyway.
        if (peer->remote_sequence == 0) peer->ack_bits = 0;
        else peer->ack_bits = shift >= _NYA_NET_UDP_ACK_WINDOW ? (shift == _NYA_NET_UDP_ACK_WINDOW ? 1U << 31 : 0) : (peer->ack_bits << shift) | (1U << (shift - 1));

        peer->remote_sequence = sequence;
        return;
    }

    // older than the newest: set its bit if still inside the window.
    u64 back = peer->remote_sequence - sequence;
    if (back == 0 || back > _NYA_NET_UDP_ACK_WINDOW) return;

    peer->ack_bits |= 1U << (back - 1);
}

void _nya_net_udp_apply_acks(_NYA_NetUdpPeer* peer, u16 ack, u32 ack_bits, u64 now_ms) {
    for (u32 bit = 0; bit <= _NYA_NET_UDP_ACK_WINDOW; bit++) {
        if (bit > 0 && (ack_bits & (1U << (bit - 1))) == 0) continue;

        u16 sequence = (u16)((u32)ack + 65536U - bit);
        u32 slot     = sequence % _NYA_NET_UDP_SENT_WINDOW;

        // the ring holds the last 64 sends; the sequence check stops a reused slot measuring a wrong round trip.
        if ((u16)peer->sent_at_sequence[slot] != sequence || peer->sent_at_sequence[slot] == 0) continue;
        if (peer->sent_at_ms[slot] == 0) continue;

        // only the newest acknowledged packet times the round trip. older bits ride along on later packets and would read long.
        if (bit == 0) {
            f32 sample = (f32)_nya_net_elapsed_ms(now_ms, peer->sent_at_ms[slot]);

            peer->jitter_ms = peer->rtt_ms == 0.0F ? 0.0F : (peer->jitter_ms * 0.9F) + (fabsf(sample - peer->rtt_ms) * 0.1F);
            peer->rtt_ms    = peer->rtt_ms == 0.0F ? nya_max(sample, 0.001F) : (peer->rtt_ms * 0.9F) + (sample * 0.1F);
        }

        peer->sent_at_ms[slot] = 0;
    }
}

u64 _nya_net_udp_resend_ms(const _NYA_NetUdpPeer* peer) {
    if (peer->rtt_ms <= 0.0F) return _NYA_NET_UDP_RESEND_INITIAL_MS;

    f32 wait_ms = (peer->rtt_ms * 1.5F) + (peer->jitter_ms * 4.0F) + 10.0F;

    return (u64)nya_clamp(wait_ms, (f32)_NYA_NET_UDP_RESEND_MIN_MS, (f32)_NYA_NET_UDP_RESEND_MAX_MS);
}

void _nya_net_udp_retire_reliable(_NYA_NetUdpPeer* peer, NYA_Arena* allocator, u16 reliable_ack) {
    // an acknowledgement ahead of anything outstanding is refused.
    if (peer->outgoing_reliable->length > 0) {
        u16 oldest = peer->outgoing_reliable->items[0].message_id;

        if (_nya_net_udp_sequence_newer(reliable_ack, (u16)(oldest + NYA_NET_MAX_RELIABLE_IN_FLIGHT))) {
            nya_log_debug("Ignoring an implausible reliable acknowledgement (%u against an oldest of %u).", reliable_ack, oldest);
            return;
        }
    }

    for (u64 i = peer->outgoing_reliable->length; i > 0; i--) {
        _NYA_NetUdpReliable* message = &peer->outgoing_reliable->items[i - 1];

        if (!_nya_net_udp_sequence_newer(reliable_ack, message->message_id)) continue;

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
    // half the space is the largest unambiguous window, and nothing here has 32768 messages in flight.
    return ((a > b) && (a - b <= 32768)) || ((b > a) && (b - a > 32768));
}

void _nya_net_udp_write_u16(u8* out, u16 value) {
    out[0] = (u8)(value & 0xFF);
    out[1] = (u8)((value >> 8) & 0xFF);
}

void _nya_net_udp_write_u32(u8* out, u32 value) {
    for (u32 i = 0; i < 4; i++) out[i] = (u8)((value >> (i * 8)) & 0xFF);
}

void _nya_net_udp_write_u64(u8* out, u64 value) {
    for (u32 i = 0; i < 8; i++) out[i] = (u8)((value >> (i * 8)) & 0xFF);
}

u16 _nya_net_udp_read_u16(const u8* in) {
    return (u16)((u16)in[0] | ((u16)in[1] << 8));
}

u32 _nya_net_udp_read_u32(const u8* in) {
    return (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
}

u64 _nya_net_udp_read_u64(const u8* in) {
    return (u64)_nya_net_udp_read_u32(in) | ((u64)_nya_net_udp_read_u32(in + 4) << 32);
}
