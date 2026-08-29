/**
 * The transparent depth sort, which stopped being a qsort and became a radix pass.
 *
 * A profile put qsort at 7.9% of frame time — 3.4% of it in the comparator alone, the signature of an
 * indirect call that cannot be inlined. Replacing a sort is exactly the kind of change that silently
 * reorders one triangle in one scene, so what this asserts is not "it is sorted" but **"it produces the
 * same order the comparison sort did"**, checked against a reference qsort over the same input.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** The comparator that used to drive this, kept here as the reference to check against. */
static int reference_compare(const void* left, const void* right) {
    const NYA_Render3DSortKey* a = left;
    const NYA_Render3DSortKey* b = right;

    if (a->depth > b->depth) return -1;
    if (a->depth < b->depth) return 1;

    return 0;
}

/** Runs both sorts over the same keys and reports whether the resulting depth sequences agree. */
static b8 agrees_with_qsort(NYA_Render3DSortKey* keys, u32 count, NYA_Arena* arena) {
    NYA_Render3DSortKey* radix     = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
    NYA_Render3DSortKey* scratch   = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
    NYA_Render3DSortKey* reference = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));

    nya_memcpy(radix, keys, count * sizeof(NYA_Render3DSortKey));
    nya_memcpy(reference, keys, count * sizeof(NYA_Render3DSortKey));

    nya_render3d_sort_keys(radix, scratch, count);
    qsort(reference, count, sizeof(NYA_Render3DSortKey), reference_compare);

    // The radix pass sorts ascending and the draw walks it backwards, so index i of the reference
    // corresponds to index count-1-i of the radix result.
    for (u32 i = 0; i < count; i++) {
        if (radix[count - 1 - i].depth != reference[i].depth) return false;
    }

    return true;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_sort");
    defer      nya_arena_destroy(arena);

    /* A plain LCG rather than NYA_RNG: this wants a reproducible spread of depths, not a good
     * distribution, and keeping it local means the test cannot fail for a reason in the RNG. */
    u32 state = 0x1234567u;
    /* Widened and masked rather than relying on wraparound: this build enables
     * -fsanitize=unsigned-integer-overflow, which an LCG would otherwise trip every call. */
    #define NEXT_DEPTH(scale)                                                                                                                        \
        ((f32)((state = (u32)((((u64)state * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull)) >> 8) / 16777216.0F * (scale))

    // ── Random depths, over a range of counts including the awkward small ones.
    {
        const u32 counts[] = { 2, 3, 4, 7, 15, 16, 17, 255, 256, 257, 1024, 4096 };

        for (u32 c = 0; c < nya_carray_length(counts); c++) {
            u32 count = counts[c];

            NYA_Render3DSortKey* keys = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
            for (u32 i = 0; i < count; i++) {
                keys[i] = (NYA_Render3DSortKey){ .depth = NEXT_DEPTH(10000.0F), .first = i * 3 };
            }

            nya_check(agrees_with_qsort(keys, count, arena), "radix and qsort disagreed at count %u", count);
        }
    }

    // ── Already sorted, reverse sorted, and all-equal: the distributions a radix pass is worst at.
    {
        const u32 count = 512;

        NYA_Render3DSortKey* ascending = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
        NYA_Render3DSortKey* descending = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
        NYA_Render3DSortKey* equal     = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));

        for (u32 i = 0; i < count; i++) {
            ascending[i]  = (NYA_Render3DSortKey){ .depth = (f32)i, .first = i * 3 };
            descending[i] = (NYA_Render3DSortKey){ .depth = (f32)(count - i), .first = i * 3 };
            equal[i]      = (NYA_Render3DSortKey){ .depth = 42.0F, .first = i * 3 };
        }

        nya_check(agrees_with_qsort(ascending, count, arena), "already-sorted input should agree");
        nya_check(agrees_with_qsort(descending, count, arena), "reverse-sorted input should agree");
        nya_check(agrees_with_qsort(equal, count, arena), "all-equal input should agree");
    }

    // ── The values a squared distance actually takes: zero, tiny, and very large.
    {
        NYA_Render3DSortKey keys[] = {
            { .depth = 0.0F, .first = 0 },
            { .depth = 1e-30F, .first = 3 },
            { .depth = 1.0F, .first = 6 },
            { .depth = 1e12F, .first = 9 },
            { .depth = 3.4e38F, .first = 12 },
            { .depth = 0.5F, .first = 15 },
        };

        nya_check(agrees_with_qsort(keys, nya_carray_length(keys), arena), "extreme magnitudes should agree");
    }

    // ── The result is genuinely ordered, and every input key survives exactly once.
    {
        const u32            count = 2000;
        NYA_Render3DSortKey* keys  = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
        NYA_Render3DSortKey* work  = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));

        for (u32 i = 0; i < count; i++) keys[i] = (NYA_Render3DSortKey){ .depth = NEXT_DEPTH(500.0F), .first = i * 3 };

        NYA_Render3DSortKey* sorted = nya_arena_alloc(arena, count * sizeof(NYA_Render3DSortKey));
        nya_memcpy(sorted, keys, count * sizeof(NYA_Render3DSortKey));

        nya_render3d_sort_keys(sorted, work, count);

        u32 out_of_order = 0;
        for (u32 i = 1; i < count; i++) {
            if (sorted[i].depth < sorted[i - 1].depth) out_of_order++;
        }
        nya_check(out_of_order == 0, "%u pairs were out of ascending order", out_of_order);

        // Every `first` must appear exactly once: a radix pass that loses or duplicates a key drops or
        // doubles a triangle, which is far harder to spot on screen than a mis-ordering.
        u32 seen = 0;
        for (u32 i = 0; i < count; i++) {
            for (u32 j = 0; j < count; j++) {
                if (sorted[j].first == keys[i].first) {
                    seen++;
                    break;
                }
            }
        }
        nya_check(seen == count, "only %u of %u keys survived the sort", seen, count);
    }

    // ── A single key, and none at all, are handled rather than reading past the array.
    {
        NYA_Render3DSortKey one[1]  = { { .depth = 7.0F, .first = 0 } };
        NYA_Render3DSortKey work[1] = { { 0 } };

        nya_render3d_sort_keys(one, work, 1);
        nya_check(one[0].depth == 7.0F && one[0].first == 0, "a single key should come back unchanged");

        nya_render3d_sort_keys(one, work, 0);
        nya_check(one[0].depth == 7.0F, "a zero count should touch nothing");
    }

    #undef NEXT_DEPTH

    return nya_check_failures() == 0 ? 0 : 1;
}
