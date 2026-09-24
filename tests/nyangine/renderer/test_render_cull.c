/**
 * The culling render3d.c runs when a draw is recorded: the frustum planes a view-projection yields, which spheres they
 * reject, the pass mask a draw keeps, and the per-pass lists and runs the playback builds from those masks.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Frustum plane order, as _nya_render3d_frustum_build writes them. */
enum { PLANE_LEFT, PLANE_RIGHT, PLANE_BOTTOM, PLANE_TOP, PLANE_NEAR, PLANE_FAR };

#define FOV_Y      ((f32)M_PI / 3.0F)
#define ASPECT     (16.0F / 9.0F)
#define NEAR_PLANE 0.1F
#define FAR_PLANE  100.0F

/** Far larger than the stack wants. */
static NYA_Window          window;
static NYA_Render3DBatch*  batch = &window.render_system.mesh_batch;
static NYA_Render3DFrustum frustum;
static NYA_OcclusionBuffer occlusion;

/** A camera at the origin looking down -z, which is the frame every coordinate below is written in. */
static f32_4x4 perspective_matrix(void) {
    f32_4x4 projection = nya_matrix_perspective(FOV_Y, ASPECT, NEAR_PLANE, FAR_PLANE);
    f32_4x4 view       = nya_matrix_look_at((f32x3){ 0, 0, 0 }, (f32x3){ 0, 0, -1 }, (f32x3){ 0, 1, 0 });

    return projection * view;
}

/** A box a hundred wide around the origin, looking down -z from z = 50. What a cascade over the scene looks like. */
static f32_4x4 orthographic_matrix(void) {
    f32_4x4 projection = nya_matrix_orthographic_3d(100.0F, 1.0F, 1.0F, 200.0F);
    f32_4x4 view       = nya_matrix_look_at((f32x3){ 0, 0, 50 }, (f32x3){ 0, 0, 0 }, (f32x3){ 0, 1, 0 });

    return projection * view;
}

/** Builds the test frustum from `view_projection`. */
static void frustum_from(f32_4x4 view_projection) {
    nya_memset(&frustum, 0, sizeof(frustum));

    _nya_render3d_frustum_build(&frustum, view_projection);
}

/** Signed distance from `point` to one plane, positive on the inside. */
static f32 plane_distance(u32 plane, f32x3 point) {
    f32x4 p = frustum.planes[plane];

    return nya_vector_dot((f32x3){ p[0], p[1], p[2] }, point) + p[3];
}

/** One bit per plane the sphere lies wholly outside of. */
static u32 planes_rejecting(f32x3 center, f32 radius) {
    u32 mask = 0;
    for (u32 plane = 0; plane < 6; plane++) {
        if (plane_distance(plane, center) < -radius) mask |= 1U << plane;
    }

    return mask;
}

s32 main(void) {
    // ── Every plane of a perspective frustum is unit length and finite, so distances are in world units.
    {
        frustum_from(perspective_matrix());

        for (u32 plane = 0; plane < 6; plane++) {
            f32x4 p      = frustum.planes[plane];
            f32   length = nya_vector_length((f32x3){ p[0], p[1], p[2] });

            nya_check(fabsf(length - 1.0F) < 1e-4F, "plane %u has length %f", plane, (f64)length);
            nya_check(isfinite(p[3]), "plane %u has a finite offset", plane);
        }

        // near and far sit where the projection put them.
        nya_check(fabsf(plane_distance(PLANE_NEAR, (f32x3){ 0, 0, -NEAR_PLANE })) < 1e-4F, "the near plane is at z = -near");
        nya_check(fabsf(plane_distance(PLANE_FAR, (f32x3){ 0, 0, -FAR_PLANE })) < 1e-2F, "the far plane is at z = -far");
    }

    // ── Inside, and outside each plane alone.
    {
        frustum_from(perspective_matrix());

        nya_check(_nya_render3d_visible(&frustum, (f32x3){ 0, 0, -10 }, 1.0F), "a sphere straight ahead is visible");
        nya_check(planes_rejecting((f32x3){ 0, 0, -10 }, 1.0F) == 0, "and no plane rejects it");

        // at z = -10 the view is about 10.3 wide either side and 5.8 tall.
        struct {
            f32x3            center;
            f32              radius;
            u32              plane;
            NYA_ConstCString name;
        } outside[] = {
            { { -20.0F, 0.0F, -10.0F }, 1.0F, PLANE_LEFT, "left" },
            { { 20.0F, 0.0F, -10.0F }, 1.0F, PLANE_RIGHT, "right" },
            { { 0.0F, -12.0F, -10.0F }, 1.0F, PLANE_BOTTOM, "bottom" },
            { { 0.0F, 12.0F, -10.0F }, 1.0F, PLANE_TOP, "top" },
            { { 0.0F, 0.0F, -0.05F }, 0.01F, PLANE_NEAR, "near" },
            { { 0.0F, 0.0F, -150.0F }, 1.0F, PLANE_FAR, "far" },
        };

        for (u32 i = 0; i < nya_carray_length(outside); i++) {
            u32 rejecting = planes_rejecting(outside[i].center, outside[i].radius);

            nya_check(!_nya_render3d_visible(&frustum, outside[i].center, outside[i].radius), "a sphere beyond %s is culled", outside[i].name);
            nya_check(rejecting == 1U << outside[i].plane, "and only %s rejects it, got mask 0x%x", outside[i].name, rejecting);
        }
    }

    // ── A sphere whose centre is outside but whose edge reaches in is kept; the test is `distance < -radius`.
    {
        frustum_from(perspective_matrix());

        for (u32 plane = 0; plane < 6; plane++) {
            f32x3 inside   = { 0, 0, -10 };
            f32x4 p        = frustum.planes[plane];
            f32x3 normal   = { p[0], p[1], p[2] };
            f32x3 straddle = inside - (normal * (plane_distance(plane, inside) + 0.5F));

            nya_check(_nya_render3d_visible(&frustum, straddle, 1.0F), "plane %u: half a unit out with radius 1 is visible", plane);
            nya_check(!_nya_render3d_visible(&frustum, straddle, 0.25F), "plane %u: with radius 0.25 it is culled", plane);
        }

        // a sphere around the camera reaches every plane.
        nya_check(_nya_render3d_visible(&frustum, (f32x3){ 0, 0, 50 }, 200.0F), "a huge sphere behind the camera still overlaps the view");
    }

    // ── Conservative at a corner: outside the view, but not wholly outside any one plane, so kept.
    {
        frustum_from(perspective_matrix());

        const f32 beyond = 0.9F;

        f32x3 inside = { 0, 0, -10 };
        f32x3 corner = inside;

        // the two normals are not orthogonal, so pushing out along one moves the other; a few rounds settle both.
        const u32 planes[] = { PLANE_LEFT, PLANE_TOP };
        for (u32 round = 0; round < 16; round++) {
            for (u32 i = 0; i < nya_carray_length(planes); i++) {
                f32x4 p = frustum.planes[planes[i]];
                corner  = corner - ((f32x3){ p[0], p[1], p[2] } * (plane_distance(planes[i], corner) + beyond));
            }
        }

        nya_check(fabsf(plane_distance(PLANE_LEFT, corner) + beyond) < 1e-3F && fabsf(plane_distance(PLANE_TOP, corner) + beyond) < 1e-3F,
                  "the centre is equally far outside both planes");

        // from equal distances outside two planes, the edge they meet at is sqrt(2 / (1 + cos)) times further.
        f32x4 left       = frustum.planes[PLANE_LEFT];
        f32x4 top        = frustum.planes[PLANE_TOP];
        f32   cosine     = nya_vector_dot((f32x3){ left[0], left[1], left[2] }, (f32x3){ top[0], top[1], top[2] });
        f32   to_frustum = beyond * sqrtf(2.0F / (1.0F + cosine));
        nya_check(to_frustum > 1.0F, "so a unit sphere there misses the view, %f away", (f64)to_frustum);

        nya_check(_nya_render3d_visible(&frustum, corner, 1.0F), "and kept, which is the safe direction to be wrong in");
    }

    // ── Orthographic: the near plane is row 2 alone, and the sides do not converge.
    {
        f32_4x4 projection = nya_matrix_orthographic_3d(10.0F, 1.0F, 1.0F, 50.0F);
        f32_4x4 view       = nya_matrix_look_at((f32x3){ 0, 0, 0 }, (f32x3){ 0, 0, -1 }, (f32x3){ 0, 1, 0 });
        frustum_from(projection * view);

        nya_check(_nya_render3d_visible(&frustum, (f32x3){ 4.0F, 4.0F, -10.0F }, 0.5F), "inside the box is visible");
        nya_check(planes_rejecting((f32x3){ 0.0F, 6.0F, -40.0F }, 0.5F) == 1U << PLANE_TOP, "above the box, however far, only top rejects");
        nya_check(planes_rejecting((f32x3){ 0.0F, 0.0F, -0.5F }, 0.1F) == 1U << PLANE_NEAR, "in front of the near plane only near rejects");
        nya_check(planes_rejecting((f32x3){ 0.0F, 0.0F, -60.0F }, 1.0F) == 1U << PLANE_FAR, "past the far plane only far rejects");
        nya_check(fabsf(plane_distance(PLANE_NEAR, (f32x3){ 0, 0, -1 })) < 1e-4F, "the near plane is at z = -1");
    }

    // ── A degenerate matrix leaves its planes alone rather than dividing by zero, and culls nothing.
    {
        frustum_from(f32_4x4_zero);

        for (u32 plane = 0; plane < 6; plane++) {
            f32x4 p = frustum.planes[plane];
            nya_check(p[0] == 0.0F && p[1] == 0.0F && p[2] == 0.0F && p[3] == 0.0F, "plane %u stays zero", plane);
        }

        nya_check(_nya_render3d_visible(&frustum, (f32x3){ 1e6F, -1e6F, 1e6F }, 0.0F), "anything is visible");
    }

    // ── A draw keeps one bit per pass that sees it: the camera's first, a cascade's after it.
    {
        nya_memset(batch, 0, sizeof(*batch));

        _nya_render3d_frustum_build(&batch->passes[0], perspective_matrix());
        _nya_render3d_frustum_build(&batch->passes[1], orthographic_matrix());
        batch->pass_count = 2;

        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, -10 }, 1.0F) == 0x3, "ahead of the camera and inside the cascade");
        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, 10 }, 1.0F) == 0x2, "behind the camera only the cascade sees it");
        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 70, 0, -90 }, 1.0F) == 0x1, "beside the cascade only the camera does");
        nya_check(batch->frame_culled == 0, "nothing seen by a pass is culled, got %u", batch->frame_culled);

        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 500, 10 }, 1.0F) == 0, "outside both, no pass sees it");
        nya_check(batch->frame_culled == 1, "and that is counted, got %u", batch->frame_culled);
    }

    // ── The occlusion buffer is the camera's alone, and asked only about what survived its frustum.
    {
        nya_memset(batch, 0, sizeof(*batch));

        _nya_render3d_frustum_build(&batch->passes[0], perspective_matrix());
        batch->pass_count = 1;

        nya_occlusion_begin(&occlusion, perspective_matrix());
        (void)nya_occlusion_quad(&occlusion, (f32x3){ -40, -40, -10 }, (f32x3){ 40, -40, -10 }, (f32x3){ 40, 40, -10 }, (f32x3){ -40, 40, -10 });

        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, -20 }, 1.0F) == 0x1, "without a buffer set, behind the wall is visible");
        nya_check(batch->frame_occluded == 0, "and nothing counts as occluded");

        batch->occlusion = &occlusion;

        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, -20 }, 1.0F) == 0, "with it set, behind the wall is hidden");
        nya_check(batch->frame_occluded == 1, "and counted, got %u", batch->frame_occluded);

        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, -5 }, 1.0F) == 0x1, "in front of the wall is visible");

        u32 tests_before = nya_occlusion_stats(&occlusion).tests;
        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ -100, 0, -20 }, 1.0F) == 0, "outside the frustum and behind the wall is culled");
        nya_check(nya_occlusion_stats(&occlusion).tests == tests_before, "by the frustum, without asking the buffer");
        nya_check(batch->frame_occluded == 1, "so it is not counted as occluded");

        _nya_render3d_frustum_build(&batch->passes[1], orthographic_matrix());
        batch->pass_count = 2;

        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, -20 }, 1.0F) == 0x2, "a cascade still sees a caster the camera's buffer hides");
        nya_check(batch->frame_occluded == 2, "which counts as occluded, got %u", batch->frame_occluded);
    }

    /* ── Both culls are switchable, and switching the frustum off takes the occlusion buffer with it: the buffer only ever removes what the frustum kept, so there is nothing for it to answer about. */
    {
        nya_memset(batch, 0, sizeof(*batch));

        _nya_render3d_frustum_build(&batch->passes[0], perspective_matrix());
        _nya_render3d_frustum_build(&batch->passes[1], orthographic_matrix());
        batch->pass_count = 2;
        batch->occlusion  = &occlusion;

        f32x3 outside = { 0, 500, 10 };

        nya_check(_nya_render3d_passes_seeing(&window, outside, 1.0F) == 0, "with culling on, nothing sees a point outside both");

        nya_render_features_set(&window, (NYA_RenderFeatures){ .occlusion_culling = NYA_RENDER_TOGGLE_OFF });

        u32 tests_before = nya_occlusion_stats(&occlusion).tests;
        nya_check(_nya_render3d_passes_seeing(&window, (f32x3){ 0, 0, -20 }, 1.0F) == 0x3, "with the buffer off, what it hid is visible");
        nya_check(nya_occlusion_stats(&occlusion).tests == tests_before, "and the buffer is not asked at all");

        nya_render_features_set(&window, (NYA_RenderFeatures){ .frustum_culling = NYA_RENDER_TOGGLE_OFF });

        u32 culled_before = batch->frame_culled;
        nya_check(_nya_render3d_passes_seeing(&window, outside, 1.0F) == 0x3, "with the frustum off, every pass sees everything");
        nya_check(batch->frame_culled == culled_before, "and nothing is counted as culled");

        // back on, so the rest of the file is not affected by where this case left the switches.
        nya_render_features_set(&window, (NYA_RenderFeatures){ 0 });
        nya_check(_nya_render3d_passes_seeing(&window, outside, 1.0F) == 0, "and switching it back on culls again");
    }

    // ── Each pass's list holds the indices of exactly the objects it sees, in recorded order.
    {
        u16                indices[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        NYA_Render3DObject objects[3]  = {
            { .first_index = 0, .passes = 0x1 },
            { .first_index = 3, .passes = 0x3 },
            { .first_index = 9, .passes = 0x2 },
        };

        NYA_Render3DStream stream = { .indices = indices, .index_count = 12, .objects = objects, .object_count = 3 };

        u16 out[12] = { 0 };

        u32 camera = _nya_render3d_pass_indices(&stream, 0, 3, 0, out);
        nya_check(camera == 9 && out[0] == 0 && out[3] == 3 && out[8] == 8, "the camera's list is the first two objects, got %u", camera);

        u32 cascade = _nya_render3d_pass_indices(&stream, 0, 3, 1, out);
        nya_check(cascade == 9 && out[0] == 3 && out[5] == 8 && out[6] == 9 && out[8] == 11, "the cascade's is the last two, got %u", cascade);

        nya_check(_nya_render3d_pass_indices(&stream, 1, 2, 0, out) == 6, "a segment's objects end where the next begins");
        nya_check(_nya_render3d_pass_indices(&stream, 0, 3, 2, out) == 0, "a pass no object is in gets nothing");
        nya_check(_nya_render3d_pass_indices(&stream, 3, 3, 0, out) == 0, "and an empty segment nothing");
    }

    // ── Instances draw in runs of neighbours the pass sees.
    {
        const u8 passes[] = { 0x1, 0x3, 0x3, 0x2, 0x1, 0x3 };

        u32 first = 99;

        nya_check(_nya_render3d_pass_run(passes, 0, 6, 0, &first) == 3 && first == 0, "the camera's first run is three from zero");
        nya_check(_nya_render3d_pass_run(passes, 3, 6, 0, &first) == 2 && first == 4, "and its next skips the copy it cannot see");
        nya_check(_nya_render3d_pass_run(passes, 0, 6, 1, &first) == 3 && first == 1, "the cascade's starts at one");
        nya_check(_nya_render3d_pass_run(passes, 4, 6, 1, &first) == 1 && first == 5, "a run stops at the end it is given");
        nya_check(_nya_render3d_pass_run(passes, 0, 6, 2, &first) == 0 && first == 6, "no run leaves the start at the end");
    }

    /* ── Which stream a draw is recorded into. Alpha decides it, except under addition. A flame particle is born at exactly alpha one. Recorded as opaque it drew through the opaque pipeline and wrote depth, so every new particle punched a hole in the plume behind it for a tick: the fire flickered. */
    {
        NYA_Color solid     = { 1.0F, 1.0F, 1.0F, 1.0F };
        NYA_Color faded     = { 1.0F, 1.0F, 1.0F, 0.5F };
        NYA_Color emissive  = { 1.15F, 0.52F, 0.14F, 1.0F };

        nya_check(!_nya_render3d_stream_transparent(solid, NYA_RENDER3D_BLEND_ALPHA), "a solid alpha-blended draw is opaque");
        nya_check(_nya_render3d_stream_transparent(faded, NYA_RENDER3D_BLEND_ALPHA), "a faded one is not");

        nya_check(_nya_render3d_stream_transparent(solid, NYA_RENDER3D_BLEND_ADDITIVE), "a solid additive draw must not be opaque");
        nya_check(_nya_render3d_stream_transparent(emissive, NYA_RENDER3D_BLEND_ADDITIVE), "nor an emissive one past white");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
