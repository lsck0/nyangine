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

NYA_Render3DShadow nya_render3d_shadow_for_camera(NYA_Camera3DPerspective camera, f32x3 light_direction, u32 cascade, NYA_Render3DShadowFit fit) {
    cascade = nya_min(cascade, (u32)(NYA_RENDER3D_SHADOW_CASCADES - 1));

    b8 light_is_unset = light_direction.x == 0.0F && light_direction.y == 0.0F && light_direction.z == 0.0F;
    if (light_is_unset) light_direction = NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT;

    f32 extent = nya_render3d_cascade_extent(fit.near_extent, cascade);

    /*
     * The centre, pushed a half-extent ahead of the camera.
     *
     * A volume centred on the viewer spends half its resolution behind them, where nothing is drawn.
     * Pushing it forward by exactly the half-width makes the box run from the camera to twice the
     * extent ahead, which is the whole of what the camera can see at that range — the cheapest
     * doubling of effective shadow resolution there is, and it has to be per cascade because each is
     * a different size.
     */
    f32x3 view_direction = camera.target - camera.position;

    f32 view_length = sqrtf((view_direction.x * view_direction.x) + (view_direction.y * view_direction.y)
                            + (view_direction.z * view_direction.z));

    // A camera aimed at itself names no direction. The volume then sits on the camera, which is wrong
    // but bounded — the alternative is a normalize that divides by zero and fills the matrix with NaN.
    f32x3 forward = view_length > 0.0001F ? view_direction / view_length : (f32x3){ 0.0F, 0.0F, 0.0F };

    f32x3 center = camera.position + (forward * extent);

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

        // The *nearest* extent, not this cascade's: nya_render3d_shadow_begin applies the ratio itself,
        // and handing it the already-widened one would apply it twice.
        .extent   = fit.near_extent,
        .depth    = fit.depth,
        .strength = fit.strength,
        .bias     = fit.bias,
        .cascade  = cascade,
    };
}

