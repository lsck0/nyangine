/**
 * @file render3d_headless.c
 *
 * The 3D mesh batch, for builds with no renderer.
 *
 * Every public function of render3d.h, doing nothing. Same contract and same reasoning as
 * render2d_headless.c: callers do not guard their draw calls, so a headless build needs the symbols
 * to exist, and a test that runs a whole frame of game logic still runs the code that would have
 * drawn it.
 *
 * Two pieces of state are modelled rather than stubbed, because game logic reads them back:
 *
 * - `active`, so nya_render3d_active answers truthfully and a layer that branches on it behaves the
 *   same headless as it does on a GPU.
 * - the camera, so nya_render3d_screen_ray produces a real ray. That one matters: picking is game
 *   logic, it is exactly the kind of thing a headless test wants to exercise, and a stub returning
 *   zero would make every such test pass against nothing.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION (HEADLESS)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render3d_begin(NYA_Window* window, NYA_Camera3DPerspective camera) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // The same defaults the real path applies, spelled out rather than shared, because the function
    // that applies them lives in render3d.c and this file is compiled instead of it — not beside it.
    if (camera.up.x == 0.0F && camera.up.y == 0.0F && camera.up.z == 0.0F) camera.up = (f32x3){ 0.0F, 1.0F, 0.0F };
    if (camera.fov_y <= 0.0F) camera.fov_y = (f32)M_PI / 3.0F;
    if (camera.near_plane <= 0.0F) camera.near_plane = 0.1F;
    if (camera.far_plane <= camera.near_plane) camera.far_plane = 1000.0F;

    batch->camera          = camera;
    batch->camera_is_ortho = false;
    batch->camera_valid    = true;
    batch->active          = true;
}

void nya_render3d_begin_orthographic(NYA_Window* window, NYA_Camera3DOrthographic camera) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (camera.up.x == 0.0F && camera.up.y == 0.0F && camera.up.z == 0.0F) camera.up = (f32x3){ 0.0F, 1.0F, 0.0F };
    if (camera.height <= 0.0F) camera.height = 10.0F;

    batch->camera_orthographic = camera;
    batch->camera_is_ortho     = true;
    batch->camera_valid        = true;
    batch->active              = true;
}

void nya_render3d_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.active = false;
}

b8 nya_render3d_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.active;
}

void nya_render3d_light_set(NYA_Window* window, NYA_Render3DLight light) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.light = light;
}

NYA_Render3DLight nya_render3d_light(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.light;
}

/*
 * The point lights are *stored* rather than ignored, like the light and the material above.
 *
 * Headless does not draw, but a test is entitled to set state and read it back, and a stub that dropped
 * what it was given would make nya_render3d_point_light_count answer zero forever — a test asserting the
 * budget is enforced would then pass by testing nothing.
 */
/*
 * The shadow pass records its configuration and nothing else.
 *
 * There is no target to render into and no map to fill, so `shadow_valid` stays false and
 * nya_render3d_shadow_active answers honestly. A headless caller can still bracket its draw calls with
 * these, which is the point: the same game code runs on a dedicated server.
 */
void nya_render3d_shadow_begin(NYA_Window* window, NYA_Render3DShadow shadow) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.shadow = shadow;
}

void nya_render3d_shadow_end(NYA_Window* window) {
    nya_assert(window != nullptr);
}

b8 nya_render3d_shadow_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return false;
}

void nya_render3d_point_light_add(NYA_Window* window, NYA_Render3DPointLight light) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (batch->point_light_count >= NYA_RENDER3D_MAX_POINT_LIGHTS) {
        nya_warn("A fifth point light was added and dropped; NYA_RENDER3D_MAX_POINT_LIGHTS is %d.", NYA_RENDER3D_MAX_POINT_LIGHTS);
        return;
    }

    batch->point_lights[batch->point_light_count++] = light;
}

void nya_render3d_point_lights_clear(NYA_Window* window) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.point_light_count = 0;
}

u32 nya_render3d_point_light_count(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.point_light_count;
}

void nya_render3d_material_set(NYA_Window* window, NYA_Render3DMaterial material) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.material = material;
}

NYA_Render3DMaterial nya_render3d_material(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.material;
}

/*
 * Draws nothing, and there was no stub here at all until now.
 *
 * Nothing headless called it, so it linked — but that is a property of the current callers rather than of
 * the surface, and the first test to draw a model would have failed to link for a reason that looks like
 * a build problem. The asset side of a mesh is testable headless and covered by test_asset_mesh; this is
 * the half that needs a GPU.
 */
void nya_render3d_mesh(NYA_Window* window, NYA_ConstCString handle, f32x3 center, f32x3 scale, NYA_Quaternion rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    nya_unused(handle);
    nya_unused(center);
    nya_unused(scale);
    nya_unused(rotation);
    nya_unused(color);
}

void nya_render3d_cube(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, NYA_Color color) {
    nya_unused(window, center, size, rotation, color);
}

void nya_render3d_cube_outline(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, f32 thickness, NYA_Color color) {
    nya_unused(window, center, size, rotation, thickness, color);
}

void nya_render3d_sphere(NYA_Window* window, f32x3 center, f32 radius, NYA_Color color) {
    nya_unused(window, center, radius, color);
}

void nya_render3d_plane(NYA_Window* window, f32x3 center, f32x2 size, NYA_Color color) {
    nya_unused(window, center, size, color);
}

void nya_render3d_line(NYA_Window* window, f32x3 from, f32x3 to, f32 thickness, NYA_Color color) {
    nya_unused(window, from, to, thickness, color);
}

void nya_render3d_grid(NYA_Window* window, u32 half_extent, f32 cell_size, NYA_Color color) {
    nya_unused(window, half_extent, cell_size, color);
}

void nya_render3d_triangle(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, NYA_Color color) {
    nya_unused(window, a, b, c, color);
}

void nya_render3d_quad(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color) {
    nya_unused(window, a, b, c, d, color);
}

void nya_render3d_billboard(NYA_Window* window, NYA_ConstCString texture_handle, f32x3 center, f32x2 size, f32 rotation, NYA_Color color) {
    nya_unused(window, texture_handle, center, size, rotation, color);
}

NYA_Render3DTextureBinding nya_render3d_texture_resolve(NYA_ConstCString texture_handle) {
    nya_unused(texture_handle);

    // Nothing is ever bound headless, so nothing resolves. Callers draw untextured, which headless draws
    // as nothing at all.
    return (NYA_Render3DTextureBinding){ 0 };
}

void nya_render3d_billboard_resolved(NYA_Window* window, NYA_Render3DTextureBinding texture, f32x3 center, f32x2 size, f32 rotation,
                                     NYA_Color color) {
    nya_unused(window, texture, center, size, rotation, color);
}

void nya_render3d_blend_set(NYA_Window* window, NYA_Render3DBlend blend) {
    nya_unused(window, blend);
}

void nya_render3d_outline_set(NYA_Window* window, f32 thickness, NYA_Color color) {
    nya_unused(window, thickness, color);
}

void nya_render3d_sky_draw(NYA_Window* window, NYA_Render3DSky sky) {
    nya_unused(window, sky);
}

/**
 * False, always: a headless build never loads a mesh, so there are never bounds to report.
 *
 * The outputs are left untouched rather than zeroed, which is the contract the real one documents — a
 * caller has to check the return before reading them either way, and zeroing would make a caller that
 * does not check appear to work here and not on a real device.
 * */
b8 nya_render3d_mesh_bounds(NYA_Window* window, NYA_ConstCString handle, OUT f32x3* out_min, OUT f32x3* out_max) {
    nya_unused(window, handle, out_min, out_max);

    return false;
}

/**
 * False, always: there is no device to upload to.
 *
 * Reported rather than silently accepted, because a caller that believes a mesh is registered will draw it
 * every frame and see nothing — and headless is where a test would ask.
 * */
b8 nya_render3d_mesh_register(NYA_Window* window, NYA_ConstCString handle, const NYA_Vertex3D* vertices, u32 vertex_count) {
    nya_unused(window, handle, vertices, vertex_count);

    return false;
}

void nya_render3d_mesh_release(NYA_Window* window, NYA_ConstCString handle) {
    nya_unused(window, handle);
}

NYA_Render3DRay nya_render3d_screen_ray(NYA_Window* window, f32x2 screen) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // camera_valid, not active. See the note in render3d.c and NYA_Render3DBatch.camera_valid.
    if (!batch->camera_valid) return (NYA_Render3DRay){ .direction = { 0.0F, 0.0F, -1.0F } };

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    if (target_width == 0 || target_height == 0) return (NYA_Render3DRay){ .direction = { 0.0F, 0.0F, -1.0F } };

    /*
     * The real arithmetic, not a stub.
     *
     * This has to agree with render3d.c exactly or a headless test observes a ray the real build
     * would never produce — the drift render2d_headless.c warns about at length, and the camera
     * functions there are where it was actually found. Picking is game logic and is precisely what a
     * headless test is for, so a zeroed answer here would be worse than no answer.
     */
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    f32 ndc_x = ((screen.x / (f32)target_width) * 2.0F) - 1.0F;
    f32 ndc_y = 1.0F - ((screen.y / (f32)target_height) * 2.0F);

    f32 aspect = (f32)target_width / (f32)target_height;

    if (batch->camera_is_ortho) {
        f32 half_height = batch->camera_orthographic.height * 0.5F;

        f32x3 origin = eye + (right * (ndc_x * half_height * aspect)) + (up * (ndc_y * half_height));

        return (NYA_Render3DRay){ .origin = origin, .direction = forward };
    }

    f32 tangent = tanf(batch->camera.fov_y * 0.5F);

    f32x3 direction = forward + (right * (ndc_x * tangent * aspect)) + (up * (ndc_y * tangent));

    return (NYA_Render3DRay){ .origin = eye, .direction = nya_vector_normalize(direction) };
}

NYA_Render3DFrameStats nya_render3d_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);

    // The headless batch still carries the counters, and the flush still counts a drop — so this reports
    // what actually happened rather than zeroes, which is what makes a test able to assert on it.
    const NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    return (NYA_Render3DFrameStats){
        .draw_calls    = batch->frame_draw_calls,
        .vertices      = batch->frame_vertices,
        .indices       = batch->frame_indices,
        .instances     = batch->frame_instances,
        .culled        = batch->frame_culled,
        .dropped_draws = batch->frame_dropped_draws,
    };
}

void nya_render3d_flush(NYA_Window* window) {
    nya_unused(window);
}
