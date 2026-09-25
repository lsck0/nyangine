/**
 * @file render_nine_slice.c
 *
 * Where a nine-slice's pieces go. See nya_render2d_nine_slice_axis.
 * */
#include "nyangine-core/nyangine.h"

void nya_render2d_nine_slice_axis(
    f32                    source_start,
    f32                    source_length,
    f32                    border_start,
    f32                    border_end,
    f32                    destination_start,
    f32                    destination_length,
    f32                    scale,
    NYA_NineSliceFill      fill,
    OUT NYA_NineSliceAxis* out_axis
) {
    nya_assert(out_axis != nullptr);
    nya_assert(fill < NYA_NINE_SLICE_FILL_COUNT);

    *out_axis = (NYA_NineSliceAxis){ 0 };
    if (source_length <= 0.0F || destination_length <= 0.0F) return;

    if (scale <= 0.0F) scale = 1.0F;

    // borders wider than the source together are cut back, or the middle would have a negative size.
    f32 start = nya_clamp(border_start, 0.0F, source_length);
    f32 end   = nya_clamp(border_end, 0.0F, source_length - start);

    // whole pixels, so a scaled border stays crisp.
    f32 start_size = roundf(start * scale);
    f32 end_size   = roundf(end * scale);

    // shorter than both corners: they shrink in proportion and meet, instead of drawing over each other.
    if (start_size + end_size > destination_length) {
        start_size = roundf(destination_length * (start_size / (start_size + end_size)));
        end_size   = destination_length - start_size;
    }

    NYA_NineSliceAxis* axis = out_axis;

    if (start_size > 0.0F) {
        axis->source[axis->count]           = source_start;
        axis->source_size[axis->count]      = start;
        axis->destination[axis->count]      = destination_start;
        axis->destination_size[axis->count] = start_size;
        axis->count++;
    }

    f32 middle_source      = source_length - start - end;
    f32 middle_destination = destination_length - start_size - end_size;

    if (middle_source > 0.0F && middle_destination > 0.0F) {
        f32 tile  = fill == NYA_NINE_SLICE_TILE ? nya_max(roundf(middle_source * scale), 1.0F) : middle_destination;
        u32 tiles = (u32)ceilf(middle_destination / tile);

        // too many to cut: the most there may be, each a whole tile stretched a little to cover the length.
        b8 stretched = fill == NYA_NINE_SLICE_STRETCH || tiles > NYA_NINE_SLICE_TILES_MAX;

        if (tiles > NYA_NINE_SLICE_TILES_MAX) {
            tiles = NYA_NINE_SLICE_TILES_MAX;
            tile  = middle_destination / (f32)tiles;
        }

        for (u32 i = 0; i < tiles; i++) {
            f32 offset = roundf((f32)i * tile);
            f32 size   = nya_min(roundf((f32)(i + 1) * tile), middle_destination) - offset;

            axis->source[axis->count]           = source_start + start;
            axis->source_size[axis->count]      = stretched ? middle_source : middle_source * nya_min(size / tile, 1.0F);
            axis->destination[axis->count]      = destination_start + start_size + offset;
            axis->destination_size[axis->count] = size;
            axis->middle[axis->count]           = true;
            axis->count++;
        }
    }

    if (end_size > 0.0F) {
        axis->source[axis->count]           = source_start + source_length - end;
        axis->source_size[axis->count]      = end;
        axis->destination[axis->count]      = destination_start + destination_length - end_size;
        axis->destination_size[axis->count] = end_size;
        axis->count++;
    }

    nya_assert(axis->count <= NYA_NINE_SLICE_TILES_MAX + 2);
}
