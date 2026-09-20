/**
 * The lenient JSON reader: comments, trailing commas and whatever a person typed by hand. It reads a
 * strict superset, so it is a target of its own rather than the strict one with a flag.
 *
 * The format sniffer rides along, because it is what decides which reader a file reaches and is as
 * much of a boundary as the readers are.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define FUZZ_TARGET "serde_jsonc"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_serde_jsonc");
    defer      nya_arena_destroy(arena);

    NYA_SerdeFormat detected = nya_serde_detect_format(data, size);
    nya_assert(detected < NYA_SERDE_FORMAT_COUNT, "the sniffer answered with a format that does not exist");

    NYA_Object* object = nullptr;
    if (!nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &object).ok) return;

    nya_assert(object != nullptr, "the reader reported success with nothing read");

    (void)nya_serialize(arena, object, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE);
}

#include "fuzz/fuzz.h"
