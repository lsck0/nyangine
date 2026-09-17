/**
 * @file render3d_headless.c
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

    // the same defaults as the real path, repeated because render3d.c is compiled instead of this file.
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

void nya_render3d_fog_set(NYA_Window* window, NYA_Render3DFog fog) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.fog = fog;
}

NYA_Render3DFog nya_render3d_fog(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.fog;
}

/*
 * The point lights are *stored* rather than ignored, like the light and the material above.
 */
/*
 * The shadow pass records its configuration and nothing else.
 */
void nya_render3d_shadow_begin(NYA_Window* window, NYA_Render3DShadow shadow) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.shadow = shadow;
}

void nya_render3d_shadow_end(NYA_Window* window) {
    nya_assert(window != nullptr);
}

void _nya_render3d_shadow_release(NYA_Window* window) {
    nya_assert(window != nullptr);
}

b8 nya_render3d_shadow_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return false;
}

/*
 * False: a headless shadow_begin only records configuration, so there is never a pass to be inside,
 * and callers that skip work during a shadow pass should skip nothing.
 */
b8 nya_render3d_shadow_pass_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return false;
}

void nya_render3d_point_light_add(NYA_Window* window, NYA_Render3DPointLight light) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (batch->point_light_count >= NYA_RENDER3D_MAX_POINT_LIGHTS) {
        nya_log_warn("A fifth point light was added and dropped; NYA_RENDER3D_MAX_POINT_LIGHTS is %d.", NYA_RENDER3D_MAX_POINT_LIGHTS);
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
 */
void nya_render3d_mesh(NYA_Window* window, NYA_ConstCString handle, f32x3 center, f32x3 scale, NYA_Quaternion rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    nya_unused(handle);
    nya_unused(center);
    nya_unused(scale);
    nya_unused(rotation);
    nya_unused(color);
}

void nya_render3d_skinned_mesh(NYA_Window* window, NYA_ConstCString handle, const f32_4x4* palette, u32 bone_count, f32_4x4 model,
                               NYA_Color tint) {
    nya_unused(window, handle, palette, bone_count, model, tint);
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

// Real rather than stubbed, both of them: the culling path is CPU only and a headless test is the
// only place it can be driven without a GPU. See render_occlusion.h.
void nya_render3d_occlusion(NYA_Window* window, const NYA_OcclusionBuffer* buffer) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.occlusion = buffer;
}

f32_4x4 nya_render3d_view_projection(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.view_projection;
}

void nya_render3d_sky_draw(NYA_Window* window, NYA_Render3DSky sky) {
    nya_unused(window, sky);
}

/**
 * False, always: a headless build never loads a mesh, so there are never bounds to report.
 * */
b8 nya_render3d_mesh_bounds(NYA_Window* window, NYA_ConstCString handle, OUT f32x3* out_min, OUT f32x3* out_max) {
    nya_unused(window, handle, out_min, out_max);

    return false;
}

/**
 * False, always: there is no device to upload to.
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

    // the headless batch still counts drops, so tests can assert on real numbers.
    const NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    return (NYA_Render3DFrameStats){
        .draw_calls    = batch->frame_draw_calls,
        .vertices      = batch->frame_vertices,
        .indices       = batch->frame_indices,
        .instances     = batch->frame_instances,
        .culled        = batch->frame_culled,
        .occluded      = batch->frame_occluded,
        .dropped_draws = batch->frame_dropped_draws,
    };
}

void nya_render3d_flush(NYA_Window* window) {
    nya_unused(window);
}
