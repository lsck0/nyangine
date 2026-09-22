/**
 * The WebSocket frame decoder and the protocol over it, fed whatever.
 *
 * Every byte of a WebSocket that is not the handshake comes through here, from a peer that chose all
 * of them: the lengths, the mask, where the fragments end, and how big it says the next payload is.
 * Both ends of this engine run this code — the server in http_websocket_server.c and the curl client
 * in plugins/curl/websocket.c — so a hole here is a hole in both.
 *
 * A refusal is the expected answer to hostile input and is not a finding. A crash, a read out of
 * bounds, an overflow, or an assertion reached from these bytes is. So is a message that grew past the
 * buffer it was assembled in, a frame accepted the wrong way round for its role, or a decoded header
 * that does not re-encode to the bytes it came from.
 **/

// clang-format off
// The engine defines the feature test macros this whole build needs, so it comes first. Sorted into
// any other order, a libc header arrives before base_basic.h and the build fails on a redefinition
// of _POSIX_C_SOURCE and on half of <signal.h> being missing.
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"
// clang-format on

#define FUZZ_TARGET "websocket_frame"

/** What a protocol under test assembles a message in, and queues its answers in. */
#define FUZZ_MESSAGE_BYTES 4096

/** Bounded so a peer that keeps a protocol busy cannot keep this loop going forever either. */
#define FUZZ_MAX_STEPS 512

/**
 * Opens one protocol over arena buffers and feeds it `data` in `slice` byte pieces, zero meaning all of
 * it at once. False when the step bound cut the walk short, so a comparison between two walks knows not
 * to read anything into where they stopped.
 * */
static b8 fuzz_protocol(NYA_Arena* arena, NYA_WebSocketRole role, const u8* data, u64 size, u64 slice, OUT NYA_WebSocketProtocol* out_protocol) {
    u8* message = nya_arena_alloc(arena, FUZZ_MESSAGE_BYTES + 1);
    u8* send    = nya_arena_alloc(arena, FUZZ_MESSAGE_BYTES);

    if (message == nullptr || send == nullptr) return false;

    NYA_EXPECT(nya_websocket_protocol_open(
        out_protocol,
        (NYA_WebSocketProtocolConfig){
            .role             = role,
            .message          = message,
            .message_capacity = FUZZ_MESSAGE_BYTES,
            .send             = send,
            .send_capacity    = FUZZ_MESSAGE_BYTES,
        }
    ));

    u64 at = 0;

    for (u32 step = 0; step < FUZZ_MAX_STEPS && at < size; step++) {
        u64 offered = slice == 0 ? size - at : nya_min(slice, size - at);

        NYA_WebSocketEvent event    = { 0 };
        u64                consumed = 0;

        b8 produced = nya_websocket_protocol_receive(out_protocol, data + at, offered, &consumed, &event);

        nya_assert(consumed <= offered, "the protocol consumed more than it was given");

        // a closed protocol takes nothing more, which is what stops a peer being answered after goodbye.
        if (nya_websocket_protocol_is_closed(out_protocol)) {
            nya_assert(event.kind == NYA_WEBSOCKET_EVENT_CLOSED || !produced, "a close was reported as something else");
            return true;
        }

        if (produced) {
            nya_assert(event.reason != nullptr, "an event with no reason string");
            nya_assert(event.size <= FUZZ_MESSAGE_BYTES, "a message grew past the buffer it was assembled in");

            // what a handler does with a text message: read it as a C string, which the NUL past the
            // end is the whole promise of.
            if (event.kind == NYA_WEBSOCKET_EVENT_TEXT) nya_assert(event.data[event.size] == '\0', "a text message is not terminated");
        }

        at += consumed;

        // nothing taken and nothing produced means the protocol wants bytes that are not there.
        if (consumed == 0 && !produced) {
            if (slice == 0 || at + offered >= size) return true;

            // a slice that was not enough for a header: hand over the next one too.
            slice *= 2;
        }

        u64       pending = 0;
        const u8* queued  = nya_websocket_protocol_pending(out_protocol, &pending);

        nya_assert(pending <= FUZZ_MESSAGE_BYTES, "the send queue grew past what it was opened over");
        nya_assert(pending == 0 || queued != nullptr, "bytes queued and nothing to send them from");

        // as a caller does: what was queued has gone to the socket, so the queue cannot grow forever
        // while a peer keeps sending pings.
        nya_websocket_protocol_flushed(out_protocol, pending);
    }

    return at >= size;
}

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_websocket_frame");
    defer      nya_arena_destroy(arena);

    /*
     * The decoder on its own. Everything downstream reads the payload at `header_size` for
     * `payload_size` bytes on the strength of these.
     */
    NYA_WebSocketFrame       frame  = { 0 };
    NYA_WebSocketFrameResult result = nya_websocket_frame_decode(data, size, &frame);

    if (result == NYA_WEBSOCKET_FRAME_OK) {
        nya_assert(frame.header_size >= 2 && frame.header_size <= NYA_WEBSOCKET_MAX_HEADER_BYTES, "a header of an impossible length");
        nya_assert(frame.header_size <= size, "a header longer than the bytes it was read from");
        nya_assert((frame.payload_size >> 63) == 0, "a payload length with the reserved bit set");

        b8 control =
            frame.opcode == NYA_WEBSOCKET_OPCODE_CLOSE || frame.opcode == NYA_WEBSOCKET_OPCODE_PING || frame.opcode == NYA_WEBSOCKET_OPCODE_PONG;

        nya_assert(!control || frame.fin, "a fragmented control frame parsed");
        nya_assert(!control || frame.payload_size <= NYA_WEBSOCKET_MAX_CONTROL_BYTES, "an oversized control frame parsed");

        /*
         * The shortest form is the only legal spelling of a length, so a header that decoded has to be
         * the header the encoder writes for what it decoded to. A second spelling of one frame is how a
         * peer says one thing to this parser and another to whatever is in front of it.
         */
        u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
        u64 header_size                            = 0;

        NYA_Error encoded =
            nya_websocket_frame_encode(frame.opcode, frame.fin, frame.payload_size, frame.masked ? frame.mask : nullptr, header, &header_size);

        nya_assert(encoded.ok, "a frame that decoded could not be written back");
        nya_assert(header_size == frame.header_size, "a header decoded at one length and encoded at another");
        nya_assert(nya_memcmp(header, data, header_size) == 0, "a header that is not the shortest spelling of itself parsed");
    }

    /*
     * And the protocol over it, from both ends. A server refuses what a client must mask and a client
     * refuses what a server must not, so the same bytes are legal to at most one of them.
     */
    NYA_WebSocketProtocol server = { 0 };
    NYA_WebSocketProtocol client = { 0 };

    b8 whole = fuzz_protocol(arena, NYA_WEBSOCKET_ROLE_SERVER, data, size, 0, &server);

    (void)fuzz_protocol(arena, NYA_WEBSOCKET_ROLE_CLIENT, data, size, 0, &client);

    /*
     * The same bytes again, a byte at a time. A stream is whatever the kernel hands over, so a frame
     * arriving in pieces has to end where it ended when it arrived whole; a decoder that reads the
     * header differently across a split is one a peer can steer by choosing its packet sizes.
     */
    NYA_WebSocketProtocol dribbled = { 0 };

    b8 split = fuzz_protocol(arena, NYA_WEBSOCKET_ROLE_SERVER, data, size, 1, &dribbled);

    // only where both walks reached the end of the input: one that stopped at the step bound stopped
    // somewhere the other did not, and nothing about that is a finding.
    if (!whole || !split) return;

    nya_assert(
        nya_websocket_protocol_is_closed(&dribbled) == nya_websocket_protocol_is_closed(&server),
        "the same bytes ended the connection when they arrived whole and did not when they arrived split"
    );

    nya_assert(
        nya_websocket_protocol_close_code(&dribbled) == nya_websocket_protocol_close_code(&server),
        "the same bytes closed with %s whole and %s split",
        nya_websocket_close_name(nya_websocket_protocol_close_code(&server)),
        nya_websocket_close_name(nya_websocket_protocol_close_code(&dribbled))
    );

    /*
     * The handshake's own hash, which is the other thing a stranger's bytes reach: a key is whatever
     * arrived in the header, and the only thing standing between it and a fixed stack buffer is the
     * length check.
     */
    char key[64] = { 0 };

    u64 length = nya_min(size, sizeof(key) - 1);
    nya_memcpy(key, data, length);

    // a NUL inside the input would end the key early, which is exactly what a header value does.
    char accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1] = { 0 };

    if (nya_websocket_accept_from_key(key, accept).ok) {
        nya_assert(strlen(key) == NYA_WEBSOCKET_KEY_TEXT_BYTES - 1, "a key that is not a key's length was hashed");
        nya_assert(strlen(accept) == NYA_WEBSOCKET_ACCEPT_LENGTH, "an accept of the wrong length");
    } else {
        nya_assert(accept[0] == '\0', "a refused key left something behind to answer with");
    }
}

#include "tests/fuzz/fuzz.h"
