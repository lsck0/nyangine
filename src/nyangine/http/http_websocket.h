/**
 * @file http_websocket.h
 *
 * RFC 6455 on the wire: the frame header, the mask, the fragments, the control frames and the closing
 * handshake, as one implementation both ends of a connection run.
 *
 * ```
 * nya_websocket_frame_encode     writes a frame header. Pure function over bytes
 * nya_websocket_frame_decode     reads one. Pure function over bytes
 * nya_websocket_accept_from_key  the answer a server owes a client's key
 * nya_websocket_close_name       a close code as text, for a log line
 *
 * nya_websocket_protocol_open    one connection's framing state, over buffers the caller owns
 * nya_websocket_protocol_close   queues the goodbye. The pair
 * nya_websocket_protocol_receive bytes in, whole messages out
 * nya_websocket_protocol_send    one message out, framed and masked for this end's role
 * nya_websocket_protocol_pending / _flushed   the bytes waiting for the socket, and what it took
 * nya_websocket_protocol_fail    the connection went away; say so without framing anything
 * nya_websocket_protocol_close_code   why it ended, once it has
 * ```
 *
 * ── why this is in http and not in the plugin that had it ──
 *
 * There are two ends of a WebSocket in this tree: the curl client in plugins/curl, which dials out to
 * something like obs-websocket, and the server in http_websocket_server.h, which a browser dials into.
 * They are the same protocol read from opposite sides, and two copies of a frame decoder is two places
 * for a length to be trusted. So the codec lives here, under http, where the other half of RFC 6455 —
 * the upgrade, which is an HTTP request — already is, and the plugin includes it.
 *
 * The dependency only runs that way. `http` names nothing in `plugins`, and nothing here links a
 * socket, the engine's own or curl's: it is bytes in and bytes out over buffers the caller owns, which is also
 * what makes it the thing the fuzzer drives (tests/fuzz/fuzz_websocket_frame.c). The one thing it does
 * reach for is `crypto`'s SHA-1, for the accept key, which the RFC names and nothing else here uses.
 *
 * ── the role is the security boundary ──
 *
 * RFC 6455 section 5.1: every frame a client sends is masked, and no frame a server sends is. The
 * masking is not confidentiality — the key is in the frame — it is there so a hostile page cannot make
 * a browser emit bytes a proxy in the middle would read as a request of its own. A protocol opened as
 * a server therefore refuses an unmasked frame and one opened as a client refuses a masked one, rather
 * than tolerating either: an endpoint that accepts both is an endpoint whose traffic can be replayed
 * back at it.
 *
 * ── what a hostile peer may do ──
 *
 * Anything, and none of it may cost more than the buffers the caller handed over. Every header is
 * bounded before a byte of its payload is kept; a frame announcing more than `max_frame_bytes` is
 * refused on its header alone; a message whose fragments pass `message_capacity`, or that takes more
 * than NYA_WEBSOCKET_MAX_FRAGMENTS of them, closes the connection rather than growing anything; a
 * reserved bit, an unknown opcode, a control frame that is fragmented or over 125 bytes, a length that
 * is not in its shortest form, text that is not UTF-8 and a close code the RFC reserves are each a
 * protocol error. None of them assert. All of them arrive as NYA_WEBSOCKET_EVENT_CLOSED with the code
 * that says which.
 *
 * ── what is not here ──
 *
 * permessage-deflate and every other extension: `Sec-WebSocket-Extensions` is never negotiated, so the
 * three reserved bits stay zero and a frame that sets one is refused. Sending a message in fragments:
 * nothing here needs to, so nya_websocket_protocol_send writes one final frame and a caller that wants
 * more sends more messages. Receiving fragments is supported, because a peer's fragmentation is not
 * ours to decide.
 *
 * Thread safety: none. One thread owns a protocol for its whole life.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Longest frame header: two bytes, an eight byte length, and a four byte mask. */
#define NYA_WEBSOCKET_MAX_HEADER_BYTES 14

/** RFC 6455: a control frame's payload never exceeds this, and it is never fragmented. */
#define NYA_WEBSOCKET_MAX_CONTROL_BYTES 125

/** What is left of a control frame for a close reason once the two byte code is in it. */
#define NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES (NYA_WEBSOCKET_MAX_CONTROL_BYTES - 2)

/** The nonce a client sends, before base64. Sixteen bytes, as the RFC requires. */
#define NYA_WEBSOCKET_KEY_BYTES 16

/** base64 of NYA_WEBSOCKET_KEY_BYTES, the terminator included. A key is always exactly this long. */
#define NYA_WEBSOCKET_KEY_TEXT_BYTES 25

/** base64 of a twenty byte SHA-1, which is what Sec-WebSocket-Accept always is. */
#define NYA_WEBSOCKET_ACCEPT_LENGTH 28

/** The only version of the protocol this engine speaks, on either end. RFC 6455 section 4.1. */
#define NYA_WEBSOCKET_VERSION 13

/**
 * The constant RFC 6455 section 1.3 appends to the client's key before hashing it. Not a secret and
 * not a choice: it is written out in the RFC so that a server which is not speaking WebSocket cannot
 * accidentally produce a matching answer.
 * */
#define NYA_WEBSOCKET_ACCEPT_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/**
 * Frames one assembled message may be built from.
 *
 * The message is already bounded by the buffer it is assembled in, but a peer can send a hundred
 * thousand empty continuation frames and never finish it: this is the second bound, on the work rather
 * than the size, and it is the same bound NYA_HTTP_MAX_CHUNKS puts on a chunked body. Sixty four is far
 * past what any client fragments a message this small into.
 * */
#define NYA_WEBSOCKET_MAX_FRAGMENTS 64

/**
 * Frames one nya_websocket_protocol_receive handles before it returns regardless.
 *
 * A call hands out at most one message, but a run of pings or empty fragments produces no event at
 * all, and without a bound a peer that kept sending them would own the caller's tick.
 * */
#define NYA_WEBSOCKET_MAX_FRAMES_PER_RECEIVE 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_WebSocketOpcode           NYA_WebSocketOpcode;
typedef enum NYA_WebSocketClose            NYA_WebSocketClose;
typedef enum NYA_WebSocketEventKind        NYA_WebSocketEventKind;
typedef enum NYA_WebSocketFrameResult      NYA_WebSocketFrameResult;
typedef enum NYA_WebSocketRole             NYA_WebSocketRole;
typedef struct NYA_WebSocketFrame          NYA_WebSocketFrame;
typedef struct NYA_WebSocketEvent          NYA_WebSocketEvent;
typedef struct NYA_WebSocketProtocolConfig NYA_WebSocketProtocolConfig;
typedef struct NYA_WebSocketProtocol       NYA_WebSocketProtocol;

/** The four bit opcode in a frame header. The gaps are reserved and rejected. */
enum NYA_WebSocketOpcode {
    NYA_WEBSOCKET_OPCODE_CONTINUATION = 0x0,
    NYA_WEBSOCKET_OPCODE_TEXT         = 0x1,
    NYA_WEBSOCKET_OPCODE_BINARY       = 0x2,
    NYA_WEBSOCKET_OPCODE_CLOSE        = 0x8,
    NYA_WEBSOCKET_OPCODE_PING         = 0x9,
    NYA_WEBSOCKET_OPCODE_PONG         = 0xA,
};

/** RFC 6455 section 7.4.1, plus the two this engine raises on its own. */
enum NYA_WebSocketClose {
    /** The peer closed without a code, which the RFC allows. */
    NYA_WEBSOCKET_CLOSE_NONE = 0,

    NYA_WEBSOCKET_CLOSE_NORMAL         = 1000,
    NYA_WEBSOCKET_CLOSE_GOING_AWAY     = 1001,
    NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR = 1002,
    NYA_WEBSOCKET_CLOSE_UNSUPPORTED    = 1003,

    /** Never sent and never received; what a connection that dropped without a close frame reports. */
    NYA_WEBSOCKET_CLOSE_ABNORMAL = 1006,

    NYA_WEBSOCKET_CLOSE_INVALID_PAYLOAD = 1007,
    NYA_WEBSOCKET_CLOSE_POLICY          = 1008,
    NYA_WEBSOCKET_CLOSE_TOO_LARGE       = 1009,
    NYA_WEBSOCKET_CLOSE_EXTENSION       = 1010,
    NYA_WEBSOCKET_CLOSE_INTERNAL        = 1011,
    NYA_WEBSOCKET_CLOSE_TLS             = 1015,
};

enum NYA_WebSocketEventKind {
    NYA_WEBSOCKET_EVENT_NONE = 0,

    /** The upgrade succeeded. Reported by whoever owns the connection, never by the codec. */
    NYA_WEBSOCKET_EVENT_OPEN,

    /** A whole text message, fragments already joined. `data` is NUL terminated past `size`. */
    NYA_WEBSOCKET_EVENT_TEXT,

    /** A whole binary message, fragments already joined. */
    NYA_WEBSOCKET_EVENT_BINARY,

    /** The peer answered a ping. `data` is whatever it echoed. */
    NYA_WEBSOCKET_EVENT_PONG,

    /** The connection is over. `code` and `reason` say why. Always the last event. */
    NYA_WEBSOCKET_EVENT_CLOSED,

    NYA_WEBSOCKET_EVENT_KIND_COUNT,
};

/**
 * What nya_websocket_frame_decode made of the bytes. Three outcomes, because "not yet" and "never"
 * are different answers and a caller has to do different things with them.
 * */
enum NYA_WebSocketFrameResult {
    /** A whole, legal header. `out_frame` is filled. */
    NYA_WEBSOCKET_FRAME_OK,

    /** Fewer bytes than this header needs. Read more and ask again; nothing is wrong. */
    NYA_WEBSOCKET_FRAME_INCOMPLETE,

    /** The bytes cannot be a legal header, however many more arrive. Close the connection. */
    NYA_WEBSOCKET_FRAME_INVALID,

    NYA_WEBSOCKET_FRAME_RESULT_COUNT,
};

/**
 * Which end of the connection a protocol is. It decides who masks and who refuses a mask; see the file
 * note, because this is the one field in the configuration that is a security property.
 * */
enum NYA_WebSocketRole {
    /** Dialled out. Masks everything it sends and refuses a masked frame from the server. */
    NYA_WEBSOCKET_ROLE_CLIENT,

    /** Was dialled into. Masks nothing and refuses an unmasked frame from the client. */
    NYA_WEBSOCKET_ROLE_SERVER,

    NYA_WEBSOCKET_ROLE_COUNT,
};

/** One frame header, as it appears on the wire. What nya_websocket_frame_decode produces. */
struct NYA_WebSocketFrame {
    b8 fin;

    /** The low four bits of the first byte. Not every value is legal; the decoder rejects the rest. */
    NYA_WebSocketOpcode opcode;

    b8 masked;

    /** The masking key in wire order, meaningful only when `masked`. */
    u8 mask[4];

    /** What follows the header. Already checked to be representable; never a negative or truncated length. */
    u64 payload_size;

    /** How many bytes the header itself took, so the payload starts at `data + header_size`. */
    u64 header_size;
};

struct NYA_WebSocketEvent {
    NYA_WebSocketEventKind kind;

    /**
     * TEXT, BINARY and PONG. Points into the buffer the protocol was opened over and is valid until the
     * next call on it. A text message carries a NUL one past `size`, so it can be passed straight to
     * anything taking a C string.
     * */
    const u8* data;
    u64       size;

    /** CLOSED only. */
    NYA_WebSocketClose code;

    /** CLOSED only: what the peer said, or this engine's own explanation. Never null. */
    NYA_ConstCString reason;
};

/**
 * Everything a protocol needs, and every buffer it will ever use.
 *
 * It allocates nothing: the two buffers are the caller's, which is what lets a client take them from an
 * arena at whatever ceiling it was configured with and a server keep a fixed table of them.
 * */
struct NYA_WebSocketProtocolConfig {
    NYA_WebSocketRole role;

    /**
     * Where a message is assembled, fragments joined. Must hold `message_capacity + 1` bytes: the last
     * one is the NUL a text event carries so a handler can treat it as a C string.
     * */
    u8* message;
    u64 message_capacity;

    /**
     * Where frames wait for the socket. A message is framed into it whole or not at all, so it bounds
     * the largest thing this end can send; at least NYA_WEBSOCKET_MAX_HEADER_BYTES plus a control frame,
     * so a goodbye always fits.
     * */
    u8* send;
    u64 send_capacity;

    /**
     * The largest payload one frame may announce. Zero means `message_capacity`.
     *
     * The tighter of the two bounds, and the one that acts first: a frame is refused on its header,
     * before a byte of its payload has been read into anything.
     * */
    u64 max_frame_bytes;
};

/**
 * One connection's framing state. Opaque in practice: everything about it is read through the calls
 * below, and it is laid out here only so a caller can hold one by value inside its own connection.
 * */
struct NYA_WebSocketProtocol {
    NYA_WebSocketRole role;

    u8* message;
    u64 message_capacity;
    u64 message_size;

    /** TEXT or BINARY while a fragmented message is in progress; CONTINUATION when none is. */
    NYA_WebSocketOpcode message_opcode;
    u32                 message_fragments;

    u8* send;
    u64 send_capacity;
    u64 send_size;

    u64 max_frame_bytes;

    /** Whether the frame below is being read through, rather than the next header being looked for. */
    b8                 in_payload;
    NYA_WebSocketFrame frame;

    /** Payload bytes of `frame` taken so far, which is also the offset into its mask. */
    u64 payload_read;

    /** A control frame's payload is collected here, so it never disturbs a fragmented message. */
    u8  control[NYA_WEBSOCKET_MAX_CONTROL_BYTES + 1];
    u64 control_size;

    b8 close_sent;
    b8 closed;

    NYA_WebSocketClose close_code;
    char               close_reason[NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES + 1];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * FRAMING
 * ─────────────────────────────────────────────────────────
 */

/*
 * The wire format on its own, as two pure functions. Public because they are the part worth testing
 * directly and the part a fuzzer drives, and because the handshake in http_websocket_server.c and the
 * one in the curl plugin both need to agree about what a frame is.
 */

/**
 * Writes the header for a frame of `payload_size` bytes into `out_header`, and says how long it is.
 *
 * `mask` is the four byte masking key for a client's frame and null for a server's, because RFC 6455
 * says a client masks everything and a server masks nothing. Fails for an opcode that is not one of the
 * six, and for a control frame that is not final or is longer than NYA_WEBSOCKET_MAX_CONTROL_BYTES.
 * */
NYA_API NYA_Error nya_websocket_frame_encode(
    NYA_WebSocketOpcode opcode,
    b8                  fin,
    u64                 payload_size,
    const u8            mask[4],
    OUT u8              out_header[NYA_WEBSOCKET_MAX_HEADER_BYTES],
    OUT u64*            out_header_size
) __attr_no_discard;

/**
 * Reads one frame header out of `data`.
 *
 * INVALID for a frame that breaks the protocol: a reserved bit set, an opcode outside the six, a
 * control frame that is fragmented or over NYA_WEBSOCKET_MAX_CONTROL_BYTES, a 64 bit length with its
 * top bit set, or a length that was not encoded in the shortest form the RFC requires. INCOMPLETE only
 * when the bytes so far are consistent with a legal header that has not all arrived.
 *
 * `out_frame` is filled only on OK. The payload is *not* checked to be present: `payload_size` may
 * exceed what `size` holds, which is exactly how a caller learns how much more to read. Whether the
 * mask bit is the right way round for this end of the connection is the protocol's question, not this
 * one's; see nya_websocket_protocol_receive.
 * */
NYA_API NYA_WebSocketFrameResult nya_websocket_frame_decode(const u8* data, u64 size, OUT NYA_WebSocketFrame* out_frame);

/**
 * The Sec-WebSocket-Accept a server owes for `key`: base64(sha1(key + the RFC's GUID)).
 *
 * RFC 6455 section 4.2.2, and the one part of the handshake that is a computation rather than a
 * constant. One implementation for both ends: the client checks the server's answer with it, and the
 * server answers with it.
 *
 * `key` is the client's Sec-WebSocket-Key exactly as it appeared on the wire. It is not decoded, so a
 * key that is not base64 produces a value that simply will not match, which is the correct outcome.
 * NYA_ERROR_INVALID_ARGUMENT for a null or empty key, or one that is not a key's length.
 * */
NYA_API NYA_Error nya_websocket_accept_from_key(NYA_ConstCString key, OUT char out_accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1]) __attr_no_discard;

/** A close code as text, for a log line. Never null, including for a code nobody has defined. */
NYA_API NYA_ConstCString nya_websocket_close_name(NYA_WebSocketClose code) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * THE PROTOCOL
 * ─────────────────────────────────────────────────────────
 */

/**
 * Opens a protocol over a connection whose handshake is already done, filling `protocol` and taking
 * both of `config`'s buffers for its whole life.
 *
 * Opens nothing itself: there is no socket in here. What it opens is the conversation, which the
 * upgrade has already agreed to and which nya_websocket_protocol_close ends.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a missing buffer, a message ceiling that cannot hold a control frame,
 * a send queue that cannot hold a goodbye, or a `max_frame_bytes` past the message ceiling.
 * */
NYA_API NYA_Error nya_websocket_protocol_open(OUT NYA_WebSocketProtocol* protocol, NYA_WebSocketProtocolConfig config) __attr_no_discard;

/**
 * Queues the closing handshake: a close frame carrying `code` and `reason`, after which only the peer's
 * answer is read and nothing else may be sent. Calling it twice is a no-op.
 *
 * `reason` is truncated to NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES rather than refused, because a goodbye
 * must not fail over a courtesy. The caller still has to flush what this queued; see _pending.
 * */
NYA_API NYA_Error nya_websocket_protocol_close(NYA_WebSocketProtocol* protocol, NYA_WebSocketClose code, NYA_ConstCString reason);

/**
 * Feeds received bytes in and takes at most one message out.
 *
 * `out_consumed` is how much of `data` was taken, which is never more than it was given and may be less
 * than all of it: an event stops the walk, so a caller hands back the rest on the next call. Returns
 * true when `out_event` was filled.
 *
 * Every refusal in the file note ends here: the protocol queues a close frame saying which, moves to
 * closed, and hands out one NYA_WEBSOCKET_EVENT_CLOSED. Pings are answered where they arrive, because
 * the RFC requires a pong with the same payload and a caller has nothing to decide about one.
 * */
NYA_API b8
nya_websocket_protocol_receive(NYA_WebSocketProtocol* protocol, const u8* data, u64 size, OUT u64* out_consumed, OUT NYA_WebSocketEvent* out_event);

/**
 * Frames `data` as one whole message and queues it, masked when this end is a client.
 *
 * NYA_ERROR_IO once a close has been sent or the protocol is closed, NYA_ERROR_INVALID_ARGUMENT for a
 * control frame over NYA_WEBSOCKET_MAX_CONTROL_BYTES or a text message that is not UTF-8, and
 * NYA_ERROR_OUT_OF_MEMORY when the send queue cannot hold the whole frame — checked before either half
 * is written, because a header with no payload behind it is a stream a peer cannot recover from.
 * */
NYA_API NYA_Error nya_websocket_protocol_send(NYA_WebSocketProtocol* protocol, NYA_WebSocketOpcode opcode, const u8* data, u64 size)
    __attr_no_discard;

/** What is queued for the socket, and how much. Null and zero when there is nothing. */
NYA_API const u8* nya_websocket_protocol_pending(const NYA_WebSocketProtocol* protocol, OUT u64* out_size);

/** Takes `count` bytes off the front of the queue, once the socket has accepted them. */
NYA_API void nya_websocket_protocol_flushed(NYA_WebSocketProtocol* protocol, u64 count);

/**
 * Ends the protocol with a reason and without framing anything, for when there is nothing left to say
 * it to: the socket dropped, the handshake failed, the program is going down.
 * */
NYA_API void nya_websocket_protocol_fail(NYA_WebSocketProtocol* protocol, NYA_WebSocketClose code, NYA_ConstCString reason);

/**
 * Why the conversation ended, or NYA_WEBSOCKET_CLOSE_NONE while it has not. The peer's code when the
 * peer closed, and this end's when this end refused something.
 * */
NYA_API NYA_WebSocketClose nya_websocket_protocol_close_code(const NYA_WebSocketProtocol* protocol) __attr_no_discard;

/** Whether the conversation is over. Nothing may be sent and nothing more will be received. */
NYA_API b8 nya_websocket_protocol_is_closed(const NYA_WebSocketProtocol* protocol) __attr_no_discard;

/** Whether a close frame has gone out and the peer's answer has not come back. */
NYA_API b8 nya_websocket_protocol_is_closing(const NYA_WebSocketProtocol* protocol) __attr_no_discard;
