/**
 * The binary .nya reader, fed whatever. It is what an `application/nya-binary` request body arrives
 * through, so every byte here is a stranger's.
 *
 * One oracle beyond the sanitizers: the format has one encoding per document, so anything the reader
 * accepts must encode back to exactly the bytes it came from. A reader that accepts two spellings of
 * one document is the gap two readers disagree through.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define FUZZ_TARGET "serde_nya_binary"

/** Decodes against `type` (or untyped) and, when that succeeds, demands the same bytes back. */
static void fuzz_read(NYA_Arena* arena, const u8* data, u64 size, const NYA_TypeReflection* type) {
    NYA_Object* object = nullptr;
    if (!nya_serde_nya_binary_decode(arena, data, size, type, &object).ok) return;

    nya_assert(object != nullptr, "the reader reported success with nothing read");

    NYA_String* again = nullptr;
    NYA_EXPECT(nya_serde_nya_binary_encode(arena, object, type, &again), "while encoding a document the reader accepted");

    nya_assert(again->length == size && nya_memcmp(again->items, data, size) == 0, "an accepted document does not encode back to its own bytes");
}

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_serde_nya_binary");
    defer      nya_arena_destroy(arena);

    fuzz_read(arena, data, size, nullptr);

    // typed as well, against a real DTO: the hash check and the shape check are branches of their own.
    fuzz_read(arena, data, size, nya_reflect_of(NYA_HttpAccountingDto));

    // and through the dispatch and the sniffer, which is how a file or a control socket reaches it.
    NYA_SerdeFormat detected = nya_serde_detect_format(data, size);
    nya_assert(detected <= NYA_SERDE_FORMAT_COUNT, "the sniffer answered with a format that does not exist");
}

#include "tests/fuzz/fuzz.h"
