/**
 * Software occlusion culling: what the depth buffer hides, and everything it refuses to.
 *
 * Most of these assert the *negative*. The whole design of render_occlusion.h is that every
 * approximation errs toward "visible", so the interesting cases are the ones where it could plausibly
 * hide something and must not: a sphere poking out from behind the wall, an occluder straddling the
 * near plane, a query running off the edge of the buffer.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A camera at the origin looking down -z, which is the frame every coordinate below is written in. */
static f32_4x4 camera_matrix(void) {
    f32_4x4 projection = nya_matrix_perspective((f32)M_PI / 3.0F, 16.0F / 9.0F, 0.1F, 1000.0F);
    f32_4x4 view       = nya_matrix_look_at((f32x3){ 0, 0, 0 }, (f32x3){ 0, 0, -1 }, (f32x3){ 0, 1, 0 });

    return projection * view;
}

/** A wall spanning `half` in x and y, at `z`. Big enough at z = -10 to fill the view. */
static void wall(NYA_OcclusionBuffer* buffer, f32 z, f32 half) {
    (void)nya_occlusion_quad(buffer, (f32x3){ -half, -half, z }, (f32x3){ half, -half, z }, (f32x3){ half, half, z },
                             (f32x3){ -half, half, z });
}

static NYA_OcclusionBuffer buffer;

s32 main(void) {
    // ── A buffer that was never begun hides nothing, whatever is asked of it.
    {
        buffer = (NYA_OcclusionBuffer){ 0 };

        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "an unbegun buffer culls nothing");
        nya_check(!nya_occlusion_quad(&buffer, (f32x3){ 0, 0, 0 }, (f32x3){ 1, 0, 0 }, (f32x3){ 1, 1, 0 }, (f32x3){ 0, 1, 0 }),
                  "and accepts no occluders");
        nya_check(!nya_occlusion_test(nullptr, (f32x3){ 0, 0, -20 }, 1.0F), "a null buffer is not a crash");
    }

    // ── The base case: a wall at -10, a sphere behind it, a sphere in front of it.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        wall(&buffer, -10.0F, 40.0F);

        nya_check(nya_occlusion_stats(&buffer).occluders_rasterized == 1, "the wall rasterised, got %u",
                  nya_occlusion_stats(&buffer).occluders_rasterized);

        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "a sphere behind the wall is hidden");
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -5 }, 1.0F), "a sphere in front of it is not");

        NYA_OcclusionStats stats = nya_occlusion_stats(&buffer);
        nya_check(stats.tests == 2 && stats.occluded == 1, "and the counters say so: %u tests, %u hidden", stats.tests, stats.occluded);
    }

    // ── Touching the wall from behind is still hidden; touching it from in front is not.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        wall(&buffer, -10.0F, 40.0F);

        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -11.5F }, 1.0F), "a sphere just behind the wall is hidden");
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -10.0F }, 1.0F), "one straddling it is not");
    }

    // ── A wall that only covers part of the view leaves the rest alone.
    {
        nya_occlusion_begin(&buffer, camera_matrix());

        // Two units across at ten units away, well inside the field of view.
        wall(&buffer, -10.0F, 1.0F);

        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 0.2F), "a small sphere behind the small wall is hidden");
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 6, 0, -20 }, 0.2F), "one beside it is not");

        // The conservative case, and the one worth having a test for: the sphere is mostly behind the
        // wall, and the part that is not is what has to keep it visible.
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 4.0F), "a sphere poking out from behind it is not hidden");
    }

    // ── An occluder the camera is inside, or behind, writes nothing rather than garbage.
    {
        nya_occlusion_begin(&buffer, camera_matrix());

        nya_check(!nya_occlusion_quad(&buffer, (f32x3){ -5, -5, 5 }, (f32x3){ 5, -5, 5 }, (f32x3){ 5, 5, 5 }, (f32x3){ -5, 5, 5 }),
                  "a wall behind the camera writes nothing");

        // Straddling the near plane: two corners in front, two behind.
        nya_check(!nya_occlusion_quad(&buffer, (f32x3){ -5, -5, 5 }, (f32x3){ 5, -5, 5 }, (f32x3){ 5, 5, -5 }, (f32x3){ -5, 5, -5 }),
                  "and so does one straddling the near plane");

        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "so nothing behind them is hidden");
    }

    // ── A degenerate occluder is not a divide by zero.
    {
        nya_occlusion_begin(&buffer, camera_matrix());

        nya_check(!nya_occlusion_quad(&buffer, (f32x3){ 0, 0, -10 }, (f32x3){ 0, 0, -10 }, (f32x3){ 0, 0, -10 }, (f32x3){ 0, 0, -10 }),
                  "a quad with no area writes nothing");

        // Too small to fully cover a single pixel of a 160×90 buffer at ten units.
        nya_check(!nya_occlusion_quad(&buffer, (f32x3){ -0.001F, -0.001F, -10 }, (f32x3){ 0.001F, -0.001F, -10 }, (f32x3){ 0.001F, 0.001F, -10 },
                                      (f32x3){ -0.001F, 0.001F, -10 }),
                  "and neither does one smaller than a pixel");
    }

    // ── Either winding works, because a caller does not know which side of a wall it ended up on.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        (void)nya_occlusion_quad(&buffer, (f32x3){ -40, 40, -10 }, (f32x3){ 40, 40, -10 }, (f32x3){ 40, -40, -10 }, (f32x3){ -40, -40, -10 });

        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "a reversed winding occludes just the same");
    }

    // ── The nearest claim wins when two occluders overlap.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        wall(&buffer, -30.0F, 100.0F);
        wall(&buffer, -10.0F, 40.0F);

        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "something between the two walls is hidden by the nearer one");
    }

    // ── A box submits the faces pointing at the eye, and hides what is behind it.
    {
        nya_occlusion_begin(&buffer, camera_matrix());

        u32 faces = nya_occlusion_box(&buffer, (f32x3){ 0, 0, -10 }, (f32x3){ 4, 4, 1 }, (f32x3){ 0, 0, 0 });

        nya_check(faces == 1, "an eye on the box's axis sees exactly one of its faces, got %u", faces);
        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "and what is behind the box is hidden");
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 8, 0, -20 }, 1.0F), "what is beside it is not");

        nya_occlusion_begin(&buffer, camera_matrix());
        faces = nya_occlusion_box(&buffer, (f32x3){ 3, 3, -10 }, (f32x3){ 2, 2, 2 }, (f32x3){ 0, 0, 0 });
        nya_check(faces == 3, "an eye off all three axes sees three faces, got %u", faces);
    }

    // ── A query whose rectangle leaves the buffer is answered visible, not hidden.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        wall(&buffer, -10.0F, 1000.0F);

        // Close enough that its bounding box projects past the edge of the buffer, and behind the wall.
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -12 }, 40.0F), "a query running off the buffer is not hidden");
    }

    // ── A query too large to be worth scanning gives up rather than spending the time.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        wall(&buffer, -5.0F, 1000.0F);

        // Fully on screen, behind the wall, and about 41×41 pixels — over the tenth of the buffer
        // NYA_OCCLUSION_MAX_QUERY_PIXELS allows.
        (void)nya_occlusion_test(&buffer, (f32x3){ 0, 0, -12 }, 2.5F);

        nya_check(nya_occlusion_stats(&buffer).abandoned == 1, "the oversized query was abandoned, got %u",
                  nya_occlusion_stats(&buffer).abandoned);
    }

    // ── Beginning again forgets everything, camera included.
    {
        nya_occlusion_begin(&buffer, camera_matrix());
        wall(&buffer, -10.0F, 40.0F);
        nya_check(nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "hidden before the reset");

        nya_occlusion_begin(&buffer, camera_matrix());
        nya_check(!nya_occlusion_test(&buffer, (f32x3){ 0, 0, -20 }, 1.0F), "and visible after it");

        NYA_OcclusionStats stats = nya_occlusion_stats(&buffer);
        nya_check(stats.occluders == 0 && stats.occluded == 0, "with the counters cleared too");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
