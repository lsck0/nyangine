#include <curl/curl.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct _NYA_WebSocketUrl _NYA_WebSocketUrl;

/** Longest host this module will talk to, the terminator included. Well past the DNS label limit of 253. */
#define _NYA_WEBSOCKET_MAX_HOST 256

/** Longest request target. A control protocol's path is a handful of characters; this is for query strings. */
#define _NYA_WEBSOCKET_MAX_PATH 1024

/** The whole upgrade request, headers included. Bounded so it is built on the stack and never grows. */
#define _NYA_WEBSOCKET_MAX_REQUEST 4096

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
    char key_text[NYA_WEBSOCKET_KEY_TEXT_BYTES];
    char expected_accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1];

    /** What the server said before the blank line. Bounded by NYA_WEBSOCKET_MAX_HANDSHAKE_BYTES. */
    u8  handshake[NYA_WEBSOCKET_MAX_HANDSHAKE_BYTES];
    u64 handshake_size;

    /** The upgrade request, waiting for the kernel. Nothing else goes out until it has. */
    u8  request[_NYA_WEBSOCKET_MAX_REQUEST];
    u64 request_size;

    /** Raw bytes from the socket that have not been parsed into frames yet. */
    u8  receive[NYA_WEBSOCKET_RECEIVE_BYTES];
    u64 receive_size;

    /** The framing, which is http_websocket.h's and not this file's. Opened at create, as a client. */
    NYA_WebSocketProtocol protocol;

    /** The two buffers the protocol was opened over: what it queues, and what it assembles. */
    u8  send[NYA_WEBSOCKET_SEND_BYTES];
    u8* message;

    NYA_WebSocketClose close_code;
    char               close_reason[NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES + 1];

    b8 open_reported;
    b8 closed_reported;

    /**
     * The opt-in reconnect. Off (a zeroed policy) is the default and means a drop is final; on, an
     * unexpected drop reopens the connection after a backoff. See base_reconnect.h.
     * */
    NYA_Reconnect reconnect;

    /**
     * What a redial needs, kept only when reconnect is on: the parsed url and a copy of the options
     * whose strings live in this socket's arena rather than the caller's, so a reconnect a minute later
     * does not read a url the caller has since freed.
     * */
    _NYA_WebSocketUrl    dial_url;
    NYA_WebSocketOptions dial_options;

    /** The caller asked to close, so the eventual CLOSED is expected and is never reconnected. */
    b8 user_closed;
};

/* ── the url ── */

/** Parses a ws or wss url into the only form anything else here accepts. */
NYA_INTERNAL NYA_Error _nya_websocket_url_parse(NYA_ConstCString text, OUT _NYA_WebSocketUrl* out_url) __attr_no_discard;

/* ── dialling ── */

/**
 * (Re)opens the connection: tears down any curl handles from a previous attempt, resets every buffer
 * and the framing, makes a fresh key, and starts curl connecting again over `url` and `options`. Both
 * the first connect and every reconnect go through here, so they cannot drift apart. On failure it
 * leaves no curl handle behind and the socket's `easy`/`multi` are null.
 * */
NYA_INTERNAL NYA_Error _nya_websocket_dial(NYA_WebSocket* socket, const _NYA_WebSocketUrl* url, const NYA_WebSocketOptions* options) __attr_no_discard;

/** Copies `source` into `out`, with its strings duplicated into `arena`, so a redial owns them. */
NYA_INTERNAL NYA_Error _nya_websocket_options_dup(NYA_Arena* arena, const NYA_WebSocketOptions* source, OUT NYA_WebSocketOptions* out) __attr_no_discard;

/** Duplicates `text` into `arena`. Null in is null out; the return is null only on out of memory. */
NYA_INTERNAL NYA_ConstCString _nya_cstring_dup(NYA_Arena* arena, NYA_ConstCString text) __attr_no_discard;

/**
 * On a drop, whether the socket should wait and dial again rather than report CLOSED. True moves it to
 * RECONNECTING and schedules the retry; false means the drop is final. Never true for a close the
 * caller asked for.
 * */
NYA_INTERNAL b8 _nya_websocket_should_reconnect(NYA_WebSocket* socket);

/* ── the handshake ── */

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

/** Copies the built upgrade request into the buffer the flush sends it from. */
NYA_INTERNAL NYA_Error _nya_websocket_queue_request(NYA_WebSocket* socket, const u8* data, u64 size) __attr_no_discard;

/** One read into the receive buffer. False means the connection is gone. */
NYA_INTERNAL b8 _nya_websocket_fill(NYA_WebSocket* socket) __attr_no_discard;

/** Parses whatever whole frames are buffered, producing at most one event. */
NYA_INTERNAL b8 _nya_websocket_drain(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event);

/** Takes `count` bytes off the front of the receive buffer. */
NYA_INTERNAL void _nya_websocket_consume(NYA_WebSocket* socket, u64 count);

/** Moves the socket to CLOSED with a reason, without sending anything. */
NYA_INTERNAL void _nya_websocket_fail(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    // field by field after a memset: the struct carries a sixty four kilobyte queue, and a compound
    // literal of the whole of it is that much stack in an unoptimized build.
    nya_memset(socket, 0, sizeof(*socket));
    socket->allocator         = arena;
    socket->state             = NYA_WEBSOCKET_STATE_CONNECTING;
    socket->socket            = CURL_SOCKET_BAD;
    socket->max_message_bytes = ceiling;

    // One past the ceiling, for the NUL a text event carries so a handler can treat it as a C string.
    socket->message = nya_arena_alloc(arena, ceiling + 1);

    if (socket->message == nullptr) {
        nya_arena_free(arena, socket, sizeof(NYA_WebSocket));
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for a websocket's message buffer");
    }

    /*
     * The reconnect policy, off unless the caller asked for it. When it is on, the redial a minute from
     * now must not read the caller's url and headers — those are the caller's to free the moment create
     * returns — so a copy of the options with its strings in this socket's arena, and the parsed url,
     * are kept. The first connect then dials from that same copy, so the first attempt and every
     * reconnect are byte for byte the same request.
     */
    nya_reconnect_init(&socket->reconnect, options.reconnect);

    const _NYA_WebSocketUrl*    dial_url     = &url;
    const NYA_WebSocketOptions* dial_options = &options;

    if (options.reconnect.enabled) {
        NYA_Error copied = _nya_websocket_options_dup(arena, &options, &socket->dial_options);

        if (!copied.ok) {
            nya_arena_free(arena, socket->message, ceiling + 1);
            nya_arena_free(arena, socket, sizeof(NYA_WebSocket));

            return copied;
        }

        socket->dial_url = url;
        dial_url         = &socket->dial_url;
        dial_options     = &socket->dial_options;
    }

    NYA_Error dialed = _nya_websocket_dial(socket, dial_url, dial_options);

    if (!dialed.ok) {
        // _nya_websocket_dial leaves no curl handle behind on failure, so what is left to free is the
        // arena the socket itself came out of.
        nya_arena_free(arena, socket->message, ceiling + 1);
        nya_arena_free(arena, socket, sizeof(NYA_WebSocket));

        return dialed;
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
     * Waiting out a backoff after a drop: nothing happens until the retry is due, and when it is the
     * socket dials again and falls through to the CONNECTING stage below in this same call. A dial that
     * cannot even start is another drop, handled where every other drop is, at the CLOSED report.
     */
    if (socket->state == NYA_WEBSOCKET_STATE_RECONNECTING) {
        if (!nya_reconnect_due(&socket->reconnect, nya_clock_get_timestamp_ms())) return false;

        // Dialling moves the socket to CONNECTING, so this branch will not fire again for this attempt;
        // the wait is cleared for good on the next OPEN, or reset by the next drop.
        NYA_Error dialed = _nya_websocket_dial(socket, &socket->dial_url, &socket->dial_options);

        if (!dialed.ok) _nya_websocket_fail(socket, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the reconnect could not be started");
    }

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

        // A connect that reached OPEN is a good one, so the backoff starts over: the next drop, if any,
        // waits base_ms again rather than picking up where the last run of failures left off. Harmless
        // on the first connect, where the attempt count is already zero.
        nya_reconnect_connected(&socket->reconnect);

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
        // Before the drop is announced, the one place that decides a drop is not final: if the policy is
        // on, the caller did not ask for this, and there are attempts left, the socket goes quiet and
        // dials again later instead of reporting CLOSED. The peer never learns the difference.
        if (_nya_websocket_should_reconnect(socket)) {
            socket->state = NYA_WEBSOCKET_STATE_RECONNECTING;
            return false;
        }

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

    return nya_websocket_protocol_send(&socket->protocol, NYA_WEBSOCKET_OPCODE_TEXT, (const u8*)text, strlen(text));
}

NYA_Error nya_websocket_send_binary(NYA_WebSocket* socket, const u8* data, u64 size) {
    nya_assert(socket != nullptr);

    if (data == nullptr && size > 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no data to send");

    return nya_websocket_protocol_send(&socket->protocol, NYA_WEBSOCKET_OPCODE_BINARY, data, size);
}

NYA_Error nya_websocket_send_object(NYA_WebSocket* socket, NYA_Arena* arena, const NYA_Object* body) {
    nya_assert(socket != nullptr);
    nya_assert(arena != nullptr);

    if (body == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no body to send");

    NYA_String* text = nya_serialize(arena, body, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    if (text == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the body could not be serialized");

    return nya_websocket_protocol_send(&socket->protocol, NYA_WEBSOCKET_OPCODE_TEXT, text->items, text->length);
}

NYA_Error nya_websocket_ping(NYA_WebSocket* socket, const u8* data, u64 size) {
    nya_assert(socket != nullptr);

    return nya_websocket_protocol_send(&socket->protocol, NYA_WEBSOCKET_OPCODE_PING, data, size);
}

NYA_Error nya_websocket_close(NYA_WebSocket* socket, NYA_WebSocketClose code, NYA_ConstCString reason) {
    nya_assert(socket != nullptr);

    // The caller asked, so whatever CLOSED follows is expected and is never reconnected, even mid-wait.
    socket->user_closed = true;

    // Asked to close while waiting out a backoff: there is no socket to say goodbye over, so end it now.
    if (socket->state == NYA_WEBSOCKET_STATE_RECONNECTING) {
        _nya_websocket_fail(socket, code, reason == nullptr ? "closed while reconnecting" : reason);
        return NYA_OK;
    }

    if (socket->state == NYA_WEBSOCKET_STATE_CLOSED || nya_websocket_protocol_is_closing(&socket->protocol)) return NYA_OK;

    // Never reached the wire, so there is nothing to say goodbye over.
    if (socket->state != NYA_WEBSOCKET_STATE_OPEN) {
        _nya_websocket_fail(socket, code, reason == nullptr ? "closed before opening" : reason);
        return NYA_OK;
    }

    socket->state = NYA_WEBSOCKET_STATE_CLOSING;

    return nya_websocket_protocol_close(&socket->protocol, code, reason);
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

    // the one url parser; what is left here is what a websocket adds to its rules.
    NYA_Url url = { 0 };
    NYA_TRY(nya_url_parse(text, strlen(text), &url, nullptr));

    if (url.scheme != NYA_URL_SCHEME_WS && url.scheme != NYA_URL_SCHEME_WSS) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a ws or wss url", text);

    // Refused rather than sent on: credentials in a url end up in logs, and the Authorization header is
    // the option that exists for this.
    if (url.has_userinfo) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket url may not carry credentials");

    // RFC 6455 3: a fragment has no meaning in a websocket url and must not be used.
    if (url.has_fragment) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket url may not carry a fragment");

    b8 secure = url.scheme == NYA_URL_SCHEME_WSS;
    b8 ipv6   = url.host_kind == NYA_URL_HOST_IPV6;

    // bracketed when it is an IPv6 literal, since both the Host header and curl's url need it that way.
    s32 host_written = snprintf(out_url->host, sizeof(out_url->host), ipv6 ? "[%.*s]" : "%.*s", (int)url.host.length, url.text + url.host.offset);
    if (host_written < 0 || (u64)host_written >= sizeof(out_url->host)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the host in '%s' is too long", text);

    // the path stays encoded: it goes onto the request line exactly as the url wrote it. Empty means the root.
    s32 path_written = snprintf(
        out_url->path,
        sizeof(out_url->path),
        "%s%.*s%s%.*s",
        url.path.length == 0 ? "/" : "",
        (int)url.path.length,
        url.text + url.path.offset,
        url.has_query ? "?" : "",
        (int)url.query.length,
        url.text + url.query.offset
    );
    if (path_written < 0 || (u64)path_written >= sizeof(out_url->path)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the path in '%s' is too long", text);

    out_url->secure = secure;
    out_url->port   = url.has_port ? url.port : (secure ? 443U : 80U);

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
 * DIALLING
 * ─────────────────────────────────────────────────────────
 */

NYA_ConstCString _nya_cstring_dup(NYA_Arena* arena, NYA_ConstCString text) {
    nya_assert(arena != nullptr);

    if (text == nullptr) return nullptr;

    u64   size = strlen(text) + 1;
    char* copy = nya_arena_alloc(arena, size);

    if (copy == nullptr) return nullptr;

    nya_memcpy(copy, text, size);

    return copy;
}

NYA_Error _nya_websocket_options_dup(NYA_Arena* arena, const NYA_WebSocketOptions* source, OUT NYA_WebSocketOptions* out) {
    nya_assert(arena != nullptr);
    nya_assert(source != nullptr);
    nya_assert(out != nullptr);

    // Scalars and the reconnect policy come across by value; every pointer is replaced with a copy in
    // `arena` below, since the originals are the caller's and outlive nothing.
    *out = *source;

    if (source->url != nullptr && (out->url = _nya_cstring_dup(arena, source->url)) == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to keep the websocket url for a reconnect");
    }

    if (source->subprotocol != nullptr && (out->subprotocol = _nya_cstring_dup(arena, source->subprotocol)) == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to keep the subprotocol for a reconnect");
    }

    if (source->bearer_token != nullptr && (out->bearer_token = _nya_cstring_dup(arena, source->bearer_token)) == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to keep the bearer token for a reconnect");
    }

    for (u32 i = 0; i < NYA_REQUEST_MAX_HEADERS && source->headers[i].name != nullptr; i++) {
        if ((out->headers[i].name = _nya_cstring_dup(arena, source->headers[i].name)) == nullptr) {
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to keep a websocket header for a reconnect");
        }

        if (source->headers[i].value != nullptr && (out->headers[i].value = _nya_cstring_dup(arena, source->headers[i].value)) == nullptr) {
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room to keep a websocket header for a reconnect");
        }
    }

    return NYA_OK;
}

b8 _nya_websocket_should_reconnect(NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);

    // A close the caller asked for is expected, and a policy that is off is the old behaviour: either
    // way the drop is final.
    if (socket->user_closed) return false;
    if (!nya_reconnect_enabled(&socket->reconnect)) return false;

    // Everything else is an unexpected drop: an abnormal close, a server going away, a protocol error
    // this end raised. Redialling a socket that fails the same way every time is bounded by the policy's
    // attempt cap, so even a doomed reconnect gives up rather than spinning.
    return nya_reconnect_dropped(&socket->reconnect, nya_clock_get_timestamp_ms());
}

NYA_Error _nya_websocket_dial(NYA_WebSocket* socket, const _NYA_WebSocketUrl* url, const NYA_WebSocketOptions* options) {
    nya_assert(socket != nullptr);
    nya_assert(url != nullptr);
    nya_assert(options != nullptr);

    // Tear down whatever a previous attempt left, so a reconnect begins from the clean slate a first
    // connect does.
    if (socket->multi != nullptr && socket->easy != nullptr) (void)curl_multi_remove_handle(socket->multi, socket->easy);
    if (socket->easy != nullptr) curl_easy_cleanup(socket->easy);
    if (socket->multi != nullptr) (void)curl_multi_cleanup(socket->multi);

    socket->easy   = nullptr;
    socket->multi  = nullptr;
    socket->socket = CURL_SOCKET_BAD;

    // Every buffer and every report flag back to where create leaves them.
    socket->handshake_size  = 0;
    socket->request_size    = 0;
    socket->receive_size    = 0;
    socket->open_reported   = false;
    socket->closed_reported = false;
    socket->close_code      = NYA_WEBSOCKET_CLOSE_NONE;
    socket->close_reason[0] = '\0';

    /*
     * Opened before the socket is: the protocol has no socket in it, so a caller may queue a message in
     * the same breath as the create and it goes out with the first flush after the 101. This end is
     * always a client.
     */
    NYA_TRY(nya_websocket_protocol_open(
        &socket->protocol,
        (NYA_WebSocketProtocolConfig){
            .role             = NYA_WEBSOCKET_ROLE_CLIENT,
            .message          = socket->message,
            .message_capacity = socket->max_message_bytes,
            .send             = socket->send,
            .send_capacity    = sizeof(socket->send),
        }
    ));

    u64 timeout_ms = options->handshake_timeout_ms == 0 ? (u64)NYA_WEBSOCKET_DEFAULT_TIMEOUT_MS : options->handshake_timeout_ms;

    socket->handshake_deadline_ms = nya_clock_get_timestamp_ms() + timeout_ms;

    // A fresh nonce every attempt: the key is per connection, and the answer the server is obliged to
    // give is checked against this one.
    u8 nonce[NYA_WEBSOCKET_KEY_BYTES] = { 0 };

    if (!nya_os_random_bytes(nonce, sizeof(nonce))) {
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed, so no websocket key could be made");
    }

    {
        NYA_Arena scratch = nya_arena_create_on_stack(.name = "websocket_key");
        defer     nya_arena_destroy_on_stack(&scratch);

        NYA_String* encoded = nya_string_create(&scratch);
        nya_base64_encode(encoded, nonce, sizeof(nonce));

        nya_assert(encoded->length == NYA_WEBSOCKET_KEY_TEXT_BYTES - 1, "a 16 byte base64 is always 24 characters");
        nya_memcpy(socket->key_text, encoded->items, encoded->length);
    }

    // Computed now, so the check after the 101 is a comparison rather than a second place that knows the rule.
    NYA_TRY(nya_websocket_accept_from_key(socket->key_text, socket->expected_accept));

    socket->easy  = curl_easy_init();
    socket->multi = curl_multi_init();

    if (socket->easy == nullptr || socket->multi == nullptr) {
        if (socket->easy != nullptr) curl_easy_cleanup(socket->easy);
        if (socket->multi != nullptr) (void)curl_multi_cleanup(socket->multi);

        socket->easy  = nullptr;
        socket->multi = nullptr;

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "curl could not make a handle");
    }

    (void)curl_easy_setopt(socket->easy, CURLOPT_URL, url->curl_url);

    /*
     * CONNECT_ONLY is the whole reason curl is here: it does the name resolution, the connect and the TLS
     * handshake and then stops, leaving a socket that curl_easy_send and curl_easy_recv talk through.
     * Everything above this line in the RFC is then this file's own code.
     */
    (void)curl_easy_setopt(socket->easy, CURLOPT_CONNECT_ONLY, 1L);
    (void)curl_easy_setopt(socket->easy, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    (void)curl_easy_setopt(socket->easy, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(socket->easy, CURLOPT_PROTOCOLS_STR, "http,https");

    if (options->insecure_skip_tls_verify) {
        (void)curl_easy_setopt(socket->easy, CURLOPT_SSL_VERIFYPEER, 0L);
        (void)curl_easy_setopt(socket->easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (curl_multi_add_handle(socket->multi, socket->easy) != CURLM_OK) {
        curl_easy_cleanup(socket->easy);
        (void)curl_multi_cleanup(socket->multi);

        socket->easy  = nullptr;
        socket->multi = nullptr;

        return nya_error(NYA_ERROR_NOT_OK, "curl refused the handle");
    }

    NYA_Error queued = _nya_websocket_handshake_send(socket, url, options);

    if (!queued.ok) {
        (void)curl_multi_remove_handle(socket->multi, socket->easy);
        curl_easy_cleanup(socket->easy);
        (void)curl_multi_cleanup(socket->multi);

        socket->easy  = nullptr;
        socket->multi = nullptr;

        return queued;
    }

    socket->state = NYA_WEBSOCKET_STATE_CONNECTING;

    return NYA_OK;
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
        if (written < 0 || ((u64)at + (u64)written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

        at += written;
    }

    if (options->bearer_token != nullptr) {
        s32 written = snprintf(request + at, sizeof(request) - (u64)at, "Authorization: Bearer %s\r\n", options->bearer_token);
        if (written < 0 || ((u64)at + (u64)written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

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
        if (written < 0 || ((u64)at + (u64)written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

        at += written;
    }

    s32 written = snprintf(request + at, sizeof(request) - (u64)at, "\r\n");
    if (written < 0 || ((u64)at + (u64)written) >= sizeof(request)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the upgrade request does not fit");

    at += written;

    return _nya_websocket_queue_request(socket, (const u8*)request, (u64)at);
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

NYA_Error _nya_websocket_queue_request(NYA_WebSocket* socket, const u8* data, u64 size) {
    nya_assert(socket != nullptr);
    nya_assert(data != nullptr || size == 0);
    nya_assert(socket->request_size + size <= sizeof(socket->request), "the request was bounded where it was built");

    nya_memcpy(socket->request + socket->request_size, data, size);
    socket->request_size += size;

    return NYA_OK;
}

b8 _nya_websocket_flush(NYA_WebSocket* socket) {
    nya_assert(socket != nullptr);

    if (socket->socket == CURL_SOCKET_BAD) return true;

    /*
     * The upgrade request goes out first and alone. RFC 6455 section 4.1: a client sends no frame
     * before the 101 has come back, so anything the caller queued in the meantime waits behind it.
     */
    if (socket->request_size > 0) {
        u64      took = 0;
        CURLcode code = curl_easy_send(socket->easy, socket->request, socket->request_size, &took);

        if (code == CURLE_AGAIN) return true;
        if (code != CURLE_OK) return false;

        nya_assert(took <= socket->request_size);

        socket->request_size -= took;
        if (socket->request_size > 0) nya_memmove(socket->request, socket->request + took, socket->request_size);

        return true;
    }

    if (socket->state != NYA_WEBSOCKET_STATE_OPEN && socket->state != NYA_WEBSOCKET_STATE_CLOSING) return true;

    u64       pending = 0;
    const u8* queued  = nya_websocket_protocol_pending(&socket->protocol, &pending);

    if (pending == 0) return true;

    u64      took = 0;
    CURLcode code = curl_easy_send(socket->easy, queued, pending, &took);

    if (code == CURLE_AGAIN) return true;
    if (code != CURLE_OK) return false;

    nya_websocket_protocol_flushed(&socket->protocol, took);

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
        u64 length = nya_min(strlen(reason), (u64)NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES);

        nya_memcpy(socket->close_reason, reason, length);
        socket->close_reason[length] = '\0';
    }

    // Nothing more will be framed on a connection that is over, and the protocol drops whatever it had
    // queued, so a caller cannot flush a message into a socket nobody is reading.
    nya_websocket_protocol_fail(&socket->protocol, code, socket->close_reason);
}

b8 _nya_websocket_drain(NYA_WebSocket* socket, OUT NYA_WebSocketEvent* out_event) {
    nya_assert(socket != nullptr);
    nya_assert(out_event != nullptr);

    u64 consumed = 0;
    b8  produced = nya_websocket_protocol_receive(&socket->protocol, socket->receive, socket->receive_size, &consumed, out_event);

    _nya_websocket_consume(socket, consumed);

    if (!produced) return false;

    if (out_event->kind != NYA_WEBSOCKET_EVENT_CLOSED) return true;

    // The goodbye the protocol queued for the peer goes out before this end gives the socket up; the
    // CLOSED report itself is nya_websocket_poll's, so it is made exactly once.
    (void)_nya_websocket_flush(socket);

    _nya_websocket_fail(socket, out_event->code, out_event->reason);

    return false;
}
