/**
 * Where a cascaded shadow volume goes: the fit, the split, and the texel snap.
 *
 * Testable at all because the placement is pure math and lives in render_shadow.c rather than inside
 * the pass — the same separation render_camera.c exists for. Nothing here rasterises anything.
 *
 * The snap is the interesting half. Its whole purpose is that the volume moves in *discrete* steps
 * while the camera moves continuously, so what is asserted is quantisation — a hundred camera
 * positions spanning one texel give at most two volume positions — and that every position it does
 * take sits on the grid. Not "a small move does nothing": a small move can cross a boundary, which
 * is what the first version of this file asserted and failed on.
 *
 * Headless: this is arithmetic over a camera and a light direction.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A camera at `x` looking down positive z, which keeps the forward axis easy to reason about. */
static NYA_Camera3DPerspective camera_at(f32 x) {
    return (NYA_Camera3DPerspective){
        .position = { x, 10.0F, 0.0F },
        .target   = { x, 10.0F, 1.0F },
        .up       = { 0.0F, 1.0F, 0.0F },
        .fov_y    = 1.05F,
    };
}

/** The sun used throughout: straight down and a little to the side, the outdoor case. */
#define SUN ((f32x3){ -0.4F, -1.0F, -0.6F })

s32 main(void) {
    // ── Cascade extents grow geometrically from the nearest.
    {
        f32 near = 8.0F;

        nya_check(nya_render3d_cascade_extent(near, 0) == near, "cascade zero is the extent given, got %f",
                  (f64)nya_render3d_cascade_extent(near, 0));

        f32 one = nya_render3d_cascade_extent(near, 1);
        f32 two = nya_render3d_cascade_extent(near, 2);

        nya_check(fabsf(one - (near * NYA_RENDER3D_SHADOW_CASCADE_RATIO)) < 0.001F, "cascade one is one ratio out, got %f", (f64)one);
        nya_check(fabsf(two - (one * NYA_RENDER3D_SHADOW_CASCADE_RATIO)) < 0.001F, "and each is a ratio past the last, got %f", (f64)two);

        // Geometric rather than linear is the whole point: the far cascade has to cover far more
        // ground than the near one, or it gives the distance the same detail as arm's length.
        nya_check(two > one && one > near, "each cascade must be wider than the one before it");

        // Zero means the compiled-in default, which is what a caller leaving the field out gets.
        nya_check(nya_render3d_cascade_extent(0.0F, 0) == NYA_RENDER3D_SHADOW_EXTENT, "zero is the default extent");
    }

    // ── The light basis is orthonormal, including for a light pointing straight down.
    {
        f32x3 directions[] = {
            SUN,
            { 0.0F, -1.0F, 0.0F },    // straight down, which is what makes the naive up vector degenerate
            { 0.0F, 1.0F, 0.0F },     // and straight up
            { 1.0F, 0.0F, 0.0F },
        };

        for (u32 i = 0; i < nya_carray_length(directions); i++) {
            f32x3 forward, right, up;
            nya_render3d_light_basis(directions[i], &forward, &right, &up);

            nya_check(fabsf(nya_vector_length(forward) - 1.0F) < 0.001F, "forward should be unit for direction " FMTu32, i);
            nya_check(fabsf(nya_vector_length(right) - 1.0F) < 0.001F, "right should be unit for direction " FMTu32, i);
            nya_check(fabsf(nya_vector_length(up) - 1.0F) < 0.001F, "up should be unit for direction " FMTu32, i);

            nya_check(fabsf(nya_vector_dot(forward, right)) < 0.001F, "forward and right should be perpendicular, direction " FMTu32, i);
            nya_check(fabsf(nya_vector_dot(forward, up)) < 0.001F, "forward and up likewise, direction " FMTu32, i);
            nya_check(fabsf(nya_vector_dot(right, up)) < 0.001F, "and right and up, direction " FMTu32, i);
        }
    }

    /*
     * ── The volume is pushed ahead of the camera, not centred on it.
     *
     * Centring on the viewer spends half the map on what is behind them. The push is by exactly the
     * cascade's half-width, so the box runs from the camera to twice the extent ahead.
     */
    {
        NYA_Camera3DPerspective camera = camera_at(0.0F);

        NYA_Render3DShadow near = nya_render3d_shadow_for_camera(camera, SUN, 0,
                                                                 (NYA_Render3DShadowFit){ .near_extent = 8.0F, .strength = 0.45F,
                                                                                          .no_texel_snap = true });

        // Forward is +z, so the push shows up there.
        nya_check(near.center.z > 0.0F, "the volume should be pushed ahead of the camera, got z=%f", (f64)near.center.z);
        nya_check(fabsf(near.center.z - 8.0F) < 0.001F, "by exactly the cascade's half-width, got %f", (f64)near.center.z);

        // A wider cascade is pushed further, because its box is bigger.
        NYA_Render3DShadow far = nya_render3d_shadow_for_camera(camera, SUN, 2,
                                                                (NYA_Render3DShadowFit){ .near_extent = 8.0F, .strength = 0.45F,
                                                                                         .no_texel_snap = true });

        nya_check(far.center.z > near.center.z, "a wider cascade is pushed further, got %f against %f", (f64)far.center.z,
                  (f64)near.center.z);

        nya_check(fabsf(far.center.z - nya_render3d_cascade_extent(8.0F, 2)) < 0.001F, "by its own half-width, got %f",
                  (f64)far.center.z);
    }

    // ── The returned shadow carries the *nearest* extent, not the cascade's own.
    {
        NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(camera_at(0.0F), SUN, 2,
                                                                   (NYA_Render3DShadowFit){ .near_extent = 8.0F, .strength = 0.5F });

        // nya_render3d_shadow_begin applies the ratio itself, so handing it the already-widened extent
        // would apply it twice and make the far cascade six times too large.
        nya_check(shadow.extent == 8.0F, "the fit must hand back the nearest extent, got %f", (f64)shadow.extent);
        nya_check(shadow.cascade == 2, "and the cascade it was asked for, got " FMTu32, shadow.cascade);
        nya_check(shadow.strength == 0.5F, "passing the strength through");
    }

    /*
     * ── The snap quantises the volume: many camera positions, few volume positions.
     *
     * This is the property the whole thing exists for, and it has to be stated as quantisation rather
     * than as "a small move does nothing" — a small move *can* cross a grid boundary, and the first
     * version of this test asserted the stronger thing and failed on exactly that. What is true is
     * that a hundred camera positions spanning one texel produce at most two volume positions,
     * against a hundred unsnapped.
     */
    {
        f32 extent = 8.0F;
        f32 texel  = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

        NYA_Render3DShadowFit fit       = { .near_extent = extent, .strength = 0.45F };
        NYA_Render3DShadowFit unsnapped = { .near_extent = extent, .strength = 0.45F, .no_texel_snap = true };

        enum { SAMPLES = 100 };

        u32   snapped_positions = 0;
        u32   raw_positions     = 0;
        f32x3 last_snapped      = { 0 };
        f32x3 last_raw          = { 0 };

        for (u32 i = 0; i < SAMPLES; i++) {
            // Across exactly one texel, so at most one boundary can be crossed.
            f32 offset = ((f32)i / (f32)SAMPLES) * texel;

            NYA_Render3DShadow snapped = nya_render3d_shadow_for_camera(camera_at(offset), SUN, 0, fit);
            NYA_Render3DShadow raw     = nya_render3d_shadow_for_camera(camera_at(offset), SUN, 0, unsnapped);

            f32x3 snapped_delta = snapped.center - last_snapped;
            f32x3 raw_delta     = raw.center - last_raw;

            if (i == 0 || nya_vector_length(snapped_delta) > 0.0001F) snapped_positions++;
            if (i == 0 || nya_vector_length(raw_delta) > 0.0F) raw_positions++;

            last_snapped = snapped.center;
            last_raw     = raw.center;
        }

        nya_check(snapped_positions <= 2, "one texel of camera travel should give at most two volume positions, got " FMTu32,
                  snapped_positions);

        // Which only means anything if the unsnapped fit really does follow the camera continuously —
        // otherwise the assertion above would also pass for a fit that ignored it.
        nya_check(raw_positions > SAMPLES / 2, "unsnapped, the volume should follow the camera continuously, got " FMTu32 " positions",
                  raw_positions);
    }

    /*
     * ── Every position the volume takes lies on the texel grid.
     *
     * Quantisation alone would be satisfied by a volume that never moves. Walking the camera across
     * several texels checks the other half: it moves, and every place it stops is on the grid.
     */
    {
        f32 extent = 8.0F;
        f32 texel  = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

        f32x3 forward, right, up;
        nya_render3d_light_basis(SUN, &forward, &right, &up);

        NYA_Render3DShadowFit fit = { .near_extent = extent, .strength = 0.45F };

        u32 distinct = 0;
        f32 previous = 0.0F;

        for (u32 step = 0; step < 64; step++) {
            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(camera_at((f32)step * texel * 0.5F), SUN, 0, fit);

            // On the grid means: the centre's coordinate along each of the light's lateral axes is a
            // whole number of texels.
            f32 along_right = nya_vector_dot(shadow.center, right) / texel;
            f32 along_up    = nya_vector_dot(shadow.center, up) / texel;

            nya_check(fabsf(along_right - roundf(along_right)) < 0.01F, "step " FMTu32 " is off the grid along right by %f", step,
                      (f64)fabsf(along_right - roundf(along_right)));
            nya_check(fabsf(along_up - roundf(along_up)) < 0.01F, "step " FMTu32 " is off the grid along up by %f", step,
                      (f64)fabsf(along_up - roundf(along_up)));

            if (step == 0 || fabsf(along_right - previous) > 0.5F) distinct++;
            previous = along_right;
        }

        nya_check(distinct > 1, "walking the camera across texels should move the volume, got " FMTu32 " distinct positions", distinct);
    }

    // ── The degenerate cases.
    {
        // A camera aimed at itself names no direction. The volume sits on it, which is wrong but
        // bounded — the point is that it is not NaN.
        NYA_Camera3DPerspective still = { .position = { 3.0F, 4.0F, 5.0F }, .target = { 3.0F, 4.0F, 5.0F } };

        NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(still, SUN, 0, (NYA_Render3DShadowFit){ .strength = 0.4F });

        nya_check(!isnan(shadow.center.x) && !isnan(shadow.center.y) && !isnan(shadow.center.z),
                  "a camera aimed at itself must not produce NaN, got (%f, %f, %f)", (f64)shadow.center.x, (f64)shadow.center.y,
                  (f64)shadow.center.z);

        // A zero light direction is read as the default sun rather than dividing by zero.
        NYA_Render3DShadow defaulted =
            nya_render3d_shadow_for_camera(camera_at(0.0F), f32x3_zero, 0, (NYA_Render3DShadowFit){ .strength = 0.4F });

        nya_check(!isnan(defaulted.center.x), "and neither must a light with no direction");

        // Past the compiled-in cascade count, clamped rather than reading off the end of the arrays.
        NYA_Render3DShadow clamped =
            nya_render3d_shadow_for_camera(camera_at(0.0F), SUN, 99, (NYA_Render3DShadowFit){ .strength = 0.4F });

        nya_check(clamped.cascade == NYA_RENDER3D_SHADOW_CASCADES - 1, "a cascade past the last is clamped, got " FMTu32,
                  clamped.cascade);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
