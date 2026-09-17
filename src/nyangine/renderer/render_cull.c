/**
 * @file render_cull.c
 *
 * The frustum and occlusion test render3d.c runs before queueing a draw. No GPU state, so both builds
 * include it and a headless test reaches the real thing.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Extracts the six inward-facing clip planes of `batch->view_projection`. Once per pass, not per draw. */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_frustum_build(NYA_Render3DBatch* batch);

/** Whether a bounding sphere is on the visible side of every frustum plane. */
NYA_INTERNAL __attr_allow_unused b8 _nya_render3d_visible(const NYA_Render3DBatch* batch, f32x3 center, f32 radius) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_render3d_frustum_build(NYA_Render3DBatch* batch) {
    /*
     * Standard plane extraction: each plane is a sum or difference of two matrix rows. Near is row 2 alone,
     * not row3 + row2, because this projection maps depth to [0, 1] (see nya_matrix_perspective).
     */
    f32_4x4 m = batch->view_projection;

    f32x4 row0 = { m[0][0], m[0][1], m[0][2], m[0][3] };
    f32x4 row1 = { m[1][0], m[1][1], m[1][2], m[1][3] };
    f32x4 row2 = { m[2][0], m[2][1], m[2][2], m[2][3] };
    f32x4 row3 = { m[3][0], m[3][1], m[3][2], m[3][3] };

    batch->frustum[0] = row3 + row0;  // left
    batch->frustum[1] = row3 - row0;  // right
    batch->frustum[2] = row3 + row1;  // bottom
    batch->frustum[3] = row3 - row1;  // top
    batch->frustum[4] = row2;         // near
    batch->frustum[5] = row3 - row2;  // far

    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = batch->frustum[i];

        f32 length = nya_vector_length((f32x3){ plane[0], plane[1], plane[2] });

        // a well formed projection has no degenerate plane. left alone if one appears, which reads as visible.
        if (length < NYA_EPSILON) continue;

        batch->frustum[i] = plane / length;
    }
}

b8 _nya_render3d_visible(const NYA_Render3DBatch* batch, f32x3 center, f32 radius) {
    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = batch->frustum[i];

        f32x3 normal = { plane[0], plane[1], plane[2] };

        // planes are normalized, so this is a signed distance.
        f32 distance = nya_vector_dot(normal, center) + plane[3];

        if (distance < -radius) return false;
    }

    /* Only for what survived the frustum, and only with a buffer set. */
    if (batch->occlusion == nullptr) return true;

    /*
     * Never during a shadow pass: the buffer belongs to the camera, and a caster the camera cannot see still
     * shadows ground it can. The frustum test still applies, rebuilt from the light's matrix.
     */
    if (batch->shadow_pass_active) return true;

    if (!nya_occlusion_test(batch->occlusion, center, radius)) return true;

    ((NYA_Render3DBatch*)batch)->frame_occluded++;
    return false;
}
