#include "nyangine/base/base_compress.h"

#include "nyangine/base/base_assert.h"

#ifdef NYA_NO_SDL
// The build tool bootstraps from one clang command before any vendor archive exists, so lz4 is compiled in rather than linked.
#include "vendor/lz4/lib/lz4.c"
#else
#include "lz4.h"
#endif

// PUBLIC API IMPLEMENTATION

u64 nya_compress_bound(u64 size_bytes) {
    // LZ4's whole API is int and it refuses anything past LZ4_MAX_INPUT_SIZE rather than wrapping; checked here so every entry point below can cast without thinking about it.
    if (size_bytes == 0 || size_bytes > (u64)LZ4_MAX_INPUT_SIZE) return 0;

    int bound = LZ4_compressBound((int)size_bytes);
    if (bound <= 0) return 0;

    return (u64)bound;
}

u64 nya_compress(const void* source, u64 source_size_bytes, void* out, u64 out_capacity_bytes) {
    nya_assert(source != nullptr || source_size_bytes == 0);
    nya_assert(out != nullptr || out_capacity_bytes == 0);

    if (source_size_bytes == 0 || source_size_bytes > (u64)LZ4_MAX_INPUT_SIZE) return 0;
    if (out_capacity_bytes == 0 || out_capacity_bytes > (u64)INT32_MAX) return 0;

    int written = LZ4_compress_default((const char*)source, (char*)out, (int)source_size_bytes, (int)out_capacity_bytes);

    // Zero is LZ4's "did not fit", which is the same answer as "did not help" to every caller here.
    if (written <= 0) return 0;

    nya_assert((u64)written <= out_capacity_bytes, "LZ4_compress_default wrote %d bytes into a buffer of " FMTu64, written,
               out_capacity_bytes);

    return (u64)written;
}

b8 nya_decompress(const void* source, u64 source_size_bytes, void* out, u64 out_size_bytes) {
    nya_assert(source != nullptr || source_size_bytes == 0);
    nya_assert(out != nullptr || out_size_bytes == 0);

    if (source_size_bytes == 0 || out_size_bytes == 0) return false;
    if (source_size_bytes > (u64)INT32_MAX || out_size_bytes > (u64)INT32_MAX) return false;

    /* The _safe variant, the only one that may be pointed at bytes the program did not write: LZ4_decompress_fast trusts its input and reads past a truncated block, a remote read primitive over a socket. */
    int produced = LZ4_decompress_safe((const char*)source, (char*)out, (int)source_size_bytes, (int)out_size_bytes);

    // Exactly, not at most: a block that expands to fewer bytes than the caller recorded is as wrong as one that overruns, and leaves the tail of `out` holding what was there before.
    return produced >= 0 && (u64)produced == out_size_bytes;
}
