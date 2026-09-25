/**
 * The .nya reader, fed whatever. The format carries a checksum and a type specifier, so this is the
 * boundary a savegame, a settings file and a network document all arrive through.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define FUZZ_TARGET "serde_nya"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_serde_nya");
    defer      nya_arena_destroy(arena);

    // every flag combination the reader takes, since the checksum and the obfuscation are each a branch a hostile file steers.
    for (u32 flags = 0; flags <= (NYA_SERDE_OBFUSCATE | NYA_SERDE_NO_CHECKSUM); flags++) {
        NYA_Object* object = nullptr;

        if (!nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_NYA, (NYA_SerdeFlags)flags, &object).ok) continue;

        nya_assert(object != nullptr, "the reader reported success with nothing read");

        // back out again: a document that parsed has to be one the writer can write, or the round trip a save file depends on is not closed.
        (void)nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, (NYA_SerdeFlags)flags);
    }
}

#include "tests/fuzz/fuzz.h"
