/**
 * The decompressor, fed whatever. A compressed block arrives from a save file and from the wire, and
 * the size it claims to expand to is the first number a hostile input gets to choose.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define FUZZ_TARGET "compress"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_compress");
    defer      nya_arena_destroy(arena);

    /*
     * The size the caller believes it is getting, which in real code comes from a header the same
     * hostile input wrote. Every plausible claim is tried, including ones far off the truth: a
     * decompressor is only safe if it writes no more than the caller's buffer whatever the block says.
     */
    static const u64 CLAIMS[] = { 0, 1, 16, 1024, 1024 * 256 };

    for (u32 i = 0; i < nya_carray_length(CLAIMS); i++) {
        u8* out = nya_arena_alloc(arena, CLAIMS[i] + 1);

        (void)nya_decompress(data, size, out, CLAIMS[i]);
    }

    // and the other direction, since compressing untrusted bytes is what a save does before writing.
    u64 bound      = nya_compress_bound(size);
    u8* compressed = nya_arena_alloc(arena, bound + 1);

    u64 written = nya_compress(data, size, compressed, bound);

    if (written > 0) {
        u8* restored = nya_arena_alloc(arena, size + 1);

        nya_assert(nya_decompress(compressed, written, restored, size), "what this compressed would not decompress");
        nya_assert(size == 0 || nya_memcmp(restored, data, size) == 0, "a round trip through the compressor changed the bytes");
    }
}

#include "tests/fuzz/fuzz.h"
