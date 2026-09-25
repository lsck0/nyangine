/**
 * Where a cascaded shadow volume goes: the frustum fit, the split, and the texel snap.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

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
    // every cascade the uniform holds, so the tiling below is tested at its finest.
    NYA_Window window = { 0 };
    nya_render3d_shadow_options_set(&window, (NYA_Render3DShadowOptions){ .cascades = NYA_RENDER3D_SHADOW_CASCADES });

    NYA_Render3DShadowFit fit = { .range = RANGE, .strength = 0.45F, .aspect = 16.0F / 9.0F };

    NYA_Render3DShadowFit unsnapped = { .range = RANGE, .strength = 0.45F, .aspect = 16.0F / 9.0F, .no_texel_snap = true };

    /* The cascades tile the view, each wider than the last. Each cascade spends its whole resolution on its own slice of the view. Nested boxes starting at the camera would cover nothing the far cascades do not. */
    {
        NYA_Camera3DPerspective camera = camera_at(0.0F);

        f32 previous_extent   = 0.0F;
        f32 previous_distance = 0.0F;

        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(&window, camera, SUN, cascade, unsnapped);

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

    /* The fit does not depend on how far the camera is from what it looks at. A fit that ignored the frustum left near cascades in empty space for an orbiting camera, and shadows changed as it moved. Two cameras with the same shape and direction must produce the same cascade sizes at any distance. */
    {
        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            NYA_Render3DShadow close = nya_render3d_shadow_for_camera(&window, camera_looking_at_origin(4.0F), SUN, cascade, unsnapped);
            NYA_Render3DShadow distant = nya_render3d_shadow_for_camera(&window, camera_looking_at_origin(40.0F), SUN, cascade, unsnapped);

            nya_check(fabsf(close.extent - distant.extent) < 0.001F,
                      "cascade " FMTu32 " must be the same size from four units away as from forty, got %f against %f", cascade,
                      (f64)close.extent, (f64)distant.extent);

            // And it must sit the same distance down the view, so it covers the same slice of what the camera can see rather than the same patch of world.
            f32 close_distance = close.center.z - (-4.0F);
            f32 far_distance   = distant.center.z - (-40.0F);

            nya_check(fabsf(close_distance - far_distance) < 0.001F, "and the same distance down the view, got %f against %f",
                      (f64)close_distance, (f64)far_distance);
        }
    }

    /* The near cascade covers what is near the camera, at any distance from the subject. The other half of the same claim, stated as coverage rather than as numbers: a point a few units in front of the camera is in cascade zero. Under the old fit, a camera forty units from its target had a cascade zero that contained nothing at all. */
    {
        f32 distances[] = { 4.0F, 20.0F, 40.0F };

        for (u32 i = 0; i < nya_carray_length(distances); i++) {
            NYA_Camera3DPerspective camera = camera_looking_at_origin(distances[i]);

            NYA_Render3DShadow first = nya_render3d_shadow_for_camera(&window, camera, SUN, 0, fit);

            // A point two units ahead of the camera, on the view axis.
            f32x3 ahead = { 0.0F, 0.0F, camera.position.z + 2.0F };

            nya_check(covers(first, ahead), "cascade zero should cover a point two units ahead of a camera %f from its target",
                      (f64)distances[i]);
        }
    }

    /* Between them the cascades reach the whole range, and stop after it. A point at the far end must land in some cascade or it draws unshadowed, and a point well past it in none. */
    {
        NYA_Camera3DPerspective camera = camera_looking_at_origin(10.0F);

        f32x3 inside  = { 0.0F, 0.0F, camera.position.z + (RANGE * 0.9F) };
        f32x3 outside = { 0.0F, 0.0F, camera.position.z + (RANGE * 8.0F) };

        b8 inside_covered  = false;
        b8 outside_covered = false;

        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(&window, camera, SUN, cascade, fit);

            if (covers(shadow, inside)) inside_covered = true;
            if (covers(shadow, outside)) outside_covered = true;
        }

        nya_check(inside_covered, "a point at nine tenths of the range should be covered by some cascade");
        nya_check(!outside_covered, "and one eight times past it by none");
    }

    /* Turning the camera does not resize a cascade. Why the fit uses the slice's bounding sphere, not its box: a box fitted to frustum corners changes size as the camera turns, so texels resize and the snap has no fixed grid, and edges crawl. A sphere is the same size in every direction. */
    {
        f32x3 targets[] = {
            { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.3F }, { -0.7F, -0.2F, -0.7F },
        };

        for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
            f32 first = 0.0F;

            for (u32 i = 0; i < nya_carray_length(targets); i++) {
                NYA_Camera3DPerspective camera = { .position = { 0.0F, 0.0F, 0.0F }, .target = targets[i], .fov_y = 1.05F };

                NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(&window, camera, SUN, cascade, unsnapped);

                if (i == 0) first = shadow.extent;

                nya_check(fabsf(shadow.extent - first) < 0.001F,
                          "cascade " FMTu32 " heading " FMTu32 " should not change size, got %f against %f", cascade, i,
                          (f64)shadow.extent, (f64)first);
            }
        }
    }

    /* A wider frustum needs a bigger cascade. Without this, a fit returning a constant would pass every assertion above. */
    {
        NYA_Camera3DPerspective narrow = camera_at(0.0F);
        NYA_Camera3DPerspective wide   = camera_at(0.0F);

        narrow.fov_y = 0.6F;
        wide.fov_y   = 1.6F;

        NYA_Render3DShadow narrow_shadow = nya_render3d_shadow_for_camera(&window, narrow, SUN, 1, unsnapped);
        NYA_Render3DShadow wide_shadow   = nya_render3d_shadow_for_camera(&window, wide, SUN, 1, unsnapped);

        nya_check(wide_shadow.extent > narrow_shadow.extent, "a wider field of view needs a wider cascade, got %f against %f",
                  (f64)wide_shadow.extent, (f64)narrow_shadow.extent);
    }

    // The light basis is orthonormal, including for a light pointing straight down.
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

    /* The basis follows the light exactly, and a turning sun moves the map smoothly. The basis used to round elevation and azimuth to half-degree steps. That froze the map for most frames and then jumped it, which reads as the shadows lagging the sun. Both halves are asserted: the basis is the direction given, and no frame of a turning sun moves a rim caster's shadow much further than the average one does. */
    {
        f32x3 forward, right, up;

        nya_render3d_light_basis(SUN, &forward, &right, &up);

        f32 turned = acosf(nya_clamp(nya_vector_dot(forward, nya_vector_normalize(SUN)), -1.0F, 1.0F));

        nya_check(turned < 1e-5F, "the basis should point exactly where the light does, off by %f", (f64)turned);

        // an arc of a degree a frame apart, the rate gnyame's two-minute day turns the sun at.
        const f32 turn_per_frame = (f32)M_PI / 60.0F / 60.0F;

        // a caster near the rim of cascade zero, where the lever arm of a turning grid is longest.
        f32x3 caster = { 14.0F, 6.0F, 12.0F };

        f32 map_size   = (f32)nya_render3d_shadow_options(&window).map_size;
        f32 previous_x = 0.0F;
        f32 previous_y = 0.0F;

        f32 total = 0.0F;
        f32 worst = 0.0F;

        const u32 frames = 240;

        for (u32 frame = 0; frame < frames; frame++) {
            f32 angle = 0.6F + ((f32)frame * turn_per_frame);

            f32x3 sun = nya_vector_normalize((f32x3){ -cosf(angle), -nya_max(sinf(angle), 0.12F), -0.45F });

            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(&window, camera_looking_at_origin(20.0F), sun, 0, fit);

            f32_4x4 light = nya_render3d_shadow_view_projection(shadow.center, sun, shadow.extent, shadow.extent * 4.0F, nullptr);

            f32x4 clip = nya_matrix_times_vector(light, (f32x4){ caster.x, caster.y, caster.z, 1.0F });

            f32 x = (((clip.x / clip.w) * 0.5F) + 0.5F) * map_size;
            f32 y = (((clip.y / clip.w) * 0.5F) + 0.5F) * map_size;

            if (frame > 0) {
                f32 step = sqrtf(((x - previous_x) * (x - previous_x)) + ((y - previous_y) * (y - previous_y)));

                total += step;
                if (step > worst) worst = step;
            }

            previous_x = x;
            previous_y = y;
        }

        f32 mean = total / (f32)(frames - 1);

        // three, not one: the cascade centre still snaps to whole texels, which lands on one frame rather than spreading over several. Snapped in angle the same run was 7.6 times its mean.
        nya_check(worst < mean * 3.0F, "a turning sun should move the map evenly, worst %f texels against a mean of %f", (f64)worst,
                  (f64)mean);
    }

    /* The eye the pass shades from is back along the light, never inside the volume. A directional light has no position, so one is invented. It has to be far enough back that the whole volume is in front of it, or geometry near the light-ward face is behind the near plane and casts nothing. */
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

    /* The snap quantises the volume: many camera positions, few volume positions. Stated as quantisation because a small move can cross a grid boundary. A hundred camera positions spanning one texel produce a handful of volume positions, against a hundred unsnapped. At most three: the snap rounds along two lateral axes, and a camera moving along world x has a component on both, so it can cross one boundary on each. */
    {
        // One texel of the cascade being measured, whose size the fit is what decides.
        f32 extent = nya_render3d_shadow_for_camera(&window, camera_at(0.0F), SUN, 0, unsnapped).extent;
        f32 texel  = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

        enum { SAMPLES = 100 };

        u32   snapped_positions = 0;
        u32   raw_positions     = 0;
        f32x3 last_snapped      = { 0 };
        f32x3 last_raw          = { 0 };

        for (u32 i = 0; i < SAMPLES; i++) {
            // Across exactly one texel, so at most one boundary per axis can be crossed.
            f32 offset = ((f32)i / (f32)SAMPLES) * texel;

            NYA_Render3DShadow snapped = nya_render3d_shadow_for_camera(&window, camera_at(offset), SUN, 0, fit);
            NYA_Render3DShadow raw     = nya_render3d_shadow_for_camera(&window, camera_at(offset), SUN, 0, unsnapped);

            f32x3 snapped_delta = snapped.center - last_snapped;
            f32x3 raw_delta     = raw.center - last_raw;

            if (i == 0 || nya_vector_length(snapped_delta) > 0.0001F) snapped_positions++;
            if (i == 0 || nya_vector_length(raw_delta) > 0.0F) raw_positions++;

            last_snapped = snapped.center;
            last_raw     = raw.center;
        }

        nya_check(snapped_positions <= 3, "one texel of camera travel should give at most three volume positions, got " FMTu32,
                  snapped_positions);

        // meaningful only if the unsnapped fit follows the camera continuously; otherwise a fit ignoring the camera would pass too.
        nya_check(raw_positions > SAMPLES / 2, "unsnapped, the volume should follow the camera continuously, got " FMTu32 " positions",
                  raw_positions);
    }

    /* Every position the volume takes lies on the texel grid. Quantisation alone would be satisfied by a volume that never moves. Walking the camera across several texels checks the other half: it moves, and every place it stops is on the grid. */
    {
        f32 extent = nya_render3d_shadow_for_camera(&window, camera_at(0.0F), SUN, 0, unsnapped).extent;
        f32 texel  = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

        f32x3 forward, right, up;
        nya_render3d_light_basis(SUN, &forward, &right, &up);

        u32 distinct = 0;
        f32 previous = 0.0F;

        for (u32 step = 0; step < 64; step++) {
            NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(&window, camera_at((f32)step * texel * 0.5F), SUN, 0, fit);

            // On the grid means: the centre's coordinate along each of the light's lateral axes is a whole number of texels.
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

    /* Options take defaults for zeroes and clamp the rest, so a config file cannot size the atlas wrongly. */
    {
        NYA_Window fresh = { 0 };

        NYA_Render3DShadowOptions defaults = nya_render3d_shadow_options(&fresh);

        nya_check(defaults.cascades == NYA_RENDER3D_SHADOW_CASCADES_DEFAULT && defaults.map_size == NYA_RENDER3D_SHADOW_MAP_SIZE,
                  "a zeroed window should get the default options, got " FMTu32 " cascades of " FMTu32, defaults.cascades, defaults.map_size);

        struct {
            NYA_Render3DShadowOptions set;
            u32                       cascades;
            u32                       map_size;
        } cases[] = {
            { { .cascades = 99, .map_size = 1000 }, NYA_RENDER3D_SHADOW_CASCADES, 1024 },
            { { .cascades = 1, .map_size = 1 }, 1, NYA_RENDER3D_SHADOW_MAP_SIZE_MIN },
            { { .cascades = 2, .map_size = 1U << 30 }, 2, NYA_RENDER3D_SHADOW_MAP_SIZE_MAX },
            { { .map_size = 2048 }, NYA_RENDER3D_SHADOW_CASCADES_DEFAULT, 2048 },
        };

        for (u32 i = 0; i < nya_carray_length(cases); i++) {
            nya_render3d_shadow_options_set(&fresh, cases[i].set);

            NYA_Render3DShadowOptions resolved = nya_render3d_shadow_options(&fresh);

            nya_check(resolved.cascades == cases[i].cascades && resolved.map_size == cases[i].map_size,
                      "case " FMTu32 " resolved to " FMTu32 " cascades of " FMTu32, i, resolved.cascades, resolved.map_size);
        }

        // the shade colour is a look, not a size: it passes through untouched and never releases the atlas.
        NYA_Color cool = { 0.4F, 0.5F, 0.9F, 0.5F };
        nya_render3d_shadow_options_set(&fresh, (NYA_Render3DShadowOptions){ .color = cool });
        nya_check(nya_render3d_shadow_options(&fresh).color.b == cool.b && nya_render3d_shadow_options(&fresh).color.a == cool.a,
                  "the shade colour should pass through");

        // fewer cascades divide the same range: the last one still reaches the far end.
        NYA_Camera3DPerspective camera = camera_at(0.0F);

        f32x3 far_point = camera.position + (f32x3){ 0.0F, 0.0F, RANGE * 0.95F };

        for (u32 count = 1; count <= NYA_RENDER3D_SHADOW_CASCADES; count++) {
            nya_render3d_shadow_options_set(&fresh, (NYA_Render3DShadowOptions){ .cascades = count });

            NYA_Render3DShadow last = nya_render3d_shadow_for_camera(&fresh, camera, SUN, count - 1, unsnapped);

            nya_check(covers(last, far_point), "with " FMTu32 " cascades the last should reach the range", count);
        }
    }

    // The degenerate cases.
    {
        // a camera aimed at itself has no direction. The volume sits on it, wrong but bounded and not NaN.
        NYA_Camera3DPerspective still = { .position = { 3.0F, 4.0F, 5.0F }, .target = { 3.0F, 4.0F, 5.0F } };

        NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(&window, still, SUN, 0, (NYA_Render3DShadowFit){ .strength = 0.4F });

        nya_check(!isnan(shadow.center.x) && !isnan(shadow.center.y) && !isnan(shadow.center.z),
                  "a camera aimed at itself must not produce NaN, got (%f, %f, %f)", (f64)shadow.center.x, (f64)shadow.center.y,
                  (f64)shadow.center.z);

        // A zero light direction is read as the default sun rather than dividing by zero.
        NYA_Render3DShadow defaulted =
            nya_render3d_shadow_for_camera(&window, camera_at(0.0F), f32x3_zero, 0, (NYA_Render3DShadowFit){ .strength = 0.4F });

        nya_check(!isnan(defaulted.center.x), "and neither must a light with no direction");

        // Past the compiled-in cascade count, clamped rather than reading off the end of the arrays.
        NYA_Render3DShadow clamped =
            nya_render3d_shadow_for_camera(&window, camera_at(0.0F), SUN, 99, (NYA_Render3DShadowFit){ .strength = 0.4F });

        nya_check(clamped.cascade == NYA_RENDER3D_SHADOW_CASCADES - 1, "a cascade past the last is clamped, got " FMTu32,
                  clamped.cascade);

        // a range inside the near plane names no slice and is clamped: ramping shadow distance to nothing gives no shadows, not a crash.
        NYA_Render3DShadow tiny = nya_render3d_shadow_for_camera(&window, camera_at(0.0F), SUN, 0,
                                                                 (NYA_Render3DShadowFit){ .range = 0.001F, .strength = 0.4F });

        nya_check(!isnan(tiny.extent) && tiny.extent > 0.0F, "a range inside the near plane must still give a usable volume, got %f",
                  (f64)tiny.extent);
    }

    // The window keeps the fit for its next scene, and what is drawn casts until told otherwise.
    {
        nya_render3d_shadow_set(&window, fit);

        NYA_Render3DShadowFit kept = nya_render3d_shadow(&window);
        nya_check(kept.range == RANGE && kept.strength == 0.45F, "the fit is kept as set");

        nya_render3d_shadow_set(&window, (NYA_Render3DShadowFit){ 0 });
        nya_check(nya_render3d_shadow(&window).strength == 0.0F, "zero strength turns shadows off");

        nya_render3d_begin(&window, camera_at(0.0F));
        nya_render3d_shadow_cast_set(&window, false);
        nya_check(!window.render_system.mesh_batch.casts_shadow, "a draw can opt out of the cascades");
        nya_render3d_end(&window);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
