/**
 * @file render_shadow.c
 *
 * Where a cascaded shadow volume goes: the arithmetic, with no GPU in it.
 *
 * **Compiled into both builds, and that is the point.** Fitting a cascade to a camera and snapping it
 * to the shadow map's texel grid is pure math over a camera, a light direction and two constants — it
 * neither reads nor writes anything on a device. Leaving it inside render3d.c would have made it
 * unreachable from a headless build and therefore untestable, which is exactly the drift render_camera.c
 * exists to have stopped happening. See its file comment.
 *
 * The pass itself — rasterising the map, the atlas viewport, the depth pipeline — stays in render3d.c,
 * because all of that genuinely needs a device.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The light's own axes: where it points, and an up that is not parallel to it.
 *
 * Shared by the shadow pass and by nya_render3d_shadow_for_camera, because the two have to agree
 * exactly — the fit snaps a position onto the grid that the pass's view matrix then rasterises, and a
 * basis that differed by a hair would put the snap on a grid the map does not use.
 * */
void nya_render3d_light_basis(f32x3 direction, OUT f32x3* out_forward, OUT f32x3* out_right, OUT f32x3* out_up) {
    f32x3 forward = nya_vector_normalize(direction);

    // An up vector not parallel to the light: a look-at with the two collinear produces a degenerate
    // basis and a matrix of NaNs, and the light pointing straight down is the *common* case here.
    f32x3 reference = fabsf(forward.y) > 0.99F ? (f32x3){ 0.0F, 0.0F, 1.0F } : (f32x3){ 0.0F, 1.0F, 0.0F };

    f32x3 right = nya_vector_normalize(nya_vector_cross(reference, forward));

    *out_forward = forward;
    *out_right   = right;
    *out_up      = nya_vector_cross(forward, right);
}

/** The half-width of cascade `index`, from the nearest cascade's. Geometric — see the ratio's note. */
f32 nya_render3d_cascade_extent(f32 near_extent, u32 index) {
    if (near_extent <= 0.0F) near_extent = NYA_RENDER3D_SHADOW_EXTENT;

    for (u32 i = 0; i < index; i++) near_extent *= NYA_RENDER3D_SHADOW_CASCADE_RATIO;

    return near_extent;
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
 *
 * A practical split: the logarithmic ideal, which puts far more resolution close to the viewer than a
 * uniform one, mixed back toward uniform because pure logarithmic spends a cascade on the first
 * centimetre in front of the near plane. NYA_RENDER3D_SHADOW_CASCADE_RATIO is no longer what sizes a
 * cascade — the frustum is — so the split is the only place the distribution is decided.
 * */
NYA_INTERNAL void _nya_render3d_cascade_slice(f32 near_plane, f32 range, u32 index, OUT f32* out_near, OUT f32* out_far) {
    const f32 blend = 0.75F;

    f32 count = (f32)NYA_RENDER3D_SHADOW_CASCADES;

    f32 bounds[NYA_RENDER3D_SHADOW_CASCADES + 1];

    for (u32 i = 0; i <= NYA_RENDER3D_SHADOW_CASCADES; i++) {
        f32 fraction = (f32)i / count;

        f32 logarithmic = near_plane * powf(range / near_plane, fraction);
        f32 uniform     = near_plane + ((range - near_plane) * fraction);

        bounds[i] = (logarithmic * blend) + (uniform * (1.0F - blend));
    }

    *out_near = bounds[index];
    *out_far  = bounds[index + 1];
}

NYA_Render3DShadow nya_render3d_shadow_for_camera(NYA_Camera3DPerspective camera, f32x3 light_direction, u32 cascade, NYA_Render3DShadowFit fit) {
    cascade = nya_min(cascade, (u32)(NYA_RENDER3D_SHADOW_CASCADES - 1));

    b8 light_is_unset = light_direction.x == 0.0F && light_direction.y == 0.0F && light_direction.z == 0.0F;
    if (light_is_unset) light_direction = NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT;

    f32 range  = fit.range > 0.0F ? fit.range : NYA_RENDER3D_SHADOW_EXTENT;
    f32 aspect = fit.aspect > 0.0F ? fit.aspect : (16.0F / 9.0F);

    // The same defaults _nya_render3d_camera_defaults applies, spelled out rather than shared: that
    // function lives in render3d.c, which the headless build replaces wholesale, and this file is
    // compiled into both.
    f32 fov_y      = camera.fov_y > 0.0F ? camera.fov_y : ((f32)M_PI / 3.0F);
    f32 near_plane = camera.near_plane > 0.0F ? camera.near_plane : 0.1F;

    // A range inside the near plane names no slice. Clamped rather than asserted: a caller ramping the
    // shadow distance down to nothing should get no shadows, not a crash.
    if (range <= near_plane) range = near_plane * 1.001F;

    f32 slice_near, slice_far;
    _nya_render3d_cascade_slice(near_plane, range, cascade, &slice_near, &slice_far);

    f32x3 view_direction = camera.target - camera.position;

    f32 view_length = sqrtf((view_direction.x * view_direction.x) + (view_direction.y * view_direction.y)
                            + (view_direction.z * view_direction.z));

    // A camera aimed at itself names no direction. The volume then sits on the camera, which is wrong
    // but bounded — the alternative is a normalize that divides by zero and fills the matrix with NaN.
    f32x3 forward = view_length > 0.0001F ? view_direction / view_length : (f32x3){ 0.0F, 0.0F, 0.0F };

    /*
     * The slice's bounding **sphere**, not its bounding box.
     *
     * A sphere is the same size whichever way the camera is pointing, and that is the property the
     * whole thing rests on: a box fitted to the frustum corners grows and shrinks as the camera turns,
     * so the shadow map's texels change size every frame and every edge crawls — which no amount of
     * texel snapping can fix, because snapping assumes a grid of a fixed pitch. With a sphere the
     * pitch is constant while the camera only rotates, and the snap below does the rest.
     *
     * The closed form: `k` is the radius of the frustum's cross-section per unit of depth, and the
     * sphere either touches both slice caps or, for a short and wide slice, is centred on the far cap.
     */
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
         *
         * Unsnapped, the volume slides continuously as the camera moves: every texel covers a slightly
         * different patch of world each frame, so the boundary between lit and shadowed lands on a
         * different sample each time and every shadow edge crawls and fizzes. It reads as a filtering
         * problem and is a placement one.
         *
         * The fix is to let the volume move only in whole-texel steps. The centre is projected onto
         * the light's right and up axes, each coordinate rounded to a multiple of one texel's world
         * size, and the difference put back — so a static shadow edge stays on the same texels while
         * the camera moves through it. The light-ward axis is deliberately not snapped: depth is not
         * quantised by the map's resolution, and rounding it would only add bias.
         */
        f32x3 light_forward, light_right, light_up;
        nya_render3d_light_basis(light_direction, &light_forward, &light_right, &light_up);

        f32 texel_world_size = (extent * 2.0F) / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE;

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
