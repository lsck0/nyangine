/**
 * The anti-tamper checks: what the startup thread, a sweep step, the watchdog and an asset check cost.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/* About the release executable: 6.8 MB of code in a 12 MB file. */
#define CODE_BYTES (7ULL * 1024ULL * 1024ULL)
#define FILE_BYTES (12ULL * 1024ULL * 1024ULL)

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_integrity");
    u8*        file  = nya_arena_alloc(arena, FILE_BYTES);
    nya_assert(file != nullptr);
    for (u64 i = 0; i < FILE_BYTES; i++) file[i] = (u8)((i * 2654435761ULL) >> 13);
    const u8* code = file + (FILE_BYTES - CODE_BYTES);

    static NYA_IntegrityState state;

    nya_bench_begin("integrity");

    // the startup thread's work, off the main thread: the file hash, then the code baseline.
    nya_bench("file hash, 12 MB", 1, { nya_bench_keep(nya_integrity_hash(file, FILE_BYTES)); });

    nya_bench("code baseline, 7 MB", 1, {
        atomic_store(&state.baseline_ready, false);
        nya_integrity_capture(&state, code, CODE_BYTES);
        nya_bench_keep(state.baseline_digest);
    });

    // what the main thread pays per step, four times a second.
    nya_bench("sweep step, one chunk", 1, { nya_bench_keep(nya_integrity_sweep_step(&state)); });

    // twice a frame and once a tick.
    nya_bench("watchdog verdict", 1, { nya_bench_keep(nya_integrity_watchdog_verdict(&state, 0, 1)); });

    // once per embedded asset on its first load; the largest entries are about this size.
    nya_bench("asset entry check, 256 KB", 1, { nya_bench_keep(nya_integrity_hash(file, 256 * 1024)); });

    s32 result = nya_bench_end();
    nya_arena_destroy(arena);
    return result;
}
