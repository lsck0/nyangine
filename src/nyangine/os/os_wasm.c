/**
 * @file os_wasm.c
 *
 * The os backend for a WebAssembly build under emscripten, the seed of the CSR path.
 *
 * A wasm module has no operating system under it: no mmap, no /proc, no threads, no sockets. What it
 * does have is a single growable linear memory reached through malloc, a monotonic-and-wall clock
 * through emscripten's libc, and an entropy source wired to the host's crypto.getRandomValues. This
 * file answers only the three os interfaces the headless serialize demo actually reaches — page, time
 * and random — and nothing else in os/. The full os.c is not compiled here (it pulls threads, sockets,
 * libbacktrace and lz4, none of which a wasm build has); wasm_demo.c includes this leaf directly.
 *
 * It is compiled only for the wasm target: the whole file is behind OS_WASM so a native build never
 * sees it, and os.c has no branch that reaches it.
 * */
#if OS_WASM

#include <emscripten/emscripten.h>
#include <time.h>

#include "nyangine/os/os_page.h"
#include "nyangine/os/os_random.h"
#include "nyangine/os/os_time.h"

// getentropy and sbrk live in <unistd.h>, which base_basic.h's strict POSIX request does not surface
// here; both are emscripten's own (getentropy over crypto.getRandomValues, sbrk over the linear-memory
// break), so they are declared by hand the way the linux page backend declares mincore.
extern int   getentropy(void* buffer, size_t length);
extern void* sbrk(intptr_t increment);

/*
 * ─────────────────────────────────────────────────────────
 * VIRTUAL MEMORY
 * ─────────────────────────────────────────────────────────
 */

/*
 * There is no reserve/commit split in a wasm linear memory: malloc is the one way to get pages, and it
 * both reserves and backs them at once. So reserve is a zeroed allocation (calloc, to hold the
 * "committed pages read as zero" contract the native backends give), commit is a no-op that only
 * confirms the range exists, and release frees it. The engine's arena takes its regions from nya_malloc
 * directly and never through this path, so only the introspection helpers (nya_arena_resident_bytes and
 * the stats report) reach it; a malloc-backed answer is exactly right for them.
 */

u64 nya_os_page_size(void) {
    // One wasm page. ALLOW_MEMORY_GROWTH grows linear memory in these, and it is the natural unit to
    // round a commit to even though malloc here needs no rounding.
    return 64u * 1024u;
}

void* nya_os_page_reserve(u64 size) {
    if (size == 0) return nullptr;

    // nya_calloc, not nya_malloc: a reserved-then-committed range reads as zero on the native backends,
    // and the arena's zero-on-first-use assumption must hold here too. This is the engine's malloc
    // wrapper (base_memory.h), the sanctioned platform-page source — the one place base takes memory
    // straight from the host.
    return nya_calloc(1, size);
}

b8 nya_os_page_commit(void* address, u64 size) {
    nya_unused(size);

    // Already backed by the calloc in reserve; there is nothing to make resident.
    return address != nullptr;
}

b8 nya_os_page_release(void* address, u64 size) {
    nya_unused(size);

    // nya_free tolerates null, and a null release is "nothing to do", which is success.
    nya_free(address);
    return true;
}

u64 nya_os_page_resident_bytes(const void* address, u64 size) {
    // Everything malloc hands back is backed immediately, so a reservation is fully resident. Reports
    // only; nothing in the frame path reads this.
    if (address == nullptr) return 0;
    return size;
}

u64 nya_os_process_resident_bytes(void) {
    // The whole linear memory is the process's footprint here. sbrk(0) is its current break, which is
    // the closest a wasm module has to a resident set.
    return (u64)(uintptr_t)sbrk(0);
}

/*
 * ─────────────────────────────────────────────────────────
 * TIME
 * ─────────────────────────────────────────────────────────
 */

u64 nya_os_time_wall_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (u64)now.tv_sec * 1'000'000'000ULL + (u64)now.tv_nsec;
}

u64 nya_os_time_monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (u64)now.tv_sec * 1'000'000'000ULL + (u64)now.tv_nsec;
}

void nya_os_time_sleep_ms(u32 milliseconds) {
    // A cooperative wasm module cannot block its single thread without ASYNCIFY, which this build does
    // not enable. The headless demo never sleeps, so this is a no-op rather than a spin that would
    // freeze the browser tab.
    nya_unused(milliseconds);
}

/*
 * ─────────────────────────────────────────────────────────
 * RANDOM
 * ─────────────────────────────────────────────────────────
 */

b8 nya_os_random_bytes(OUT u8* out, u64 size) {
    // Same refusal as the native backends: this layer is below the assertion machinery.
    if (out == nullptr || size == 0 || size > NYA_OS_RANDOM_MAX_BYTES) return false;

    // emscripten's getentropy is backed by the host CSPRNG (crypto.getRandomValues) and fills up to 256
    // bytes at a time, so it is looped the way the linux backend loops getrandom.
    u64 filled = 0;
    while (filled < size) {
        u64 chunk = size - filled;
        if (chunk > 256) chunk = 256;

        if (getentropy(out + filled, (size_t)chunk) != 0) return false;

        filled += chunk;
    }

    return true;
}

#endif // OS_WASM
