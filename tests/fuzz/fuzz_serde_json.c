/**
 * The strict JSON reader, fed whatever. Every asset manifest, tilemap and HTTP body arrives here.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define FUZZ_TARGET "serde_json"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_serde_json");
    defer      nya_arena_destroy(arena);

    NYA_Object* object = nullptr;
    if (!nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &object).ok) return;

    nya_assert(object != nullptr, "the reader reported success with nothing read");

    (void)nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);
}

#include "tests/fuzz/fuzz.h"
