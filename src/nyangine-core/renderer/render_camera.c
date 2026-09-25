/**
 * @file render_camera.c
 *
 * Camera defaults and screen/world conversion shared by render2d.c and render2d_headless.c. Two
 * copies had drifted: headless left zoom at zero and returned points unconverted, so tests saw a
 * camera the real build never produces.
 * */
#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Camera2DTopDown nya_camera2d_top_down_sanitized(NYA_Camera2DTopDown camera) {
    // Zoom of zero divides by zero on the way back out, so correct it rather than propagate it.
    if (camera.zoom <= 0.0F) camera.zoom = 1.0F;

    return camera;
}

NYA_Camera2DIsometric nya_camera2d_isometric_sanitized(NYA_Camera2DIsometric camera) {
    if (camera.zoom <= 0.0F) camera.zoom = 1.0F;

    // Zero tile size collapses every tile onto one point, like zero zoom; 64x32 is the classic 2:1 diamond.
    if (camera.tile_width <= 0.0F) camera.tile_width = 64.0F;
    if (camera.tile_height <= 0.0F) camera.tile_height = 32.0F;

    return camera;
}

NYA_Camera2DTopDown nya_camera2d_top_down_or_identity(NYA_Camera2D camera) {
    // Answer the identity, not the zeroed struct, so zoom zero is not passed straight back in.
    if (camera.kind != NYA_CAMERA2D_KIND_TOP_DOWN) return (NYA_Camera2DTopDown){ .zoom = 1.0F };

    return camera.as_top_down;
}

void nya_camera2d_basis(const NYA_Camera2D* camera, OUT f32* out_a, OUT f32* out_b, OUT f32* out_c, OUT f32* out_d) {
    switch (camera->kind) {
        case NYA_CAMERA2D_KIND_TOP_DOWN: {
            // A rotation scaled by the zoom; positive rotation reads clockwise, matching the 2D solver.
            f32 c = cosf(camera->as_top_down.rotation) * camera->as_top_down.zoom;
            f32 s = sinf(camera->as_top_down.rotation) * camera->as_top_down.zoom;

            *out_a = c;
            *out_b = -s;
            *out_c = s;
            *out_d = c;
        } break;

        case NYA_CAMERA2D_KIND_ISOMETRIC: {
            // Tile to screen: a square grid draws as diamonds; halves because tile sizes are the full diamond.
            f32 half_width  = camera->as_isometric.tile_width * 0.5F * camera->as_isometric.zoom;
            f32 half_height = camera->as_isometric.tile_height * 0.5F * camera->as_isometric.zoom;

            *out_a = half_width;
            *out_b = -half_width;
            *out_c = half_height;
            *out_d = half_height;
        } break;

        case NYA_CAMERA2D_KIND_NONE:
        default: {
            // The identity; a basis of zeroes would divide by zero in the inverse.
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

    // The inverse of the view's 2x2; the determinant is never zero since the setters correct zero zoom and tile size.
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
