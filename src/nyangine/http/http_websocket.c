#include <string.h>

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_base64.h"
#include "nyangine/base/base_compare.h"
#include "nyangine/base/base_string.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/http/http_websocket.h"
#include "nyangine/os/os_random.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether `opcode` is one of the six the RFC defines, rather than a reserved value. */
NYA_INTERNAL b8 _nya_websocket_opcode_is_known(NYA_WebSocketOpcode opcode) __attr_no_discard;

/** Whether `opcode` names a control frame, which is the high bit of the four. */
NYA_INTERNAL b8 _nya_websocket_opcode_is_control(NYA_WebSocketOpcode opcode) __attr_no_discard;

/** Whether `data` is well formed UTF-8, which RFC 6455 requires of every text message and close reason. */
NYA_INTERNAL b8 _nya_websocket_is_utf8(const u8* data, u64 size) __attr_no_discard;

/** Whether a peer may put `code` in a close frame. The reserved ones say "no code was sent" and cannot be sent. */
NYA_INTERNAL b8 _nya_websocket_close_code_is_sendable(u32 code) __attr_no_discard;

/** Copies `reason` into the protocol's own bounded buffer, truncating rather than failing. */
NYA_INTERNAL void _nya_websocket_reason_set(NYA_WebSocketProtocol* protocol, NYA_ConstCString reason);

/** Says goodbye with `code`, moves to closed, and fills the event that reports it. */
NYA_INTERNAL void
_nya_websocket_protocol_refuse(NYA_WebSocketProtocol* protocol, NYA_WebSocketClose code, NYA_ConstCString reason, OUT NYA_WebSocketEvent* out_event);

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

    *out_header_size = 0;

    if (!_nya_websocket_opcode_is_known(opcode)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "0x%x is not a websocket opcode", (u32)opcode);

    b8 control = _nya_websocket_opcode_is_control(opcode);

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

    // The mask bit is the role: a client masks every frame and a server masks none, so a null key here
    // is a server's frame rather than a client's that forgot one.
    u32 masked = mask != nullptr ? 0x80U : 0U;

    if (payload_size < 126) {
        out_header[at++] = (u8)(masked | (u32)payload_size);
    } else if (payload_size <= 0xFFFFU) {
        out_header[at++] = (u8)(masked | 126U);
        out_header[at++] = (u8)((payload_size >> 8) & 0xFFU);
        out_header[at++] = (u8)(payload_size & 0xFFU);
    } else {
        out_header[at++] = (u8)(masked | 127U);
        for (s32 shift = 56; shift >= 0; shift -= 8) out_header[at++] = (u8)((payload_size >> shift) & 0xFFU);
    }

    if (mask != nullptr)
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

    if (!_nya_websocket_opcode_is_known(opcode)) return NYA_WEBSOCKET_FRAME_INVALID;

    b8 fin     = (first & 0x80U) != 0;
    b8 masked  = (second & 0x80U) != 0;
    b8 control = _nya_websocket_opcode_is_control(opcode);

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

NYA_Error nya_websocket_accept_from_key(NYA_ConstCString key, OUT char out_accept[NYA_WEBSOCKET_ACCEPT_LENGTH + 1]) {
    nya_assert(out_accept != nullptr);

    out_accept[0] = '\0';

    if (key == nullptr || key[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket key may not be empty");

    u64 length = strlen(key);

    // A key is base64 of sixteen bytes, so it is always exactly this long. A longer one is refused
    // rather than hashed, which is what keeps the material below a fixed size on the stack.
    if (length != NYA_WEBSOCKET_KEY_TEXT_BYTES - 1) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "a websocket key is %u characters, not %llu",
            (u32)(NYA_WEBSOCKET_KEY_TEXT_BYTES - 1),
            (unsigned long long)length
        );
    }

    u8  material[NYA_WEBSOCKET_KEY_TEXT_BYTES + sizeof(NYA_WEBSOCKET_ACCEPT_GUID)] = { 0 };
    u64 material_size                                                              = 0;

    nya_memcpy(material, key, length);
    material_size += length;

    u64 guid_length = strlen(NYA_WEBSOCKET_ACCEPT_GUID);

    nya_assert(material_size + guid_length <= sizeof(material));
    nya_memcpy(material + material_size, NYA_WEBSOCKET_ACCEPT_GUID, guid_length);
    material_size += guid_length;

    // SHA-1 because RFC 6455 section 4.2.2 names it; see nya_crypto_sha1 for why that is the only use allowed.
    NYA_CryptoSha1Digest digest = { 0 };
    nya_crypto_sha1(material, material_size, &digest);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "websocket_accept");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_String* encoded = nya_string_create(&scratch);
    nya_base64_encode(encoded, digest.bytes, sizeof(digest.bytes));

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

/*
 * ─────────────────────────────────────────────────────────
 * THE PROTOCOL
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_websocket_protocol_open(OUT NYA_WebSocketProtocol* protocol, NYA_WebSocketProtocolConfig config) {
    nya_assert(protocol != nullptr);

    nya_memset(protocol, 0, sizeof(*protocol));

    if (config.role != NYA_WEBSOCKET_ROLE_CLIENT && config.role != NYA_WEBSOCKET_ROLE_SERVER) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket is one end or the other");
    }

    if (config.message == nullptr || config.send == nullptr)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a websocket protocol owns no memory of its own");

    if (config.message_capacity < NYA_WEBSOCKET_MAX_CONTROL_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message ceiling of %llu cannot hold a frame", (unsigned long long)config.message_capacity);
    }

    // A goodbye is the one thing that must always fit, since it is what every refusal below sends.
    if (config.send_capacity < NYA_WEBSOCKET_MAX_HEADER_BYTES + NYA_WEBSOCKET_MAX_CONTROL_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a send queue that cannot hold a close frame");
    }

    u64 max_frame_bytes = config.max_frame_bytes == 0 ? config.message_capacity : config.max_frame_bytes;

    if (max_frame_bytes > config.message_capacity) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a frame bound above the message bound is not a bound");
    }

    protocol->role             = config.role;
    protocol->message          = config.message;
    protocol->message_capacity = config.message_capacity;
    protocol->message_opcode   = NYA_WEBSOCKET_OPCODE_CONTINUATION;
    protocol->send             = config.send;
    protocol->send_capacity    = config.send_capacity;
    protocol->max_frame_bytes  = max_frame_bytes;

    return NYA_OK;
}

NYA_Error nya_websocket_protocol_close(NYA_WebSocketProtocol* protocol, NYA_WebSocketClose code, NYA_ConstCString reason) {
    nya_assert(protocol != nullptr);

    if (protocol->closed || protocol->close_sent) return NYA_OK;

    u8  payload[NYA_WEBSOCKET_MAX_CONTROL_BYTES] = { 0 };
    u64 size                                     = 2;

    payload[0] = (u8)(((u32)code >> 8) & 0xFFU);
    payload[1] = (u8)((u32)code & 0xFFU);

    if (reason != nullptr) {
        // Truncated rather than refused: a close must not fail, and a reason is a courtesy.
        u64 length = nya_min(strlen(reason), (u64)NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES);

        nya_memcpy(payload + size, reason, length);
        size += length;
    }

    // Set before the frame is queued: a send that races this one must already be refused.
    protocol->close_sent = true;
    protocol->close_code = code;
    _nya_websocket_reason_set(protocol, reason);

    u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
    u64 header_size                            = 0;
    u8  mask[4]                                = { 0 };

    b8 masking = protocol->role == NYA_WEBSOCKET_ROLE_CLIENT;

    if (masking && !nya_os_random_bytes(mask, sizeof(mask)))
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed, so no mask could be made");

    NYA_TRY(nya_websocket_frame_encode(NYA_WEBSOCKET_OPCODE_CLOSE, true, size, masking ? mask : nullptr, header, &header_size));

    nya_assert(header_size + size <= protocol->send_capacity, "a close frame always fits; see nya_websocket_protocol_open");

    if (header_size + size > protocol->send_capacity - protocol->send_size) {
        // Nothing this end has queued matters more than the goodbye, and the peer is about to stop
        // reading anyway, so the queue is given over to it rather than the close being dropped.
        protocol->send_size = 0;
    }

    nya_memcpy(protocol->send + protocol->send_size, header, header_size);
    protocol->send_size += header_size;

    for (u64 i = 0; i < size; i++) protocol->send[protocol->send_size + i] = masking ? (u8)(payload[i] ^ mask[i & 3U]) : payload[i];

    protocol->send_size += size;

    return NYA_OK;
}

NYA_Error nya_websocket_protocol_send(NYA_WebSocketProtocol* protocol, NYA_WebSocketOpcode opcode, const u8* data, u64 size) {
    nya_assert(protocol != nullptr);
    nya_assert(data != nullptr || size == 0);

    if (protocol->closed) return nya_error(NYA_ERROR_IO, "the websocket is closed");

    // A close is sent through nya_websocket_protocol_close, which is the only thing that may follow one.
    if (protocol->close_sent) return nya_error(NYA_ERROR_IO, "the websocket is closing");

    if (!_nya_websocket_opcode_is_known(opcode) || opcode == NYA_WEBSOCKET_OPCODE_CLOSE) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "0x%x is not a message this end sends", (u32)opcode);
    }

    if (_nya_websocket_opcode_is_control(opcode) && size > NYA_WEBSOCKET_MAX_CONTROL_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a control frame carries at most %u bytes", (u32)NYA_WEBSOCKET_MAX_CONTROL_BYTES);
    }

    // Checked going out as well as coming in: a peer is entitled to close a connection whose text is
    // not UTF-8, so sending some would be this end breaking the protocol rather than the other.
    if (opcode == NYA_WEBSOCKET_OPCODE_TEXT && !_nya_websocket_is_utf8(data, size)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a text message has to be utf-8");
    }

    u8 mask[4] = { 0 };
    b8 masking = protocol->role == NYA_WEBSOCKET_ROLE_CLIENT;

    if (masking && !nya_os_random_bytes(mask, sizeof(mask)))
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed, so no mask could be made");

    u8  header[NYA_WEBSOCKET_MAX_HEADER_BYTES] = { 0 };
    u64 header_size                            = 0;

    NYA_TRY(nya_websocket_frame_encode(opcode, true, size, masking ? mask : nullptr, header, &header_size));

    nya_assert(protocol->send_size <= protocol->send_capacity);

    // Room for both halves checked before either is written, so a message never goes out as a header
    // with no body, which a peer cannot recover from.
    if (header_size + size > protocol->send_capacity - protocol->send_size) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a %llu byte message does not fit the websocket's queue", (unsigned long long)size);
    }

    nya_memcpy(protocol->send + protocol->send_size, header, header_size);
    protocol->send_size += header_size;

    for (u64 i = 0; i < size; i++) protocol->send[protocol->send_size + i] = masking ? (u8)(data[i] ^ mask[i & 3U]) : data[i];

    protocol->send_size += size;

    return NYA_OK;
}

b8 nya_websocket_protocol_receive(
    NYA_WebSocketProtocol*  protocol,
    const u8*               data,
    u64                     size,
    OUT u64*                out_consumed,
    OUT NYA_WebSocketEvent* out_event
) {
    nya_assert(protocol != nullptr);
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_consumed != nullptr);
    nya_assert(out_event != nullptr);

    *out_consumed = 0;
    *out_event    = (NYA_WebSocketEvent){ .reason = "" };

    if (protocol->closed) return false;

    for (u32 step = 0; step < NYA_WEBSOCKET_MAX_FRAMES_PER_RECEIVE; step++) {
        if (!protocol->in_payload) {
            NYA_WebSocketFrame       frame  = { 0 };
            NYA_WebSocketFrameResult result = nya_websocket_frame_decode(data + *out_consumed, size - *out_consumed, &frame);

            if (result == NYA_WEBSOCKET_FRAME_INCOMPLETE) return false;

            if (result == NYA_WEBSOCKET_FRAME_INVALID) {
                _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a frame that is not a frame", out_event);
                return true;
            }

            /*
             * The role, which is the one rule a peer cannot be given the benefit of the doubt on: a
             * client masks everything and a server masks nothing, so a frame the wrong way round is
             * either a confused peer or something replaying the other direction's traffic back at us.
             */
            if (protocol->role == NYA_WEBSOCKET_ROLE_SERVER && !frame.masked) {
                _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a client frame that is not masked", out_event);
                return true;
            }

            if (protocol->role == NYA_WEBSOCKET_ROLE_CLIENT && frame.masked) {
                _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a server frame that is masked", out_event);
                return true;
            }

            if (frame.payload_size > protocol->max_frame_bytes) {
                _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_TOO_LARGE, "a frame larger than this connection accepts", out_event);
                return true;
            }

            if (!_nya_websocket_opcode_is_control(frame.opcode)) {
                b8 assembling = protocol->message_opcode != NYA_WEBSOCKET_OPCODE_CONTINUATION;

                if (frame.opcode == NYA_WEBSOCKET_OPCODE_CONTINUATION && !assembling) {
                    _nya_websocket_protocol_refuse(
                        protocol,
                        NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR,
                        "a continuation with nothing to continue",
                        out_event
                    );
                    return true;
                }

                if (frame.opcode != NYA_WEBSOCKET_OPCODE_CONTINUATION && assembling) {
                    _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a new message inside an unfinished one", out_event);
                    return true;
                }

                if (protocol->message_size + frame.payload_size > protocol->message_capacity) {
                    _nya_websocket_protocol_refuse(
                        protocol,
                        NYA_WEBSOCKET_CLOSE_TOO_LARGE,
                        "a message larger than this connection accepts",
                        out_event
                    );
                    return true;
                }

                // The bound on the work rather than the size: a peer may not spend forever not finishing
                // a message. See NYA_WEBSOCKET_MAX_FRAGMENTS.
                if (protocol->message_fragments >= NYA_WEBSOCKET_MAX_FRAGMENTS) {
                    _nya_websocket_protocol_refuse(
                        protocol,
                        NYA_WEBSOCKET_CLOSE_TOO_LARGE,
                        "a message in more fragments than this connection accepts",
                        out_event
                    );
                    return true;
                }

                protocol->message_fragments++;

                if (frame.opcode != NYA_WEBSOCKET_OPCODE_CONTINUATION) protocol->message_opcode = frame.opcode;
            }

            *out_consumed          += frame.header_size;
            protocol->frame         = frame;
            protocol->payload_read  = 0;
            protocol->control_size  = 0;
            protocol->in_payload    = true;
        }

        b8  control   = _nya_websocket_opcode_is_control(protocol->frame.opcode);
        u64 remaining = protocol->frame.payload_size - protocol->payload_read;
        u64 available = nya_min(remaining, size - *out_consumed);

        if (available > 0) {
            const u8* payload = data + *out_consumed;

            u8* into = control ? protocol->control + protocol->control_size : protocol->message + protocol->message_size;

            nya_assert(
                control ? protocol->control_size + available <= NYA_WEBSOCKET_MAX_CONTROL_BYTES
                        : protocol->message_size + available <= protocol->message_capacity,
                "a payload was let past its bound before it was copied"
            );

            // Unmasked on the way in rather than in place, because `data` is the caller's buffer and the
            // mask runs across whatever slice of the frame happened to arrive.
            if (protocol->frame.masked) {
                for (u64 i = 0; i < available; i++) into[i] = (u8)(payload[i] ^ protocol->frame.mask[(protocol->payload_read + i) & 3U]);
            } else {
                nya_memcpy(into, payload, available);
            }

            if (control) {
                protocol->control_size += available;
            } else {
                protocol->message_size += available;
            }

            protocol->payload_read += available;
            *out_consumed          += available;
        }

        if (protocol->payload_read < protocol->frame.payload_size) return false;

        protocol->in_payload = false;

        if (control) {
            protocol->control[protocol->control_size] = '\0';

            switch (protocol->frame.opcode) {
                case NYA_WEBSOCKET_OPCODE_PING: {
                    // Answered rather than reported: the RFC requires a pong with the same payload, and a
                    // caller has nothing to decide about one. A queue with no room for it is a peer that
                    // has stopped reading, which is the caller's pending-write bound to notice.
                    (void)nya_websocket_protocol_send(protocol, NYA_WEBSOCKET_OPCODE_PONG, protocol->control, protocol->control_size);
                    break;
                }

                case NYA_WEBSOCKET_OPCODE_PONG: {
                    *out_event = (NYA_WebSocketEvent){
                        .kind   = NYA_WEBSOCKET_EVENT_PONG,
                        .data   = protocol->control,
                        .size   = protocol->control_size,
                        .reason = "",
                    };

                    return true;
                }

                case NYA_WEBSOCKET_OPCODE_CLOSE: {
                    // A close carries either nothing or a two byte code and a reason. One byte is neither.
                    if (protocol->control_size == 1) {
                        _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR, "a close frame with half a code", out_event);
                        return true;
                    }

                    NYA_WebSocketClose code = NYA_WEBSOCKET_CLOSE_NONE;

                    if (protocol->control_size >= 2) {
                        u32 announced = ((u32)protocol->control[0] << 8) | (u32)protocol->control[1];

                        if (!_nya_websocket_close_code_is_sendable(announced)) {
                            _nya_websocket_protocol_refuse(
                                protocol,
                                NYA_WEBSOCKET_CLOSE_PROTOCOL_ERROR,
                                "a close code that may not be sent",
                                out_event
                            );
                            return true;
                        }

                        code = (NYA_WebSocketClose)announced;
                    }

                    if (!_nya_websocket_is_utf8(protocol->control + 2, protocol->control_size > 2 ? protocol->control_size - 2 : 0)) {
                        _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_INVALID_PAYLOAD, "a close reason that is not utf-8", out_event);
                        return true;
                    }

                    /*
                     * Echoed once, so the peer may close the socket, unless we already said goodbye. The
                     * same code back and nothing else: a reason is the sender's to give, and echoing the
                     * peer's own bytes back at it says nothing it does not know.
                     */
                    if (!protocol->close_sent) (void)nya_websocket_protocol_close(protocol, code, nullptr);

                    protocol->closed     = true;
                    protocol->close_code = code;
                    _nya_websocket_reason_set(protocol, protocol->control_size > 2 ? (NYA_ConstCString)(protocol->control + 2) : "the peer closed");

                    *out_event = (NYA_WebSocketEvent){
                        .kind   = NYA_WEBSOCKET_EVENT_CLOSED,
                        .code   = protocol->close_code,
                        .reason = protocol->close_reason,
                    };

                    return true;
                }

                default: nya_unreachable();
            }

            continue;
        }

        if (!protocol->frame.fin) continue;

        NYA_WebSocketOpcode completed = protocol->message_opcode;

        protocol->message_opcode                  = NYA_WEBSOCKET_OPCODE_CONTINUATION;
        protocol->message_fragments               = 0;
        protocol->message[protocol->message_size] = '\0';

        if (completed == NYA_WEBSOCKET_OPCODE_TEXT && !_nya_websocket_is_utf8(protocol->message, protocol->message_size)) {
            _nya_websocket_protocol_refuse(protocol, NYA_WEBSOCKET_CLOSE_INVALID_PAYLOAD, "a text message that is not utf-8", out_event);
            return true;
        }

        *out_event = (NYA_WebSocketEvent){
            .kind   = completed == NYA_WEBSOCKET_OPCODE_TEXT ? NYA_WEBSOCKET_EVENT_TEXT : NYA_WEBSOCKET_EVENT_BINARY,
            .data   = protocol->message,
            .size   = protocol->message_size,
            .reason = "",
        };

        protocol->message_size = 0;

        return true;
    }

    return false;
}

const u8* nya_websocket_protocol_pending(const NYA_WebSocketProtocol* protocol, OUT u64* out_size) {
    nya_assert(protocol != nullptr);
    nya_assert(out_size != nullptr);

    *out_size = protocol->send_size;

    return protocol->send_size > 0 ? protocol->send : nullptr;
}

void nya_websocket_protocol_flushed(NYA_WebSocketProtocol* protocol, u64 count) {
    nya_assert(protocol != nullptr);
    nya_assert(count <= protocol->send_size, "more was flushed than was ever queued");

    protocol->send_size -= count;

    if (protocol->send_size > 0) nya_memmove(protocol->send, protocol->send + count, protocol->send_size);
}

void nya_websocket_protocol_fail(NYA_WebSocketProtocol* protocol, NYA_WebSocketClose code, NYA_ConstCString reason) {
    nya_assert(protocol != nullptr);

    if (protocol->closed) return;

    protocol->closed     = true;
    protocol->close_code = code;
    protocol->send_size  = 0;

    _nya_websocket_reason_set(protocol, reason);
}

NYA_WebSocketClose nya_websocket_protocol_close_code(const NYA_WebSocketProtocol* protocol) {
    nya_assert(protocol != nullptr);

    return protocol->close_code;
}

b8 nya_websocket_protocol_is_closed(const NYA_WebSocketProtocol* protocol) {
    nya_assert(protocol != nullptr);

    return protocol->closed;
}

b8 nya_websocket_protocol_is_closing(const NYA_WebSocketProtocol* protocol) {
    nya_assert(protocol != nullptr);

    return protocol->close_sent && !protocol->closed;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_websocket_opcode_is_known(NYA_WebSocketOpcode opcode) {
    return opcode == NYA_WEBSOCKET_OPCODE_CONTINUATION || opcode == NYA_WEBSOCKET_OPCODE_TEXT || opcode == NYA_WEBSOCKET_OPCODE_BINARY ||
           opcode == NYA_WEBSOCKET_OPCODE_CLOSE || opcode == NYA_WEBSOCKET_OPCODE_PING || opcode == NYA_WEBSOCKET_OPCODE_PONG;
}

b8 _nya_websocket_opcode_is_control(NYA_WebSocketOpcode opcode) {
    return ((u32)opcode & 0x8U) != 0;
}

b8 _nya_websocket_close_code_is_sendable(u32 code) {
    // 1005 and 1006 are what a local end reports when nothing was sent at all, and 1015 is the TLS
    // failure; a peer that puts one on the wire is saying something the RFC says it cannot say.
    if (code >= 1000 && code <= 1003) return true;
    if (code >= 1007 && code <= 1014) return true;

    // the private range, which is whatever an application above this agreed between its two ends.
    return code >= 3000 && code <= 4999;
}

void _nya_websocket_reason_set(NYA_WebSocketProtocol* protocol, NYA_ConstCString reason) {
    protocol->close_reason[0] = '\0';

    if (reason == nullptr) return;

    u64 length = nya_min(strlen(reason), (u64)NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES);

    nya_memcpy(protocol->close_reason, reason, length);
    protocol->close_reason[length] = '\0';
}

void _nya_websocket_protocol_refuse(
    NYA_WebSocketProtocol*  protocol,
    NYA_WebSocketClose      code,
    NYA_ConstCString        reason,
    OUT NYA_WebSocketEvent* out_event
) {
    nya_assert(protocol != nullptr);
    nya_assert(out_event != nullptr);

    // Said out loud before the connection is given up on: RFC 6455 section 7.1.7 lets a failing endpoint
    // send one close frame, and a peer that is told 1002 can fix its framing where one that is dropped
    // learns nothing.
    (void)nya_websocket_protocol_close(protocol, code, reason);

    protocol->closed     = true;
    protocol->close_code = code;
    _nya_websocket_reason_set(protocol, reason);

    *out_event = (NYA_WebSocketEvent){
        .kind   = NYA_WEBSOCKET_EVENT_CLOSED,
        .code   = code,
        .reason = protocol->close_reason,
    };
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
