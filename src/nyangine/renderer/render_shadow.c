/**
 * @file render_shadow.c
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The light's own axes: where it points, and an up that is not parallel to it.
 * */
void nya_render3d_light_basis(f32x3 direction, OUT f32x3* out_forward, OUT f32x3* out_right, OUT f32x3* out_up) {
    nya_assert(out_forward != nullptr && out_right != nullptr && out_up != nullptr);

    f32x3 unit = nya_vector_normalize(direction);

    /*
     * Snapped in elevation and azimuth. A sun that turns every frame turns the texel grid with it, and caster
     * edges crawl even under a still camera; snapped, the grid holds between steps.
     */
    f32 step      = NYA_RENDER3D_SHADOW_ANGLE_STEP;
    f32 elevation = roundf(asinf(nya_clamp(unit.y, -1.0F, 1.0F)) / step) * step;
    f32 azimuth   = roundf(atan2f(unit.z, unit.x) / step) * step;

    f32x3 forward = { cosf(elevation) * cosf(azimuth), sinf(elevation), cosf(elevation) * sinf(azimuth) };

    // An up vector not parallel to the light: a look-at with the two collinear produces a degenerate
    // basis and a matrix of NaNs, and the light pointing straight down is the *common* case here.
    f32x3 reference = fabsf(forward.y) > 0.99F ? (f32x3){ 0.0F, 0.0F, 1.0F } : (f32x3){ 0.0F, 1.0F, 0.0F };

    f32x3 right = nya_vector_normalize(nya_vector_cross(reference, forward));

    *out_forward = forward;
    *out_right   = right;
    *out_up      = nya_vector_cross(forward, right);
}

f32_4x4 nya_render3d_shadow_view_projection(f32x3 center, f32x3 light_direction, f32 extent, f32 depth, OUT f32x3* out_eye) {
    b8 light_is_unset = light_direction.x == 0.0F && light_direction.y == 0.0F && light_direction.z == 0.0F;
    if (light_is_unset) light_direction = NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT;

    f32x3 direction, right, up;
    nya_render3d_light_basis(light_direction, &direction, &right, &up);

    nya_unused(right);

    // A directional light has no position, so one is invented: back along the light by half the depth,
    // far enough that the whole volume is in front of it. `direction` is the way light travels, so
    // backing off means subtracting it.
    f32x3 eye = center - (direction * (depth * 0.5F));

    if (out_eye != nullptr) *out_eye = eye;

    // Orthographic, because a directional light's rays are parallel. Aspect one: the map is square, and
    // the height is the full width, so `extent` is the half-width the volume actually covers.
    f32_4x4 projection = nya_matrix_orthographic_3d(extent * 2.0F, 1.0F, 0.01F, depth);
    f32_4x4 view       = nya_matrix_look_at(eye, center, up);

    return projection * view;
}

/**
 * Where cascade `index`'s slice of the view starts and ends, in world units down the view axis.
 * */
NYA_INTERNAL void _nya_render3d_cascade_slice(f32 near_plane, f32 range, u32 index, u32 cascades, OUT f32* out_near, OUT f32* out_far) {
    nya_assert(index < cascades && cascades <= NYA_RENDER3D_SHADOW_CASCADES);

    const f32 blend = 0.75F;

    f32 bounds[NYA_RENDER3D_SHADOW_CASCADES + 1];

    for (u32 i = 0; i <= cascades; i++) {
        f32 fraction = (f32)i / (f32)cascades;

        f32 logarithmic = near_plane * powf(range / near_plane, fraction);
        f32 uniform     = near_plane + ((range - near_plane) * fraction);

        bounds[i] = (logarithmic * blend) + (uniform * (1.0F - blend));
    }

    *out_near = bounds[index];
    *out_far  = bounds[index + 1];
}

NYA_Render3DShadow nya_render3d_shadow_for_camera(const NYA_Window* window, NYA_Camera3DPerspective camera, f32x3 light_direction, u32 cascade,
                                                  NYA_Render3DShadowFit fit) {
    nya_assert(window != nullptr);

    NYA_Render3DShadowOptions options = nya_render3d_shadow_options(window);

    cascade = nya_min(cascade, options.cascades - 1);

    b8 light_is_unset = light_direction.x == 0.0F && light_direction.y == 0.0F && light_direction.z == 0.0F;
    if (light_is_unset) light_direction = NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT;

    f32 range  = fit.range > 0.0F ? fit.range : NYA_RENDER3D_SHADOW_EXTENT;
    f32 aspect = fit.aspect > 0.0F ? fit.aspect : (16.0F / 9.0F);

    // The same defaults _nya_render3d_camera_defaults applies, spelled out rather than shared: that
    // function lives in render3d.c, which the headless build replaces wholesale, and this file is
    // compiled into both.
    f32 fov_y      = camera.fov_y > 0.0F ? camera.fov_y : ((f32)M_PI / 3.0F);
    f32 near_plane = camera.near_plane > 0.0F ? camera.near_plane : 0.1F;

    /*
     * Where the cascades start. See NYA_Render3DShadowFit.near_distance.
     *
     * The camera's near plane is wrong for an orbit camera: the sharpest cascades would cover the empty
     * gap to the subject. A caller that knows where its casters begin says so.
     */
    f32 shadow_near = fit.near_distance > near_plane ? fit.near_distance : near_plane;

    // A range inside the near plane names no slice. Clamped rather than asserted: a caller ramping the
    // shadow distance down to nothing should get no shadows, not a crash.
    if (range <= shadow_near) range = shadow_near * 1.001F;

    f32 slice_near, slice_far;
    _nya_render3d_cascade_slice(shadow_near, range, cascade, options.cascades, &slice_near, &slice_far);

    f32x3 view_direction = camera.target - camera.position;

    f32 view_length = sqrtf((view_direction.x * view_direction.x) + (view_direction.y * view_direction.y)
                            + (view_direction.z * view_direction.z));

    // a camera aimed at itself has no direction. The volume then sits on the camera, wrong but bounded,
    // instead of normalizing a zero vector into NaN.
    f32x3 forward = view_length > 0.0001F ? view_direction / view_length : (f32x3){ 0.0F, 0.0F, 0.0F };

    /* The slice's bounding sphere, not its bounding box. */
    f32 tan_half = tanf(fov_y * 0.5F);
    f32 k        = sqrtf(1.0F + (aspect * aspect)) * tan_half;

    f32 center_distance;
    f32 extent;

    f32 k2 = k * k;

    if ((k2 * k2) >= ((slice_far - slice_near) / (slice_far + slice_near))) {
        // Wide enough that the far cap's own circle contains the whole slice.
        center_distance = slice_far;
        extent          = slice_far * k;
    } else {
        center_distance = 0.5F * (slice_far + slice_near) * (1.0F + k2);

        extent = 0.5F
               * sqrtf(((slice_far - slice_near) * (slice_far - slice_near))
                       + (2.0F * ((slice_far * slice_far) + (slice_near * slice_near)) * k2)
                       + (((slice_far + slice_near) * (slice_far + slice_near)) * k2 * k2));
    }

    f32x3 center = camera.position + (forward * center_distance);

    if (!fit.no_texel_snap) {
        /*
         * Snapped to whole shadow-map texels, in the light's own frame.
         */
        f32x3 light_forward, light_right, light_up;
        nya_render3d_light_basis(light_direction, &light_forward, &light_right, &light_up);

        f32 texel_world_size = (extent * 2.0F) / (f32)options.map_size;

        f32 along_right = nya_vector_dot(center, light_right);
        f32 along_up    = nya_vector_dot(center, light_up);

        f32 snapped_right = floorf(along_right / texel_world_size) * texel_world_size;
        f32 snapped_up    = floorf(along_up / texel_world_size) * texel_world_size;

        center += (light_right * (snapped_right - along_right)) + (light_up * (snapped_up - along_up));
    }

    return (NYA_Render3DShadow){
        .center = center,

        // This cascade's own half-width, already final. nya_render3d_shadow_begin applies no ratio to
        // it any more: the size comes from the frustum slice, so there is nothing left to widen.
        .extent   = extent,
        .depth    = fit.depth,
        .strength = fit.strength,
        .bias     = fit.bias,
        .cascade  = cascade,
    };
}

void nya_render3d_shadow_options_set(NYA_Window* window, NYA_Render3DShadowOptions options) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    NYA_Render3DShadowOptions before = _nya_render3d_shadow_options_resolve(batch->shadow_options);
    NYA_Render3DShadowOptions after  = _nya_render3d_shadow_options_resolve(options);

    batch->shadow_options = options;

    if (before.cascades == after.cascades && before.map_size == after.map_size) return;

    nya_assert(!batch->shadow_pass_active, "shadow options cannot change inside a shadow pass");

    _nya_render3d_shadow_release(window);
}

NYA_Render3DShadowOptions nya_render3d_shadow_options(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_render3d_shadow_options_resolve(window->render_system.mesh_batch.shadow_options);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Render3DShadowOptions _nya_render3d_shadow_options_resolve(NYA_Render3DShadowOptions options) {
    u32 cascades = options.cascades > 0 ? options.cascades : NYA_RENDER3D_SHADOW_CASCADES_DEFAULT;
    u32 map_size = options.map_size > 0 ? options.map_size : NYA_RENDER3D_SHADOW_MAP_SIZE;

    map_size = nya_clamp(map_size, (u32)NYA_RENDER3D_SHADOW_MAP_SIZE_MIN, (u32)NYA_RENDER3D_SHADOW_MAP_SIZE_MAX);

    // rounded up, so a texel is an exact binary fraction of the map and the snap grid does not drift.
    u32 power = NYA_RENDER3D_SHADOW_MAP_SIZE_MIN;
    while (power < map_size) power *= 2;

    return (NYA_Render3DShadowOptions){
        .cascades = nya_clamp(cascades, 1U, (u32)NYA_RENDER3D_SHADOW_CASCADES),
        .map_size = power,
        .color    = options.color,
    };
}
