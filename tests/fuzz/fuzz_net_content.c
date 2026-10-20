/**
 * The content manifest a HELLO carries after its document, fed whatever.
 *
 * Whatever decodes must encode back to the same bytes, since decode refuses every order and repeat that would
 * give one set two encodings; and whatever decodes is then compared and described the way a server refusing it would.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define FUZZ_TARGET "net_content"

static void fuzz_once(const u8* data, u64 size) {
    NYA_NetContentManifest theirs = { 0 };
    if (!nya_net_content_decode(data, size, &theirs).ok) return;

    nya_assert(theirs.entry_count <= NYA_NET_CONTENT_ENTRY_MAX);

    NYA_Arena* arena = nya_arena_create(.name = "fuzz_net_content");
    defer      nya_arena_destroy(arena);

    NYA_String* again = nya_string_create(arena);
    nya_net_content_encode(again, &theirs);
    nya_assert(again->length == size && nya_memcmp(again->items, data, size) == 0, "a decoded manifest encodes to other bytes");

    NYA_NetContentManifest ours = { 0 };
    NYA_EXPECT(nya_net_content_collect(&ours));

    NYA_NetContentPolicy policies[2] = { { .allow_client_only = false }, { .allow_client_only = true } };

    for (u32 i = 0; i < 2; i++) {
        NYA_NetContentMismatch mismatch = nya_net_content_compare(&ours, &theirs, policies[i]);

        char line[NYA_NET_CONTENT_DESCRIBE_MAX];
        u64  written = nya_net_content_mismatch_describe(&mismatch, line, sizeof(line));
        nya_assert(written < sizeof(line));
    }
}

#include "tests/fuzz/fuzz.h"
