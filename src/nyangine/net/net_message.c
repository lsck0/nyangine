#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_net_message_begin(NYA_String* out, NYA_NetMessageKind kind) {
    nya_assert(out != nullptr);
    nya_assert(kind > 0 && kind < NYA_NET_MSG_COUNT, "message kind %d is not one of ours", (int)kind);

    nya_string_push_back(out, (u8)kind);
}

NYA_NetMessageKind nya_net_message_kind(const u8* data, u64 size, OUT u64* out_body_offset) {
    if (out_body_offset != nullptr) *out_body_offset = 0;

    if (data == nullptr || size == 0) return NYA_NET_MSG_COUNT;

    u8 kind = data[0];

    /*
     * An unknown kind is reported as COUNT rather than refused.
     *
     * A newer peer may send something this build has never heard of, and the right answer is to
     * ignore that one message. Dropping the connection instead would make every protocol addition a
     * hard compatibility break even where the two versions could otherwise coexist — and the
     * handshake already refuses a genuinely incompatible peer before it gets this far.
     */
    if (kind == 0 || kind >= NYA_NET_MSG_COUNT) return NYA_NET_MSG_COUNT;

    if (out_body_offset != nullptr) *out_body_offset = 1;

    return (NYA_NetMessageKind)kind;
}

NYA_Error nya_net_message_write_object(NYA_Arena* arena, NYA_String* out, const NYA_Object* object) {
    nya_assert(arena != nullptr);
    nya_assert(out != nullptr);
    nya_assert(object != nullptr);

    NYA_String* document = nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE);
    if (document == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not serialize a network message");

    // Length prefixed, so a reader can find the end without parsing — and can skip a document it does
    // not understand rather than guessing.
    u32 length = (u32)document->length;
    for (u32 i = 0; i < 4; i++) nya_string_push_back(out, (u8)((length >> (i * 8)) & 0xFF));

    nya_string_extend(out, document);

    return NYA_OK;
}

NYA_Error nya_net_message_read_object(NYA_Arena* arena, const u8* data, u64 size, OUT NYA_Object** out_object) {
    nya_assert(arena != nullptr);
    nya_assert(out_object != nullptr);

    *out_object = nullptr;

    if (data == nullptr || size < 4) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message too short to carry a length");

    u32 length = 0;
    for (u32 i = 0; i < 4; i++) length |= (u32)data[i] << (i * 8);

    /*
     * The prefix is checked against what actually arrived, not trusted.
     *
     * It is a number a peer chose. Handing it to the deserializer unchecked would have it read past
     * the end of the datagram — and this is reached from a HELLO, which is the very first thing an
     * unauthenticated peer sends.
     *
     * Written as a subtraction because `4 + length` can overflow and then compare as fitting.
     */
    if (length > size - 4) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message claiming %u bytes of document in %llu bytes", length, (unsigned long long)size);
    }

    return nya_deserialize(arena, data + 4, length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, out_object);
}
