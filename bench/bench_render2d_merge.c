/**
 * The 2D flush's CPU side for a menu: one draw per sorted range as it was, against merging same-state ranges now.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

// what a rounded widget body and an eight glyph label cost in indices.
#define BODY_INDICES  60U
#define LABEL_INDICES 48U

/** The comparator the flush sorted with before it merged. */
static int reference_compare(const void* left, const void* right) {
    const NYA_Render2DDrawRange* a = left;
    const NYA_Render2DDrawRange* b = right;

    if (a->layer != b->layer) return a->layer < b->layer ? -1 : 1;
    return a->sequence < b->sequence ? -1 : (a->sequence > b->sequence ? 1 : 0);
}

/** Appends a range after the ones before it, as the flush would find it. */
static void range_add(NYA_Render2DDrawRange* ranges, u32* count, SDL_GPUTexture* texture, NYA_Rectf bounds, u32 index_count) {
    u32 first = *count > 0 ? ranges[*count - 1].first_index + ranges[*count - 1].index_count : 0;

    ranges[*count] = (NYA_Render2DDrawRange){
        .sequence      = *count,
        .first_index   = first,
        .index_count   = index_count,
        .pipeline      = (NYA_CString)(texture != nullptr ? NYA_RENDER2D_PIPELINE_TEXT : NYA_RENDER2D_PIPELINE_SHAPES),
        .texture       = texture,
        .target_width  = 1280,
        .target_height = 800,
        .bounds        = bounds,
    };

    (*count)++;
}

/** A panel with a title in its own font, then a column of widgets, each a body with its label inside. */
static u32 menu_build(NYA_Render2DDrawRange* ranges, u32 widgets) {
    SDL_GPUTexture* title_atlas = (SDL_GPUTexture*)1;
    SDL_GPUTexture* label_atlas = (SDL_GPUTexture*)2;
    NYA_Rectf       panel       = { 20.0F, 20.0F, 400.0F, 40.0F + (44.0F * (f32)widgets) };
    u32             count       = 0;

    range_add(ranges, &count, nullptr, panel, BODY_INDICES);
    range_add(ranges, &count, title_atlas, (NYA_Rectf){ 30.0F, 24.0F, 200.0F, 24.0F }, LABEL_INDICES);

    for (u32 i = 0; i < widgets; i++) {
        NYA_Rectf body = { 30.0F, 60.0F + (44.0F * (f32)i), 380.0F, 36.0F };

        range_add(ranges, &count, nullptr, body, BODY_INDICES);
        range_add(ranges, &count, label_atlas, nya_rect_expand(body, -8.0F), LABEL_INDICES);
    }

    return count;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_render2d_merge");
    defer      nya_arena_destroy(arena);

    const u32 range_bytes = NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDrawRange);
    const u32 index_bytes = NYA_RENDER2D_MAX_RANGES * BODY_INDICES * sizeof(u32);

    NYA_Render2DDrawRange* base    = nya_arena_alloc(arena, range_bytes);
    NYA_Render2DDrawRange* work    = nya_arena_alloc(arena, range_bytes);
    NYA_Render2DDraw*      draws   = nya_arena_alloc(arena, NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDraw));
    u32*                   indices = nya_arena_alloc(arena, index_bytes);
    u32*                   mapped  = nya_arena_alloc(arena, index_bytes);

    for (u32 i = 0; i < index_bytes / sizeof(u32); i++) indices[i] = i;

    const u32 widget_counts[] = { 8, 32, 128 };

    // the second pass raises the middle widget like an open dropdown, so the ranges need the sort.
    for (u32 v = 0; v < 2 * nya_carray_length(widget_counts); v++) {
        u32 widgets     = widget_counts[v % nya_carray_length(widget_counts)];
        u32 raised      = v / nya_carray_length(widget_counts);
        u32 count       = menu_build(base, widgets);
        u32 index_total = base[count - 1].first_index + base[count - 1].index_count;
        u64 bytes       = (u64)count * sizeof(NYA_Render2DDrawRange);

        // past the panel and its title, the middle widget's body and then its label.
        u32 middle             = 2 + (2 * (widgets / 2));
        base[middle].layer     = (s32)raised;
        base[middle + 1].layer = (s32)raised;

        nya_memcpy(work, base, bytes);
        u32 draw_count = nya_render2d_ranges_merge(work, count, draws);

        char group[128];
        (void)snprintf(
            group,
            sizeof(group),
            "render2d flush, menu of %u widgets%s: %u draw calls was, %u now",
            widgets,
            raised ? ", one raised" : "",
            count,
            draw_count
        );
        nya_bench_begin(group);

        // the copy is in both, so each sort sees the declaration order the flush would.
        nya_bench("sort, one draw per range (was)", count, {
            nya_memcpy(work, base, bytes);
            qsort(work, count, sizeof(NYA_Render2DDrawRange), reference_compare);
            nya_memcpy(mapped, indices, index_total * sizeof(u32));
            nya_bench_keep(mapped[0]);
        });

        nya_bench("sort and merge (now)", count, {
            nya_memcpy(work, base, bytes);
            u32 merged = nya_render2d_ranges_merge(work, count, draws);
            nya_render2d_draws_indices_write(work, draws, merged, indices, mapped);
            nya_bench_keep(mapped[0]);
        });

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
