#include "nyangine-std/platform/web/web_random.h"

#include "nyangine-std/os/os_random.h" // NYA_OS_RANDOM_MAX_BYTES, and the native fallback below

#if OS_WASM

#include <emscripten/emscripten.h>

// Fills [out, out + size) from crypto.getRandomValues, straight into linear memory. The call is
// synchronous and allocates nothing, so the HEAPU8 view cannot be detached by a memory growth under it,
// and subarray writes directly into the wasm buffer the caller passed. Returns 1 on success, 0 if the
// browser has no crypto object (a non-secure context that predates the API). getRandomValues itself
// refuses a view longer than 65536 bytes, which the C loop below never hands it.
//
// clang-format is off across the EM_JS body: the braces hold JavaScript, and the formatter reads it as C
// and breaks it (splitting `===`, reflowing the object literals). The C around it is formatted as usual.
// clang-format off
EM_JS(int, _nya_web_random_fill, (unsigned char* out, int size), {
    if (typeof crypto === "undefined" || !crypto.getRandomValues) return 0;
    crypto.getRandomValues(HEAPU8.subarray(out, out + size));
    return 1;
})
// clang-format on

b8 nya_web_random_bytes(OUT u8* out, u64 size) {
    if (out == nullptr || size == 0 || size > NYA_OS_RANDOM_MAX_BYTES) return false;

    // 65536 is getRandomValues' per-call ceiling; loop the way the linux os backend loops getrandom.
    u64 filled = 0;
    while (filled < size) {
        u64 chunk = size - filled;
        if (chunk > 65536) chunk = 65536;

        if (_nya_web_random_fill(out + filled, (int)chunk) == 0) return false;

        filled += chunk;
    }

    return true;
}

#else // native fallback, so the same call compiles and runs off wasm

b8 nya_web_random_bytes(OUT u8* out, u64 size) {
    return nya_os_random_bytes(out, size);
}

#endif // OS_WASM
