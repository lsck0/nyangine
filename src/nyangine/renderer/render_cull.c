/**
 * @file render_cull.c
 *
 * Where a draw goes: which passes see it, and which of the two streams it is recorded into. render3d.c asks both once
 * when a draw is recorded and keeps the answers. No GPU state, so both builds include this and a headless test reaches
 * the real thing.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether a draw of `color` under `blend` belongs in the transparent stream rather than the opaque one.
 *
 * Alpha decides it, except under addition: emission is never opaque, and a particle is born at exactly alpha one.
 * */
NYA_INTERNAL __attr_allow_unused b8 _nya_render3d_stream_transparent(NYA_Color color, NYA_Render3DBlend blend) __attr_no_discard;

/** Extracts the six inward-facing clip planes of `view_projection`. Once per pass, not per draw. */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_frustum_build(NYA_Render3DFrustum* frustum, f32_4x4 view_projection);

/** Whether a bounding sphere is on the visible side of every plane. */
NYA_INTERNAL __attr_allow_unused b8 _nya_render3d_visible(const NYA_Render3DFrustum* frustum, f32x3 center, f32 radius) __attr_no_discard;

/**
 * One bit for each of the window's passes that sees the sphere, the camera in bit zero. The camera's bit also answers
 * to the occlusion buffer. Zero is counted as culled.
 *
 * Takes the window rather than the batch because both culls are switchable; see NYA_RENDER_FEATURE_FRUSTUM_CULLING.
 * */
NYA_INTERNAL __attr_allow_unused u8 _nya_render3d_passes_seeing(NYA_Window* window, f32x3 center, f32 radius) __attr_no_discard;

/**
 * Appends the indices of `stream`'s objects in [first_object, end_object) that `pass` sees to `out`, and returns how
 * many. Neighbours share one copy.
 * */
NYA_INTERNAL __attr_allow_unused u32 _nya_render3d_pass_indices(const NYA_Render3DStream* stream, u32 first_object, u32 end_object, u32 pass,
                                                                u16* out) __attr_no_discard;

/** The first run of entries in [at, end) whose mask has `pass`, as its start and length. Zero when there is none. */
NYA_INTERNAL __attr_allow_unused u32 _nya_render3d_pass_run(const u8* passes, u32 at, u32 end, u32 pass, OUT u32* out_first) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_render3d_stream_transparent(NYA_Color color, NYA_Render3DBlend blend) {
    nya_assert(blend < NYA_RENDER3D_BLEND_COUNT, "a draw's blend mode is one of the declared ones");

    return color.a < 1.0F || blend == NYA_RENDER3D_BLEND_ADDITIVE;
}

void _nya_render3d_frustum_build(NYA_Render3DFrustum* frustum, f32_4x4 view_projection) {
    nya_assert(frustum != nullptr);

    /*
     * Standard plane extraction: each plane is a sum or difference of two matrix rows. Near is row 2 alone,
     * not row3 + row2, because this projection maps depth to [0, 1] (see nya_matrix_perspective).
     */
    f32_4x4 m = view_projection;

    f32x4 row0 = { m[0][0], m[0][1], m[0][2], m[0][3] };
    f32x4 row1 = { m[1][0], m[1][1], m[1][2], m[1][3] };
    f32x4 row2 = { m[2][0], m[2][1], m[2][2], m[2][3] };
    f32x4 row3 = { m[3][0], m[3][1], m[3][2], m[3][3] };

    frustum->planes[0] = row3 + row0;  // left
    frustum->planes[1] = row3 - row0;  // right
    frustum->planes[2] = row3 + row1;  // bottom
    frustum->planes[3] = row3 - row1;  // top
    frustum->planes[4] = row2;         // near
    frustum->planes[5] = row3 - row2;  // far

    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = frustum->planes[i];

        f32 length = nya_vector_length((f32x3){ plane[0], plane[1], plane[2] });

        // a well formed projection has no degenerate plane. left alone if one appears, which reads as visible.
        if (length < NYA_EPSILON) continue;

        frustum->planes[i] = plane / length;
    }
}

b8 _nya_render3d_visible(const NYA_Render3DFrustum* frustum, f32x3 center, f32 radius) {
    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = frustum->planes[i];

        f32x3 normal = { plane[0], plane[1], plane[2] };

        // planes are normalized, so this is a signed distance.
        f32 distance = nya_vector_dot(normal, center) + plane[3];

        if (distance < -radius) return false;
    }

    return true;
}

u8 _nya_render3d_passes_seeing(NYA_Window* window, f32x3 center, f32 radius) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    nya_assert(batch->pass_count >= 1 && batch->pass_count <= NYA_RENDER3D_PASSES, "the passes are fitted before anything is recorded");

    // every pass sees everything, which is exactly what the cull is worth. the occlusion buffer goes with it: it
    // only ever removes what the frustum kept.
    if (!nya_render_feature_enabled(window, NYA_RENDER_FEATURE_FRUSTUM_CULLING)) return (u8)((1U << batch->pass_count) - 1U);

    u8 passes = 0;

    for (u32 pass = 0; pass < batch->pass_count; pass++) {
        if (_nya_render3d_visible(&batch->passes[pass], center, radius)) passes |= (u8)(1U << pass);
    }

    /*
     * Only the camera answers to the occlusion buffer, and only for what survived its frustum: a caster the camera
     * cannot see still shadows ground it can.
     */
    if ((passes & 1U) != 0 && batch->occlusion != nullptr && nya_render_feature_enabled(window, NYA_RENDER_FEATURE_OCCLUSION_CULLING)
        && nya_occlusion_test(batch->occlusion, center, radius)) {
        passes &= (u8)~1U;
        batch->frame_occluded++;
    }

    if (passes == 0) batch->frame_culled++;

    return passes;
}

u32 _nya_render3d_pass_indices(const NYA_Render3DStream* stream, u32 first_object, u32 end_object, u32 pass, u16* out) {
    nya_assert(stream != nullptr && out != nullptr);
    nya_assert(first_object <= end_object && end_object <= stream->object_count);

    u32 written = 0;

    for (u32 i = first_object; i < end_object; i++) {
        const NYA_Render3DObject* object = &stream->objects[i];

        if ((object->passes & (1U << pass)) == 0) continue;

        u32 end = i + 1 < stream->object_count ? stream->objects[i + 1].first_index : stream->index_count;

        nya_memcpy(out + written, stream->indices + object->first_index, (u64)(end - object->first_index) * sizeof(u16));
        written += end - object->first_index;
    }

    return written;
}

u32 _nya_render3d_pass_run(const u8* passes, u32 at, u32 end, u32 pass, OUT u32* out_first) {
    nya_assert(passes != nullptr || at == end);
    nya_assert(out_first != nullptr);

    u8 bit = (u8)(1U << pass);

    while (at < end && (passes[at] & bit) == 0) at++;

    *out_first = at;

    u32 last = at;
    while (last < end && (passes[last] & bit) != 0) last++;

    return last - at;
}
