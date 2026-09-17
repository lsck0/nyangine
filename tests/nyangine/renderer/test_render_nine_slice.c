/**
 * Where a nine-slice's pieces go, headless: corners keep their scaled size and never stretch, a destination smaller
 * than the corners shrinks them in proportion, a sheet region offsets the source, and tiles repeat at the source size,
 * cut the last one short, and stop at the most there may be.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Whether the pieces cover the destination edge to edge, with nothing overlapping. */
static b8 covers(const NYA_NineSliceAxis* axis, f32 start, f32 length) {
    f32 at = start;

    for (u32 i = 0; i < axis->count; i++) {
        if (axis->destination[i] != at) return false;
        at += axis->destination_size[i];
    }

    return at == start + length;
}

s32 main(void) {
    NYA_NineSliceAxis axis;

    // ── Stretched: the corners keep their size, the middle takes the rest.
    {
        nya_render2d_nine_slice_axis(0.0F, 48.0F, 12.0F, 8.0F, 100.0F, 300.0F, 1.0F, NYA_NINE_SLICE_STRETCH, &axis);

        nya_check(axis.count == 3 && covers(&axis, 100.0F, 300.0F), "three pieces cover the destination, got %u", axis.count);
        nya_check(axis.source_size[0] == 12.0F && axis.destination_size[0] == 12.0F, "the start corner is not stretched");
        nya_check(axis.source[2] == 40.0F && axis.source_size[2] == 8.0F && axis.destination_size[2] == 8.0F, "nor is the end one");
        nya_check(axis.source[1] == 12.0F && axis.source_size[1] == 28.0F && axis.destination_size[1] == 280.0F && axis.middle[1], "the middle stretches");
        nya_check(!axis.middle[0] && !axis.middle[2], "and only the middle is middle");
    }

    // ── Scaled: corners grow with the scale and snap to whole pixels; a region in a sheet offsets the source.
    {
        nya_render2d_nine_slice_axis(64.0F, 48.0F, 12.0F, 8.0F, 0.0F, 300.0F, 1.5F, NYA_NINE_SLICE_STRETCH, &axis);

        nya_check(axis.destination_size[0] == 18.0F && axis.destination_size[2] == 12.0F, "borders scale, got %f %f", (f64)axis.destination_size[0], (f64)axis.destination_size[2]);
        nya_check(axis.source[0] == 64.0F && axis.source[1] == 76.0F && axis.source[2] == 104.0F, "the region's offset carries into the source");
        nya_check(covers(&axis, 0.0F, 300.0F), "and the pieces still cover the destination");

        nya_render2d_nine_slice_axis(0.0F, 30.0F, 5.0F, 5.0F, 0.0F, 100.0F, 1.25F, NYA_NINE_SLICE_STRETCH, &axis);
        nya_check(axis.destination_size[0] == roundf(5.0F * 1.25F) && covers(&axis, 0.0F, 100.0F), "a fractional border rounds and the middle absorbs it");
    }

    // ── Degenerate: shorter than both corners, they shrink in proportion and meet, and there is no middle.
    {
        nya_render2d_nine_slice_axis(0.0F, 48.0F, 12.0F, 12.0F, 0.0F, 10.0F, 1.0F, NYA_NINE_SLICE_STRETCH, &axis);
        nya_check(axis.count == 2 && axis.destination_size[0] == 5.0F && axis.destination_size[1] == 5.0F && covers(&axis, 0.0F, 10.0F), "even corners halve the space");
        nya_check(axis.source_size[0] == 12.0F, "while still sampling the whole corner");

        nya_render2d_nine_slice_axis(0.0F, 48.0F, 10.0F, 30.0F, 0.0F, 20.0F, 1.0F, NYA_NINE_SLICE_STRETCH, &axis);
        nya_check(axis.destination_size[0] == 5.0F && axis.destination_size[1] == 15.0F, "uneven ones keep their proportion, got %f %f", (f64)axis.destination_size[0], (f64)axis.destination_size[1]);

        nya_render2d_nine_slice_axis(0.0F, 48.0F, 12.0F, 12.0F, 0.0F, 0.0F, 1.0F, NYA_NINE_SLICE_STRETCH, &axis);
        nya_check(axis.count == 0, "nothing to cover is nothing to cut");

        nya_render2d_nine_slice_axis(0.0F, 20.0F, 15.0F, 15.0F, 0.0F, 100.0F, 1.0F, NYA_NINE_SLICE_STRETCH, &axis);
        nya_check(axis.source_size[0] + axis.source_size[axis.count - 1] <= 20.0F, "borders wider than the source are cut back to it");
    }

    // ── Tiled: the middle repeats at the source size, the last piece cut short in source and destination alike.
    {
        nya_render2d_nine_slice_axis(0.0F, 50.0F, 10.0F, 10.0F, 0.0F, 200.0F, 1.0F, NYA_NINE_SLICE_TILE, &axis);
        nya_check(axis.count == 2 + 6 && covers(&axis, 0.0F, 200.0F), "180 of middle is six tiles of 30, got %u", axis.count);

        nya_render2d_nine_slice_axis(0.0F, 50.0F, 10.0F, 10.0F, 0.0F, 190.0F, 1.0F, NYA_NINE_SLICE_TILE, &axis);
        u32 last = axis.count - 2;
        nya_check(axis.count == 2 + 6 && axis.destination_size[last] == 20.0F && axis.source_size[last] == 20.0F, "170 ends on a tile of 20 from 20 source pixels, got %f %f",
                  (f64)axis.destination_size[last], (f64)axis.source_size[last]);
        nya_check(covers(&axis, 0.0F, 190.0F), "and still covers the destination");

        nya_render2d_nine_slice_axis(0.0F, 50.0F, 10.0F, 10.0F, 0.0F, 200.0F, 2.0F, NYA_NINE_SLICE_TILE, &axis);
        nya_check(axis.count == 2 + 3 && axis.destination_size[1] == 60.0F, "tiles scale with the borders, got %u", axis.count);

        nya_render2d_nine_slice_axis(0.0F, 22.0F, 10.0F, 10.0F, 0.0F, 1000.0F, 1.0F, NYA_NINE_SLICE_TILE, &axis);
        nya_check(axis.count == 2 + NYA_NINE_SLICE_TILES_MAX && covers(&axis, 0.0F, 1000.0F), "a two pixel tile over 980 stops at the most tiles, got %u", axis.count);
        nya_check(axis.source_size[1] == 2.0F, "each still sampling the whole tile");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
