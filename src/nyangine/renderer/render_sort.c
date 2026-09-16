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

        for (u32 i = 0; i < count; i++) histogram[(_nya_render3d_sort_bits(source[i].depth) >> shift) & 0xFFu]++;

        // A pass whose digit is the same for every key would only copy the array; skipping it also
        // keeps the ping-pong parity correct, since the result must end up back in `keys`.
        u32 offset = 0;
        for (u32 bucket = 0; bucket < 256; bucket++) {
            u32 written    = histogram[bucket];
            histogram[bucket] = offset;
            offset           += written;
        }

        for (u32 i = 0; i < count; i++) {
            u32 digit = (_nya_render3d_sort_bits(source[i].depth) >> shift) & 0xFFu;

            destination[histogram[digit]++] = source[i];
        }

        NYA_Render3DSortKey* swap = source;
        source                    = destination;
        destination               = swap;
    }

    // Four passes is even, so `source` is `keys` again and nothing has to be copied back.
    nya_assert(source == keys, "the radix sort must end with the result in the caller's array");
}
