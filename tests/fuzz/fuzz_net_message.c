/**
 * The message framing and the document inside it, fed whatever.
 *
 * A message is the outer envelope every reliable payload arrives in: a kind byte, a body offset, and
 * then a serialized document. The chat layer is fed whatever parses, because a document that parsed is
 * not a document anybody has checked.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define FUZZ_TARGET "net_message"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_net_message");
    defer      nya_arena_destroy(arena);

    u64 body = 0;

    NYA_NetMessageKind kind = nya_net_message_kind(data, size, &body);

    // what the framing promises: a kind inside the enum and a body inside the datagram. Everything downstream slices at `body` without checking it again.
    nya_assert(kind <= NYA_NET_MSG_COUNT, "the framing reported a message kind that does not exist");
    nya_assert(body <= size, "the framing reported a body past the end of the message");

    NYA_Object* object = nullptr;
    if (!nya_net_message_read_object(arena, data, size, &object).ok || object == nullptr) return;

    // what a parsed event meets next: the chat layer, which trusts nothing in it.
    (void)nya_net_chat_client_consume(object);
}

#include "tests/fuzz/fuzz.h"
