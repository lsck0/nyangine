/**
 * Where a cascaded shadow volume goes: the frustum fit, the split, and the texel snap.
 *
 * Testable at all because the placement is pure math and lives in render_shadow.c rather than inside
 * the pass — the same separation render_camera.c exists for. Nothing here rasterises anything.
 *
 * ## What broke, and what these assert
 *
 * The fit used to size each cascade from a constant and put it a fixed distance in front of the
 * camera. That is only right when the camera is close to what it is looking at: an orbit camera well
 * outside the near cascade's reach spent that cascade on empty air, so the whole scene was shadowed by
 * the coarsest map, and moving the camera moved patches of ground between cascades of very different
 * resolution. On screen that read as shadows growing, shrinking and changing quality with nothing in
 * the scene having moved.
 *
 * A cascade is now fitted to its slice of the camera's own frustum, so the properties worth asserting
 * are the ones that failed before: that the cascades tile the view rather than nest, that the near one
 * covers what is near *whatever the camera's distance from its subject*, and that the fit depends on
 * the camera's shape rather than its position or heading.
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

/** A camera `distance` back from the origin along negative z, aimed at it. The orbit case. */
static NYA_Camera3DPerspective camera_looking_at_origin(f32 distance) {
    return (NYA_Camera3DPerspective){
        .position = { 0.0F, 0.0F, -distance },
        .target   = { 0.0F, 0.0F, 0.0F },
        .up       = { 0.0F, 1.0F, 0.0F },
        .fov_y    = 1.05F,
    };
}

/** The sun used throughout: straight down and a little to the side, the outdoor case. */
#define SUN ((f32x3){ -0.4F, -1.0F, -0.6F })

#define RANGE 64.0F

/** Whether `point` is inside the volume `shadow` names, by the same rule the shader selects a cascade by. */
static b8 covers(NYA_Render3DShadow shadow, f32x3 point) {
    f32 depth = shadow.depth > 0.0F ? shadow.depth : shadow.extent * 4.0F;

    f32_4x4 light = nya_render3d_shadow_view_projection(shadow.center, SUN, shadow.extent, depth, nullptr);

    f32x4 clip = nya_matrix_times_vector(light, (f32x4){ point.x, point.y, point.z, 1.0F });

    f32x3 projected = { clip.x / clip.w, clip.y / clip.w, clip.z / clip.w };

    return fabsf(projected.x) <= 1.0F && fabsf(projected.y) <= 1.0F && projected.z >= 0.0F && projected.z <= 1.0F;
}

s32 main(void) {
    NYA_Render3DShadowFit fit = { .range = RANGE, .strength = 0.45F, .aspect = 16.0F / 9.0F };

    NYA_Render3DShadowFit unsnapped = { .range = RANGE, .strength = 0.45F, .aspect = 16.0F / 9.0F, .no_texel_snap = true };

    /*
     * ── The cascades tile the view, and each is wider than the last.
     *
     * Nesting is what the old fit did — every cascade was a box starting at the camera, so the near
     * ones were entirely inside the far ones and covered nothing the far ones did not. Tiling is the
     * point of having cascades at all: each takes a slice of the view and spends its whole resolution
     * on that slice.
     */
    {
        NYA_Camera3DPerspective camera = camera_at(0.0F);

        f32 previous_extent   = 0.0F;
        f32 previous_distance = 0.0F;

        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(camera, SUN, cascade, unsnapped);

            // Forward is +z, so how far down the view this cascade sits reads straight off z.
            f32 distance = shadow.center.z - camera.position.z;

            nya_check(shadow.extent > 0.0F, "cascade " FMTu32 " must have a positive extent, got %f", cascade, (f64)shadow.extent);

            if (cascade > 0) {
                nya_check(shadow.extent > previous_extent, "cascade " FMTu32 " should be wider than the one before it, %f against %f",
                          cascade, (f64)shadow.extent, (f64)previous_extent);

                nya_check(distance > previous_distance, "and sit further down the view, %f against %f", (f64)distance,
                          (f64)previous_distance);
            }

            previous_extent   = shadow.extent;
            previous_distance = distance;
        }
    }

    /*
     * ── ⭐ The fit does not depend on how far the camera is from what it is looking at.
     *
     * This is the regression test for the whole bug. The old fit derived nothing from the frustum, so
     * a camera that orbited outward left its near cascades behind in empty space and the scene fell
     * through to the coarsest map — which is why the shadows changed as the camera moved. Two cameras
     * with the same shape pointed the same way must produce the same cascade sizes whatever they are
     * looking at and from how far.
     */
    {
        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            NYA_Render3DShadow close = nya_render3d_shadow_for_camera(camera_looking_at_origin(4.0F), SUN, cascade, unsnapped);
            NYA_Render3DShadow far   = nya_render3d_shadow_for_camera(camera_looking_at_origin(40.0F), SUN, cascade, unsnapped);

            nya_check(fabsf(close.extent - far.extent) < 0.001F,
                      "cascade " FMTu32 " must be the same size from four units away as from forty, got %f against %f", cascade,
                      (f64)close.extent, (f64)far.extent);

            // And it must sit the same distance down the view, so it covers the same slice of what the
            // camera can see rather than the same patch of world.
            f32 close_distance = close.center.z - (-4.0F);
            f32 far_distance   = far.center.z - (-40.0F);

            nya_check(fabsf(close_distance - far_distance) < 0.001F, "and the same distance down the view, got %f against %f",
                      (f64)close_distance, (f64)far_distance);
        }
    }

    /*
     * ── The near cascade covers what is near the camera, at any distance from the subject.
     *
     * The other half of the same claim, stated as coverage rather than as numbers: a point a few units
     * in front of the camera is in cascade zero. Under the old fit, a camera forty units from its
     * target had a cascade zero that contained nothing at all.
     */
    {
        f32 distances[] = { 4.0F, 20.0F, 40.0F };

        for (u32 i = 0; i < nya_carray_length(distances); i++) {
            NYA_Camera3DPerspective camera = camera_looking_at_origin(distances[i]);

            NYA_Render3DShadow near = nya_render3d_shadow_for_camera(camera, SUN, 0, fit);

            // A point two units ahead of the camera, on the view axis.
            f32x3 ahead = { 0.0F, 0.0F, camera.position.z + 2.0F };

            nya_check(covers(near, ahead), "cascade zero should cover a point two units ahead of a camera %f from its target",
                      (f64)distances[i]);
        }
    }

    /*
     * ── Between them the cascades reach the whole range, and stop after it.
     *
     * A point at the far end of the range has to land in *some* cascade or it draws unshadowed, and a
     * point well past the range has to land in none — that is what makes the range a range rather than
     * a suggestion.
     */
    {
        NYA_Camera3DPerspective camera = camera_looking_at_origin(10.0F);

        f32x3 inside  = { 0.0F, 0.0F, camera.position.z + (RANGE * 0.9F) };
        f32x3 outside = { 0.0F, 0.0F, camera.position.z + (RANGE * 8.0F) };

        b8 inside_covered  = false;
        b8 outside_covered = false;

        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(camera, SUN, cascade, fit);

            if (covers(shadow, inside)) inside_covered = true;
            if (covers(shadow, outside)) outside_covered = true;
        }

        nya_check(inside_covered, "a point at nine tenths of the range should be covered by some cascade");
        nya_check(!outside_covered, "and one eight times past it by none");
    }

    /*
     * ── ⭐ Turning the camera does not resize a cascade.
     *
     * The reason the fit takes the slice's bounding *sphere* rather than its bounding box, and the
     * property behind "the shadows move when I turn". A box fitted to the frustum corners changes size
     * as the camera turns, so every texel changes size with it and the snap below has no fixed grid to
     * snap to — the edges then crawl however carefully they are rounded. A sphere is the same size in
     * every direction, which is what makes the snap work at all.
     */
    {
        f32x3 targets[] = {
            { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.3F }, { -0.7F, -0.2F, -0.7F },
        };

        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            f32 first = 0.0F;

            for (u32 i = 0; i < nya_carray_length(targets); i++) {
                NYA_Camera3DPerspective camera = { .position = { 0.0F, 0.0F, 0.0F }, .target = targets[i], .fov_y = 1.05F };

                NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(camera, SUN, cascade, unsnapped);

                if (i == 0) first = shadow.extent;

                nya_check(fabsf(shadow.extent - first) < 0.001F,
                          "cascade " FMTu32 " heading " FMTu32 " should not change size, got %f against %f", cascade, i,
                          (f64)shadow.extent, (f64)first);
            }
        }
    }

    /*
     * ── A wider frustum needs a bigger cascade.
     *
     * The fit measures the frustum, so it has to actually respond to its shape — otherwise every
     * assertion above would also pass for a fit that ignored the camera and returned a constant.
     */
    {
        NYA_Camera3DPerspective narrow = camera_at(0.0F);
        NYA_Camera3DPerspective wide   = camera_at(0.0F);

        narrow.fov_y = 0.6F;
        wide.fov_y   = 1.6F;

        NYA_Render3DShadow narrow_shadow = nya_render3d_shadow_for_camera(narrow, SUN, 1, unsnapped);
        NYA_Render3DShadow wide_shadow   = nya_render3d_shadow_for_camera(wide, SUN, 1, unsnapped);

        nya_check(wide_shadow.extent > narrow_shadow.extent, "a wider field of view needs a wider cascade, got %f against %f",
                  (f64)wide_shadow.extent, (f64)narrow_shadow.extent);
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
     * ── The eye the pass shades from is back along the light, never inside the volume.
     *
     * A directional light has no position, so one is invented. It has to be far enough back that the
     * whole volume is in front of it, or geometry near the light-ward face is behind the near plane and
     * casts nothing.
     */
    {
        f32 extent = 8.0F;
        f32 depth  = extent * 4.0F;

        f32x3 center = { 1.0F, 2.0F, 3.0F };
        f32x3 eye    = { 0 };

        (void)nya_render3d_shadow_view_projection(center, SUN, extent, depth, &eye);

        f32 back = nya_vector_length(center - eye);

        nya_check(fabsf(back - (depth * 0.5F)) < 0.001F, "the eye sits half the depth back along the light, got %f", (f64)back);

        f32x3 forward, right, up;
        nya_render3d_light_basis(SUN, &forward, &right, &up);

        nya_check(nya_vector_dot(center - eye, forward) > 0.0F, "and back *along* the light, not across or against it");

        // A zero light direction is the default sun here too, rather than a normalize by zero.
        f32x3 defaulted_eye = { 0 };

        (void)nya_render3d_shadow_view_projection(center, f32x3_zero, extent, depth, &defaulted_eye);

        nya_check(!isnan(defaulted_eye.x) && !isnan(defaulted_eye.y) && !isnan(defaulted_eye.z),
                  "a light with no direction must not produce a NaN eye");
    }

    /*
     * ── The snap quantises the volume: many camera positions, few volume positions.
     *
     * This is the property the snap exists for, and it has to be stated as quantisation rather than as
     * "a small move does nothing" — a small move *can* cross a grid boundary. What is true is that a
     * hundred camera positions spanning one texel produce a handful of volume positions, against a
     * hundred unsnapped.
     *
     * A handful, and specifically at most three: the snap rounds along *two* lateral axes, and a camera
     * walking along world x has a component on both, so it can cross one boundary on each. Asserting
     * two was asserting that the camera moved along a single snap axis, which this one does not.
     */
    {
        // One texel of the cascade being measured, whose size the fit is what decides.
        f32 extent = nya_render3d_shadow_for_camera(camera_at(0.0F), SUN, 0, unsnapped).extent;
        f32 texel  = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

        enum { SAMPLES = 100 };

        u32   snapped_positions = 0;
        u32   raw_positions     = 0;
        f32x3 last_snapped      = { 0 };
        f32x3 last_raw          = { 0 };

        for (u32 i = 0; i < SAMPLES; i++) {
            // Across exactly one texel, so at most one boundary per axis can be crossed.
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

        nya_check(snapped_positions <= 3, "one texel of camera travel should give at most three volume positions, got " FMTu32,
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
        f32 extent = nya_render3d_shadow_for_camera(camera_at(0.0F), SUN, 0, unsnapped).extent;
        f32 texel  = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

        f32x3 forward, right, up;
        nya_render3d_light_basis(SUN, &forward, &right, &up);

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

        // A range inside the near plane names no slice, and is clamped rather than asserted — a caller
        // ramping the shadow distance to nothing should get no shadows, not a crash.
        NYA_Render3DShadow tiny = nya_render3d_shadow_for_camera(camera_at(0.0F), SUN, 0,
                                                                 (NYA_Render3DShadowFit){ .range = 0.001F, .strength = 0.4F });

        nya_check(!isnan(tiny.extent) && tiny.extent > 0.0F, "a range inside the near plane must still give a usable volume, got %f",
                  (f64)tiny.extent);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
