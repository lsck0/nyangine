/**
 * @file websocket.h
 *
 * An outgoing WebSocket client, RFC 6455, over `ws://` and `wss://`. What talks to obs-websocket, a
 * streaming overlay, a chat bridge, or anything else that expects a long lived socket rather than a
 * request.
 *
 * Nothing here blocks. A socket is created, polled once a frame, and written to whenever; the
 * connect, the TLS handshake, the upgrade and every read and write are advanced a little at a time by
 * the poll, so the frame is never waiting on a peer.
 *
 * ```
 * nya_websocket_create        starts connecting. Returns immediately, state CONNECTING
 * nya_websocket_destroy       the pair. Closes whatever stage it had reached
 * nya_websocket_poll          advances the connection and hands out one event. Never blocks
 * nya_websocket_state         where the connection is
 *
 * nya_websocket_send_text     queues a text message
 * nya_websocket_send_binary   queues a binary message
 * nya_websocket_send_object   queues an object as compact json text. The common case
 * nya_websocket_ping          queues a ping; the pong arrives as an event
 * nya_websocket_close         starts the closing handshake
 *
 * nya_websocket_frame_encode  writes a frame header. Pure function over bytes
 * nya_websocket_frame_decode  reads one. Pure function over bytes
 * nya_websocket_accept_from_key  the answer a server owes a client's key
 * nya_websocket_close_name    a close code as text, for a log line
 * ```
 *
 * ```c
 * NYA_WebSocket* socket = nullptr;
 * NYA_EXPECT(nya_websocket_create(arena, (NYA_WebSocketOptions){ .url = "ws://127.0.0.1:4455" }, &socket));
 * defer nya_websocket_destroy(socket);
 *
 * // once a frame
 * NYA_WebSocketEvent event = { 0 };
 * while (nya_websocket_poll(socket, &event)) {
 *     switch (event.kind) {
 *         case NYA_WEBSOCKET_EVENT_OPEN:   nya_log_info("obs connected"); break;
 *         case NYA_WEBSOCKET_EVENT_TEXT:   handle(event.data, event.size); break;
 *         case NYA_WEBSOCKET_EVENT_CLOSED: nya_log_warn("obs closed: %s", nya_websocket_close_name(event.code)); break;
 *         default: break;
 *     }
 * }
 * ```
 *
 * ── why curl and not SDL_net ──
 *
 * Both are already vendored. SDL_net gives a TCP stream and nothing else, so `wss://` would mean
 * vendoring a TLS stack or shipping a client that only works in plaintext on localhost. curl is
 * already linked for the REST client, already carries a TLS backend on both platforms (OpenSSL on
 * Linux, schannel on Windows) and already uses the system trust store, and CURLOPT_CONNECT_ONLY hands
 * back exactly what this needs: a connected, TLS wrapped socket with curl_easy_send and
 * curl_easy_recv on top of it and no transfer machinery in the way.
 *
 * curl's own websocket support is not used and stays disabled in the vendor build. It is marked
 * experimental upstream, it cannot be driven a frame at a time the way this is, and turning it on
 * would put an unfinished protocol implementation in the link for every program that wants a REST
 * call. The framing below is this file's, and is what the fuzzer in tests/nyangine/net attacks.
 *
 * The connect is driven by curl's multi interface rather than curl_easy_perform on a job thread.
 * curl_multi_perform returns after whatever progress it could make without blocking, which makes the
 * whole connection a state machine this file steps once per frame. A thread was the alternative and
 * it loses on every count: it needs a handoff, it needs the socket to belong to one side at a time,
 * and it makes a failure depend on when the scheduler ran.
 *
 * ── what a hostile server may do ──
 *
 * The server is untrusted. Every frame header is bounded before a byte of its payload is kept, a
 * message that would exceed `max_message_bytes` closes the connection rather than growing a buffer, a
 * masked frame from a server is a protocol error as RFC 6455 requires, a reserved bit that was never
 * negotiated is a protocol error, a control frame longer than 125 bytes or split across frames is a
 * protocol error, and text that is not valid UTF-8 is a protocol error. None of those assert; all of
 * them arrive as NYA_WEBSOCKET_EVENT_CLOSED with a code saying which.
 *
 * Thread safety: none. One thread owns a socket for its whole life.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"
#include "nyangine/plugins/curl/request.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Longest frame header: two bytes, an eight byte length, and a four byte mask. */
#define NYA_WEBSOCKET_MAX_HEADER_BYTES 14

/** RFC 6455: a control frame's payload never exceeds this, and it is never fragmented. */
#define NYA_WEBSOCKET_MAX_CONTROL_BYTES 125

/** The nonce a client sends, before base64. Sixteen bytes, as the RFC requires. */
#define NYA_WEBSOCKET_KEY_BYTES 16

/** base64 of a twenty byte SHA-1, which is what Sec-WebSocket-Accept always is. */
#define NYA_WEBSOCKET_ACCEPT_LENGTH 28

/**
 * Bytes of upgrade response headers read before the connection is given up on.
 *
 * A conforming 101 is a few hundred bytes. This is the point past which a server is either not
 * speaking HTTP or is feeding an endless header to see whether the buffer grows.
 * */
#define NYA_WEBSOCKET_MAX_HANDSHAKE_BYTES 8192

/**
 * Queued outgoing bytes, frame headers included.
 *
 * A message has to fit this whole, since a half written frame cannot be taken back. Sixty four
 * kilobytes is far past any control protocol's request; something that sends megabytes wants a
 * different transport, not a bigger buffer here.
 * */
#define NYA_WEBSOCKET_SEND_BYTES 65536

/** What one read takes from the socket at a time. */
#define NYA_WEBSOCKET_RECEIVE_BYTES 16384

/**
 * The default ceiling on one assembled message, fragments included.
 *
 * Sized for the largest thing a control protocol answers with, which for obs-websocket is a scene
 * list of a few tens of kilobytes. Raise it per socket through the options when a peer is known to
 * send more; the buffer is allocated once at create, so the cost is visible and paid up front.
 * */
#define NYA_WEBSOCKET_DEFAULT_MAX_MESSAGE_BYTES 262144

/** What the whole connect, TLS and upgrade are given before the socket gives up. */
#define NYA_WEBSOCKET_DEFAULT_TIMEOUT_MS 30000

/**
 * The constant RFC 6455 section 1.3 appends to the client's key before hashing it. Not a secret and
 * not a choice: it is written out in the RFC so that a server which is not speaking WebSocket cannot
 * accidentally produce a matching answer.
 * */
#define NYA_WEBSOCKET_ACCEPT_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_WebSocketState       NYA_WebSocketState;
typedef enum NYA_WebSocketOpcode      NYA_WebSocketOpcode;
typedef enum NYA_WebSocketClose       NYA_WebSocketClose;
typedef enum NYA_WebSocketEventKind   NYA_WebSocketEventKind;
typedef enum NYA_WebSocketFrameResult NYA_WebSocketFrameResult;
typedef struct NYA_WebSocketFrame     NYA_WebSocketFrame;
typedef struct NYA_WebSocketEvent     NYA_WebSocketEvent;
typedef struct NYA_WebSocketOptions   NYA_WebSocketOptions;
typedef struct NYA_WebSocket          NYA_WebSocket;

enum NYA_WebSocketState {
    /** Resolving, connecting and negotiating TLS. Nothing may be sent yet. */
    NYA_WEBSOCKET_STATE_CONNECTING,

    /** The upgrade request is on the wire and the 101 has not come back. */
    NYA_WEBSOCKET_STATE_HANDSHAKING,

    /** Messages move in both directions. */
    NYA_WEBSOCKET_STATE_OPEN,

    /** A close frame has been sent and the peer's has not come back. */
    NYA_WEBSOCKET_STATE_CLOSING,

    /** Done, for any reason. The socket never leaves this state; create another one. */
    NYA_WEBSOCKET_STATE_CLOSED,

    NYA_WEBSOCKET_STATE_COUNT,
};

/** The four bit opcode in a frame header. The gaps are reserved and rejected. */
enum NYA_WebSocketOpcode {
    NYA_WEBSOCKET_OPCODE_CONTINUATION = 0x0,
    NYA_WEBSOCKET_OPCODE_TEXT         = 0x1,
    NYA_WEBSOCKET_OPCODE_BINARY       = 0x2,
    NYA_WEBSOCKET_OPCODE_CLOSE        = 0x8,
    NYA_WEBSOCKET_OPCODE_PING         = 0x9,
    NYA_WEBSOCKET_OPCODE_PONG         = 0xA,
};

/** RFC 6455 section 7.4.1, plus the two this module raises on its own. */
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

    /** The upgrade succeeded. Sends made before this were queued and go out now. */
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
 * One frame header, as it appears on the wire. What nya_websocket_frame_decode produces.
 * */
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
     * TEXT, BINARY and PONG. Points into the socket's own message buffer and is valid until the next
     * call on that socket. A text message carries a NUL one past `size`, so it can be passed straight
     * to anything taking a C string.
     * */
    const u8* data;
    u64       size;

    /** CLOSED only. */
    NYA_WebSocketClose code;

    /** CLOSED only: what the peer said, or this module's own explanation. Never null. */
    NYA_ConstCString reason;
};

struct NYA_WebSocketOptions {
    /** Required. `ws://host[:port][/path]` or `wss://...`. Anything else is refused at create. */
    NYA_ConstCString url;

    /** Sent as Sec-WebSocket-Protocol, and checked against what the server picks. Null sends none. */
    NYA_ConstCString subprotocol;

    /**
     * Added to the upgrade request. Terminated by the first entry with a null name. The handshake's own
     * headers cannot be overridden; an entry naming one is refused at create.
     * */
    NYA_RequestHeader headers[NYA_REQUEST_MAX_HEADERS];

    /** Sent as `Authorization: Bearer <token>` on the upgrade request. */
    NYA_ConstCString bearer_token;

    /** Connect, TLS and upgrade together. Zero means NYA_WEBSOCKET_DEFAULT_TIMEOUT_MS. */
    u64 handshake_timeout_ms;

    /**
     * Ceiling on one assembled message. Zero means NYA_WEBSOCKET_DEFAULT_MAX_MESSAGE_BYTES. A peer that
     * exceeds it is closed with NYA_WEBSOCKET_CLOSE_TOO_LARGE.
     * */
    u64 max_message_bytes;

    /** Accept any TLS certificate. Only for a test against a local server. */
    b8 insecure_skip_tls_verify;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

/**
 * Parses the options, allocates the socket's buffers from `arena`, and starts connecting.
 *
 * Returns as soon as the connect is under way, so a failure here is a bad option rather than an
 * unreachable server: NYA_ERROR_INVALID_ARGUMENT for a url that is not ws or wss, a header the
 * handshake owns, or a message ceiling below what a frame needs. An unreachable server arrives later
 * as NYA_WEBSOCKET_EVENT_CLOSED.
 * */
NYA_API NYA_Error nya_websocket_create(NYA_Arena* arena, NYA_WebSocketOptions options, OUT NYA_WebSocket** out_socket) __attr_no_discard;

/**
 * Closes the connection at whatever stage it reached and frees the socket. Null is a no-op.
 *
 * Does not wait for a closing handshake. Call nya_websocket_close first and poll until CLOSED when the
 * peer should be told why.
 * */
NYA_API void nya_websocket_destroy(NYA_WebSocket* socket);

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Advances the connection and hands out one event, or returns false when there is nothing to report.
 *
 * This is where everything happens: the connect, the TLS handshake, the upgrade, reads, writes, and
 * the pong for every ping. It never blocks and does a bounded amount of work per call, so calling it
 * in a loop until it returns false is the intended use and cannot be driven by the peer.
 * */
NYA_API b8 nya_websocket_poll(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event);

NYA_API NYA_WebSocketState nya_websocket_state(const NYA_WebSocket* socket) __attr_no_discard;

/**
 * Queues `text` as one text message.
 *
 * Legal before the connection is open: it goes out with the first flush after the upgrade, which is
 * what lets a caller create and send in one breath. NYA_ERROR_OUT_OF_MEMORY when the queue cannot hold
 * it, NYA_ERROR_IO once the socket is closing or closed.
 * */
NYA_API NYA_Error nya_websocket_send_text(NYA_WebSocket* socket, NYA_ConstCString text) __attr_no_discard;

NYA_API NYA_Error nya_websocket_send_binary(NYA_WebSocket* socket, const u8* data, u64 size) __attr_no_discard;

/**
 * Serializes `body` as compact json and sends it as text. What a control protocol wants.
 *
 * ```c
 * NYA_Object* request = nya_object_create(arena);
 * nya_object_set(request, "op", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 6 });
 * NYA_TRY(nya_websocket_send_object(socket, arena, request));
 * ```
 * */
NYA_API NYA_Error nya_websocket_send_object(NYA_WebSocket* socket, NYA_Arena* arena, const NYA_Object* body) __attr_no_discard;

/** Queues a ping. `size` is at most NYA_WEBSOCKET_MAX_CONTROL_BYTES; the pong arrives as an event. */
NYA_API NYA_Error nya_websocket_ping(NYA_WebSocket* socket, const u8* data, u64 size) __attr_no_discard;

/**
 * Starts the closing handshake: sends a close frame and moves to CLOSING. The peer's answer arrives as
 * NYA_WEBSOCKET_EVENT_CLOSED. Calling it twice is a no-op.
 *
 * `reason` is at most 123 bytes once encoded, which is the control frame limit minus the code, and a
 * longer one is truncated rather than refused, because a close must not fail.
 * */
NYA_API NYA_Error nya_websocket_close(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * FRAMING
 * ─────────────────────────────────────────────────────────
 */

/*
 * The wire format on its own, as two pure functions. Public because they are the part worth testing
 * directly and the part a fuzzer drives, and because a caller writing its own framing over a raw
 * socket should not be rewriting them.
 */

/**
 * Writes the header for a frame of `payload_size` bytes into `out_header`, and says how long it is.
 *
 * `mask` is required: a client's frames are always masked, and RFC 6455 says a server must close a
 * connection that sends an unmasked one. Fails for an opcode that is not one of the six, and for a
 * control frame that is not final or is longer than NYA_WEBSOCKET_MAX_CONTROL_BYTES.
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
 * exceed what `size` holds, which is exactly how a caller learns how much more to read.
 * */
NYA_API NYA_WebSocketFrameResult nya_websocket_frame_decode(const u8* data, u64 size, OUT NYA_WebSocketFrame* out_frame);

/**
 * The Sec-WebSocket-Accept a server owes for `key`: base64(sha1(key + the RFC's GUID)).
 *
 * RFC 6455 section 4.2.2, and the one part of the handshake that is a computation rather than a
 * constant. Exported because both ends need it and neither should own a second copy: this client
 * checks the server's answer with it, and a server built on this engine answers with it.
 *
 * `key` is the client's Sec-WebSocket-Key exactly as it appeared on the wire. It is not decoded, so a
 * key that is not base64 produces a value that simply will not match, which is the correct outcome.
 * NYA_ERROR_INVALID_ARGUMENT for a null or empty key, or one longer than a key can be.
 * */
NYA_API NYA_Error nya_websocket_accept_from_key(NYA_ConstCString key, OUT char out_accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1]) __attr_no_discard;

/** A close code as text, for a log line. Never null, including for a code nobody has defined. */
NYA_API NYA_ConstCString nya_websocket_close_name(NYA_WebSocketClose code) __attr_no_discard;
