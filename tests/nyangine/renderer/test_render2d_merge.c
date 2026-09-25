/**
 * Merging sorted 2D ranges into draw calls: same state joins across ranges that do not overlap it, overlap, a
 * different space or the lookback keeps them apart, and the index stream comes out in draw then paint order.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

// PIPELINE_ rather than bare, since windows.h already defines TEXT.
#define PIPELINE_SHAPES "shapes"
#define PIPELINE_TEXT   "text"

static NYA_Render2DDrawRange ranges[64];
static NYA_Render2DDraw      draws[64];
static u32                   count = 0;

/** Appends a range of `index_count` indices after the ones before it, covering `bounds`, in the window's space. */
static NYA_Render2DDrawRange* range(NYA_ConstCString pipeline, NYA_Rectf bounds, u32 index_count) {
    u32 first = count > 0 ? ranges[count - 1].first_index + ranges[count - 1].index_count : 0;

    ranges[count] = (NYA_Render2DDrawRange){
        .sequence      = count,
        .first_index   = first,
        .index_count   = index_count,
        .pipeline      = (NYA_CString)pipeline,
        .target_width  = 800,
        .target_height = 600,
        .bounds        = bounds,
    };

    return &ranges[count++];
}

static u32 merge(void) {
    u32 draw_count = nya_render2d_ranges_merge(ranges, count, draws);
    count          = 0;

    return draw_count;
}

/** A widget in row `i`: its body, then its label inside it. */
static void widget(u32 i) {
    NYA_Rectf body = { 10.0F, 10.0F + (50.0F * (f32)i), 200.0F, 40.0F };

    (void)range(PIPELINE_SHAPES, body, 6);
    (void)range(PIPELINE_TEXT, nya_rect_expand(body, -8.0F), 6);
}

s32 main(void) {
    // A menu of widgets that do not overlap each other is two draws, however many widgets it has.
    {
        for (u32 i = 0; i < 5; i++) widget(i);

        u32 draw_count = merge();
        nya_check(draw_count == 2, "five bodies and five labels, got %u draws", draw_count);
        nya_check(draws[0].index_count == 30 && draws[1].index_count == 30, "each draw holds all five, got %u %u", draws[0].index_count, draws[1].index_count);
        nya_check(nya_string_equals(ranges[draws[0].first_range].pipeline, PIPELINE_SHAPES), "bodies first, as declared");
    }

    // The index stream is written in draw order, and inside a draw in paint order.
    {
        u32 indices[36];
        for (u32 i = 0; i < nya_carray_length(indices); i++) indices[i] = i;

        for (u32 i = 0; i < 3; i++) widget(i);

        u32 draw_count = merge();
        u32 out[36]    = { 0 };
        nya_render2d_draws_indices_write(ranges, draws, draw_count, indices, out);

        // bodies were declared at indices 0, 12 and 24, labels at 6, 18 and 30.
        u32 expected[] = { 0, 12, 24, 6, 18, 30 };
        b8  ordered    = draws[0].first_index == 0 && draws[1].first_index == 18;
        for (u32 i = 0; i < 6; i++) ordered = ordered && out[i * 6] == expected[i] && out[(i * 6) + 5] == expected[i] + 5;

        nya_check(ordered, "bodies then labels, each in declaration order");
    }

    // A label that overlaps the next body keeps that body out of the earlier draw.
    {
        (void)range(PIPELINE_SHAPES, (NYA_Rectf){ 0.0F, 0.0F, 100.0F, 100.0F }, 6);
        (void)range(PIPELINE_TEXT, (NYA_Rectf){ 150.0F, 0.0F, 100.0F, 20.0F }, 6);
        (void)range(PIPELINE_SHAPES, (NYA_Rectf){ 140.0F, 10.0F, 100.0F, 100.0F }, 6);

        nya_check(merge() == 3, "the body would paint under the label it covers");

        (void)range(PIPELINE_SHAPES, (NYA_Rectf){ 0.0F, 0.0F, 100.0F, 100.0F }, 6);
        (void)range(PIPELINE_TEXT, (NYA_Rectf){ 100.0F, 0.0F, 100.0F, 20.0F }, 6);
        (void)range(PIPELINE_SHAPES, (NYA_Rectf){ 200.0F, 0.0F, 100.0F, 100.0F }, 6);

        nya_check(merge() == 2, "but an edge against an edge shares no pixel");
    }

    // Layers sort first, and a merge never lifts a range over a higher layer that covers it.
    {
        NYA_Rectf everywhere = { 0.0F, 0.0F, 800.0F, 600.0F };

        range(PIPELINE_TEXT, everywhere, 6)->layer   = 2;
        range(PIPELINE_SHAPES, everywhere, 6)->layer = 1;
        range(PIPELINE_TEXT, everywhere, 6)->layer   = 0;

        u32 draw_count = merge();
        nya_check(draw_count == 3, "got %u", draw_count);
        nya_check(ranges[draws[0].first_range].layer == 0 && ranges[draws[2].first_range].layer == 2, "painted by layer, not as declared");

        range(PIPELINE_TEXT, everywhere, 6)->layer   = 1;
        range(PIPELINE_SHAPES, everywhere, 6)->layer = 0;
        range(PIPELINE_TEXT, everywhere, 6)->layer   = 0;

        nya_check(merge() == 2, "and ranges a sort brings together merge");
    }

    // State: another texture, scissor, uniform or camera is another draw.
    {
        NYA_Rectf a = { 0.0F, 0.0F, 10.0F, 10.0F };
        NYA_Rectf b = { 20.0F, 0.0F, 10.0F, 10.0F };

        (void)range(PIPELINE_SHAPES, a, 3);
        range(PIPELINE_SHAPES, b, 3)->texture = (SDL_GPUTexture*)1;
        nya_check(merge() == 2, "another texture");

        (void)range(PIPELINE_SHAPES, a, 3);
        NYA_Render2DDrawRange* clipped = range(PIPELINE_SHAPES, b, 3);
        clipped->scissor_active        = true;
        clipped->scissor_width         = 10;
        nya_check(merge() == 2, "another scissor");

        range(PIPELINE_SHAPES, a, 3)->uniform_size = 4;
        NYA_Render2DDrawRange* tinted     = range(PIPELINE_SHAPES, b, 3);
        tinted->uniform_size              = 4;
        tinted->uniform[0]                = 1;
        nya_check(merge() == 2, "another uniform");

        range(PIPELINE_SHAPES, a, 3)->uniform_size = 4;
        range(PIPELINE_SHAPES, b, 3)->uniform_size = 4;
        nya_check(merge() == 1, "and the same uniform merges");

        // the label's bounds are in world units, so they say nothing about where it lands against the others.
        (void)range(PIPELINE_SHAPES, a, 3);
        range(PIPELINE_TEXT, b, 3)->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_TOP_DOWN, .as_top_down = { .zoom = 2.0F } };
        (void)range(PIPELINE_SHAPES, (NYA_Rectf){ 40.0F, 0.0F, 10.0F, 10.0F }, 3);
        nya_check(merge() == 3, "a range in another space in between stops the search");
    }

    // The search looks back a fixed number of draws.
    {
        NYA_Rectf spot = { 0.0F, 0.0F, 1.0F, 1.0F };

        (void)range(PIPELINE_SHAPES, spot, 3);

        for (u32 i = 0; i < NYA_RENDER2D_MERGE_LOOKBACK; i++) {
            range(PIPELINE_SHAPES, (NYA_Rectf){ 10.0F + (f32)i, 0.0F, 0.5F, 0.5F }, 3)->texture = (SDL_GPUTexture*)(uintptr_t)(i + 1);
        }

        (void)range(PIPELINE_SHAPES, spot, 3);

        nya_check(merge() == NYA_RENDER2D_MERGE_LOOKBACK + 2, "past the lookback a range starts a draw of its own");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
