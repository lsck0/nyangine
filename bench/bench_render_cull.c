/**
 * The per-object frustum test render3d.c runs for every draw against every pass: the vectorised sphere test against the
 * scalar plane loop it replaced.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** The scalar plane loop this replaced, kept as the thing to beat. */
static b8 reference_visible(const NYA_Render3DFrustum* frustum, f32x3 center, f32 radius) {
    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = frustum->planes[i];

        f32x3 normal = { plane[0], plane[1], plane[2] };

        f32 distance = nya_vector_dot(normal, center) + plane[3];

        if (distance < -radius) return false;
    }

    return true;
}

static u32 lcg_state = 0x1234567u;

static f32 next_coord(f32 span) {
    lcg_state = (u32)((((u64)lcg_state * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull);
    return (((f32)(lcg_state >> 8) / 16777216.0F) - 0.5F) * span;
}

/** A camera and three shadow cascades: the four passes a lit scene culls against. */
static void build_passes(NYA_Render3DFrustum passes[4]) {
    f32_4x4 view = nya_matrix_look_at((f32x3){ 0, 20, 60 }, (f32x3){ 0, 0, 0 }, (f32x3){ 0, 1, 0 });

    _nya_render3d_frustum_build(&passes[0], nya_matrix_perspective((f32)M_PI / 3.0F, 16.0F / 9.0F, 0.1F, 300.0F) * view);

    const f32 extents[3] = { 40.0F, 100.0F, 220.0F };
    for (u32 c = 0; c < 3; c++) {
        f32_4x4 ortho = nya_matrix_orthographic_3d(extents[c], 1.0F, 1.0F, extents[c] * 4.0F);
        _nya_render3d_frustum_build(&passes[c + 1], ortho * nya_matrix_look_at((f32x3){ 0, 80, 0 }, (f32x3){ 0, 0, 0 }, (f32x3){ 0, 0, -1 }));
    }
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_cull");
    defer      nya_arena_destroy(arena);

    NYA_Render3DFrustum passes[4];
    build_passes(passes);

    const u32 counts[] = { 1024, 8192, 65536 };

    for (u32 c = 0; c < nya_carray_length(counts); c++) {
        const u32 count = counts[c];

        f32x3* centers = nya_arena_alloc(arena, (u64)count * sizeof(f32x3));
        f32*   radii   = nya_arena_alloc(arena, (u64)count * sizeof(f32));

        for (u32 i = 0; i < count; i++) {
            centers[i] = (f32x3){ next_coord(240.0F), next_coord(120.0F), next_coord(240.0F) };
            radii[i]   = 0.5F + (next_coord(2.0F) + 1.0F);
        }

        char group[80];
        (void)snprintf(group, sizeof(group), "render3d cull, %u objects x 4 passes", count);
        nya_bench_begin(group);

        nya_bench("scalar (was)", count, {
            u32 seen = 0;
            for (u32 i = 0; i < count; i++) {
                for (u32 p = 0; p < 4; p++) seen += reference_visible(&passes[p], centers[i], radii[i]) ? 1U : 0U;
            }
            nya_bench_keep(seen);
        });

        nya_bench("simd (now)", count, {
            u32 seen = 0;
            for (u32 i = 0; i < count; i++) {
                for (u32 p = 0; p < 4; p++) seen += _nya_render3d_visible(&passes[p], centers[i], radii[i]) ? 1U : 0U;
            }
            nya_bench_keep(seen);
        });

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
