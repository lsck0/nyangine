#include <curl/curl.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct _NYA_WebSocketUrl _NYA_WebSocketUrl;
typedef enum _NYA_WebSocketRead  _NYA_WebSocketRead;

/** Longest host this module will talk to, the terminator included. Well past the DNS label limit of 253. */
#define _NYA_WEBSOCKET_MAX_HOST 256

/** Longest request target. A control protocol's path is a handful of characters; this is for query strings. */
#define _NYA_WEBSOCKET_MAX_PATH 1024

/** The whole upgrade request, headers included. Bounded so it is built on the stack and never grows. */
#define _NYA_WEBSOCKET_MAX_REQUEST 4096

/** base64 of NYA_WEBSOCKET_KEY_BYTES, the terminator included. */
#define _NYA_WEBSOCKET_KEY_TEXT_BYTES 25

/** A SHA-1 digest. */
#define _NYA_WEBSOCKET_SHA1_BYTES 20

/** A close frame's payload is a two byte code and then the reason, inside the control frame limit. */
#define _NYA_WEBSOCKET_MAX_CLOSE_REASON (NYA_WEBSOCKET_MAX_CONTROL_BYTES - 2)

/**
 * Frames handled in one poll before it returns regardless.
 *
 * A poll hands out at most one message, but a run of pings or empty continuations produces no event at
 * all, and without a bound a peer could keep one poll call going for as long as it kept sending them.
 * */
#define _NYA_WEBSOCKET_MAX_FRAMES_PER_POLL 64

/**
 * A url this module is willing to open, which is the only kind anything downstream is ever handed.
 * */
struct _NYA_WebSocketUrl {
    b8 secure;

    char host[_NYA_WEBSOCKET_MAX_HOST];
    u16  port;

    /** Always starts with '/', because the request line needs one and an empty path means the root. */
    char path[_NYA_WEBSOCKET_MAX_PATH];

    /** The same url with ws swapped for http, which is what curl is given. */
    char curl_url[_NYA_WEBSOCKET_MAX_HOST + _NYA_WEBSOCKET_MAX_PATH + 16];
};

/** Which half of a frame the reader is in the middle of. */
enum _NYA_WebSocketRead {
    _NYA_WEBSOCKET_READ_HEADER,
    _NYA_WEBSOCKET_READ_PAYLOAD,
};

struct NYA_WebSocket {
    NYA_Arena* allocator;

    NYA_WebSocketState state;

    CURL*  easy;
    CURLM* multi;

    /** The socket curl connected, once it has. -1 until then. */
    curl_socket_t socket;

    /** When the connect, TLS and upgrade together run out of time. */
    u64 handshake_deadline_ms;

    u64 max_message_bytes;

    /** The nonce sent as Sec-WebSocket-Key, and the answer it obliges the server to give. */
    char key_text[_NYA_WEBSOCKET_KEY_TEXT_BYTES];
    char expected_accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1];

    /** What the server said before the blank line. Bounded by NYA_WEBSOCKET_MAX_HANDSHAKE_BYTES. */
    u8  handshake[NYA_WEBSOCKET_MAX_HANDSHAKE_BYTES];
    u64 handshake_size;

    /** Queued and not yet taken by the kernel. */
    u8  send[NYA_WEBSOCKET_SEND_BYTES];
    u64 send_size;

    /** Raw bytes from the socket that have not been parsed into frames yet. */
    u8  receive[NYA_WEBSOCKET_RECEIVE_BYTES];
    u64 receive_size;

    _NYA_WebSocketRead read_state;
    NYA_WebSocketFrame frame;
    u64                payload_remaining;

    /** A control frame's payload is collected here instead, so it never disturbs a fragmented message. */
    u8  control[NYA_WEBSOCKET_MAX_CONTROL_BYTES + 1];
    u64 control_size;

    /**
     * The message being assembled. One byte longer than the ceiling, for the NUL a text event carries.
     * */
    u8* message;
    u64 message_size;

    /** TEXT or BINARY while a fragmented message is in progress; CONTINUATION when none is. */
    NYA_WebSocketOpcode message_opcode;

    NYA_WebSocketClose close_code;
    char               close_reason[_NYA_WEBSOCKET_MAX_CLOSE_REASON + 1];

    b8 close_sent;
    b8 open_reported;
    b8 closed_reported;
};

/* ── the url ── */

/** Parses a ws or wss url into the only form anything else here accepts. */
NYA_INTERNAL NYA_Error _nya_websocket_url_parse(NYA_ConstCString text, OUT _NYA_WebSocketUrl* out_url) __attr_no_discard;

/* ── the handshake ── */

NYA_INTERNAL void _nya_websocket_sha1(const u8* data, u64 size, OUT u8 out_digest[_NYA_WEBSOCKET_SHA1_BYTES]);

/** Builds the upgrade request and queues it. */
NYA_INTERNAL NYA_Error _nya_websocket_handshake_send(NYA_WebSocket* socket, const _NYA_WebSocketUrl* url, const NYA_WebSocketOptions* options)
    __attr_no_discard;

/** Reads the 101 and checks it. False means the socket was closed with a reason already set. */
NYA_INTERNAL b8 _nya_websocket_handshake_receive(NYA_WebSocket* socket) __attr_no_discard;

/**
 * Whether the response headers in `text` carry `name: value`, matched case insensitively as HTTP
 * requires. `value` null only checks that the header is present.
 * */
NYA_INTERNAL b8 _nya_websocket_header_matches(NYA_ConstCString text, NYA_ConstCString name, NYA_ConstCString value) __attr_no_discard;

/* ── the connection ── */

/** Steps curl's connect. False means the socket was closed with a reason already set. */
NYA_INTERNAL b8 _nya_websocket_pump_connect(NYA_WebSocket* socket) __attr_no_discard;

/** Pushes what the kernel will take of the queue. False means the connection is gone. */
NYA_INTERNAL b8 _nya_websocket_flush(NYA_WebSocket* socket) __attr_no_discard;

/** One read into the receive buffer. False means the connection is gone. */
NYA_INTERNAL b8 _nya_websocket_fill(NYA_WebSocket* socket) __attr_no_discard;

/** Appends to the send queue. NYA_ERROR_OUT_OF_MEMORY when it does not fit. */
NYA_INTERNAL NYA_Error _nya_websocket_queue(NYA_WebSocket* socket, const u8* data, u64 size) __attr_no_discard;

/** Frames and queues one whole message. */
NYA_INTERNAL NYA_Error _nya_websocket_queue_frame(NYA_WebSocket* socket, NYA_WebSocketOpcode opcode, const u8* data, u64 size) __attr_no_discard;

/** Parses whatever whole frames are buffered, producing at most one event. */
NYA_INTERNAL b8 _nya_websocket_drain(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event);

/** Takes `count` bytes off the front of the receive buffer. */
NYA_INTERNAL void _nya_websocket_consume(NYA_WebSocket* socket, u64 count);

/** Moves the socket to CLOSED with a reason, without sending anything. */
NYA_INTERNAL void _nya_websocket_fail(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason);

/** Whether `data` is well formed UTF-8, which RFC 6455 requires of every text message. */
NYA_INTERNAL b8 _nya_websocket_is_utf8(const u8* data, u64 size) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * FRAMING
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_websocket_accept_from_key(NYA_ConstCString key, OUT char out_accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1]) {
    nya_assert(out_accept != nullptr);

    out_accept[0] = '\0';

    if (key == nullptr || key[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket key may not be empty");

    u64 length = strlen(key);

    // A key is base64 of sixteen bytes, so it is always exactly this long. A longer one is refused
    // rather than hashed, which is what keeps the fixed block in _nya_websocket_sha1 a fixed block.
    if (length != _NYA_WEBSOCKET_KEY_TEXT_BYTES - 1) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "a websocket key is %u characters, not %llu",
            (u32)(_NYA_WEBSOCKET_KEY_TEXT_BYTES - 1),
            (unsigned long long)length
        );
    }

    u8  material[_NYA_WEBSOCKET_KEY_TEXT_BYTES + sizeof(NYA_WEBSOCKET_ACCEPT_GUID)] = { 0 };
    u64 material_size                                                               = 0;

    nya_memcpy(material, key, length);
    material_size += length;

    u64 guid_length = strlen(NYA_WEBSOCKET_ACCEPT_GUID);

    nya_assert(material_size + guid_length <= sizeof(material));
    nya_memcpy(material + material_size, NYA_WEBSOCKET_ACCEPT_GUID, guid_length);
    material_size += guid_length;

    u8 digest[_NYA_WEBSOCKET_SHA1_BYTES] = { 0 };
    _nya_websocket_sha1(material, material_size, digest);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "websocket_accept");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_String* encoded = nya_string_create(&scratch);
    nya_base64_encode(encoded, digest, sizeof(digest));

    nya_assert(encoded->length == NYA_WEBSOCKET_ACCEPT_LENGTH, "base64 of twenty bytes is always twenty eight characters");

    nya_memcpy(out_accept, encoded->items, encoded->length);
    out_accept[NYA_WEBSOCKET_ACCEPT_LENGTH] = '\0';

    return NYA_OK;
}

NYA_ConstCString nya_websocket_close_name(NYA_WebSocketClose code) {
    switch (code) {
        case NYA_WEBSOCKET_CLOSE_NONE:            return "none";
        case NYA_WEBSOCKET_CLOSE_NORMAL:          return "normal";
        case NYA_WEBSOCKET_CLOSE_GOING_AWAY:      return "going away";
        case NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR:  return "protocol error";
        case NYA_WEBSOCKET_CLOSE_UNSUPPORTED:     return "unsupported data";
        case NYA_WEBSOCKET_CLOSE_ABNORMAL:        return "abnormal";
        case NYA_WEBSOCKET_CLOSE_INVALID_PAYLOAD: return "invalid payload";
        case NYA_WEBSOCKET_CLOSE_POLICY:          return "policy violation";
        case NYA_WEBSOCKET_CLOSE_TOO_LARGE:       return "message too large";
        case NYA_WEBSOCKET_CLOSE_EXTENSION:       return "extension required";
        case NYA_WEBSOCKET_CLOSE_INTERNAL:        return "internal error";
        case NYA_WEBSOCKET_CLOSE_TLS:             return "tls failure";
        default:                                  return "unknown";
    }
}

NYA_Error nya_websocket_frame_encode(
    NYA_WebSocketOpcode opcode,
    b8                  fin,
    u64                 payload_size,
    const u8            mask[4],
    OUT u8              out_header[NYA_WEBSOCKET_MAX_HEADER_BYTES],
    OUT u64*            out_header_size
) {
    nya_assert(out_header != nullptr);
    nya_assert(out_header_size != nullptr);
    nya_assert(mask != nullptr);

    *out_header_size = 0;

    b8 known = opcode == NYA_WEBSOCKET_OPCODE_CONTINUATION || opcode == NYA_WEBSOCKET_OPCODE_TEXT || opcode == NYA_WEBSOCKET_OPCODE_BINARY ||
               opcode == NYA_WEBSOCKET_OPCODE_CLOSE || opcode == NYA_WEBSOCKET_OPCODE_PING || opcode == NYA_WEBSOCKET_OPCODE_PONG;

    if (!known) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "0x%x is not a websocket opcode", (u32)opcode);

    b8 control = ((u32)opcode & 0x8U) != 0;

    if (control && !fin) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a control frame is never fragmented");
    if (control && payload_size > NYA_WEBSOCKET_MAX_CONTROL_BYTES) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "a control frame carries at most %u bytes, not %llu",
            (u32)NYA_WEBSOCKET_MAX_CONTROL_BYTES,
            (unsigned long long)payload_size
        );
    }

    // The RFC reads the length as a signed 63 bit number, so the top bit must be clear.
    if (payload_size > (U64_MAX >> 1)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a payload longer than a websocket frame can describe");

    u64 at = 0;

    out_header[at++] = (u8)((fin ? 0x80U : 0U) | ((u32)opcode & 0x0FU));

    // Always masked: a client's frames are, and this module is only ever a client.
    if (payload_size < 126) {
        out_header[at++] = (u8)(0x80U | (u32)payload_size);
    } else if (payload_size <= 0xFFFFU) {
        out_header[at++] = (u8)(0x80U | 126U);
        out_header[at++] = (u8)((payload_size >> 8) & 0xFFU);
        out_header[at++] = (u8)(payload_size & 0xFFU);
    } else {
        out_header[at++] = (u8)(0x80U | 127U);
        for (s32 shift = 56; shift >= 0; shift -= 8) out_header[at++] = (u8)((payload_size >> shift) & 0xFFU);
    }

    for (u32 i = 0; i < 4; i++) out_header[at++] = mask[i];

    nya_assert(at <= NYA_WEBSOCKET_MAX_HEADER_BYTES);
    *out_header_size = at;

    return NYA_OK;
}

NYA_WebSocketFrameResult nya_websocket_frame_decode(const u8* data, u64 size, OUT NYA_WebSocketFrame* out_frame) {
    nya_assert(out_frame != nullptr);

    *out_frame = (NYA_WebSocketFrame){ 0 };

    if (data == nullptr || size < 2) return NYA_WEBSOCKET_FRAME_INCOMPLETE;

    u8 first  = data[0];
    u8 second = data[1];

    // No extension was negotiated, so a reserved bit can only be a peer that is confused or probing.
    if ((first & 0x70U) != 0) return NYA_WEBSOCKET_FRAME_INVALID;

    NYA_WebSocketOpcode opcode = (NYA_WebSocketOpcode)(first & 0x0FU);

    b8 known = opcode == NYA_WEBSOCKET_OPCODE_CONTINUATION || opcode == NYA_WEBSOCKET_OPCODE_TEXT || opcode == NYA_WEBSOCKET_OPCODE_BINARY ||
               opcode == NYA_WEBSOCKET_OPCODE_CLOSE || opcode == NYA_WEBSOCKET_OPCODE_PING || opcode == NYA_WEBSOCKET_OPCODE_PONG;

    if (!known) return NYA_WEBSOCKET_FRAME_INVALID;

    b8 fin     = (first & 0x80U) != 0;
    b8 masked  = (second & 0x80U) != 0;
    b8 control = ((u32)opcode & 0x8U) != 0;

    u64 length = second & 0x7FU;

    if (control && !fin) return NYA_WEBSOCKET_FRAME_INVALID;
    if (control && length > NYA_WEBSOCKET_MAX_CONTROL_BYTES) return NYA_WEBSOCKET_FRAME_INVALID;

    u64 at = 2;

    if (length == 126) {
        if (size < at + 2) return NYA_WEBSOCKET_FRAME_INCOMPLETE;

        length  = ((u64)data[at] << 8) | (u64)data[at + 1];
        at     += 2;

        // The RFC requires the shortest encoding, so a value that fitted seven bits is a bad frame.
        if (length < 126) return NYA_WEBSOCKET_FRAME_INVALID;
    } else if (length == 127) {
        if (size < at + 8) return NYA_WEBSOCKET_FRAME_INCOMPLETE;

        length = 0;
        for (u32 i = 0; i < 8; i++) length = (length << 8) | (u64)data[at + i];
        at += 8;

        if ((length >> 63) != 0) return NYA_WEBSOCKET_FRAME_INVALID;
        if (length <= 0xFFFFU) return NYA_WEBSOCKET_FRAME_INVALID;
    }

    if (masked) {
        if (size < at + 4) return NYA_WEBSOCKET_FRAME_INCOMPLETE;

        at += 4;
    }

    *out_frame = (NYA_WebSocketFrame){
        .fin          = fin,
        .opcode       = opcode,
        .masked       = masked,
        .payload_size = length,
        .header_size  = at,
    };

    if (masked)
        for (u32 i = 0; i < 4; i++) out_frame->mask[i] = data[at - 4 + i];

    nya_assert(out_frame->header_size >= 2 && out_frame->header_size <= NYA_WEBSOCKET_MAX_HEADER_BYTES);

    return NYA_WEBSOCKET_FRAME_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_websocket_create(NYA_Arena* arena, NYA_WebSocketOptions options, OUT NYA_WebSocket** out_socket) {
    nya_assert(arena != nullptr);
    nya_assert(out_socket != nullptr);

    *out_socket = nullptr;

    _NYA_WebSocketUrl url = { 0 };
    NYA_TRY(_nya_websocket_url_parse(options.url, &url));

    u64 ceiling = options.max_message_bytes == 0 ? (u64)NYA_WEBSOCKET_DEFAULT_MAX_MESSAGE_BYTES : options.max_message_bytes;

    if (ceiling < NYA_WEBSOCKET_MAX_CONTROL_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message ceiling of %llu cannot hold a control frame", (unsigned long long)ceiling);
    }

    NYA_WebSocket* socket = nya_arena_alloc(arena, sizeof(NYA_WebSocket));
    if (socket == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for a websocket");

    *socket = (NYA_WebSocket){
        .allocator         = arena,
        .state             = NYA_WEBSOCKET_STATE_CONNECTING,
        .socket            = CURL_SOCKET_BAD,
        .max_message_bytes = ceiling,
        .message_opcode    = NYA_WEBSOCKET_OPCODE_CONTINUATION,
        .read_state        = _NYA_WEBSOCKET_READ_HEADER,
    };

    // One past the ceiling, for the NUL a text event carries so a handler can treat it as a C string.
    socket->message = nya_arena_alloc(arena, ceiling + 1);

    if (socket->message == nullptr) {
        nya_arena_free(arena, socket, sizeof(NYA_WebSocket));
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for a websocket's message buffer");
    }

    u64 timeout_ms = options.handshake_timeout_ms == 0 ? (u64)NYA_WEBSOCKET_DEFAULT_TIMEOUT_MS : options.handshake_timeout_ms;

    socket->handshake_deadline_ms = nya_clock_get_timestamp_ms() + timeout_ms;

    u8 nonce[NYA_WEBSOCKET_KEY_BYTES] = { 0 };

    if (!nya_random_bytes(nonce, sizeof(nonce))) {
        nya_arena_free(arena, socket->message, ceiling + 1);
        nya_arena_free(arena, socket, sizeof(NYA_WebSocket));

        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed, so no websocket key could be made");
    }

    {
        NYA_Arena scratch = nya_arena_create_on_stack(.name = "websocket_key");
        defer     nya_arena_destroy_on_stack(&scratch);

        NYA_String* encoded = nya_string_create(&scratch);
        nya_base64_encode(encoded, nonce, sizeof(nonce));

        nya_assert(encoded->length == _NYA_WEBSOCKET_KEY_TEXT_BYTES - 1, "a 16 byte base64 is always 24 characters");
        nya_memcpy(socket->key_text, encoded->items, encoded->length);
    }

    // Computed now, so the check after the 101 is a comparison rather than a second place that knows
    // the rule.
    NYA_Error expected = nya_websocket_accept_from_key(socket->key_text, socket->expected_accept);

    if (!expected.ok) {
        nya_arena_free(arena, socket->message, ceiling + 1);
        nya_arena_free(arena, socket, sizeof(NYA_WebSocket));

        return expected;
    }

    socket->easy  = curl_easy_init();
    socket->multi = curl_multi_init();

    if (socket->easy == nullptr || socket->multi == nullptr) {
        nya_websocket_destroy(socket);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "curl could not make a handle");
    }

    (void)curl_easy_setopt(socket->easy, CURLOPT_URL, url.curl_url);

    /*
     * CONNECT_ONLY is the whole reason curl is here: it does the name resolution, the connect and the TLS
     * handshake and then stops, leaving a socket that curl_easy_send and curl_easy_recv talk through.
     * Everything above this line in the RFC is then this file's own code.
     */
    (void)curl_easy_setopt(socket->easy, CURLOPT_CONNECT_ONLY, 1L);
    (void)curl_easy_setopt(socket->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    (void)curl_easy_setopt(socket->easy, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(socket->easy, CURLOPT_PROTOCOLS_STR, "http,https");

    if (options.insecure_skip_tls_verify) {
        (void)curl_easy_setopt(socket->easy, CURLOPT_SSL_VERIFYPEER, 0L);
        (void)curl_easy_setopt(socket->easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (curl_multi_add_handle(socket->multi, socket->easy) != CURLM_OK) {
        nya_websocket_destroy(socket);
        return nya_error(NYA_ERROR_NOT_OK, "curl refused the handle");
    }

    NYA_Error queued = _nya_websocket_handshake_send(socket, &url, &options);
    if (!queued.ok) {
        nya_websocket_destroy(socket);
        return queued;
    }

    *out_socket = socket;

    return NYA_OK;
}

void nya_websocket_destroy(NYA_WebSocket* socket) {
    if (socket == nullptr) return;

    if (socket->multi != nullptr && socket->easy != nullptr) (void)curl_multi_remove_handle(socket->multi, socket->easy);
    if (socket->easy != nullptr) curl_easy_cleanup(socket->easy);
    if (socket->multi != nullptr) (void)curl_multi_cleanup(socket->multi);

    NYA_Arena* arena = socket->allocator;
    u64        size  = socket->max_message_bytes + 1;

    if (socket->message != nullptr) nya_arena_free(arena, socket->message, size);
    nya_arena_free(arena, socket, sizeof(NYA_WebSocket));
}

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_WebSocketState nya_websocket_state(const NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);

    return socket->state;
}

b8 nya_websocket_poll(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event) {
    nya_assert(socket != nullptr);
    nya_assert(out_event != nullptr);

    *out_event = (NYA_WebSocketEvent){ 0 };

    /*
     * One pass through the stages, in order, with no recursion and no loop: each stage either advances
     * the socket, leaves it exactly where it was, or closes it, and a stage that closed it falls through
     * to the CLOSED report at the bottom rather than calling back in.
     */
    if (socket->state == NYA_WEBSOCKET_STATE_CONNECTING) {
        if (_nya_websocket_pump_connect(socket) && socket->state == NYA_WEBSOCKET_STATE_CONNECTING) return false;
    }

    if (socket->state == NYA_WEBSOCKET_STATE_HANDSHAKING) {
        if (!_nya_websocket_flush(socket)) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped during the upgrade");
        } else if (_nya_websocket_handshake_receive(socket) && socket->state == NYA_WEBSOCKET_STATE_HANDSHAKING) {
            if (nya_clock_get_timestamp_ms() > socket->handshake_deadline_ms) {
                _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the upgrade timed out");
            } else {
                return false;
            }
        }
    }

    if (!socket->open_reported && socket->state == NYA_WEBSOCKET_STATE_OPEN) {
        socket->open_reported = true;

        *out_event = (NYA_WebSocketEvent){ .kind = NYA_WEBSOCKET_EVENT_OPEN, .reason = "" };
        return true;
    }

    if (socket->state == NYA_WEBSOCKET_STATE_OPEN || socket->state == NYA_WEBSOCKET_STATE_CLOSING) {
        if (!_nya_websocket_flush(socket) || !_nya_websocket_fill(socket)) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection dropped");
        } else if (_nya_websocket_drain(socket, out_event)) {
            return true;
        }
    }

    if (socket->state == NYA_WEBSOCKET_STATE_CLOSED && !socket->closed_reported) {
        socket->closed_reported = true;

        *out_event = (NYA_WebSocketEvent){
            .kind   = NYA_WEBSOCKET_EVENT_CLOSED,
            .code   = socket->close_code,
            .reason = socket->close_reason,
        };

        return true;
    }

    return false;
}

NYA_Error nya_websocket_send_text(NYA_WebSocket* socket, NYA_ConstCString text) {
    nya_assert(socket != nullptr);

    if (text == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no text to send");

    return _nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_TEXT, (const u8*)text, strlen(text));
}

NYA_Error nya_websocket_send_binary(NYA_WebSocket* socket, const u8* data, u64 size) {
    nya_assert(socket != nullptr);

    if (data == nullptr && size > 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no data to send");

    return _nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_BINARY, data, size);
}

NYA_Error nya_websocket_send_object(NYA_WebSocket* socket, NYA_Arena* arena, const NYA_Object* body) {
    nya_assert(socket != nullptr);
    nya_assert(arena != nullptr);

    if (body == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no body to send");

    NYA_String* text = nya_serialize(arena, body, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    if (text == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the body could not be serialized");

    return _nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_TEXT, text->items, text->length);
}

NYA_Error nya_websocket_ping(NYA_WebSocket* socket, const u8* data, u64 size) {
    nya_assert(socket != nullptr);

    if (size > NYA_WEBSOCKET_MAX_CONTROL_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a ping carries at most %u bytes", (u32)NYA_WEBSOCKET_MAX_CONTROL_BYTES);
    }

    return _nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_PING, data, size);
}

NYA_Error nya_websocket_close(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason) {
    nya_assert(socket != nullptr);

    if (socket->close_sent || socket->state == NYA_WEBSOCKET_STATE_CLOSED) return NYA_OK;

    socket->close_sent = true;

    // Never reached the wire, so there is nothing to say goodbye over.
    if (socket->state != NYA_WEBSOCKET_STATE_OPEN) {
        _nya_websocket_fail(socket, code, reason == nullptr ? "closed before opening" : reason);
        return NYA_OK;
    }

    u8  payload[NYA_WEBSOCKET_MAX_CONTROL_BYTES] = { 0 };
    u64 size                                     = 2;

    payload[0] = (u8)(((u32)code >> 8) & 0xFFU);
    payload[1] = (u8)((u32)code & 0xFFU);

    if (reason != nullptr) {
        // Truncated rather than refused: a close must not fail, and a reason is a courtesy.
        u64 length = nya_min(strlen(reason), (u64)_NYA_WEBSOCKET_MAX_CLOSE_REASON);

        nya_memcpy(payload + size, reason, length);
        size += length;
    }

    socket->state = NYA_WEBSOCKET_STATE_CLOSING;

    return _nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_CLOSE, payload, size);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * THE URL
 * ─────────────────────────────────────────────────────────
 */

NYA_Error _nya_websocket_url_parse(NYA_ConstCString text, OUT _NYA_WebSocketUrl* out_url) {
    nya_assert(out_url != nullptr);

    *out_url = (_NYA_WebSocketUrl){ 0 };

    if (text == nullptr || text[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket needs a url");

    b8               secure = false;
    NYA_ConstCString cursor = nullptr;

    if (nya_string_starts_with(text, "wss://")) {
        secure = true;
        cursor = text + strlen("wss://");
    } else if (nya_string_starts_with(text, "ws://")) {
        cursor = text + strlen("ws://");
    } else {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a ws or wss url", text);
    }

    // Refused rather than sent on: credentials in a url end up in logs, and the Authorization header is
    // the option that exists for this.
    for (NYA_ConstCString scan = cursor; *scan != '\0' && *scan != '/'; scan++) {
        if (*scan == '@') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket url may not carry credentials");
    }

    u64 host_length = 0;
    while (cursor[host_length] != '\0' && cursor[host_length] != ':' && cursor[host_length] != '/') host_length++;

    if (host_length == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has no host", text);
    if (host_length >= sizeof(out_url->host)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the host in '%s' is too long", text);

    nya_memcpy(out_url->host, cursor, host_length);
    cursor += host_length;

    u32 port = secure ? 443U : 80U;

    if (*cursor == ':') {
        cursor++;

        u32 parsed = 0;
        u32 digits = 0;

        while (*cursor >= '0' && *cursor <= '9') {
            parsed = (parsed * 10U) + (u32)(*cursor - '0');
            cursor++;
            digits++;

            if (parsed > 65535U || digits > 5) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the port in '%s' is not a port", text);
        }

        if (digits == 0 || parsed == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the port in '%s' is not a port", text);

        port = parsed;
    }

    u64 path_length = strlen(cursor);

    if (path_length == 0) {
        out_url->path[0] = '/';
    } else {
        if (*cursor != '/') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' has something after the port that is not a path", text);
        if (path_length >= sizeof(out_url->path)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the path in '%s' is too long", text);

        // A request line is one line, so anything that could end it early cannot be in the path.
        for (u64 i = 0; i < path_length; i++) {
            u8 character = (u8)cursor[i];
            if (character <= 0x20U || character == 0x7FU)
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the path in '%s' has a control character", text);
        }

        nya_memcpy(out_url->path, cursor, path_length);
    }

    // The host goes into a Host header and into curl's url, so the same rule applies to it.
    for (u64 i = 0; i < host_length; i++) {
        u8 character = (u8)out_url->host[i];
        if (character <= 0x20U || character == 0x7FU) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the host in '%s' has a control character", text);
    }

    out_url->secure = secure;
    out_url->port   = (u16)port;

    s32 written = snprintf(
        out_url->curl_url,
        sizeof(out_url->curl_url),
        "%s://%s:%u%s",
        secure ? "https" : "http",
        out_url->host,
        (unsigned)out_url->port,
        out_url->path
    );

    if (written < 0 || (u64)written >= sizeof(out_url->curl_url)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is too long", text);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * SHA-1
 * ─────────────────────────────────────────────────────────
 */

/*
 * FIPS 180-4, and here only because RFC 6455 names it. The four round constants below are the RFC 3174
 * ones: the first is 0x5A827999, which is floor(2^30 * sqrt(2)), and the others are the same
 * construction over sqrt(3), sqrt(5) and sqrt(10). Nothing here is a choice.
 *
 * This is not a general hash for the engine to reach for. SHA-1 is broken for anything that needs
 * collision resistance; the websocket handshake uses it as a fixed transformation of a nonce, where a
 * collision buys an attacker nothing, and that is the only place it may be used.
 */

NYA_INTERNAL u32 _nya_websocket_rotate(u32 value, u32 bits) {
    nya_assert(bits > 0 && bits < 32);

    return (value << bits) | (value >> (32U - bits));
}

// The additions below are modular by definition: SHA-1 is specified over 32 bit words that wrap, so
// the unsigned overflow sanitizer would report the algorithm working correctly. Same reason and same
// spelling as base_hash.c.
__attr_no_sanitize("unsigned-integer-overflow") void _nya_websocket_sha1(const u8* data, u64 size, OUT u8 out_digest[_NYA_WEBSOCKET_SHA1_BYTES]) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_digest != nullptr);

    u32 state[5] = { 0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U };

    /*
     * The message, its 0x80 terminator and its 64 bit length, padded to a multiple of 64. Bounded by the
     * caller: the only thing hashed here is a base64 key plus the RFC's GUID, which is 60 bytes.
     */
    nya_assert(size <= 64, "sha1 here only ever hashes a websocket key and the RFC's guid");

    u8  block[128] = { 0 };
    u64 total      = size;

    nya_memcpy(block, data, size);
    block[size] = 0x80U;

    u64 blocks = (size + 1 + 8 + 63) / 64;
    nya_assert(blocks * 64 <= sizeof(block));

    u64 bits = total * 8;
    for (u32 i = 0; i < 8; i++) block[(blocks * 64) - 1 - i] = (u8)((bits >> (8U * i)) & 0xFFU);

    for (u64 b = 0; b < blocks; b++) {
        const u8* chunk = block + (b * 64);

        u32 w[80] = { 0 };

        for (u32 i = 0; i < 16; i++) {
            w[i] = ((u32)chunk[i * 4] << 24) | ((u32)chunk[(i * 4) + 1] << 16) | ((u32)chunk[(i * 4) + 2] << 8) | (u32)chunk[(i * 4) + 3];
        }

        for (u32 i = 16; i < 80; i++) w[i] = _nya_websocket_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        u32 a = state[0];
        u32 c = state[1];
        u32 d = state[2];
        u32 e = state[3];
        u32 f = state[4];

        for (u32 i = 0; i < 80; i++) {
            u32 mix      = 0;
            u32 constant = 0;

            if (i < 20) {
                mix      = (c & d) | ((~c) & e);
                constant = 0x5A827999U;
            } else if (i < 40) {
                mix      = c ^ d ^ e;
                constant = 0x6ED9EBA1U;
            } else if (i < 60) {
                mix      = (c & d) | (c & e) | (d & e);
                constant = 0x8F1BBCDCU;
            } else {
                mix      = c ^ d ^ e;
                constant = 0xCA62C1D6U;
            }

            u32 next = _nya_websocket_rotate(a, 5) + mix + f + constant + w[i];

            f = e;
            e = d;
            d = _nya_websocket_rotate(c, 30);
            c = a;
            a = next;
        }

        state[0] += a;
        state[1] += c;
        state[2] += d;
        state[3] += e;
        state[4] += f;
    }

    for (u32 i = 0; i < 5; i++) {
        out_digest[i * 4]       = (u8)((state[i] >> 24) & 0xFFU);
        out_digest[(i * 4) + 1] = (u8)((state[i] >> 16) & 0xFFU);
        out_digest[(i * 4) + 2] = (u8)((state[i] >> 8) & 0xFFU);
        out_digest[(i * 4) + 3] = (u8)(state[i] & 0xFFU);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * THE HANDSHAKE
 * ─────────────────────────────────────────────────────────
 */

NYA_Error _nya_websocket_handshake_send(NYA_WebSocket* socket, const _NYA_WebSocketUrl* url, const NYA_WebSocketOptions* options) {
    nya_assert(socket != nullptr);
    nya_assert(url != nullptr);
    nya_assert(options != nullptr);

    char request[_NYA_WEBSOCKET_MAX_REQUEST] = { 0 };
    s32  at                                  = 0;

    /*
     * Built with snprintf into a fixed buffer rather than a string, so the whole request has one bound
     * and a header that does not fit fails here instead of being truncated onto the wire.
     */
    at = snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        url->path,
        url->host,
        (unsigned)url->port,
        socket->key_text
    );

    if (at < 0 || (u64)at >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

    if (options->subprotocol != nullptr) {
        s32 written = snprintf(request + at, sizeof(request) - (u64)at, "Sec-WebSocket-Protocol: %s\r\n", options->subprotocol);
        if (written < 0 || (u64)(at + written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

        at += written;
    }

    if (options->bearer_token != nullptr) {
        s32 written = snprintf(request + at, sizeof(request) - (u64)at, "Authorization: Bearer %s\r\n", options->bearer_token);
        if (written < 0 || (u64)(at + written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

        at += written;
    }

    static NYA_ConstCString reserved[] = {
        "host", "upgrade", "connection", "sec-websocket-key", "sec-websocket-version", "sec-websocket-protocol", "sec-websocket-accept",
    };

    for (u32 i = 0; i < NYA_REQUEST_MAX_HEADERS && options->headers[i].name != nullptr; i++) {
        NYA_ConstCString name  = options->headers[i].name;
        NYA_ConstCString value = options->headers[i].value == nullptr ? "" : options->headers[i].value;

        {
            NYA_Arena scratch = nya_arena_create_on_stack(.name = "websocket_header");
            defer     nya_arena_destroy_on_stack(&scratch);

            NYA_String* lowered = nya_string_from(&scratch, name);
            nya_string_to_lower(lowered);

            for (u32 r = 0; r < sizeof(reserved) / sizeof(reserved[0]); r++) {
                if (nya_string_equals(lowered, reserved[r])) {
                    return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is the handshake's own header and cannot be set", name);
                }
            }
        }

        // A newline in either half would let a caller append headers of its own, which is request
        // splitting. Refused rather than escaped, because there is no legal reason to send one.
        for (NYA_ConstCString scan = name; *scan != '\0'; scan++) {
            if (*scan == '\r' || *scan == '\n') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a header name may not carry a newline");
        }

        for (NYA_ConstCString scan = value; *scan != '\0'; scan++) {
            if (*scan == '\r' || *scan == '\n') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a header value may not carry a newline");
        }

        s32 written = snprintf(request + at, sizeof(request) - (u64)at, "%s: %s\r\n", name, value);
        if (written < 0 || (u64)(at + written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

        at += written;
    }

    s32 written = snprintf(request + at, sizeof(request) - (u64)at, "\r\n");
    if (written < 0 || (u64)(at + written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

    at += written;

    return _nya_websocket_queue(socket, (const u8*)request, (u64)at);
}

b8 _nya_websocket_header_matches(NYA_ConstCString text, NYA_ConstCString name, NYA_ConstCString value) {
    nya_assert(text != nullptr);
    nya_assert(name != nullptr);

    u64 name_length = strlen(name);

    for (NYA_ConstCString line = text; *line != '\0';) {
        // The header name, compared case insensitively as HTTP requires.
        b8 matched = true;

        for (u64 i = 0; i < name_length && matched; i++) {
            char left  = line[i];
            char right = name[i];

            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');

            matched = left == right;
        }

        if (matched && line[name_length] == ':') {
            NYA_ConstCString at = line + name_length + 1;
            while (*at == ' ' || *at == '\t') at++;

            if (value == nullptr) return true;

            u64 value_length = strlen(value);
            b8  same         = true;

            for (u64 i = 0; i < value_length && same; i++) {
                char left  = at[i];
                char right = value[i];

                if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
                if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');

                same = left == right;
            }

            // Only the token, so "Upgrade: websocket, foo" matches and "Upgrade: websockets" does not.
            if (same) {
                char after = at[value_length];
                if (after == '\0' || after == '\r' || after == '\n' || after == ',' || after == ' ' || after == ';') return true;
            }
        }

        while (*line != '\0' && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    return false;
}

b8 _nya_websocket_handshake_receive(NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);
    nya_assert(socket->state == NYA_WEBSOCKET_STATE_HANDSHAKING);

    u64 room = sizeof(socket->handshake) - 1 - socket->handshake_size;

    if (room > 0) {
        u64 got = 0;

        CURLcode code = curl_easy_recv(socket->easy, socket->handshake + socket->handshake_size, room, &got);

        if (code == CURLE_OK && got == 0) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the server closed during the upgrade");
            return false;
        }

        if (code != CURLE_OK && code != CURLE_AGAIN) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, curl_easy_strerror(code));
            return false;
        }

        socket->handshake_size += got;
    }

    nya_assert(socket->handshake_size < sizeof(socket->handshake));
    socket->handshake[socket->handshake_size] = '\0';

    /*
     * The blank line, found over the whole buffer each time rather than incrementally: the response is
     * at most eight kilobytes and this runs a handful of times, so the simple version is the right one.
     */
    u64 end      = 0;
    b8  complete = false;

    for (u64 i = 0; i + 3 < socket->handshake_size; i++) {
        if (socket->handshake[i] == '\r' && socket->handshake[i + 1] == '\n' && socket->handshake[i + 2] == '\r' &&
            socket->handshake[i + 3] == '\n') {
            end      = i + 4;
            complete = true;
            break;
        }
    }

    if (!complete) {
        if (socket->handshake_size >= sizeof(socket->handshake) - 1) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the upgrade response has no end");
            return false;
        }

        return true;
    }

    socket->handshake[end - 2] = '\0';

    NYA_ConstCString response = (NYA_ConstCString)socket->handshake;

    if (!nya_string_starts_with(response, "HTTP/1.1 101") && !nya_string_starts_with(response, "HTTP/1.0 101")) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the server did not switch protocols");
        return false;
    }

    if (!_nya_websocket_header_matches(response, "Upgrade", "websocket")) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the server did not name the websocket protocol");
        return false;
    }

    if (!_nya_websocket_header_matches(response, "Connection", "upgrade")) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the server did not agree to upgrade");
        return false;
    }

    if (!_nya_websocket_header_matches(response, "Sec-WebSocket-Accept", socket->expected_accept)) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the server's accept key does not answer ours");
        return false;
    }

    /*
     * Anything after the blank line is already frames. Moved rather than dropped: a server is allowed to
     * send its first message in the same packet as the 101, and obs-websocket does exactly that.
     */
    u64 extra = socket->handshake_size - end;

    if (extra > sizeof(socket->receive)) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_TOO_LARGE, "the server sent more than a read's worth with its upgrade");
        return false;
    }

    nya_memcpy(socket->receive, socket->handshake + end, extra);
    socket->receive_size = extra;

    socket->state = NYA_WEBSOCKET_STATE_OPEN;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE CONNECTION
 * ─────────────────────────────────────────────────────────
 */

b8 _nya_websocket_pump_connect(NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);
    nya_assert(socket->state == NYA_WEBSOCKET_STATE_CONNECTING);

    s32 running = 0;

    CURLMcode code = curl_multi_perform(socket->multi, &running);

    if (code != CURLM_OK) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, curl_multi_strerror(code));
        return false;
    }

    /*
     * Bounded: curl_multi_info_read hands out one message per call and the queue holds one transfer, so
     * this loop runs at most twice.
     */
    s32 remaining = 0;

    for (CURLMsg* message = curl_multi_info_read(socket->multi, &remaining); message != nullptr;
         message          = curl_multi_info_read(socket->multi, &remaining)) {
        if (message->msg != CURLMSG_DONE) continue;

        if (message->data.result != CURLE_OK) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, curl_easy_strerror(message->data.result));
            return false;
        }

        curl_socket_t connected = CURL_SOCKET_BAD;

        if (curl_easy_getinfo(socket->easy, CURLINFO_ACTIVESOCKET, &connected) != CURLE_OK || connected == CURL_SOCKET_BAD) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "curl connected but handed back no socket");
            return false;
        }

        socket->socket = connected;
        socket->state  = NYA_WEBSOCKET_STATE_HANDSHAKING;

        return true;
    }

    if (nya_clock_get_timestamp_ms() > socket->handshake_deadline_ms) {
        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the connection timed out");
        return false;
    }

    return true;
}

NYA_Error _nya_websocket_queue(NYA_WebSocket* socket, const u8* data, u64 size) {
    nya_assert(socket != nullptr);
    nya_assert(data != nullptr || size == 0);
    nya_assert(socket->send_size <= sizeof(socket->send));

    if (size == 0) return NYA_OK;

    if (size > sizeof(socket->send) - socket->send_size) {
        return nya_error(
            NYA_ERROR_OUT_OF_MEMORY,
            "%llu bytes do not fit the %llu still free in the websocket's queue",
            (unsigned long long)size,
            (unsigned long long)(sizeof(socket->send) - socket->send_size)
        );
    }

    nya_memcpy(socket->send + socket->send_size, data, size);
    socket->send_size += size;

    return NYA_OK;
}

NYA_Error _nya_websocket_queue_frame(NYA_WebSocket* socket, NYA_WebSocketOpcode opcode, const u8* data, u64 size) {
    nya_assert(socket != nullptr);

    if (socket->state == NYA_WEBSOCKET_STATE_CLOSED) return nya_error(NYA_ERROR_IO, "the websocket is closed");

    // A close is the one thing that may be sent while closing; anything else after it would arrive after
    // the goodbye.
    if (socket->state == NYA_WEBSOCKET_STATE_CLOSING && opcode != NYA_WEBSOCKET_OPCODE_CLOSE) {
        return nya_error(NYA_ERROR_IO, "the websocket is closing");
    }

    u8 mask[4] = { 0 };
    if (!nya_random_bytes(mask, sizeof(mask))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed, so no mask could be made");

    u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
    u64 header_size                            = 0;

    NYA_TRY(nya_websocket_frame_encode(opcode, true, size, mask, header, &header_size));

    nya_assert(socket->send_size <= sizeof(socket->send));

    // Room for both halves checked before either is written, so a message never goes out as a header
    // with no body, which a peer cannot recover from.
    if (header_size + size > sizeof(socket->send) - socket->send_size) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a %llu byte message does not fit the websocket's queue", (unsigned long long)size);
    }

    nya_memcpy(socket->send + socket->send_size, header, header_size);
    socket->send_size += header_size;

    u8* payload = socket->send + socket->send_size;
    for (u64 i = 0; i < size; i++) payload[i] = (u8)(data[i] ^ mask[i & 3U]);

    socket->send_size += size;

    return NYA_OK;
}

b8 _nya_websocket_flush(NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);

    if (socket->send_size == 0) return true;
    if (socket->socket == CURL_SOCKET_BAD) return true;

    u64      took = 0;
    CURLcode code = curl_easy_send(socket->easy, socket->send, socket->send_size, &took);

    if (code == CURLE_AGAIN) return true;
    if (code != CURLE_OK) return false;

    nya_assert(took <= socket->send_size);

    socket->send_size -= took;
    if (socket->send_size > 0) nya_memmove(socket->send, socket->send + took, socket->send_size);

    return true;
}

b8 _nya_websocket_fill(NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);
    nya_assert(socket->receive_size <= sizeof(socket->receive));

    u64 room = sizeof(socket->receive) - socket->receive_size;
    if (room == 0) return true;

    u64      got  = 0;
    CURLcode code = curl_easy_recv(socket->easy, socket->receive + socket->receive_size, room, &got);

    if (code == CURLE_AGAIN) return true;
    if (code != CURLE_OK) return false;

    // Zero bytes with no error is end of file; "nothing right now" is CURLE_AGAIN.
    if (got == 0) return false;

    socket->receive_size += got;

    return true;
}

void _nya_websocket_consume(NYA_WebSocket* socket, u64 count) {
    nya_assert(socket != nullptr);
    nya_assert(count <= socket->receive_size);

    socket->receive_size -= count;
    if (socket->receive_size > 0) nya_memmove(socket->receive, socket->receive + count, socket->receive_size);
}

void _nya_websocket_fail(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason) {
    nya_assert(socket != nullptr);

    if (socket->state == NYA_WEBSOCKET_STATE_CLOSED) return;

    socket->state      = NYA_WEBSOCKET_STATE_CLOSED;
    socket->close_code = code;

    socket->close_reason[0] = '\0';

    if (reason != nullptr) {
        u64 length = nya_min(strlen(reason), (u64)_NYA_WEBSOCKET_MAX_CLOSE_REASON);

        nya_memcpy(socket->close_reason, reason, length);
        socket->close_reason[length] = '\0';
    }
}

b8 _nya_websocket_is_utf8(const u8* data, u64 size) {
    nya_assert(data != nullptr || size == 0);

    for (u64 i = 0; i < size;) {
        u8  lead      = data[i];
        u32 following = 0;
        u32 codepoint = 0;

        if (lead < 0x80U) {
            i++;
            continue;
        }

        if ((lead & 0xE0U) == 0xC0U) {
            following = 1;
            codepoint = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            following = 2;
            codepoint = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            following = 3;
            codepoint = lead & 0x07U;
        } else {
            // A continuation byte on its own, or a five or six byte form that UTF-8 no longer has.
            return false;
        }

        // The continuation bytes have to be there. Written as an addition on the left so nothing underflows.
        if (i + following >= size) return false;

        for (u32 c = 1; c <= following; c++) {
            u8 continuation = data[i + c];
            if ((continuation & 0xC0U) != 0x80U) return false;

            codepoint = (codepoint << 6) | (continuation & 0x3FU);
        }

        // The shortest form is the only legal one, and the surrogate range is not a character.
        if (following == 1 && codepoint < 0x80U) return false;
        if (following == 2 && codepoint < 0x800U) return false;
        if (following == 3 && codepoint < 0x10000U) return false;
        if (codepoint > 0x10FFFFU) return false;
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) return false;

        i += following + 1;
    }

    return true;
}

b8 _nya_websocket_drain(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event) {
    nya_assert(socket != nullptr);
    nya_assert(out_event != nullptr);

    for (u32 step = 0; step < _NYA_WEBSOCKET_MAX_FRAMES_PER_POLL; step++) {
        if (socket->state == NYA_WEBSOCKET_STATE_CLOSED) return false;

        if (socket->read_state == _NYA_WEBSOCKET_READ_HEADER) {
            NYA_WebSocketFrame       frame  = { 0 };
            NYA_WebSocketFrameResult result = nya_websocket_frame_decode(socket->receive, socket->receive_size, &frame);

            if (result == NYA_WEBSOCKET_FRAME_INCOMPLETE) return false;

            if (result == NYA_WEBSOCKET_FRAME_INVALID) {
                _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the server sent a frame that is not a frame");
                return false;
            }

            // RFC 6455 section 5.1: a server never masks. A masked frame is either a confused peer or
            // something replaying a client's traffic back at us.
            if (frame.masked) {
                _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "the server masked a frame");
                return false;
            }

            b8 control = ((u32)frame.opcode & 0x8U) != 0;

            if (!control) {
                b8 assembling = socket->message_opcode != NYA_WEBSOCKET_OPCODE_CONTINUATION;

                if (frame.opcode == NYA_WEBSOCKET_OPCODE_CONTINUATION && !assembling) {
                    _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a continuation with nothing to continue");
                    return false;
                }

                if (frame.opcode != NYA_WEBSOCKET_OPCODE_CONTINUATION && assembling) {
                    _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a new message inside an unfinished one");
                    return false;
                }

                if (socket->message_size + frame.payload_size > socket->max_message_bytes) {
                    _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_TOO_LARGE, "the server sent more than this socket accepts");
                    return false;
                }

                if (frame.opcode != NYA_WEBSOCKET_OPCODE_CONTINUATION) socket->message_opcode = frame.opcode;
            }

            _nya_websocket_consume(socket, frame.header_size);

            socket->frame             = frame;
            socket->payload_remaining = frame.payload_size;
            socket->control_size      = 0;
            socket->read_state        = _NYA_WEBSOCKET_READ_PAYLOAD;
        }

        nya_assert(socket->read_state == _NYA_WEBSOCKET_READ_PAYLOAD);

        b8 control = ((u32)socket->frame.opcode & 0x8U) != 0;

        u64 available = nya_min(socket->payload_remaining, socket->receive_size);

        if (available > 0) {
            if (control) {
                nya_assert(socket->control_size + available <= sizeof(socket->control) - 1);
                nya_memcpy(socket->control + socket->control_size, socket->receive, available);
                socket->control_size += available;
            } else {
                nya_assert(socket->message_size + available <= socket->max_message_bytes);
                nya_memcpy(socket->message + socket->message_size, socket->receive, available);
                socket->message_size += available;
            }

            _nya_websocket_consume(socket, available);
            socket->payload_remaining -= available;
        }

        if (socket->payload_remaining > 0) return false;

        socket->read_state = _NYA_WEBSOCKET_READ_HEADER;

        if (control) {
            socket->control[socket->control_size] = '\0';

            switch (socket->frame.opcode) {
                case NYA_WEBSOCKET_OPCODE_PING: {
                    // Answered rather than reported: the RFC requires a pong with the same payload, and a
                    // caller has nothing to decide about one.
                    (void)_nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_PONG, socket->control, socket->control_size);
                    break;
                }

                case NYA_WEBSOCKET_OPCODE_PONG: {
                    *out_event = (NYA_WebSocketEvent){
                        .kind   = NYA_WEBSOCKET_EVENT_PONG,
                        .data   = socket->control,
                        .size   = socket->control_size,
                        .reason = "",
                    };

                    return true;
                }

                case NYA_WEBSOCKET_OPCODE_CLOSE: {
                    NYA_WebSocketClose code = NYA_WEBSOCKET_CLOSE_NONE;

                    // A close carries either nothing or a two byte code and a reason. One byte is neither.
                    if (socket->control_size == 1) {
                        _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a close frame with half a code");
                        return false;
                    }

                    if (socket->control_size >= 2) code = (NYA_WebSocketClose)(((u32)socket->control[0] << 8) | (u32)socket->control[1]);

                    // Echoed once, so the server may close the socket, unless we already said goodbye.
                    if (!socket->close_sent) {
                        socket->close_sent = true;
                        socket->state      = NYA_WEBSOCKET_STATE_CLOSING;

                        // The same code back and nothing else. A reason is the sender's to give, and
                        // echoing the peer's own bytes back at it says nothing it does not know.
                        u8  answer[2]   = { 0 };
                        u64 answer_size = 0;

                        if (socket->control_size >= 2) {
                            answer[0]   = socket->control[0];
                            answer[1]   = socket->control[1];
                            answer_size = 2;
                        }

                        (void)_nya_websocket_queue_frame(socket, NYA_WEBSOCKET_OPCODE_CLOSE, answer, answer_size);
                        (void)_nya_websocket_flush(socket);
                    }

                    _nya_websocket_fail(socket, code, socket->control_size > 2 ? (NYA_ConstCString)(socket->control + 2) : "the server closed");
                    return false;
                }

                default: nya_unreachable();
            }

            continue;
        }

        if (!socket->frame.fin) continue;

        NYA_WebSocketOpcode completed = socket->message_opcode;

        socket->message_opcode                = NYA_WEBSOCKET_OPCODE_CONTINUATION;
        socket->message[socket->message_size] = '\0';

        if (completed == NYA_WEBSOCKET_OPCODE_TEXT && !_nya_websocket_is_utf8(socket->message, socket->message_size)) {
            _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_INVALID_PAYLOAD, "a text message that is not utf-8");
            return false;
        }

        *out_event = (NYA_WebSocketEvent){
            .kind   = completed == NYA_WEBSOCKET_OPCODE_TEXT ? NYA_WEBSOCKET_EVENT_TEXT : NYA_WEBSOCKET_EVENT_BINARY,
            .data   = socket->message,
            .size   = socket->message_size,
            .reason = "",
        };

        socket->message_size = 0;

        return true;
    }

    return false;
}
