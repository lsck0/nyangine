/**
 * @file render_camera.c
 *
 * The 2D camera arithmetic, shared by the real renderer and the headless one.
 *
 * ⚠ **This file exists because the two renderers drifted, and the drift was invisible.** Camera
 * defaulting was written twice — once in `render2d.c`, once in `render2d_headless.c` — and the
 * headless copy was missing the zoom correction, so a headless caller that reset the camera and read
 * it back got a zoom of zero where the real build gives one. Nothing failed; game logic dividing by
 * that zoom simply produced infinities in a build nothing looked at. The screen/world conversions
 * had drifted further still: headless returned its argument unchanged, so a test that set a camera
 * and asked where a world point landed was told "wherever it already was".
 *
 * Everything here is a pure function of a camera and a target size. There is no GPU state in the
 * camera path at all — the projection is applied at flush, from these same four numbers — so both
 * builds can and now do call exactly this code. `render2d.c` and `render2d_headless.c` are left
 * holding only the parts that genuinely differ: closing a draw range, and having one to close.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Camera2DTopDown nya_camera2d_top_down_sanitized(NYA_Camera2DTopDown camera) {
    // A zoom of zero collapses the world to a point and divides by zero on the way back out, so it is
    // corrected rather than propagated. A caller wanting nothing drawn should not draw.
    if (camera.zoom <= 0.0F) camera.zoom = 1.0F;

    return camera;
}

NYA_Camera2DIsometric nya_camera2d_isometric_sanitized(NYA_Camera2DIsometric camera) {
    if (camera.zoom <= 0.0F) camera.zoom = 1.0F;

    // A tile with no size projects every tile coordinate onto the same point, which is the isometric
    // spelling of a zero zoom and is corrected the same way. 64x32 is the classic 2:1 diamond.
    if (camera.tile_width <= 0.0F) camera.tile_width = 64.0F;
    if (camera.tile_height <= 0.0F) camera.tile_height = 32.0F;

    return camera;
}

NYA_Camera2DTopDown nya_camera2d_top_down_or_identity(NYA_Camera2D camera) {
    // The identity, spelled out, rather than the zeroed struct a batch holds when no camera is set — a
    // zoom of zero would be a surprising thing to hand back and then pass straight back in. An
    // isometric camera answers the identity too: it is not a top-down camera and there is no top-down
    // camera that means the same thing, so the honest answer is "none".
    if (camera.kind != NYA_CAMERA2D_KIND_TOP_DOWN) return (NYA_Camera2DTopDown){ .zoom = 1.0F };

    return camera.as_top_down;
}

void nya_camera2d_basis(const NYA_Camera2D* camera, OUT f32* out_a, OUT f32* out_b, OUT f32* out_c, OUT f32* out_d) {
    switch (camera->kind) {
        case NYA_CAMERA2D_KIND_TOP_DOWN: {
            // A rotation scaled by the zoom. Positive rotation reads clockwise on screen, which is
            // the same sense NYA_Render2DTexture.rotation and the 2D solver already use.
            f32 c = cosf(camera->as_top_down.rotation) * camera->as_top_down.zoom;
            f32 s = sinf(camera->as_top_down.rotation) * camera->as_top_down.zoom;

            *out_a = c;
            *out_b = -s;
            *out_c = s;
            *out_d = c;
        } break;

        case NYA_CAMERA2D_KIND_ISOMETRIC: {
            // The classic tile-to-screen map, as a matrix: stepping one tile along +x moves half a tile
            // right and half a tile down, and one along +y moves half a tile *left* and half a tile
            // down — which is what makes a square grid draw as diamonds, and rows further down the
            // screen read as further away. Halves because the widths are the full diamond, as the art
            // is authored.
            f32 half_width  = camera->as_isometric.tile_width * 0.5F * camera->as_isometric.zoom;
            f32 half_height = camera->as_isometric.tile_height * 0.5F * camera->as_isometric.zoom;

            *out_a = half_width;
            *out_b = -half_width;
            *out_c = half_height;
            *out_d = half_height;
        } break;

        case NYA_CAMERA2D_KIND_NONE:
        default: {
            // The identity. Not reachable through the callers, which all check the kind first, but a
            // basis of zeroes would be a division by zero in the inverse rather than a visible bug.
            *out_a = 1.0F;
            *out_b = 0.0F;
            *out_c = 0.0F;
            *out_d = 1.0F;
        } break;
    }
}

f32x2 nya_camera2d_position(const NYA_Camera2D* camera) {
    switch (camera->kind) {
        case NYA_CAMERA2D_KIND_TOP_DOWN:  return camera->as_top_down.position;
        case NYA_CAMERA2D_KIND_ISOMETRIC: return camera->as_isometric.position;
        case NYA_CAMERA2D_KIND_NONE:
        default:                          return f32x2_zero;
    }
}

f32x2 nya_camera2d_screen_to_world(const NYA_Camera2D* camera, f32x2 screen, u32 target_width, u32 target_height) {
    if (camera == nullptr || camera->kind == NYA_CAMERA2D_KIND_NONE) return screen;

    f32 a, b, c, d;
    nya_camera2d_basis(camera, &a, &b, &c, &d);

    f32x2 position = nya_camera2d_position(camera);

    f32 center_x = (f32)target_width * 0.5F;
    f32 center_y = (f32)target_height * 0.5F;

    // Inverting the 2x2 the view matrix applies, written out rather than through a general matrix
    // inverse. The determinant is the one thing worth naming: zoom squared for a top-down camera, half
    // the tile area times zoom squared for an isometric one — neither can be zero, since both setters
    // correct a zero zoom or tile size before storing anything.
    f32 determinant = (a * d) - (b * c);

    f32 dx = screen[0] - center_x;
    f32 dy = screen[1] - center_y;

    return (f32x2){
        (((d * dx) - (b * dy)) / determinant) + position[0],
        ((((-c) * dx) + (a * dy)) / determinant) + position[1],
    };
}

f32x2 nya_camera2d_world_to_screen(const NYA_Camera2D* camera, f32x2 world, u32 target_width, u32 target_height) {
    if (camera == nullptr || camera->kind == NYA_CAMERA2D_KIND_NONE) return world;

    f32 a, b, c, d;
    nya_camera2d_basis(camera, &a, &b, &c, &d);

    f32x2 position = nya_camera2d_position(camera);

    f32 center_x = (f32)target_width * 0.5F;
    f32 center_y = (f32)target_height * 0.5F;

    f32 dx = world[0] - position[0];
    f32 dy = world[1] - position[1];

    return (f32x2){ (a * dx) + (b * dy) + center_x, (c * dx) + (d * dy) + center_y };
}
