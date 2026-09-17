#include "nyangine/nyangine.h"

/**
 * A depth's sortable bit pattern.
 */
NYA_INTERNAL u32 _nya_render3d_sort_bits(f32 depth) {
    u32 bits = 0;
    nya_memcpy(&bits, &depth, sizeof(bits));

    return bits;
}

/**
 * Ascending radix sort over the depth bits: four passes of eight, ping-ponging between the two arrays.
 */
void nya_render3d_sort_keys(NYA_Render3DSortKey* keys, NYA_Render3DSortKey* scratch, u32 count) {
    NYA_Render3DSortKey* source      = keys;
    NYA_Render3DSortKey* destination = scratch;

    for (u32 shift = 0; shift < 32; shift += 8) {
        u32 histogram[256] = { 0 };

        for (u32 i = 0; i < count; i++) histogram[(_nya_render3d_sort_bits(source[i].depth) >> shift) & 0xFFU]++;

        // A pass whose digit is the same for every key would only copy the array; skipping it also
        // keeps the ping-pong parity correct, since the result must end up back in `keys`.
        u32 offset = 0;
        for (u32 bucket = 0; bucket < 256; bucket++) {
            u32 written    = histogram[bucket];
            histogram[bucket] = offset;
            offset           += written;
        }

        for (u32 i = 0; i < count; i++) {
            u32 digit = (_nya_render3d_sort_bits(source[i].depth) >> shift) & 0xFFU;

            destination[histogram[digit]++] = source[i];
        }

        NYA_Render3DSortKey* swap = source;
        source                    = destination;
        destination               = swap;
    }

    // Four passes is even, so `source` is `keys` again and nothing has to be copied back.
    nya_assert(source == keys, "the radix sort must end with the result in the caller's array");
}

/**
 * Layer first, declaration order second.
 */
NYA_INTERNAL s32 _nya_render2d_range_compare(const void* a, const void* b) {
    const NYA_Render2DDrawRange* left  = a;
    const NYA_Render2DDrawRange* right = b;

    if (left->layer != right->layer) return left->layer < right->layer ? -1 : 1;
    if (left->sequence != right->sequence) return left->sequence < right->sequence ? -1 : 1;

    return 0;
}

/**
 * Whether the two draw into the same space: the bounds of one only mean something against the other's then.
 */
NYA_INTERNAL b8 _nya_render2d_range_space_equal(const NYA_Render2DDrawRange* a, const NYA_Render2DDrawRange* b) {
    if (a->target_width != b->target_width || a->target_height != b->target_height) return false;
    if (a->camera.kind != b->camera.kind) return false;

    return a->camera.kind == NYA_CAMERA2D_KIND_NONE || nya_memcmp(&a->camera, &b->camera, sizeof(a->camera)) == 0;
}

/**
 * Whether the two can be one draw call: everything the replay binds, pushes or sets per range.
 */
NYA_INTERNAL b8 _nya_render2d_range_state_equal(const NYA_Render2DDrawRange* a, const NYA_Render2DDrawRange* b) {
    if (a->pipeline != b->pipeline || a->texture != b->texture || a->sampler != b->sampler || a->shader_texture != b->shader_texture) return false;
    if (a->scissor_active != b->scissor_active) return false;

    if (a->scissor_active) {
        if (a->scissor_x != b->scissor_x || a->scissor_y != b->scissor_y || a->scissor_width != b->scissor_width || a->scissor_height != b->scissor_height) return false;
    }

    if (a->uniform_size != b->uniform_size || nya_memcmp(a->uniform, b->uniform, a->uniform_size) != 0) return false;

    return _nya_render2d_range_space_equal(a, b);
}

u32 nya_render2d_ranges_merge(NYA_Render2DDrawRange* ranges, u32 count, NYA_Render2DDraw* out_draws) {
    nya_assert((ranges != nullptr && out_draws != nullptr) || count == 0);

    qsort(ranges, count, sizeof(NYA_Render2DDrawRange), _nya_render2d_range_compare);

    u32 draw_count = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_Render2DDrawRange* range = &ranges[i];
        range->next                  = U32_MAX;

        u32 floor = draw_count > NYA_RENDER2D_MERGE_LOOKBACK ? draw_count - NYA_RENDER2D_MERGE_LOOKBACK : 0;
        u32 join  = U32_MAX;

        for (u32 d = draw_count; d > floor; d--) {
            const NYA_Render2DDraw*      draw  = &out_draws[d - 1];
            const NYA_Render2DDrawRange* first = &ranges[draw->first_range];

            if (_nya_render2d_range_state_equal(first, range)) {
                join = d - 1;
                break;
            }

            // painted between the draw it would join and where it was declared, so it has to stay under this range.
            if (!_nya_render2d_range_space_equal(first, range) || nya_rect_overlaps(draw->bounds, range->bounds)) break;
        }

        if (join == U32_MAX) {
            out_draws[draw_count++] = (NYA_Render2DDraw){ .first_range = i, .last_range = i, .index_count = range->index_count, .bounds = range->bounds };
            continue;
        }

        NYA_Render2DDraw* draw        = &out_draws[join];
        ranges[draw->last_range].next = i;
        draw->last_range              = i;
        draw->index_count            += range->index_count;
        draw->bounds                  = nya_rect_union(draw->bounds, range->bounds);
    }

    nya_assert(draw_count <= count);

    return draw_count;
}

void nya_render2d_draws_indices_write(const NYA_Render2DDrawRange* ranges, NYA_Render2DDraw* draws, u32 draw_count, const u32* indices, u32* out_indices) {
    nya_assert((ranges != nullptr && draws != nullptr && indices != nullptr && out_indices != nullptr) || draw_count == 0);

    u32 written = 0;

    for (u32 d = 0; d < draw_count; d++) {
        draws[d].first_index = written;

        for (u32 r = draws[d].first_range; r != U32_MAX; r = ranges[r].next) {
            nya_memcpy(&out_indices[written], &indices[ranges[r].first_index], ranges[r].index_count * sizeof(u32));
            written += ranges[r].index_count;
        }

        nya_assert(written - draws[d].first_index == draws[d].index_count);
    }
}
