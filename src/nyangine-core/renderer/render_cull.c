/**
 * @file render_cull.c
 *
 * Where a draw goes: which passes see it, and which of the two streams it is recorded into. render3d.c asks both once
 * when a draw is recorded and keeps the answers. No GPU state, so both builds include this and a headless test reaches
 * the real thing.
 * */
#include "nyangine-core/nyangine.h"

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

/**
 * The world bounding sphere of an instanced grass patch, from its instance placements. The centre is the midpoint of
 * the instance translations' box; the radius is half its diagonal plus the reach of the tallest blade and how far the
 * sway throws its tip (`sway_reach`), both grown by the largest instance scale. Culls the field as one and picks its
 * disturbers. `count` must be at least one. See nya_render3d_grass.
 * */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_grass_bounds(const NYA_Render3DInstance* instances, u32 count, f32 blade_radius,
                                                                 f32 sway_reach, OUT f32x3* out_center, OUT f32* out_radius);

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

    // Standard plane extraction from row sums/differences; near is row 2 alone because depth maps to [0, 1] (see nya_matrix_perspective).
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

    // Cull off: every pass sees everything, and the occlusion buffer goes with it since it only removes what the frustum kept.
    if (!nya_render_feature_enabled(window, NYA_RENDER_FEATURE_FRUSTUM_CULLING)) return (u8)((1U << batch->pass_count) - 1U);

    u8 passes = 0;

    for (u32 pass = 0; pass < batch->pass_count; pass++) {
        if (_nya_render3d_visible(&batch->passes[pass], center, radius)) passes |= (u8)(1U << pass);
    }

    // Only the camera answers to the occlusion buffer, and only for what survived its frustum, so hidden casters still shadow.
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

b8 nya_render3d_skinned_bounds(const f32_4x4* palette, u32 bone_count, f32_4x4 model, f32x3 rest_min, f32x3 rest_max,
                               OUT f32x3* out_center, OUT f32* out_radius) {
    nya_assert(out_center != nullptr);
    nya_assert(out_radius != nullptr);

    if (palette == nullptr || bone_count == 0) return false;

    const u32 bones = bone_count < NYA_SHADER_SKIN_MAX_BONES ? bone_count : NYA_SHADER_SKIN_MAX_BONES;

    f32x3 minimum = { F32_MAX, F32_MAX, F32_MAX };
    f32x3 maximum = { -F32_MAX, -F32_MAX, -F32_MAX };

    for (u32 b = 0; b < bones; b++) {
        const f32_4x4 placed = model * palette[b];

        // column three is the translation: where this bone ended up in the world.
        const f32x3 origin = { placed[0][3], placed[1][3], placed[2][3] };

        minimum = nya_min(minimum, origin);
        maximum = nya_max(maximum, origin);
    }

    // The rest radius scaled by the largest axis column, so the skin off each bone stays inside the sphere.
    const f32x3 extent = (rest_max - rest_min) * 0.5F;

    const f32 scale = nya_max(
        nya_vector_length((f32x3){ model[0][0], model[1][0], model[2][0] }),
        nya_max(nya_vector_length((f32x3){ model[0][1], model[1][1], model[2][1] }),
                nya_vector_length((f32x3){ model[0][2], model[1][2], model[2][2] }))
    );

    *out_center = (maximum + minimum) * 0.5F;
    *out_radius = nya_vector_length((maximum - minimum) * 0.5F) + (nya_vector_length(extent) * scale);

    return true;
}

void _nya_render3d_grass_bounds(const NYA_Render3DInstance* instances, u32 count, f32 blade_radius, f32 sway_reach, OUT f32x3* out_center,
                                OUT f32* out_radius) {
    nya_assert(instances != nullptr);
    nya_assert(count > 0);
    nya_assert(out_center != nullptr);
    nya_assert(out_radius != nullptr);

    // the translation is the model matrix's fourth column, the convention the skinned bounds and the sort read.
    f32x3 minimum   = { instances[0].model[0][3], instances[0].model[1][3], instances[0].model[2][3] };
    f32x3 maximum   = minimum;
    f32   max_scale = 1.0F;

    for (u32 i = 0; i < count; i++) {
        const f32x3 origin = { instances[i].model[0][3], instances[i].model[1][3], instances[i].model[2][3] };

        minimum = nya_min(minimum, origin);
        maximum = nya_max(maximum, origin);

        // A column's length is that axis's scale; the largest grows the margin so a scaled-up blade stays inside the field sphere.
        const f32 scale = nya_max(
            nya_vector_length((f32x3){ instances[i].model[0][0], instances[i].model[1][0], instances[i].model[2][0] }),
            nya_max(nya_vector_length((f32x3){ instances[i].model[0][1], instances[i].model[1][1], instances[i].model[2][1] }),
                    nya_vector_length((f32x3){ instances[i].model[0][2], instances[i].model[1][2], instances[i].model[2][2] }))
        );

        max_scale = nya_max(max_scale, scale);
    }

    *out_center = (maximum + minimum) * 0.5F;
    *out_radius = nya_vector_length((maximum - minimum) * 0.5F) + ((blade_radius + sway_reach) * max_scale);
}
