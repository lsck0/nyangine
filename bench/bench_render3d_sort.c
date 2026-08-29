/**
 * The transparent depth sort: the radix pass against the qsort it replaced.
 *
 * This exists because the claim it checks came from a *sanitizer* profile, where the sort measured 7.9%
 * of frame time. That number was inflated, so the speedup had to be re-established somewhere the
 * measurement means something. Built with FLAGS_BENCH — optimised, no sanitizers.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** The comparator this replaced, kept as the thing to beat. */
static int reference_compare(const void* left, const void* right) {
    const NYA_Render3DSortKey* a = left;
    const NYA_Render3DSortKey* b = right;

    if (a->depth > b->depth) return -1;
    if (a->depth < b->depth) return 1;

    return 0;
}

static u32 lcg_state = 0x1234567u;

static f32 next_depth(void) {
    lcg_state = (u32)((((u64)lcg_state * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull);
    return (f32)(lcg_state >> 8) / 16777216.0F * 10000.0F;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_sort");
    defer      nya_arena_destroy(arena);

    const u32 counts[] = { 256, 1024, 4096, 16384 };

    for (u32 c = 0; c < nya_carray_length(counts); c++) {
        const u32 count = counts[c];
        const u64 bytes = (u64)count * sizeof(NYA_Render3DSortKey);

        NYA_Render3DSortKey* base    = nya_arena_alloc(arena, bytes);
        NYA_Render3DSortKey* work    = nya_arena_alloc(arena, bytes);
        NYA_Render3DSortKey* scratch = nya_arena_alloc(arena, bytes);

        for (u32 i = 0; i < count; i++) base[i] = (NYA_Render3DSortKey){ .depth = next_depth(), .first = i * 3 };

        char group[64];
        (void)snprintf(group, sizeof(group), "render3d transparent sort, %u triangles", count);
        nya_bench_begin(group);

        // The memcpy is inside both cases on purpose: each sort must see the same unsorted input, and
        // charging the copy to both keeps the comparison honest rather than flattering the second.
        nya_bench("qsort (was)", count, {
            nya_memcpy(work, base, bytes);
            qsort(work, count, sizeof(NYA_Render3DSortKey), reference_compare);
            nya_bench_keep(work[0].first);
        });

        nya_bench("radix (now)", count, {
            nya_memcpy(work, base, bytes);
            nya_render3d_sort_keys(work, scratch, count);
            nya_bench_keep(work[0].first);
        });

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
