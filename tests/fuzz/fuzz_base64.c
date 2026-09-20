/**
 * The base64 decoder, fed whatever. Reached by every embedded blob in a tilemap and every key in a
 * settings file, which is to say by anything a player can edit.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define FUZZ_TARGET "base64"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_base64");
    defer      nya_arena_destroy(arena);

    NYA_String* decoded = nya_string_create(arena);
    nya_base64_decode(decoded, data, size);

    // what came out has to encode back to something the decoder accepts, and to the same length. A
    // decoder that produced bytes no encoder could have written read past its input.
    NYA_String* encoded = nya_string_create(arena);
    nya_base64_encode(encoded, (const u8*)decoded->items, decoded->length);

    NYA_String* again = nya_string_create(arena);
    nya_base64_decode(again, (const u8*)encoded->items, encoded->length);

    nya_assert(again->length == decoded->length, "re-decoding what was re-encoded changed the length");
}

#include "fuzz/fuzz.h"
