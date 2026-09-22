#include "assets/shader/uniforms.h"

#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_internal.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts a recorded primitive: routes it by colour, makes room for `vertices` and `indices`, and records which passes
 * see its bounding sphere. False when no pass does, or it cannot fit an empty batch; nothing is written then. The
 * texture is part of the request so a primitive queued after a textured one cannot draw against its atlas by
 * accident. Null is the untextured pipeline.
 * */
NYA_INTERNAL b8 _nya_render3d_object_begin(NYA_Window* window, NYA_Color color, f32x3 center, f32 radius, u32 vertices, u32 indices,
                                           SDL_GPUTexture* texture, SDL_GPUSampler* sampler) __attr_no_discard;

/** Room for `vertices` more vertices and `indices` more indices, drawing the scene so far first if needed. */
NYA_INTERNAL b8 _nya_render3d_reserve(NYA_Window* window, u32 vertices, u32 indices, SDL_GPUTexture* texture, SDL_GPUSampler* sampler);

/** Two triangles over four corners, wound counter-clockwise seen from outside, into room already reserved. */
NYA_INTERNAL void _nya_render3d_quad_emit(NYA_Render3DBatch* batch, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color, b8 textured);

/** A square prism along a line, into room already reserved for its six quads. */
NYA_INTERNAL void _nya_render3d_line_emit(NYA_Render3DBatch* batch, f32x3 from, f32x3 to, f32 thickness, NYA_Color color);

/** Fills in whatever the caller left at zero, so a `{ 0 }` camera still renders something. */
NYA_INTERNAL NYA_Camera3DPerspective  _nya_render3d_camera_defaults(NYA_Camera3DPerspective camera);
NYA_INTERNAL NYA_Camera3DOrthographic _nya_render3d_camera_orthographic_defaults(NYA_Camera3DOrthographic camera);

/** Shared tail of both begin functions: flush 2D, store the view-projection, reset light and material. */
NYA_INTERNAL void _nya_render3d_begin_with(NYA_Window* window, f32_4x4 view_projection);

/** Creates the shadow map and its depth buffer if they are not there yet. False when the GPU refused. */
NYA_INTERNAL b8 _nya_render3d_shadow_ensure(NYA_Window* window);

/** Fits the cascades to the camera and the light, and builds every pass's frustum. Once per scene. */
NYA_INTERNAL void _nya_render3d_passes_prepare(NYA_Window* window);

/** nya_render3d_mesh_register without the reserved-handle check, so the renderer can fill its own slots. */
NYA_INTERNAL b8 _nya_render3d_mesh_register(NYA_Window* window, NYA_ConstCString handle, const NYA_Vertex3D* vertices, u32 vertex_count);

/** Whichever staging stream the primitives are currently writing into. See NYA_Render3DStream. */
NYA_INTERNAL NYA_Render3DStream* _nya_render3d_stream(NYA_Render3DBatch* batch) __attr_no_discard;

/** Orders a run of transparent triangles back to front in place. Nothing to do for fewer than two. */
NYA_INTERNAL void _nya_render3d_sort_transparent(NYA_Render3DBatch* batch, u16* indices, u32 index_count, f32x3 eye);

/** Starts a new segment at the end of what is recorded. */
NYA_INTERNAL void _nya_render3d_segment_open(NYA_Window* window);

/** Closes the open segment even if it recorded nothing, as a skinned draw does, and opens the next. */
NYA_INTERNAL void _nya_render3d_segment_close(NYA_Window* window);

/**
 * Draws everything recorded: one upload, then every shadow cascade and the camera pass from the same buffers, then
 * starts recording again. The open segment must be closed.
 * */
NYA_INTERNAL void _nya_render3d_playback(NYA_Window* window);

/** Draws the recorded segments into one pass: the camera for zero, cascade `pass - 1` otherwise. */
NYA_INTERNAL void _nya_render3d_pass_draw(NYA_Window* window, u32 pass);

/**
 * Copies the current colour target into the refraction capture, creating or resizing it. False when
 * there is nothing to capture from, in which case glass falls back to plain blending.
 * */
NYA_INTERNAL b8 _nya_render3d_refraction_capture(NYA_Window* window);

/** The registered mesh for `handle`, or null. */
NYA_INTERNAL NYA_Render3DRegisteredMesh* _nya_render3d_registered(NYA_Render3DBatch* batch, NYA_ConstCString handle) __attr_no_discard;

/**
 * The entry for `handle`, zeroed, or null when the table is full or the handle does not fit. An existing
 * registration is released first.
 * */
NYA_INTERNAL NYA_Render3DRegisteredMesh* _nya_render3d_registered_claim(NYA_Render3DBatch* batch, NYA_ConstCString handle) __attr_no_discard;

/** Bounds of a mesh already looked up as `registered`, or failing that `asset`. */
NYA_INTERNAL b8 _nya_render3d_resolved_bounds(const NYA_Render3DRegisteredMesh* registered, NYA_Asset* asset, OUT f32x3* out_min,
                                              OUT f32x3* out_max) __attr_no_discard;

/** The registry's destructor: the vertex buffer, and a staged copy that never got a frame. */
NYA_INTERNAL void _nya_render3d_registered_destroy(void* value, void* user_data);

/**
 * Creates a GPU vertex buffer and a transfer buffer filled with `vertices`. Does not copy: that needs a
 * command buffer, and this is reachable outside a frame. See NYA_Render3DRegisteredMesh.pending_upload.
 * */
NYA_INTERNAL b8 _nya_render3d_vertex_buffer_stage(
    const void*                 vertices,
    u32                         size,
    NYA_ConstCString            label,
    OUT SDL_GPUBuffer**         out_buffer,
    OUT SDL_GPUTransferBuffer** out_transfer
);

/** Performs a registered mesh's staged copy, if it has one. Must be called with a command buffer open. */
NYA_INTERNAL void _nya_render3d_registered_flush_upload(NYA_Window* window, NYA_Render3DRegisteredMesh* mesh);

/** Uploads a loaded mesh's vertices into a GPU buffer it then keeps. False when it could not. */
NYA_INTERNAL b8 _nya_render3d_mesh_upload(NYA_Window* window, NYA_Asset* asset);

/** The group for `handle` in the open segment, appending one if it is the first copy. Null when the table is full. */
NYA_INTERNAL NYA_Render3DMeshGroup* _nya_render3d_mesh_group(NYA_Render3DBatch* batch, NYA_ConstCString handle, b8 transparent);

/** The fragment uniform block, built from the batch's light and material. The shadow fields are the playback's. */
NYA_INTERNAL struct NYA_ShaderMesh3DUniform _nya_render3d_shading_uniform(const NYA_Window* window) __attr_no_discard;

/** The shadow atlas once the cascades are drawn, and a placeholder texel before that or without shadows. */
NYA_INTERNAL SDL_GPUTexture* _nya_render3d_shadow_map(const NYA_Render3DBatch* batch) __attr_no_discard;

/** Binds the shadow map, and the base colour before it when there is one. */
NYA_INTERNAL void _nya_render3d_bind_samplers(NYA_Window* window, SDL_GPUTexture* texture, SDL_GPUSampler* sampler);

/** Draws a segment's CPU-baked triangles in one pass. */
NYA_INTERNAL void _nya_render3d_immediate_draw(NYA_Window* window, const NYA_Render3DSegment* segment,
                                               const struct NYA_ShaderMesh3DUniform* uniform, u32 pass);

/** Draws a segment's mesh groups in one pass, a call per mesh part and run of visible copies. */
NYA_INTERNAL void _nya_render3d_instanced_draw(NYA_Window* window, const NYA_Render3DSegment* segment,
                                               const struct NYA_ShaderMesh3DUniform* uniform, u32 pass);

/** Draws a segment's posed mesh in one pass. */
NYA_INTERNAL void _nya_render3d_skinned_draw(NYA_Window* window, const NYA_Render3DSegment* segment,
                                             const struct NYA_ShaderMesh3DUniform* uniform, u32 pass);

/** The matrix a pass rasterises with. */
NYA_INTERNAL f32_4x4 _nya_render3d_pass_view_projection(const NYA_Render3DBatch* batch, u32 pass) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * FRAME
 * ─────────────────────────────────────────────────────────
 */

void nya_render3d_begin(NYA_Window* window, NYA_Camera3DPerspective camera) {
    nya_assert(window != nullptr);

    camera = _nya_render3d_camera_defaults(camera);

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    // from the target, not the window, so a render texture of another shape is not stretched. a minimised
    // window reports zero, and a zero aspect asserts in the projection.
    f32 aspect = target_height > 0 ? (f32)target_width / (f32)target_height : 1.0F;

    f32_4x4 projection = nya_matrix_perspective(camera.fov_y, aspect, camera.near_plane, camera.far_plane);
    f32_4x4 view       = nya_matrix_look_at(camera.position, camera.target, camera.up);

    _nya_render3d_begin_with(window, projection * view);

    // kept so nya_render3d_screen_ray can rebuild a ray from the camera basis instead of inverting a matrix.
    window->render_system.mesh_batch.camera          = camera;
    window->render_system.mesh_batch.camera_is_ortho = false;
    window->render_system.mesh_batch.camera_valid    = true;
}

void nya_render3d_begin_orthographic(NYA_Window* window, NYA_Camera3DOrthographic camera) {
    nya_assert(window != nullptr);

    camera = _nya_render3d_camera_orthographic_defaults(camera);

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    f32 aspect = target_height > 0 ? (f32)target_width / (f32)target_height : 1.0F;

    f32_4x4 projection = nya_matrix_orthographic_3d(camera.height, aspect, camera.near_plane, camera.far_plane);
    f32_4x4 view       = nya_matrix_look_at(camera.position, camera.target, camera.up);

    _nya_render3d_begin_with(window, projection * view);

    window->render_system.mesh_batch.camera_orthographic = camera;
    window->render_system.mesh_batch.camera_is_ortho     = true;
    window->render_system.mesh_batch.camera_valid        = true;
}

void nya_render3d_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;
    if (!batch->active) return;

    // drawn now, so render2d output afterwards lands in front. 2D pipelines do not test depth.
    nya_render3d_flush(window);
    _nya_render3d_playback(window);

    batch->active       = false;
    batch->passes_ready = false;

    // the shadow map expires with its scene, so a frame that stops casting shadows draws unshadowed at once.
    batch->shadow_valid = false;

    // and the cascade count, so a frame with fewer cascades does not index last frame's matrices.
    batch->shadow_cascade_count = 0;
}

b8 nya_render3d_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.active;
}

void nya_render3d_sky_draw(NYA_Window* window, NYA_Render3DSky sky) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // no camera, no basis to shade a ray from.
    if (!batch->active) return;

    // switched off the window's clear colour shows instead, which is what a sky is hiding.
    if (!nya_render_feature_enabled(window, NYA_RENDER_FEATURE_SKY)) return;

    u32 target_width  = 0;
    u32 target_height = 0;

    nya_render2d_target_size(window, &target_width, &target_height);

    if (target_width == 0 || target_height == 0) return;

    // the camera basis, rebuilt as nya_render3d_screen_ray does, so the sky and the picker agree.
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    // zero means unset, and the defaults are a clear midday sky.
    NYA_Color zenith  = sky.zenith.a > 0.0F ? sky.zenith : (NYA_Color){ 0.28F, 0.51F, 0.85F, 1.0F };
    NYA_Color horizon = sky.horizon.a > 0.0F ? sky.horizon : (NYA_Color){ 0.72F, 0.84F, 0.96F, 1.0F };
    NYA_Color ground  = sky.ground.a > 0.0F ? sky.ground : (NYA_Color){ 0.16F, 0.18F, 0.24F, 1.0F };
    NYA_Color sun     = sky.sun_color.a > 0.0F ? sky.sun_color : (NYA_Color){ 1.0F, 0.96F, 0.84F, 1.0F };

    f32x3 sun_direction = sky.sun_direction;

    if (nya_vector_length(sun_direction) < NYA_EPSILON) sun_direction = (f32x3){ 0.0F, 1.0F, 0.0F };

    sun_direction = nya_vector_normalize(sun_direction);

    // half a degree, life-size. the shader takes the cosine so it can compare against a dot product.
    f32 sun_angle = sky.sun_angle > 0.0F ? sky.sun_angle : 0.0087F;

    f32 tangent = batch->camera_is_ortho ? 0.0F : tanf(batch->camera.fov_y * 0.5F);

    struct NYA_ShaderSkyUniform uniform = {
        .camera_right_x = right.x,
        .camera_right_y = right.y,
        .camera_right_z = right.z,
        .tangent        = tangent,

        .camera_up_x = up.x,
        .camera_up_y = up.y,
        .camera_up_z = up.z,
        .aspect      = (f32)target_width / (f32)target_height,

        .camera_forward_x = forward.x,
        .camera_forward_y = forward.y,
        .camera_forward_z = forward.z,
        .horizon_softness = sky.horizon_softness > 0.0F ? sky.horizon_softness : 1.0F,

        .sun_direction_x = sun_direction.x,
        .sun_direction_y = sun_direction.y,
        .sun_direction_z = sun_direction.z,
        .sun_size        = cosf(sun_angle),

        .zenith_r      = zenith.r,
        .zenith_g      = zenith.g,
        .zenith_b      = zenith.b,
        .sun_sharpness = sky.sun_halo > 0.0F ? sky.sun_halo : 64.0F,

        .horizon_r    = horizon.r,
        .horizon_g    = horizon.g,
        .horizon_b    = horizon.b,
        .ground_blend = sky.ground_blend > 0.0F ? sky.ground_blend : 0.08F,

        .sun_r         = sun.r,
        .sun_g         = sun.g,
        .sun_b         = sun.b,
        .sun_intensity = sky.sun_intensity > 0.0F ? sky.sun_intensity : 1.0F,

        .ground_r = ground.r,
        .ground_g = ground.g,
        .ground_b = ground.b,
    };

    // drawn now and writing no depth, behind whatever the scene records, which draws at nya_render3d_end.
    nya_render2d_procedural(window, NYA_RENDER3D_PIPELINE_SKY, 3, &uniform, sizeof(uniform));
}

void nya_render3d_blend_set(NYA_Window* window, NYA_Render3DBlend blend) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // batch state: it selects the pipeline, which is per draw call.
    if (blend != batch->blend) nya_render3d_flush(window);

    batch->blend = blend;
}

void nya_render3d_depth_set(NYA_Window* window, NYA_Render3DDepth depth) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // batch state: it selects the pipeline, which is per draw call.
    if (depth != batch->depth) nya_render3d_flush(window);

    batch->depth = depth;
}

NYA_Render3DDepth nya_render3d_depth(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.depth;
}

void nya_render3d_billboard_resolved(NYA_Window* window, NYA_Render3DTextureBinding texture, f32x3 center, f32x2 size, f32 rotation,
                                     NYA_Color color) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    f32x2 half = size * 0.5F;

    // the half-diagonal bounds the quad however it spins in the view plane. the texture is resolved to a bound
    // texture because the batch changes segment on texture change, not on handle change.
    if (!_nya_render3d_object_begin(window, color, center, nya_vector_length(half), 4, 6, texture.texture, texture.sampler)) return;

    // the camera's right and up, so the quad faces the viewer, and every cascade casts the shadow of that same quad.
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    // spun in the view plane, so the quad stays flat to the camera.
    f32 cosine = cosf(rotation);
    f32 sine   = sinf(rotation);

    f32x3 axis_x = (right * cosine) + (up * sine);
    f32x3 axis_y = (up * cosine) - (right * sine);

    f32x3 offset_x = axis_x * half.x;
    f32x3 offset_y = axis_y * half.y;

    // wound toward the camera: the quad is single sided and back faces are culled.
    _nya_render3d_quad_emit(batch, center - offset_x - offset_y, center - offset_x + offset_y, center + offset_x + offset_y,
                            center + offset_x - offset_y, color, true);
}

NYA_Render3DTextureBinding nya_render3d_texture_resolve(NYA_ConstCString texture_handle) {
    if (texture_handle == nullptr) return (NYA_Render3DTextureBinding){ 0 };

    // resolved to a bound texture, since the batch flushes on texture change. a missing or loading texture
    // draws untextured, as nya_render3d_mesh does.
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)texture_handle);

    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_TEXTURE
        || asset->as_texture.texture == nullptr) {
        return (NYA_Render3DTextureBinding){ 0 };
    }

    return (NYA_Render3DTextureBinding){
        .texture = asset->as_texture.texture,
        .sampler = _nya_render_sampler_for(asset->as_texture.filter),
    };
}

void nya_render3d_billboard(NYA_Window* window, NYA_ConstCString texture_handle, f32x3 center, f32x2 size, f32 rotation, NYA_Color color) {
    // one lookup per call, fine for a handful and wrong for a crowd.
    nya_render3d_billboard_resolved(window, nya_render3d_texture_resolve(texture_handle), center, size, rotation, color);
}

void nya_render3d_light_set(NYA_Window* window, NYA_Render3DLight light) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // part of the per-draw fragment uniform, so what is queued draws under the light it was queued with.
    nya_render3d_flush(window);

    if (light.direction.x == 0.0F && light.direction.y == 0.0F && light.direction.z == 0.0F) light.direction = (f32x3){ 0.0F, -1.0F, 0.0F };
    if (light.intensity <= 0.0F) light.intensity = 1.0F;

    batch->light = light;
}

NYA_Render3DLight nya_render3d_light(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.light;
}

void nya_render3d_fog_set(NYA_Window* window, NYA_Render3DFog fog) {
    nya_assert(window != nullptr);
    nya_assert(fog.density >= 0.0F, "fog density is a rate, not a signed value: %f", (f64)fog.density);
    nya_assert(fog.height_falloff >= 0.0F, "fog height falloff thins upward and never thickens: %f", (f64)fog.height_falloff);
    nya_assert(fog.sun_amount >= 0.0F && fog.sun_amount <= 1.0F, "fog sun_amount is a mix in [0, 1]: %f", (f64)fog.sun_amount);

    // part of the per-draw fragment uniform, as with nya_render3d_light_set.
    nya_render3d_flush(window);

    // usually from a config file.
    fog.aerial = nya_clamp(fog.aerial, 0.0F, 16.0F);

    window->render_system.mesh_batch.fog = fog;
}

NYA_Render3DFog nya_render3d_fog(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.fog;
}

/**
 * The light a pass installs when the caller chose none. Shared by nya_render3d_begin and the shadow pass
 * so the shadow is always cast from the direction the scene is lit from.
 * */
NYA_INTERNAL NYA_Render3DLight _nya_render3d_default_light(void) {
    return (NYA_Render3DLight){
        .direction = NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT,
        .color     = NYA_COLOR_WHITE,
        // high on purpose: this is the darkest an object gets, and flat colours must still read. see
        // NYA_Render3DLight.ambient.
        .ambient   = 0.6F,
        .intensity = 1.0F,
    };
}

void nya_render3d_shadow_cast_set(NYA_Window* window, b8 casts_shadow) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // segment state: a cascade skips a whole segment.
    if (casts_shadow != batch->casts_shadow) nya_render3d_flush(window);

    batch->casts_shadow = casts_shadow;
}

b8 nya_render3d_shadow_casts(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.casts_shadow;
}

void _nya_render3d_passes_prepare(NYA_Window* window) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (batch->passes_ready) return;

    batch->passes_ready         = true;
    batch->pass_count           = 1;
    batch->shadow_cascade_count = 0;

    NYA_Render3DShadowFit fit = batch->shadow_fit;

    // the fit follows a perspective frustum; an orthographic view casts nothing. switched off, no cascade is
    // fitted and no scene pass runs, which is the whole cost of shadows.
    if (fit.strength <= 0.0F || batch->camera_is_ortho || !nya_render_feature_enabled(window, NYA_RENDER_FEATURE_SHADOWS)) return;

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    if (fit.aspect <= 0.0F && target_height > 0) fit.aspect = (f32)target_width / (f32)target_height;

    // the light the scene is lit by, or the default sun when the caller has not set one.
    NYA_Render3DLight light = batch->light;

    if (light.direction.x == 0.0F && light.direction.y == 0.0F && light.direction.z == 0.0F) light = _nya_render3d_default_light();

    u32 cascades = nya_render3d_shadow_options(window).cascades;

    for (u32 cascade = 0; cascade < cascades; cascade++) {
        NYA_Render3DShadow shadow = nya_render3d_shadow_for_camera(window, batch->camera, light.direction, cascade, fit);

        if (shadow.depth <= 0.0F) shadow.depth = shadow.extent * 4.0F;
        if (shadow.bias <= 0.0F) shadow.bias = NYA_RENDER3D_SHADOW_BIAS;

        shadow.strength = nya_clamp(shadow.strength, 0.0F, 1.0F);

        // the same function the headless tests use, so the rasterised and sampled matrices cannot drift.
        f32_4x4 view_projection = nya_render3d_shadow_view_projection(shadow.center, light.direction, shadow.extent, shadow.depth, nullptr);

        batch->shadow                          = shadow;
        batch->shadow_view_projection[cascade] = view_projection;
        batch->shadow_cascade_extent[cascade]  = shadow.extent;

        _nya_render3d_frustum_build(&batch->passes[cascade + 1], view_projection);
    }

    batch->shadow_cascade_count = cascades;
    batch->pass_count           = cascades + 1;
}

f32_4x4 _nya_render3d_pass_view_projection(const NYA_Render3DBatch* batch, u32 pass) {
    nya_assert(pass < batch->pass_count);

    return pass == 0 ? batch->view_projection : batch->shadow_view_projection[pass - 1];
}

void _nya_render3d_shadow_release(NYA_Window* window) {
    NYA_Render3DBatch* batch      = &window->render_system.mesh_batch;
    SDL_GPUDevice*     gpu_device = nya_app_get()->render_system.gpu_device;

    if (batch->shadow_color != nullptr) nya_gpu_texture_release(gpu_device, batch->shadow_color);
    if (batch->shadow_depth != nullptr) nya_gpu_texture_release(gpu_device, batch->shadow_depth);

    batch->shadow_color = nullptr;
    batch->shadow_depth = nullptr;

    // the matrices were fitted to the released atlas, so this frame's scene pass must not sample it.
    batch->shadow_valid         = false;
    batch->shadow_cascade_count = 0;
}

b8 _nya_render3d_shadow_ensure(NYA_Window* window) {
    nya_trace_scope(NYA_TRACE_SHADOWS);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (batch->shadow_color != nullptr && batch->shadow_depth != nullptr) return true;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;
    if (gpu_device == nullptr) return false;

    /*
     * The atlas is a strip, one cascade wide per cascade. A texture array would need array-texture support on
     * every backend, and a square atlas wastes a quadrant at three cascades. mesh3d_shadow insets the filter so a
     * cascade's kernel does not read its neighbour.
     */
    NYA_Render3DShadowOptions options = nya_render3d_shadow_options(window);

    u32 atlas_width  = options.map_size * options.cascades;
    u32 atlas_height = options.map_size;

    // single sampled: a depth map has nothing to antialias, and multisampling would need a resolve to read.
    batch->shadow_color = nya_gpu_texture_create(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = NYA_RENDER3D_SHADOW_FORMAT,
            .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = atlas_width,
            .height               = atlas_height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = SDL_GPU_SAMPLECOUNT_1,
        }
    );

    if (batch->shadow_color == nullptr) {
        nya_log_error("SDL_CreateGPUTexture() failed for the shadow map: %s", SDL_GetError());
        return false;
    }

    batch->shadow_depth = nya_gpu_texture_create(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = nya_app_get()->render_system.depth_format,
            .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            .width                = atlas_width,
            .height               = atlas_height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = SDL_GPU_SAMPLECOUNT_1,
        }
    );

    if (batch->shadow_depth == nullptr) {
        nya_log_error("SDL_CreateGPUTexture() failed for the shadow map's depth buffer: %s", SDL_GetError());

        nya_gpu_texture_release(gpu_device, batch->shadow_color);
        batch->shadow_color = nullptr;
        return false;
    }

    batch->shadow_atlas = options;

    nya_log_debug("Shadow atlas created at %ux%u: %u cascades of %ux%u.", atlas_width, atlas_height, options.cascades, options.map_size,
                  options.map_size);

    return true;
}

void nya_render3d_point_light_add(NYA_Window* window, NYA_Render3DPointLight light) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // logged, so a missing light is not debugged as a wrong colour or position.
    if (batch->point_light_count >= NYA_RENDER3D_MAX_POINT_LIGHTS) {
        nya_log_warn("A fifth point light was added and dropped; NYA_RENDER3D_MAX_POINT_LIGHTS is %d.", NYA_RENDER3D_MAX_POINT_LIGHTS);
        return;
    }

    // the light set is a fragment uniform, so queued draws were lit by the previous set.
    nya_render3d_flush(window);

    batch->point_lights[batch->point_light_count++] = light;
}

void nya_render3d_point_lights_clear(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (batch->point_light_count == 0) return;

    nya_render3d_flush(window);

    batch->point_light_count = 0;
}

u32 nya_render3d_point_light_count(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.point_light_count;
}

void nya_render3d_material_set(NYA_Window* window, NYA_Render3DMaterial material) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    nya_render3d_flush(window);

    // clamped rather than asserted, since designers drag these past their ends.
    material.metallic  = nya_clamp(material.metallic, 0.0F, 1.0F);
    material.roughness = nya_clamp(material.roughness, 0.02F, 1.0F);

    // not clamped to one: above one lifts an emissive surface past the bloom threshold.
    material.emission = nya_max(material.emission, 0.0F);

    material.edge = nya_clamp(material.edge, 0.0F, 1.0F);

    // zero means unset here. 0.5 is the glTF default.
    if (material.reflectance <= 0.0F) material.reflectance = 0.5F;

    batch->material = material;
}

NYA_Render3DMaterial nya_render3d_material(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.material;
}

/*
 * ─────────────────────────────────────────────────────────
 * PRIMITIVES
 * ─────────────────────────────────────────────────────────
 */

void nya_render3d_cube(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    f32x3 half = size * 0.5F;

    // culled once for the whole cube rather than per face. the half-diagonal is rotation invariant.
    if (!_nya_render3d_object_begin(window, color, center, nya_vector_length(half), 24, 36, nullptr, nullptr)) return;

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // rotated on the CPU: a model matrix is a per-draw uniform, and a draw call per cube is what this avoids.
    f32x3 corners[8];
    for (u32 i = 0; i < 8; i++) {
        f32x3 local = {
            (i & 1) ? half.x : -half.x,
            (i & 2) ? half.y : -half.y,
            (i & 4) ? half.z : -half.z,
        };

        corners[i] = center + nya_quaternion_rotate(rotation, local);
    }

    // counter-clockwise from outside, or back-face culling removes the visible face.
    _nya_render3d_quad_emit(batch, corners[0], corners[2], corners[3], corners[1], color, false); // -z
    _nya_render3d_quad_emit(batch, corners[5], corners[7], corners[6], corners[4], color, false); // +z
    _nya_render3d_quad_emit(batch, corners[4], corners[6], corners[2], corners[0], color, false); // -x
    _nya_render3d_quad_emit(batch, corners[1], corners[3], corners[7], corners[5], color, false); // +x
    _nya_render3d_quad_emit(batch, corners[0], corners[1], corners[5], corners[4], color, false); // -y
    _nya_render3d_quad_emit(batch, corners[6], corners[7], corners[3], corners[2], color, false); // +y
}

void nya_render3d_cube_outline(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) thickness = 0.02F;

    // one object for the outline instead of one per edge.
    if (!_nya_render3d_object_begin(window, color, center, nya_vector_length(size * 0.5F) + thickness, 12 * 24, 12 * 36, nullptr, nullptr)) return;

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    f32x3 half = size * 0.5F;

    f32x3 corners[8];
    for (u32 i = 0; i < 8; i++) {
        f32x3 local = {
            (i & 1) ? half.x : -half.x,
            (i & 2) ? half.y : -half.y,
            (i & 4) ? half.z : -half.z,
        };

        corners[i] = center + nya_quaternion_rotate(rotation, local);
    }

    // the edges as corner index pairs, checkable against a drawing.
    static const u32 edges[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, // along x
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, // along y
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, // along z
    };

    for (u32 i = 0; i < 12; i++) _nya_render3d_line_emit(batch, corners[edges[i][0]], corners[edges[i][1]], thickness, color);
}

/** Builds and registers the unit sphere if it is not registered already. */
NYA_INTERNAL b8 _nya_render3d_unit_sphere_ensure(NYA_Window* window) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (_nya_render3d_registered(batch, NYA_RENDER3D_MESH_UNIT_SPHERE) != nullptr) return true;

    const u32 segments = NYA_RENDER3D_SPHERE_SEGMENTS;
    const u32 rings    = NYA_RENDER3D_SPHERE_SEGMENTS / 2;

    // six vertices per quad: nya_render3d_mesh_register takes a plain vertex list.
    u32 vertex_count = segments * rings * 6;

    NYA_Vertex3D* vertices = nya_arena_alloc(nya_app_get()->frame_allocator, vertex_count * sizeof(NYA_Vertex3D));
    if (vertices == nullptr) return false;

    u32 at = 0;

    for (u32 ring = 0; ring < rings; ring++) {
        f32 phi_0 = (f32)M_PI * ((f32)ring / (f32)rings);
        f32 phi_1 = (f32)M_PI * ((f32)(ring + 1) / (f32)rings);

        for (u32 segment = 0; segment < segments; segment++) {
            f32 theta_0 = 2.0F * (f32)M_PI * ((f32)segment / (f32)segments);
            f32 theta_1 = 2.0F * (f32)M_PI * ((f32)(segment + 1) / (f32)segments);

            // on a unit sphere the normal is the position.
            f32x3 a = { sinf(phi_0) * cosf(theta_0), cosf(phi_0), sinf(phi_0) * sinf(theta_0) };
            f32x3 b = { sinf(phi_0) * cosf(theta_1), cosf(phi_0), sinf(phi_0) * sinf(theta_1) };
            f32x3 c = { sinf(phi_1) * cosf(theta_1), cosf(phi_1), sinf(phi_1) * sinf(theta_1) };
            f32x3 d = { sinf(phi_1) * cosf(theta_0), cosf(phi_1), sinf(phi_1) * sinf(theta_0) };

            // white, because the draw tint multiplies it.
            vertices[at++] = nya_vertex3d(a, NYA_COLOR_WHITE, a, f32x2_zero);
            vertices[at++] = nya_vertex3d(b, NYA_COLOR_WHITE, b, f32x2_zero);
            vertices[at++] = nya_vertex3d(c, NYA_COLOR_WHITE, c, f32x2_zero);
            vertices[at++] = nya_vertex3d(a, NYA_COLOR_WHITE, a, f32x2_zero);
            vertices[at++] = nya_vertex3d(c, NYA_COLOR_WHITE, c, f32x2_zero);
            vertices[at++] = nya_vertex3d(d, NYA_COLOR_WHITE, d, f32x2_zero);
        }
    }

    nya_assert(at == vertex_count, "the unit sphere emitted %u vertices, not the %u it sized for", at, vertex_count);

    return _nya_render3d_mesh_register(window, NYA_RENDER3D_MESH_UNIT_SPHERE, vertices, vertex_count);
}

void nya_render3d_sphere(NYA_Window* window, f32x3 center, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    if (radius <= 0.0F) return;

    // registered on first use, so a scene without spheres pays nothing.
    if (!_nya_render3d_unit_sphere_ensure(window)) return;

    // uniform scale by the radius keeps the normals valid.
    nya_render3d_mesh(window, NYA_RENDER3D_MESH_UNIT_SPHERE, center, (f32x3){ radius, radius, radius }, nya_quaternion_identity, color);
}

void nya_render3d_plane(NYA_Window* window, f32x3 center, f32x2 size, NYA_Color color) {
    nya_assert(window != nullptr);

    f32x2 half = size * 0.5F;

    if (!_nya_render3d_object_begin(window, color, center, nya_vector_length(half), 4, 6, nullptr, nullptr)) return;

    _nya_render3d_quad_emit(
        &window->render_system.mesh_batch,
        center + (f32x3){ -half.x, 0.0F, half.y },
        center + (f32x3){ half.x, 0.0F, half.y },
        center + (f32x3){ half.x, 0.0F, -half.y },
        center + (f32x3){ -half.x, 0.0F, -half.y },
        color,
        false
    );
}

void nya_render3d_triangle(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, NYA_Color color) {
    nya_assert(window != nullptr);

    /*
     * Culled even for one triangle: a surface of thousands has most behind the camera. Six dot products are
     * cheaper than the vertex writes they save above roughly one hit in twenty. The circumcircle is loose for
     * thin triangles, which is the safe direction.
     */
    f32x3 centroid = (a + b + c) / 3.0F;

    f32 radius = nya_max(nya_vector_length(a - centroid), nya_max(nya_vector_length(b - centroid), nya_vector_length(c - centroid)));

    if (!_nya_render3d_object_begin(window, color, centroid, radius, 3, 3, nullptr, nullptr)) return;

    NYA_Render3DStream* stream = _nya_render3d_stream(&window->render_system.mesh_batch);

    /*
     * The face normal from the winding. A degenerate triangle normalizes to zero and shades as unlit, which
     * is cheaper than checking every triangle's length.
     */
    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    u32 base = stream->vertex_count;

    stream->vertices[base + 0] = nya_vertex3d(a, color, normal, f32x2_zero);
    stream->vertices[base + 1] = nya_vertex3d(b, color, normal, f32x2_zero);
    stream->vertices[base + 2] = nya_vertex3d(c, color, normal, f32x2_zero);

    stream->indices[stream->index_count++] = (u16)(base + 0);
    stream->indices[stream->index_count++] = (u16)(base + 1);
    stream->indices[stream->index_count++] = (u16)(base + 2);

    stream->vertex_count += 3;
}

void _nya_render3d_quad_emit(NYA_Render3DBatch* batch, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color, b8 textured) {
    NYA_Render3DStream* stream = _nya_render3d_stream(batch);

    // flat shading on purpose: shared normals would round a cube's corners. smooth meshes bring normals.
    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    u32 base = stream->vertex_count;

    // v grows down in a texture and up in the quad, so the first corner takes v = 1.
    stream->vertices[base + 0] = nya_vertex3d(a, color, normal, textured ? (f32x2){ 0.0F, 1.0F } : f32x2_zero);
    stream->vertices[base + 1] = nya_vertex3d(b, color, normal, f32x2_zero);
    stream->vertices[base + 2] = nya_vertex3d(c, color, normal, textured ? (f32x2){ 1.0F, 0.0F } : f32x2_zero);
    stream->vertices[base + 3] = nya_vertex3d(d, color, normal, textured ? (f32x2){ 1.0F, 1.0F } : f32x2_zero);

    u16* indices = stream->indices + stream->index_count;

    indices[0] = (u16)(base + 0);
    indices[1] = (u16)(base + 1);
    indices[2] = (u16)(base + 2);
    indices[3] = (u16)(base + 0);
    indices[4] = (u16)(base + 2);
    indices[5] = (u16)(base + 3);

    stream->vertex_count += 4;
    stream->index_count  += 6;
}

void nya_render3d_quad(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color) {
    nya_assert(window != nullptr);

    f32x3 center = (a + b + c + d) * 0.25F;

    f32 radius = nya_max(nya_max(nya_vector_length(a - center), nya_vector_length(b - center)),
                         nya_max(nya_vector_length(c - center), nya_vector_length(d - center)));

    if (!_nya_render3d_object_begin(window, color, center, radius, 4, 6, nullptr, nullptr)) return;

    _nya_render3d_quad_emit(&window->render_system.mesh_batch, a, b, c, d, color, false);
}

void _nya_render3d_line_emit(NYA_Render3DBatch* batch, f32x3 from, f32x3 to, f32 thickness, NYA_Color color) {
    f32x3 along  = to - from;
    f32   length = nya_vector_length(along);

    f32x3 forward = along / nya_max(length, NYA_EPSILON);

    // crossed against the world axis the line is least aligned with, so a vertical line does not collapse.
    f32x3 reference = fabsf(forward.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 right = nya_vector_normalize(nya_vector_cross(forward, reference)) * (thickness * 0.5F);
    f32x3 up    = nya_vector_normalize(nya_vector_cross(forward, right)) * (thickness * 0.5F);

    f32x3 corners[8] = {
        from - right - up, from + right - up, from - right + up, from + right + up,
        to - right - up,   to + right - up,   to - right + up,   to + right + up,
    };

    _nya_render3d_quad_emit(batch, corners[0], corners[2], corners[3], corners[1], color, false);
    _nya_render3d_quad_emit(batch, corners[5], corners[7], corners[6], corners[4], color, false);
    _nya_render3d_quad_emit(batch, corners[4], corners[6], corners[2], corners[0], color, false);
    _nya_render3d_quad_emit(batch, corners[1], corners[3], corners[7], corners[5], color, false);
    _nya_render3d_quad_emit(batch, corners[0], corners[1], corners[5], corners[4], color, false);
    _nya_render3d_quad_emit(batch, corners[6], corners[7], corners[3], corners[2], color, false);
}

void nya_render3d_line(NYA_Window* window, f32x3 from, f32x3 to, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    f32 length = nya_vector_length(to - from);

    if (length < NYA_EPSILON) return;
    if (thickness <= 0.0F) thickness = 0.02F;

    // one test for the whole prism. thickness is added so a line seen end-on is not culled.
    if (!_nya_render3d_object_begin(window, color, (from + to) * 0.5F, (length * 0.5F) + thickness, 24, 36, nullptr, nullptr)) return;

    _nya_render3d_line_emit(&window->render_system.mesh_batch, from, to, thickness, color);
}

/*
 * uniforms.h cannot include engine headers, so the bone cap is written twice. A mismatch would misread the
 * uniform block as garbage geometry, so it is checked here.
 */
static_assert(NYA_SHADER_SKIN_MAX_BONES == NYA_SKELETON_MAX_BONES,
              "the shader's bone palette and NYA_SKELETON_MAX_BONES have drifted apart");

/**
 * Draws a skinned mesh through `palette` (see core_skeleton.h). Recorded as a segment of its own and drawn un-instanced
 * in every pass, since every copy of a character has its own pose.
 * */
void nya_render3d_skinned_mesh(NYA_Window* window, NYA_ConstCString handle, const f32_4x4* palette, u32 bone_count, f32_4x4 model,
                               NYA_Color tint) {
    nya_trace_scope(NYA_TRACE_SKINNING);

    nya_assert(window != nullptr);

    if (handle == nullptr || palette == nullptr || bone_count == 0) return;

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (!batch->active) return;

    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)handle);

    // not loaded yet is not an error.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_MESH) return;

    if (asset->as_mesh.skinned_vertices == nullptr || asset->as_mesh.skeleton == nullptr) {
        nya_log_error("'%s' is not a skinned mesh; draw it with nya_render3d_mesh.", handle);
        return;
    }

    if (asset->as_mesh.vertex_count == 0) return;

    // the vertex buffer never changes, only the pose, which travels as a uniform.
    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(batch, handle);

    if (registered == nullptr) {
        SDL_GPUBuffer*         buffer   = nullptr;
        SDL_GPUTransferBuffer* transfer = nullptr;

        u32 size = asset->as_mesh.vertex_count * (u32)sizeof(NYA_VertexSkinned3D);

        if (!_nya_render3d_vertex_buffer_stage(asset->as_mesh.skinned_vertices, size, handle, &buffer, &transfer)) return;

        registered = _nya_render3d_registered_claim(batch, handle);
        if (registered == nullptr) {
            SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;
            nya_gpu_buffer_release(gpu_device, buffer);
            nya_gpu_transfer_buffer_release(gpu_device, transfer);
            return;
        }

        registered->vertices       = buffer;
        registered->vertex_count   = asset->as_mesh.vertex_count;
        registered->pending_upload = transfer;
        registered->pending_size   = size;
    }

    _nya_render3d_registered_flush_upload(window, registered);

    // in the frame arena, since it is only read by this scene's playback.
    struct NYA_ShaderSkinUniform* skin = nya_arena_alloc(nya_app_get()->frame_allocator, sizeof(struct NYA_ShaderSkinUniform));
    if (skin == nullptr) return;

    // the palette, three rows a bone, at most 64.
    u32 bones = bone_count < NYA_SHADER_SKIN_MAX_BONES ? bone_count : NYA_SHADER_SKIN_MAX_BONES;

    for (u32 b = 0; b < bones; b++) {
        // the model transform folded into each bone on the CPU, so the vertex stage does no extra multiply.
        f32_4x4 placed = model * palette[b];

        for (u32 row = 0; row < 3; row++) {
            for (u32 column = 0; column < 4; column++) skin->bones[b][row][column] = placed[row][column];
        }
    }

    // unfilled slots get the model transform, so a vertex weighted to an unset bone stays with the model
    // instead of collapsing to the origin.
    for (u32 b = bones; b < NYA_SHADER_SKIN_MAX_BONES; b++) {
        for (u32 row = 0; row < 3; row++) {
            for (u32 column = 0; column < 4; column++) skin->bones[b][row][column] = model[row][column];
        }
    }

    skin->tint_r = tint.r;
    skin->tint_g = tint.g;
    skin->tint_b = tint.b;
    skin->tint_a = tint.a;

    /*
     * Which passes see it, from the pose rather than from the rest bounds.
     *
     * A skinned mesh is wherever its bones are, and the rest bounds only say where it stands before it
     * is animated. The bone origins are already in world space here — `placed` folded the model in — so
     * their box is where the skeleton actually is, and padding it by the rest model's own radius covers
     * the skin hanging off each bone. Loose on purpose: a bound that is too big costs a draw that could
     * have been skipped, and one that is too small deletes a limb from a shadow.
     *
     * Without this the posed mesh was recorded for every pass, camera and all three shadow cascades, and
     * drawn whether or not it was anywhere near any of them.
     */
    _nya_render3d_passes_prepare(window);

    // a mesh whose bounds are not known yet is seen by every pass, as an instanced one is.
    u8 passes = (u8)((1U << batch->pass_count) - 1U);

    f32x3 rest_min = f32x3_zero;
    f32x3 rest_max = f32x3_zero;
    (void)_nya_render3d_resolved_bounds(registered, asset, &rest_min, &rest_max);

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    if (nya_render3d_skinned_bounds(palette, bone_count, model, rest_min, rest_max, &center, &radius)) {
        passes = _nya_render3d_passes_seeing(window, center, radius);
        if (passes == 0) return;
    }

    // what came before draws before it, as its own segment.
    nya_render3d_flush(window);

    NYA_Render3DSegment* segment = &batch->segments[batch->segment_count];

    segment->skinned        = handle;
    segment->skin           = skin;
    segment->skinned_passes = passes;

    _nya_render3d_segment_close(window);
}

void nya_render3d_mesh(NYA_Window* window, NYA_ConstCString handle, f32x3 center, f32x3 scale, NYA_Quaternion rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    if (handle == nullptr) return;

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (!batch->active) return;

    if (nya_render3d_lod_count() > 0) {
        f32x3 eye    = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
        f32x3 to_eye = center - eye;

        handle = nya_render3d_lod_select_squared(handle, nya_vector_dot(to_eye, to_eye));

        // Past the last level, which is the chain's draw distance.
        if (handle == nullptr) {
            batch->frame_culled++;
            return;
        }
    }

    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(batch, handle);

    NYA_Asset* asset = nullptr;

    if (registered != nullptr) {
        // registered outside a frame, so the copy happens on this first draw.
        _nya_render3d_registered_flush_upload(window, registered);
    } else {
        asset = nya_asset_get((NYA_AssetHandle)handle);

        if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_MESH) return;

        if (asset->as_mesh.part_count == 0 || asset->as_mesh.vertex_count == 0) return;
    }

    /*
     * Records an instance rather than emitting vertices. The model is uploaded once, and a draw appends a model
     * matrix and a tint. The immediate batch is for geometry generated every frame, which has nothing to reuse.
     */
    _nya_render3d_passes_prepare(window);

    // a mesh without bounds yet is seen by every pass.
    u8 passes = (u8)((1U << batch->pass_count) - 1U);

    f32x3 bounds_min;
    f32x3 bounds_max;

    if (_nya_render3d_resolved_bounds(registered, asset, &bounds_min, &bounds_max)) {
        f32x3 extent = (bounds_max - bounds_min) * scale * 0.5F;
        f32x3 middle = (bounds_max + bounds_min) * scale * 0.5F;

        // a sphere around the scaled bounds does not change as the object turns. looser than a rotated box, which
        // is the safe direction.
        f32x3 world_center = center + nya_quaternion_rotate(rotation, middle);

        passes = _nya_render3d_passes_seeing(window, world_center, nya_vector_length(extent));

        if (passes == 0) return;
    }

    // uploaded on first draw because the copy needs a frame, which the asset system does not have.
    if (asset != nullptr && asset->as_mesh.gpu_vertices == nullptr && !_nya_render3d_mesh_upload(window, asset)) return;

    // full: what is recorded draws now, and this copy starts the next playback.
    if (batch->instance_count >= NYA_RENDER3D_MAX_INSTANCES || batch->mesh_group_count >= NYA_RENDER3D_MAX_MESH_GROUPS) {
        nya_render3d_flush(window);
        _nya_render3d_playback(window);
    }

    // the tint's alpha picks the pass, by the same rule as primitives.
    NYA_Render3DMeshGroup* group = _nya_render3d_mesh_group(batch, handle, _nya_render3d_stream_transparent(color, batch->blend));

    /*
     * Appended at the end of the instance array. A group's instances must be contiguous because a draw names a
     * first instance and a count, so _nya_render3d_mesh_group only returns the last group or a new one.
     */
    batch->instances[batch->instance_count] = (NYA_Render3DInstance){
        .model = nya_matrix_transform(center, nya_quaternion_to_matrix3(rotation), scale),
        .tint  = color,
    };

    batch->instance_passes[batch->instance_count] = passes;

    // transparent groups draw back to front by their furthest copy. instances in a group are sorted too, since
    // groups can interleave in depth.
    f32x3 eye    = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 offset = center - eye;

    f32 depth = nya_vector_dot(offset, offset);

    if (group->instance_count == 0 || depth > group->depth) group->depth = depth;

    batch->instance_count++;
    group->instance_count++;
}

b8 nya_render3d_mesh_register(NYA_Window* window, NYA_ConstCString handle, const NYA_Vertex3D* vertices, u32 vertex_count) {
    // the unit sphere's slot belongs to the renderer. _nya_render3d_unit_sphere_ensure fills it below this check.
    if (handle != nullptr && nya_string_equals(handle, NYA_RENDER3D_MESH_UNIT_SPHERE)) {
        nya_log_error("'%s' is reserved for nya_render3d_sphere's shared geometry; pick another handle.", handle);
        return false;
    }

    return _nya_render3d_mesh_register(window, handle, vertices, vertex_count);
}

NYA_INTERNAL b8 _nya_render3d_mesh_register(NYA_Window* window, NYA_ConstCString handle, const NYA_Vertex3D* vertices, u32 vertex_count) {
    nya_assert(window != nullptr);

    if (handle == nullptr || vertices == nullptr || vertex_count == 0) {
        nya_log_error("nya_render3d_mesh_register was given nothing to register.");
        return false;
    }

    if (vertex_count % 3 != 0) {
        nya_log_error("'%s' has %u vertices, which is not a whole number of triangles.", handle, vertex_count);
        return false;
    }

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    SDL_GPUBuffer*         buffer   = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;

    // the old buffers stay until the new ones exist, so a failed re-registration keeps the old geometry.
    if (!_nya_render3d_vertex_buffer_stage(vertices, (u32)(vertex_count * sizeof(NYA_Vertex3D)), handle, &buffer, &transfer)) return false;

    // releases a registration being replaced, including a copy that never ran.
    NYA_Render3DRegisteredMesh* slot = _nya_render3d_registered_claim(batch, handle);

    if (slot == nullptr) {
        SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;
        nya_gpu_buffer_release(gpu_device, buffer);
        nya_gpu_transfer_buffer_release(gpu_device, transfer);
        return false;
    }

    f32x3 min = nya_vertex3d_position(vertices[0]);
    f32x3 max = min;

    for (u32 i = 1; i < vertex_count; i++) {
        f32x3 position = nya_vertex3d_position(vertices[i]);

        min = (f32x3){ nya_min(min.x, position.x), nya_min(min.y, position.y), nya_min(min.z, position.z) };
        max = (f32x3){ nya_max(max.x, position.x), nya_max(max.y, position.y), nya_max(max.z, position.z) };
    }

    slot->vertices       = buffer;
    slot->vertex_count   = vertex_count;
    slot->pending_upload = transfer;
    slot->pending_size   = (u32)(vertex_count * sizeof(NYA_Vertex3D));
    slot->bounds_min     = min;
    slot->bounds_max     = max;

    // done now inside a frame, otherwise on the first draw.
    _nya_render3d_registered_flush_upload(window, slot);

    nya_log_debug("Registered '%s': %u vertices, %u KiB, kept until it is released or replaced.", handle, vertex_count,
              (u32)(vertex_count * sizeof(NYA_Vertex3D)) / 1024);

    return true;
}

void nya_render3d_mesh_release(NYA_Window* window, NYA_ConstCString handle) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (_nya_render3d_registered(batch, handle) == nullptr) return;

    b8 removed = nya_cache_remove(batch->registered_meshes, handle, strlen(handle));
    nya_assert(removed, "'%s' was registered a moment ago", handle);
}

b8 nya_render3d_mesh_bounds(NYA_Window* window, NYA_ConstCString handle, OUT f32x3* out_min, OUT f32x3* out_max) {
    nya_assert(window != nullptr);
    nya_assert(out_min != nullptr && out_max != nullptr);

    if (handle == nullptr) return false;

    // registered geometry first, so a registration shadows an asset with the same handle.
    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(&window->render_system.mesh_batch, handle);
    NYA_Asset*                  asset      = registered == nullptr ? nya_asset_get((NYA_AssetHandle)handle) : nullptr;

    return _nya_render3d_resolved_bounds(registered, asset, out_min, out_max);
}

b8 _nya_render3d_resolved_bounds(const NYA_Render3DRegisteredMesh* registered, NYA_Asset* asset, OUT f32x3* out_min, OUT f32x3* out_max) {
    if (registered != nullptr) {
        *out_min = registered->bounds_min;
        *out_max = registered->bounds_max;

        return true;
    }

    // not loaded yet: a queued load lands at the end of the frame.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_MESH) return false;

    if (asset->as_mesh.vertex_count == 0) return false;

    // cached on the asset since culling asks every draw of every pass. a reload zeroes it.
    if (asset->as_mesh.bounds_valid) {
        *out_min = asset->as_mesh.bounds_min;
        *out_max = asset->as_mesh.bounds_max;

        return true;
    }

    f32x3 min = asset->as_mesh.positions[0];
    f32x3 max = min;

    for (u32 i = 1; i < asset->as_mesh.vertex_count; i++) {
        f32x3 position = asset->as_mesh.positions[i];

        min = (f32x3){ nya_min(min.x, position.x), nya_min(min.y, position.y), nya_min(min.z, position.z) };
        max = (f32x3){ nya_max(max.x, position.x), nya_max(max.y, position.y), nya_max(max.z, position.z) };
    }

    asset->as_mesh.bounds_min   = min;
    asset->as_mesh.bounds_max   = max;
    asset->as_mesh.bounds_valid = true;

    *out_min = min;
    *out_max = max;

    return true;
}

void nya_render3d_grid(NYA_Window* window, u32 half_extent, f32 cell_size, NYA_Color color) {
    nya_assert(window != nullptr);

    if (cell_size <= 0.0F) return;

    f32 extent = (f32)half_extent * cell_size;

    // thin relative to the cell and scaled with it, so any grid size reads as lines.
    f32 thickness = cell_size * 0.02F;

    for (u32 i = 0; i <= half_extent * 2; i++) {
        f32 offset = ((f32)i * cell_size) - extent;

        nya_render3d_line(window, (f32x3){ offset, 0.0F, -extent }, (f32x3){ offset, 0.0F, extent }, thickness, color);
        nya_render3d_line(window, (f32x3){ -extent, 0.0F, offset }, (f32x3){ extent, 0.0F, offset }, thickness, color);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * PICKING
 * ─────────────────────────────────────────────────────────
 */

NYA_Render3DRay nya_render3d_screen_ray(NYA_Window* window, f32x2 screen) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // `camera_valid`, not `active`: clicks arrive in on_event, a phase before on_render sets the camera, so
    // the ray uses last frame's camera, which is what the player saw.
    if (!batch->camera_valid) return (NYA_Render3DRay){ .direction = { 0.0F, 0.0F, -1.0F } };

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    if (target_width == 0 || target_height == 0) return (NYA_Render3DRay){ .direction = { 0.0F, 0.0F, -1.0F } };

    // from the camera basis: inverting a perspective matrix with a small near plane loses precision near the
    // camera, which is where a picker's ray starts.
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    // normalized device coordinates, y flipped because screen y grows down.
    f32 ndc_x = ((screen.x / (f32)target_width) * 2.0F) - 1.0F;
    f32 ndc_y = 1.0F - ((screen.y / (f32)target_height) * 2.0F);

    f32 aspect = (f32)target_width / (f32)target_height;

    if (batch->camera_is_ortho) {
        // orthographic rays are parallel: the pixel picks the origin, not the direction.
        f32 half_height = batch->camera_orthographic.height * 0.5F;

        f32x3 origin = eye + (right * (ndc_x * half_height * aspect)) + (up * (ndc_y * half_height));

        return (NYA_Render3DRay){ .origin = origin, .direction = forward };
    }

    f32 tangent = tanf(batch->camera.fov_y * 0.5F);

    f32x3 direction = forward + (right * (ndc_x * tangent * aspect)) + (up * (ndc_y * tangent));

    return (NYA_Render3DRay){ .origin = eye, .direction = nya_vector_normalize(direction) };
}

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM
 * ─────────────────────────────────────────────────────────
 */

NYA_Render3DFrameStats nya_render3d_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    return (NYA_Render3DFrameStats){
        .draw_calls    = batch->frame_draw_calls,
        .vertices      = batch->frame_vertices,
        .indices       = batch->frame_indices,
        .instances     = batch->frame_instances,
        .culled        = batch->frame_culled,
        .occluded      = batch->frame_occluded,
        .dropped_draws = batch->frame_dropped_draws,
        .passes        = batch->frame_passes,
    };
}

void nya_render3d_flush(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow*    render = &window->render_system;
    NYA_Render3DBatch*         batch  = &render->mesh_batch;
    const NYA_Render3DSegment* open   = &batch->segments[batch->segment_count];

    // nothing recorded since the last change, so the open segment simply takes the new state.
    b8 empty = batch->opaque.object_count == open->opaque_objects && batch->transparent.object_count == open->transparent_objects
            && batch->mesh_group_count == open->first_group && render->decals_gpu.count == open->first_decal;

    if (empty) return;

    _nya_render3d_segment_close(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_ShaderMesh3DUniform _nya_render3d_shading_uniform(const NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    NYA_Render3DShadowOptions shadow_options = _nya_render3d_shadow_options_resolve(batch->shadow_options);

    /*
     * The switches that only change what the shader is told, applied once per segment. Each leaves the term at the
     * identity the shader already branches on, so an off feature costs nothing in the fragment stage either.
     */
    b8 lit          = nya_render_feature_enabled(window, NYA_RENDER_FEATURE_LIGHTING);
    b8 reflective   = nya_render_feature_enabled(window, NYA_RENDER_FEATURE_REFLECTIONS);
    b8 point_lights = nya_render_feature_enabled(window, NYA_RENDER_FEATURE_POINT_LIGHTS);

    u32 point_light_count = point_lights ? batch->point_light_count : 0;

    struct NYA_ShaderMesh3DUniform uniform = {
        // negated so the shader gets surface-to-light. callers think in the direction light travels.
        .light_direction_x = -batch->light.direction.x,
        .light_direction_y = -batch->light.direction.y,
        .light_direction_z = -batch->light.direction.z,
        .ambient           = batch->light.ambient,

        .light_color_r = batch->light.color.r,
        .light_color_g = batch->light.color.g,
        .light_color_b = batch->light.color.b,
        // zero leaves every surface at its ambient, which is flat albedo.
        .intensity     = lit ? batch->light.intensity : 0.0F,

        .camera_x = batch->camera_is_ortho ? batch->camera_orthographic.position.x : batch->camera.position.x,
        .camera_y = batch->camera_is_ortho ? batch->camera_orthographic.position.y : batch->camera.position.y,
        .camera_z = batch->camera_is_ortho ? batch->camera_orthographic.position.z : batch->camera.position.z,
        .metallic = reflective ? batch->material.metallic : 0.0F,

        .roughness   = batch->material.roughness,
        .reflectance = reflective ? batch->material.reflectance : 0.0F,
        .emission    = batch->material.emission,

        .point_light_count = (f32)point_light_count,

        .edge = batch->material.edge,

        // one cascade's texel, not the atlas's: the shader offsets its kernel in cascade-local uv. the atlas
        // texel would shrink the kernel and harden every contact shadow.
        .shadow_texel = 1.0F / (f32)shadow_options.map_size,

        .atlas_cascades = (f32)shadow_options.cascades,
    };

    /* Zero alpha colours default to the flat ambient and no shade, which the shader applies as identities. */
    NYA_Color sky    = batch->light.sky.a > 0.0F ? batch->light.sky : batch->light.color;
    NYA_Color ground = batch->light.ground.a > 0.0F ? batch->light.ground : batch->light.color;

    uniform.ambient_sky_r    = sky.r;
    uniform.ambient_sky_g    = sky.g;
    uniform.ambient_sky_b    = sky.b;
    uniform.ambient_ground_r = ground.r;
    uniform.ambient_ground_g = ground.g;
    uniform.ambient_ground_b = ground.b;

    // the hue at unit luminance, so the tint turns shade toward it without darkening it further.
    NYA_Color shade = shadow_options.color;
    f32       luma  = (0.2126F * shade.r) + (0.7152F * shade.g) + (0.0722F * shade.b);
    f32       amount = luma > 0.0F ? nya_clamp(shade.a, 0.0F, 1.0F) : 0.0F;

    uniform.shade_tint_r = nya_lerp(1.0F, shade.r / nya_max(luma, NYA_EPSILON), amount);
    uniform.shade_tint_g = nya_lerp(1.0F, shade.g / nya_max(luma, NYA_EPSILON), amount);
    uniform.shade_tint_b = nya_lerp(1.0F, shade.b / nya_max(luma, NYA_EPSILON), amount);

    /* Fog defaults are resolved here once per segment, since the shader only tests `density`. */
    if (batch->fog.density > 0.0F && nya_render_feature_enabled(window, NYA_RENDER_FEATURE_FOG)) {
        NYA_Color color = batch->fog.color;

        // alpha is ignored because fog is a lerp target. black fog needs one tiny non-zero channel.
        if (color.r == 0.0F && color.g == 0.0F && color.b == 0.0F) color = NYA_RENDER3D_FOG_COLOR;

        uniform.fog_color_r = color.r;
        uniform.fog_color_g = color.g;
        uniform.fog_color_b = color.b;
        uniform.fog_density = batch->fog.density;

        uniform.fog_height_falloff = batch->fog.height_falloff;
        uniform.fog_height_base    = batch->fog.height_base;
        uniform.fog_sun_amount     = batch->fog.sun_amount;
        uniform.fog_aerial         = batch->fog.aerial;
    }

    // field by field: f32x3 is sixteen bytes and the uniform block must match the HLSL layout exactly.
    for (u32 i = 0; i < point_light_count; i++) {
        const NYA_Render3DPointLight* light = &batch->point_lights[i];

        uniform.point_light_position_range[i][0] = light->position.x;
        uniform.point_light_position_range[i][1] = light->position.y;
        uniform.point_light_position_range[i][2] = light->position.z;
        uniform.point_light_position_range[i][3] = light->range > 0.0F ? light->range : NYA_RENDER3D_POINT_LIGHT_RANGE;

        uniform.point_light_color_intensity[i][0] = light->color.r;
        uniform.point_light_color_intensity[i][1] = light->color.g;
        uniform.point_light_color_intensity[i][2] = light->color.b;
        uniform.point_light_color_intensity[i][3] = light->intensity;
    }

    // normalized here because nya_render3d_begin installs the default without the setter.
    f32x3 direction = nya_vector_normalize((f32x3){ uniform.light_direction_x, uniform.light_direction_y, uniform.light_direction_z });

    uniform.light_direction_x = direction.x;
    uniform.light_direction_y = direction.y;
    uniform.light_direction_z = direction.z;

    return uniform;
}

SDL_GPUTexture* _nya_render3d_shadow_map(const NYA_Render3DBatch* batch) {
    return batch->shadow_valid ? batch->shadow_color : batch->shadow_none;
}

void _nya_render3d_bind_samplers(NYA_Window* window, SDL_GPUTexture* texture, SDL_GPUSampler* sampler) {
    NYA_RenderSystemWindow* render = &window->render_system;

    /*
     * Bindings in each shader's declared order. The untextured scene pipeline declares one sampler (the shadow map)
     * and the textured one two (base colour, then shadow map).
     */
    SDL_GPUTextureSamplerBinding bindings[2] = {
        { .texture = texture, .sampler = sampler },
        { .texture = _nya_render3d_shadow_map(&render->mesh_batch), .sampler = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR) },
    };

    // untextured draws bind the shadow map alone, at slot zero.
    if (texture != nullptr) SDL_BindGPUFragmentSamplers(render->render_pass, 0, bindings, 2);
    else SDL_BindGPUFragmentSamplers(render->render_pass, 0, &bindings[1], 1);
}

void _nya_render3d_playback(NYA_Window* window) {
    nya_perf_time_this_function();
    nya_trace_scope(NYA_TRACE_SCENE);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    // no pass: the window is occluded or minimised, and what was recorded is dropped rather than drawn stale later.
    if (batch->segment_count > 0 && render->render_pass != nullptr) {
        _nya_render3d_passes_prepare(window);

        // only a scene that casts shadows allocates the atlas. without one the placeholder is bound.
        b8  atlas      = batch->pass_count > 1 && _nya_render3d_shadow_ensure(window);
        u32 pass_count = atlas ? batch->pass_count : 1;

        SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

        f32x3 eye = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;

        /*
         * Every pass's visible indices, written straight into the upload one pass after another. The streams' own
         * indices stay as recorded, so each pass reads them unsorted.
         */
        u16* indices = SDL_MapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer, true);
        nya_assert(indices != nullptr, "SDL_MapGPUTransferBuffer() failed for the 3D batch's indices: %s", SDL_GetError());

        u32 index_count = 0;

        for (u32 pass = 0; pass < pass_count; pass++) {
            for (u32 s = 0; s < batch->segment_count; s++) {
                NYA_Render3DSegment* segment = &batch->segments[s];

                b8  last            = s + 1 == batch->segment_count;
                u32 opaque_end      = last ? batch->opaque.object_count : segment[1].opaque_objects;
                u32 transparent_end = last ? batch->transparent.object_count : segment[1].transparent_objects;

                segment->opaque[pass]      = (NYA_Render3DIndexRange){ .first = index_count };
                segment->transparent[pass] = (NYA_Render3DIndexRange){ .first = index_count };

                if (pass > 0 && !segment->casts_shadow) continue;

                u32 opaque = _nya_render3d_pass_indices(&batch->opaque, segment->opaque_objects, opaque_end, pass, indices + index_count);

                segment->opaque[pass].count  = opaque;
                index_count                 += opaque;

                // a cascade draws translucent geometry solid, as the map holds depth, not transmittance. what adds light casts nothing.
                if (pass > 0 && segment->blend == NYA_RENDER3D_BLEND_ADDITIVE) continue;

                u16* transparent = indices + index_count;
                u32  count       = _nya_render3d_pass_indices(&batch->transparent, segment->transparent_objects, transparent_end, pass, transparent);

                // only the camera blends, and adding does not depend on order. switched off, the triangles draw in
                // the order they were recorded, which is what the sort is worth on screen.
                if (pass == 0 && segment->blend != NYA_RENDER3D_BLEND_ADDITIVE
                    && nya_render_feature_enabled(window, NYA_RENDER_FEATURE_DRAW_SORTING)) {
                    _nya_render3d_sort_transparent(batch, transparent, count, eye);
                }

                segment->transparent[pass].count  = count;
                index_count                      += count;
            }
        }

        SDL_UnmapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer);

        /*
         * Instances are ordered within each transparent group before upload. Groups alone are not enough: one group
         * holds many copies at many depths. Opaque groups are left alone; the depth buffer handles them.
         */
        for (u32 g = 0; g < batch->mesh_group_count; g++) {
            const NYA_Render3DMeshGroup* group = &batch->mesh_groups[g];

            if (!group->transparent || group->instance_count < 2) continue;

            for (u32 i = 0; i < group->instance_count; i++) {
                const NYA_Render3DInstance* instance = &batch->instances[group->first_instance + i];

                /* The translation is the matrix's fourth column. */
                f32x3 offset = (f32x3){ instance->model[0][3], instance->model[1][3], instance->model[2][3] } - eye;

                batch->sort_keys[i] = (NYA_Render3DSortKey){ .depth = nya_vector_dot(offset, offset), .first = i };
            }

            nya_render3d_sort_keys(batch->sort_keys, batch->sort_keys_scratch, group->instance_count);

            u8 passes[NYA_RENDER3D_MAX_INSTANCES];

            // backwards: ascending radix, furthest-first draw. the pass masks move with their copies.
            for (u32 i = 0; i < group->instance_count; i++) {
                u32 source                 = batch->sort_keys[group->instance_count - 1 - i].first;
                batch->sorted_instances[i] = batch->instances[group->first_instance + source];
                passes[i]                  = batch->instance_passes[group->first_instance + source];
            }

            nya_memcpy(&batch->instances[group->first_instance], batch->sorted_instances,
                       (u64)group->instance_count * sizeof(NYA_Render3DInstance));
            nya_memcpy(&batch->instance_passes[group->first_instance], passes, group->instance_count);
        }

        const NYA_Render3DStream* opaque      = &batch->opaque;
        const NYA_Render3DStream* transparent = &batch->transparent;

        // both streams share one buffer, opaque first. transparent indices are relative to their own first vertex,
        // so a draw's vertex offset rebases them.
        u32 vertex_size   = (opaque->vertex_count + transparent->vertex_count) * (u32)sizeof(NYA_Vertex3D);
        u32 index_size    = index_count * (u32)sizeof(u16);
        u32 instance_size = batch->instance_count * (u32)sizeof(NYA_Render3DInstance);

        if (vertex_size > 0) {
            NYA_Vertex3D* mapped = SDL_MapGPUTransferBuffer(gpu_device, batch->transfer_buffer, true);
            nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed for the 3D batch: %s", SDL_GetError());
            nya_memcpy(mapped, opaque->vertices, (u64)opaque->vertex_count * sizeof(NYA_Vertex3D));
            nya_memcpy(mapped + opaque->vertex_count, transparent->vertices, (u64)transparent->vertex_count * sizeof(NYA_Vertex3D));
            SDL_UnmapGPUTransferBuffer(gpu_device, batch->transfer_buffer);
        }

        if (instance_size > 0) {
            void* mapped = SDL_MapGPUTransferBuffer(gpu_device, batch->instance_transfer_buffer, true);
            nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed for the 3D instance stream: %s", SDL_GetError());
            nya_memcpy(mapped, batch->instances, instance_size);
            SDL_UnmapGPUTransferBuffer(gpu_device, batch->instance_transfer_buffer);
        }

        // one copy pass for the whole scene, since a copy pass cannot run inside a render pass.
        _nya_render2d_pass_suspend(window);

        SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);

        const struct {
            SDL_GPUTransferBuffer* transfer;
            SDL_GPUBuffer*         buffer;
            u32                    size;
        } uploads[] = {
            { batch->transfer_buffer, batch->vertex_buffer, vertex_size },
            { batch->index_transfer_buffer, batch->index_buffer, index_size },
            { batch->instance_transfer_buffer, batch->instance_buffer, instance_size },
        };

        for (u32 i = 0; i < nya_carray_length(uploads); i++) {
            if (uploads[i].size == 0) continue;

            SDL_UploadToGPUBuffer(copy_pass, &(SDL_GPUTransferBufferLocation){ .transfer_buffer = uploads[i].transfer },
                                  &(SDL_GPUBufferRegion){ .buffer = uploads[i].buffer, .size = uploads[i].size }, true);

            render->frame_stats.uploads++;
            render->frame_stats.upload_bytes += uploads[i].size;
        }

        _nya_render3d_decals_upload(window, copy_pass);

        SDL_EndGPUCopyPass(copy_pass);

        for (u32 pass = 1; pass < pass_count; pass++) _nya_render3d_pass_draw(window, pass);

        if (pass_count > 1) batch->shadow_valid = true;

        // the camera pass writes the normal buffer when its target has one.
        render->render_pass_normals = render->draw_batch.target_normal != nullptr;

        _nya_render2d_pass_resume(window);

        // what the cascades just drew, which no segment could know when it was recorded.
        for (u32 s = 0; s < batch->segment_count; s++) {
            struct NYA_ShaderMesh3DUniform* uniform = &batch->segment_uniforms[s];

            uniform->shadow_strength = batch->shadow_valid ? batch->shadow.strength : 0.0F;
            uniform->shadow_bias     = batch->shadow.bias;
            uniform->cascade_count   = (f32)batch->shadow_cascade_count;

            for (u32 i = 0; i < NYA_RENDER3D_SHADOW_CASCADES; i++) {
                uniform->light_view_projection[i] = batch->shadow_view_projection[i];
                uniform->cascade_extent[i]        = batch->shadow_cascade_extent[i];
            }
        }

        _nya_render3d_pass_draw(window, 0);

        batch->frame_passes   += pass_count;
        batch->frame_vertices += opaque->vertex_count + transparent->vertex_count;
        batch->frame_indices  += index_count;
    }

    batch->opaque.vertex_count      = 0;
    batch->opaque.index_count       = 0;
    batch->opaque.object_count      = 0;
    batch->transparent.vertex_count = 0;
    batch->transparent.index_count  = 0;
    batch->transparent.object_count = 0;

    batch->instance_count    = 0;
    batch->mesh_group_count  = 0;
    batch->segment_count     = 0;
    render->decals_gpu.count = 0;

    _nya_render3d_segment_open(window);
}

void _nya_render3d_pass_draw(NYA_Window* window, u32 pass) {
    nya_trace_scope(pass > 0 ? NYA_TRACE_SHADOWS : NYA_TRACE_SCENE);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    if (pass > 0) {
        u32 cascade = pass - 1;

        // the first cascade clears the whole atlas, and a later playback in the same scene adds to what it holds.
        SDL_GPULoadOp load = cascade == 0 && !batch->shadow_valid ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;

        // the shadow pipelines have no normals variant.
        render->render_pass_normals = false;

        _nya_render_trace_mark(window);

        render->render_pass = SDL_BeginGPURenderPass(
            render->render_commands,
            &(SDL_GPUColorTargetInfo){
                .texture = batch->shadow_color,
                // white is the far plane in this encoding, so unseen texels compare as unoccluded.
                .clear_color = (SDL_FColor){ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
                .load_op     = load,
                .store_op    = SDL_GPU_STOREOP_STORE,
            },
            1,
            &(SDL_GPUDepthStencilTargetInfo){
                .texture     = batch->shadow_depth,
                .clear_depth = 1.0F,
                .load_op     = load,

                // stored for the next cascade's slice, since only the first one clears.
                .store_op         = SDL_GPU_STOREOP_STORE,
                .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
                .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
            }
        );

        if (render->render_pass == nullptr) {
            nya_log_error("SDL_BeginGPURenderPass() failed for shadow cascade %u: %s", cascade, SDL_GetError());
            return;
        }

        render->frame_stats.passes++;

        f32 size = (f32)nya_render3d_shadow_options(window).map_size;

        // a viewport rather than a scissor, because it has to map clip space onto the slice, not just clip it.
        SDL_SetGPUViewport(render->render_pass, &(SDL_GPUViewport){ .x = (f32)cascade * size, .w = size, .h = size, .max_depth = 1.0F });
    }

    for (u32 s = 0; s < batch->segment_count; s++) {
        const NYA_Render3DSegment*            segment = &batch->segments[s];
        const struct NYA_ShaderMesh3DUniform* uniform = &batch->segment_uniforms[s];

        if (pass > 0 && !segment->casts_shadow) continue;

        if (segment->skinned != nullptr) {
            // the bits _nya_render3d_passes_seeing set when the pose was recorded.
            if ((segment->skinned_passes & (u8)(1U << pass)) != 0) _nya_render3d_skinned_draw(window, segment, uniform, pass);
            continue;
        }

        _nya_render3d_immediate_draw(window, segment, uniform, pass);
        _nya_render3d_instanced_draw(window, segment, uniform, pass);

        // last, over the ground they lie on, whichever of the two paths drew it.
        if (pass == 0) _nya_render3d_decals_draw(window, segment, uniform);
    }

    if (pass == 0 || render->render_pass == nullptr) return;

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;
}

void _nya_render3d_immediate_draw(NYA_Window* window, const NYA_Render3DSegment* segment, const struct NYA_ShaderMesh3DUniform* uniform,
                                  u32 pass) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    NYA_Render3DIndexRange opaque      = segment->opaque[pass];
    NYA_Render3DIndexRange transparent = segment->transparent[pass];

    if (opaque.count == 0 && transparent.count == 0) return;

    b8 shadow  = pass > 0;
    b8 overlay = segment->depth == NYA_RENDER3D_DEPTH_OVERLAY;

    /*
     * Translucent surfaces test depth but do not write it, or a nearer pane hides the one behind it. The
     * overlay replaces both streams, because gizmos are usually opaque.
     */
    NYA_ConstCString opaque_handle = shadow                      ? NYA_RENDER3D_PIPELINE_SHADOW
                                   : overlay                     ? NYA_RENDER3D_PIPELINE_OVERLAY
                                   : segment->texture != nullptr ? NYA_RENDER3D_PIPELINE_MESH_TEXTURED
                                                                 : NYA_RENDER3D_PIPELINE_MESH;

    NYA_ConstCString transparent_handle;

    if (shadow) {
        transparent_handle = NYA_RENDER3D_PIPELINE_SHADOW;
    } else if (overlay) {
        // the overlay check comes first, whatever the blend mode, so a gizmo is always on top.
        transparent_handle = NYA_RENDER3D_PIPELINE_OVERLAY;
    } else if (segment->blend == NYA_RENDER3D_BLEND_ADDITIVE) {
        transparent_handle = segment->texture != nullptr ? NYA_RENDER3D_PIPELINE_ADDITIVE_TEXTURED : NYA_RENDER3D_PIPELINE_ADDITIVE;
    } else {
        transparent_handle = segment->texture != nullptr ? NYA_RENDER3D_PIPELINE_TRANSPARENT_TEXTURED : NYA_RENDER3D_PIPELINE_TRANSPARENT;
    }

    NYA_Asset* opaque_pipeline      = nya_asset_get((NYA_AssetHandle)opaque_handle);
    NYA_Asset* transparent_pipeline = nya_asset_get((NYA_AssetHandle)transparent_handle);

    /*
     * Only the intent is decided here. The capture happens after the opaque draw, because glass has to see the
     * opaque scene drawn so far. Both pipelines are looked up now to avoid a lookup mid-pass.
     */
    b8 wants_glass = !shadow && segment->material.refraction > 0.0F && transparent.count > 0;

    NYA_Asset* glass_pipeline = wants_glass ? nya_asset_get((NYA_AssetHandle)NYA_RENDER3D_PIPELINE_GLASS) : nullptr;

    if (glass_pipeline != nullptr && glass_pipeline->status != NYA_ASSET_STATUS_LOADED) glass_pipeline = nullptr;

    // still loading on the first frames.
    b8 opaque_ready      = opaque_pipeline != nullptr && opaque_pipeline->status == NYA_ASSET_STATUS_LOADED;
    b8 transparent_ready = transparent_pipeline != nullptr && transparent_pipeline->status == NYA_ASSET_STATUS_LOADED;

    if (!opaque_ready && !transparent_ready && glass_pipeline == nullptr) return;

    f32_4x4 view_projection = _nya_render3d_pass_view_projection(batch, pass);

    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer }, 1);
    SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer }, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    // the shadow pipelines declare no sampler and no fragment uniform.
    if (!shadow) _nya_render3d_bind_samplers(window, segment->texture, segment->sampler);

    SDL_PushGPUVertexUniformData(render->render_commands, 0, &view_projection, sizeof(view_projection));

    if (!shadow) SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));

    // opaque first, so transparent surfaces test against the opaque depth.
    if (opaque.count > 0 && opaque_ready) {
        SDL_BindGPUGraphicsPipeline(render->render_pass, _nya_render_pipeline(window, opaque_pipeline));
        SDL_DrawGPUIndexedPrimitives(render->render_pass, opaque.count, 1, opaque.first, 0, 0);

        batch->frame_draw_calls++;
        nya_trace_draws(1);
    }

    if (transparent.count == 0) return;

    /*
     * The capture sits between the opaque and transparent halves, so glass sees everything opaque drawn so far.
     * Glass behind glass sees an unrefracted backdrop, the limit of a single capture.
     */
    b8 glass = glass_pipeline != nullptr && _nya_render3d_refraction_capture(window);

    if (glass) {
        // a resume begins a new pass with nothing bound, so everything is rebound, not only the samplers.
        SDL_BindGPUGraphicsPipeline(render->render_pass, _nya_render_pipeline(window, glass_pipeline));

        SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer }, 1);
        SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer }, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        // capture at t0 and shadow map at t1, the order mesh3d_glass.frag.hlsl declares.
        SDL_GPUSampler* linear = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR);

        SDL_BindGPUFragmentSamplers(
            render->render_pass,
            0,
            (SDL_GPUTextureSamplerBinding[]){
                { .texture = batch->refraction_capture, .sampler = linear },
                { .texture = _nya_render3d_shadow_map(batch), .sampler = linear },
            },
            2
        );

        struct NYA_ShaderGlassUniform glass_uniform = {
            .texel_x    = batch->refraction_width > 0 ? 1.0F / (f32)batch->refraction_width : 0.0F,
            .texel_y    = batch->refraction_height > 0 ? 1.0F / (f32)batch->refraction_height : 0.0F,
            .refraction = segment->material.refraction,
            .blur       = segment->material.blur,
        };

        SDL_PushGPUVertexUniformData(render->render_commands, 0, &view_projection, sizeof(view_projection));
        SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));
        SDL_PushGPUFragmentUniformData(render->render_commands, 1, &glass_uniform, sizeof(glass_uniform));
    } else if (transparent_ready) {
        SDL_BindGPUGraphicsPipeline(render->render_pass, _nya_render_pipeline(window, transparent_pipeline));
    }

    if (glass || transparent_ready) {
        // the vertex offset rebases this stream's indices onto the second half of the buffer.
        SDL_DrawGPUIndexedPrimitives(render->render_pass, transparent.count, 1, transparent.first, (s32)batch->opaque.vertex_count, 0);

        batch->frame_draw_calls++;
        nya_trace_draws(1);
    }
}

void _nya_render3d_sort_transparent(NYA_Render3DBatch* batch, u16* indices, u32 index_count, f32x3 eye) {
    nya_trace_scope(NYA_TRACE_TRANSPARENT);

    const NYA_Render3DStream* stream = &batch->transparent;

    u32 triangles = index_count / 3;

    if (triangles < 2) return;

    for (u32 i = 0; i < triangles; i++) {
        u32 first = i * 3;

        f32x3 a = nya_vertex3d_position(stream->vertices[indices[first + 0]]);
        f32x3 b = nya_vertex3d_position(stream->vertices[indices[first + 1]]);
        f32x3 c = nya_vertex3d_position(stream->vertices[indices[first + 2]]);

        f32x3 centroid = (a + b + c) / 3.0F;

        /*
         * A quad's two triangles take one key from the quad's middle, as _nya_render3d_quad_emit writes them (the
         * second starts at the first's first vertex and ends on a new one). Keyed apart, two overlapping billboards
         * at similar distance interleave half by half, and the seam flickers as they move.
         */
        b8 quad = i + 1 < triangles && indices[first + 3] == indices[first] && indices[first + 4] == indices[first + 2];

        if (quad) {
            f32x3 d  = nya_vertex3d_position(stream->vertices[indices[first + 5]]);
            centroid = (a + b + c + d) / 4.0F;
        }

        f32x3 offset = centroid - eye;
        f32   depth  = nya_vector_dot(offset, offset);

        batch->sort_keys[i] = (NYA_Render3DSortKey){ .depth = depth, .first = first };

        if (!quad) continue;

        // the radix sort is stable, so the pair stays together.
        i++;
        batch->sort_keys[i] = (NYA_Render3DSortKey){ .depth = depth, .first = first + 3 };
    }

    nya_render3d_sort_keys(batch->sort_keys, batch->sort_keys_scratch, triangles);

    // walked backwards because the radix sort is ascending and this wants furthest first.
    for (u32 i = 0; i < triangles; i++) {
        u32 source = batch->sort_keys[triangles - 1 - i].first;

        batch->sorted_indices[(i * 3) + 0] = indices[source + 0];
        batch->sorted_indices[(i * 3) + 1] = indices[source + 1];
        batch->sorted_indices[(i * 3) + 2] = indices[source + 2];
    }

    nya_memcpy(indices, batch->sorted_indices, (u64)triangles * 3 * sizeof(u16));
}

void _nya_render3d_instanced_draw(NYA_Window* window, const NYA_Render3DSegment* segment, const struct NYA_ShaderMesh3DUniform* uniform,
                                  u32 pass) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    if (segment->group_count == 0) return;

    b8 shadow = pass > 0;

    f32_4x4 view_projection = _nya_render3d_pass_view_projection(batch, pass);

    /*
     * One draw call per mesh part and run of copies the pass sees, since a part is what has a texture. Cost scales
     * with distinct materials on screen, not with the number of copies.
     */
    u32 order[NYA_RENDER3D_MAX_MESH_GROUPS];
    u32 order_count = 0;

    u32 end = segment->first_group + segment->group_count;

    for (u32 g = segment->first_group; g < end; g++) {
        if (!batch->mesh_groups[g].transparent) order[order_count++] = g;
    }

    u32 opaque_groups = order_count;

    for (u32 g = segment->first_group; g < end; g++) {
        if (batch->mesh_groups[g].transparent) order[order_count++] = g;
    }

    // insertion sort: a handful of groups, nearly sorted from frame to frame.
    for (u32 i = opaque_groups + 1; i < order_count; i++) {
        u32 moving = order[i];

        f32 depth = batch->mesh_groups[moving].depth;

        u32 j = i;

        while (j > opaque_groups && batch->mesh_groups[order[j - 1]].depth < depth) {
            order[j] = order[j - 1];
            j--;
        }

        order[j] = moving;
    }

    for (u32 o = 0; o < order_count; o++) {
        const NYA_Render3DMeshGroup* group = &batch->mesh_groups[order[o]];

        u32 instances_end = group->first_instance + group->instance_count;

        u32 first_seen = 0;
        if (_nya_render3d_pass_run(batch->instance_passes, group->first_instance, instances_end, pass, &first_seen) == 0) continue;

        // registered geometry is treated as one untextured part, so both sources share the loop below.
        NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(batch, group->handle);

        NYA_Asset* asset = nullptr;

        SDL_GPUBuffer* mesh_vertices = nullptr;

        NYA_MeshPart        single_part = { 0 };
        const NYA_MeshPart* parts       = &single_part;

        u32 part_count = 1;

        if (registered != nullptr) {
            mesh_vertices = registered->vertices;
            single_part   = (NYA_MeshPart){ .first_vertex = 0, .vertex_count = registered->vertex_count, .texture = -1 };
        } else {
            asset = nya_asset_get((NYA_AssetHandle)group->handle);

            // unloaded mid-frame by a hot reload.
            if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_mesh.gpu_vertices == nullptr) continue;

            mesh_vertices = asset->as_mesh.gpu_vertices;
            parts         = asset->as_mesh.parts;
            part_count    = asset->as_mesh.part_count;
        }

        for (u32 p = 0; p < part_count; p++) {
            const NYA_MeshPart* part = &parts[p];

            if (part->vertex_count == 0) continue;

            // a part's own texture, unless textures are switched off, in which case it draws in its vertex colour.
            b8 textured = asset != nullptr && part->texture >= 0 && nya_render_feature_enabled(window, NYA_RENDER_FEATURE_TEXTURES);

            SDL_GPUTexture* texture = textured ? asset->as_mesh.textures[part->texture] : nullptr;
            SDL_GPUSampler* sampler = texture != nullptr ? _nya_render_sampler_for(asset->as_mesh.filter) : nullptr;

            // a cascade draws translucent meshes solid, as the immediate path does.
            NYA_ConstCString pipeline_handle;

            if (shadow) {
                pipeline_handle = NYA_RENDER3D_PIPELINE_INSTANCED_SHADOW;
            } else if (group->transparent) {
                pipeline_handle = texture != nullptr ? NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT_TEXTURED
                                                     : NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT;
            } else {
                pipeline_handle = texture != nullptr ? NYA_RENDER3D_PIPELINE_INSTANCED_TEXTURED : NYA_RENDER3D_PIPELINE_INSTANCED;
            }

            NYA_Asset* pipeline_asset = nya_asset_get((NYA_AssetHandle)pipeline_handle);
            if (pipeline_asset == nullptr || pipeline_asset->status != NYA_ASSET_STATUS_LOADED) continue;

            SDL_BindGPUGraphicsPipeline(render->render_pass, _nya_render_pipeline(window, pipeline_asset));

            if (!shadow) _nya_render3d_bind_samplers(window, texture, sampler);

            SDL_PushGPUVertexUniformData(render->render_commands, 0, &view_projection, sizeof(view_projection));

            if (!shadow) SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));

            u32 first = first_seen;
            u32 run   = 0;

            while ((run = _nya_render3d_pass_run(batch->instance_passes, first, instances_end, pass, &first)) > 0) {
                // bound from the run's first copy, so the draw's first-instance stays zero. some backends apply
                // first_instance to the buffer but not to SV_InstanceID.
                SDL_BindGPUVertexBuffers(
                    render->render_pass,
                    0,
                    (SDL_GPUBufferBinding[]){
                        { .buffer = mesh_vertices },
                        { .buffer = batch->instance_buffer, .offset = first * (u32)sizeof(NYA_Render3DInstance) },
                    },
                    2
                );

                // not indexed: the loader de-indexes, so an index buffer would be the identity.
                SDL_DrawGPUPrimitives(render->render_pass, part->vertex_count, run, part->first_vertex, 0);

                batch->frame_draw_calls++;
                nya_trace_draws(1);
                batch->frame_vertices  += part->vertex_count * run;
                batch->frame_instances += p == 0 ? run : 0;

                first += run;
            }
        }
    }
}

void _nya_render3d_skinned_draw(NYA_Window* window, const NYA_Render3DSegment* segment, const struct NYA_ShaderMesh3DUniform* uniform,
                                u32 pass) {
    nya_trace_scope(NYA_TRACE_SKINNING);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    b8 shadow = pass > 0;

    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(batch, segment->skinned);

    // released since it was recorded, or its copy has not run.
    if (registered == nullptr || registered->pending_upload != nullptr) return;

    /*
     * The mesh's parts, so each material's run draws with its own texture, as the immediate and
     * instanced paths already do. The whole mesh used to go out as one untextured draw, which drew a
     * rigged model in its vertex colours and nothing else however many materials it had.
     *
     * The skinned vertices are the same geometry as `vertices` in another layout, so a part's run of
     * positions is a part's run here too. A mesh that came in through nya_render3d_mesh_register has no
     * asset behind it and is one untextured part, exactly as it is over there.
     */
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)segment->skinned);

    NYA_MeshPart        single_part = { .first_vertex = 0, .vertex_count = registered->vertex_count, .texture = -1 };
    const NYA_MeshPart* parts       = &single_part;
    u32                 part_count  = 1;

    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED && asset->as_mesh.part_count > 0) {
        parts      = asset->as_mesh.parts;
        part_count = asset->as_mesh.part_count;
    } else {
        asset = nullptr;
    }

    f32_4x4 view_projection = _nya_render3d_pass_view_projection(batch, pass);

    for (u32 p = 0; p < part_count; p++) {
        const NYA_MeshPart* part = &parts[p];

        if (part->vertex_count == 0) continue;
        if (part->first_vertex + part->vertex_count > registered->vertex_count) continue;

        // a part's own texture, unless textures are switched off, in which case it draws in its vertex colour.
        const b8 textured = !shadow && asset != nullptr && part->texture >= 0 && nya_render_feature_enabled(window, NYA_RENDER_FEATURE_TEXTURES);

        SDL_GPUTexture* texture = textured ? asset->as_mesh.textures[part->texture] : nullptr;
        SDL_GPUSampler* sampler = texture != nullptr ? _nya_render_sampler_for(asset->as_mesh.filter) : nullptr;

        NYA_ConstCString pipeline_handle = shadow      ? NYA_RENDER3D_PIPELINE_SKINNED_SHADOW
                                         : texture     ? NYA_RENDER3D_PIPELINE_SKINNED_TEXTURED
                                                       : NYA_RENDER3D_PIPELINE_SKINNED;

        NYA_Asset* pipeline = nya_asset_get((NYA_AssetHandle)pipeline_handle);
        if (pipeline == nullptr || pipeline->status != NYA_ASSET_STATUS_LOADED) continue;

        SDL_BindGPUGraphicsPipeline(render->render_pass, _nya_render_pipeline(window, pipeline));
        SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = registered->vertices }, 1);

        SDL_PushGPUVertexUniformData(render->render_commands, 0, &view_projection, sizeof(view_projection));
        SDL_PushGPUVertexUniformData(render->render_commands, 1, segment->skin, sizeof(*segment->skin));

        // the shadow pipeline declares no uniform or sampler, and binding one is a validation error.
        if (!shadow) {
            SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));

            // mesh3d.frag.hlsl always declares the shadow map's sampler.
            _nya_render3d_bind_samplers(window, texture, sampler);
        }

        SDL_DrawGPUPrimitives(render->render_pass, part->vertex_count, 1, part->first_vertex, 0);

        batch->frame_draw_calls++;
        nya_trace_draws(1);
    }
}

NYA_Render3DMeshGroup* _nya_render3d_mesh_group(NYA_Render3DBatch* batch, NYA_ConstCString handle, b8 transparent) {
    /*
     * Only the last group of the open segment or a new one: a group is a contiguous run of the instance array.
     * Alternating between two meshes therefore costs a draw call per switch; draw scenes grouped by model.
     */
    if (batch->mesh_group_count > batch->segments[batch->segment_count].first_group) {
        NYA_Render3DMeshGroup* last = &batch->mesh_groups[batch->mesh_group_count - 1];

        // transparency must match too, since the two draw through different pipelines.
        if (last->handle == handle && last->transparent == transparent) return last;
    }

    nya_assert(batch->mesh_group_count < NYA_RENDER3D_MAX_MESH_GROUPS, "nya_render3d_mesh plays the scene back before the groups fill");

    NYA_Render3DMeshGroup* group = &batch->mesh_groups[batch->mesh_group_count];

    *group = (NYA_Render3DMeshGroup){
        .handle         = handle,
        .first_instance = batch->instance_count,
        .instance_count = 0,
        .transparent    = transparent,

        // replaced by the first instance.
        .depth = 0.0F,
    };

    batch->mesh_group_count++;

    return group;
}

b8 _nya_render3d_refraction_capture(NYA_Window* window) {
    nya_trace_scope(NYA_TRACE_TRANSPARENT);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;
    NYA_Render2DBatch*      target = &render->draw_batch;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;
    if (gpu_device == nullptr) return false;

    /*
     * Only from a render texture. The capture copies the resolved colour target, and only a render texture
     * resolves mid-frame; the window resolves at the end of the frame. A stale copy inside moving glass looks
     * worse than glass that does not refract.
     */
    if (!target->target_is_texture || target->target_texture == nullptr) return false;

    u32 width  = target->target_width;
    u32 height = target->target_height;

    if (width == 0 || height == 0) return false;

    // recreated, because a GPU texture cannot be resized. rare: a window moving between monitors.
    if (batch->refraction_capture != nullptr && (batch->refraction_width != width || batch->refraction_height != height)) {
        nya_gpu_texture_release(gpu_device, batch->refraction_capture);

        batch->refraction_capture = nullptr;
    }

    if (batch->refraction_capture == nullptr) {
        batch->refraction_capture = nya_gpu_texture_create(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type   = SDL_GPU_TEXTURETYPE_2D,
                .format = window->render_system.color_format,

                // a colour target as well, because SDL_BlitGPUTexture writes through a render pass.
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,

                // single sampled: it copies an already resolved image and must stay samplable.
                .sample_count = SDL_GPU_SAMPLECOUNT_1,
            }
        );

        if (batch->refraction_capture == nullptr) {
            nya_log_error("SDL_CreateGPUTexture() failed for the refraction capture: %s", SDL_GetError());
            return false;
        }

        batch->refraction_width  = width;
        batch->refraction_height = height;

        nya_log_debug("Refraction capture created at %ux%u.", width, height);
    }

    /* The pass is suspended around the blit, so the copy sees a resolved image. */
    _nya_render2d_pass_suspend(window);

    SDL_BlitGPUTexture(
        render->render_commands,
        &(SDL_GPUBlitInfo){
            .source      = { .texture = target->target_texture, .w = width, .h = height },
            .destination = { .texture = batch->refraction_capture, .w = width, .h = height },
            .load_op     = SDL_GPU_LOADOP_DONT_CARE,
            .filter      = SDL_GPU_FILTER_NEAREST,
        }
    );

    // a blit is a render pass of its own.
    render->frame_stats.passes++;

    _nya_render2d_pass_resume(window);

    return true;
}

void _nya_render3d_registered_destroy(void* value, void* user_data) {
    nya_unused(user_data);

    NYA_Render3DRegisteredMesh* mesh       = value;
    SDL_GPUDevice*              gpu_device = nya_app_get()->render_system.gpu_device;

    if (mesh->vertices != nullptr) nya_gpu_buffer_release(gpu_device, mesh->vertices);
    if (mesh->pending_upload != nullptr) nya_gpu_transfer_buffer_release(gpu_device, mesh->pending_upload);
}

NYA_Render3DRegisteredMesh* _nya_render3d_registered(NYA_Render3DBatch* batch, NYA_ConstCString handle) {
    if (handle == nullptr || handle[0] == '\0') return nullptr;

    // a registration is never stale, so every entry carries tag zero.
    return nya_cache_get(batch->registered_meshes, handle, strlen(handle), 0);
}

NYA_Render3DRegisteredMesh* _nya_render3d_registered_claim(NYA_Render3DBatch* batch, NYA_ConstCString handle) {
    nya_assert(handle != nullptr);

    u64 length = strlen(handle);

    if (length == 0 || length >= NYA_RENDER3D_MESH_HANDLE_MAX) {
        nya_log_error("The mesh handle '%s' is empty or longer than NYA_RENDER3D_MESH_HANDLE_MAX (%d).", handle, NYA_RENDER3D_MESH_HANDLE_MAX);
        return nullptr;
    }

    void*     slot  = nullptr;
    NYA_Error error = nya_cache_insert(batch->registered_meshes, handle, length, 0, &slot);

    if (!error.ok) {
        nya_log_error("No room to register the mesh '%s'; raise NYA_RENDER3D_MAX_REGISTERED_MESHES.", handle);
        return nullptr;
    }

    return slot;
}

b8 _nya_render3d_vertex_buffer_stage(
    const void*                 vertices,
    u32                         size,
    NYA_ConstCString            label,
    OUT SDL_GPUBuffer**         out_buffer,
    OUT SDL_GPUTransferBuffer** out_transfer
) {
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    if (gpu_device == nullptr) return false;

    SDL_GPUBuffer* buffer = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = size });

    if (buffer == nullptr) {
        nya_log_error("Could not create a vertex buffer for '%s': %s", label, SDL_GetError());
        return false;
    }

    /* A transfer buffer per registration, kept until the copy happens. */
    SDL_GPUTransferBuffer* transfer =
        nya_gpu_transfer_buffer_create(gpu_device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size });

    if (transfer == nullptr) {
        nya_log_error("Could not create a transfer buffer for '%s': %s", label, SDL_GetError());
        nya_gpu_buffer_release(gpu_device, buffer);
        return false;
    }

    void* staging = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);

    if (staging == nullptr) {
        nya_log_error("Could not map the transfer buffer for '%s': %s", label, SDL_GetError());
        nya_gpu_transfer_buffer_release(gpu_device, transfer);
        nya_gpu_buffer_release(gpu_device, buffer);
        return false;
    }

    nya_memcpy(staging, vertices, size);
    SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

    *out_buffer   = buffer;
    *out_transfer = transfer;

    return true;
}

void _nya_render3d_registered_flush_upload(NYA_Window* window, NYA_Render3DRegisteredMesh* mesh) {
    if (mesh->pending_upload == nullptr) return;

    NYA_RenderSystemWindow* render = &window->render_system;

    // no frame yet; the next draw inside one performs the copy.
    if (render->render_commands == nullptr) return;

    _nya_render2d_pass_suspend(window);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);

    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = mesh->pending_upload, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = mesh->vertices, .offset = 0, .size = mesh->pending_size },
        false
    );

    SDL_EndGPUCopyPass(copy_pass);

    render->frame_stats.uploads++;
    render->frame_stats.upload_bytes += mesh->pending_size;

    _nya_render2d_pass_resume(window);

    nya_gpu_transfer_buffer_release(nya_app_get()->render_system.gpu_device, mesh->pending_upload);

    mesh->pending_upload = nullptr;
    mesh->pending_size   = 0;
}

b8 _nya_render3d_mesh_upload(NYA_Window* window, NYA_Asset* asset) {
    nya_trace_scope(NYA_TRACE_ASSETS);

    NYA_RenderSystemWindow* render     = &window->render_system;
    SDL_GPUDevice*          gpu_device = nya_app_get()->render_system.gpu_device;

    u32 vertex_count = asset->as_mesh.vertex_count;
    u32 size         = (u32)(vertex_count * sizeof(NYA_Vertex3D));

    SDL_GPUBuffer* buffer = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = size });

    if (buffer == nullptr) {
        nya_log_error("Could not create a vertex buffer for the mesh '%s': %s", asset->handle, SDL_GetError());
        return false;
    }

    // a transfer buffer for this upload only: the shared one is sized for the immediate path, and this runs
    // once per mesh.
    SDL_GPUTransferBuffer* transfer =
        nya_gpu_transfer_buffer_create(gpu_device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size });

    if (transfer == nullptr) {
        nya_log_error("Could not create a transfer buffer for the mesh '%s': %s", asset->handle, SDL_GetError());
        nya_gpu_buffer_release(gpu_device, buffer);
        return false;
    }

    NYA_Vertex3D* staging = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);

    if (staging == nullptr) {
        nya_log_error("Could not map the transfer buffer for the mesh '%s': %s", asset->handle, SDL_GetError());
        nya_gpu_transfer_buffer_release(gpu_device, transfer);
        nya_gpu_buffer_release(gpu_device, buffer);
        return false;
    }

    // material colours are baked into the vertices, so the instance tint is a plain multiply and nothing is
    // pushed per frame.
    for (u32 p = 0; p < asset->as_mesh.part_count; p++) {
        const NYA_MeshPart* part = &asset->as_mesh.parts[p];

        for (u32 i = 0; i < part->vertex_count; i++) {
            u32 source = part->first_vertex + i;

            // zero uv for an untextured model.
            staging[source] = nya_vertex3d(asset->as_mesh.positions[source], part->base_color, asset->as_mesh.normals[source],
                                           asset->as_mesh.uvs != nullptr ? asset->as_mesh.uvs[source] : f32x2_zero);
        }
    }

    SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

    _nya_render2d_pass_suspend(window);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);

    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = transfer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = buffer, .offset = 0, .size = size },
        false
    );

    SDL_EndGPUCopyPass(copy_pass);

    render->frame_stats.uploads++;
    render->frame_stats.upload_bytes += size;

    _nya_render2d_pass_resume(window);

    // released "as soon as it is safe", which already waits for the queued copy.
    nya_gpu_transfer_buffer_release(gpu_device, transfer);

    asset->as_mesh.gpu_vertices     = buffer;
    asset->as_mesh.gpu_vertex_count = vertex_count;

    nya_log_debug("Uploaded '%s' to the GPU: %u vertices, %u KiB, kept for the life of the asset.", asset->handle, vertex_count, size / 1024);

    return true;
}

void nya_render3d_occlusion(NYA_Window* window, const NYA_OcclusionBuffer* buffer) {
    nya_assert(window != nullptr);

    window->render_system.mesh_batch.occlusion = buffer;
}

f32_4x4 nya_render3d_view_projection(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.view_projection;
}


NYA_Camera3DPerspective _nya_render3d_camera_defaults(NYA_Camera3DPerspective camera) {
    // zero means unset. sixty degrees and y up, as in physics3d.h.
    if (camera.up.x == 0.0F && camera.up.y == 0.0F && camera.up.z == 0.0F) camera.up = (f32x3){ 0.0F, 1.0F, 0.0F };
    if (camera.fov_y <= 0.0F) camera.fov_y = nya_render_fov_y();
    if (camera.near_plane <= 0.0F) camera.near_plane = 0.1F;
    if (camera.far_plane <= camera.near_plane) camera.far_plane = 1000.0F;

    return camera;
}

NYA_Camera3DOrthographic _nya_render3d_camera_orthographic_defaults(NYA_Camera3DOrthographic camera) {
    if (camera.up.x == 0.0F && camera.up.y == 0.0F && camera.up.z == 0.0F) camera.up = (f32x3){ 0.0F, 1.0F, 0.0F };
    if (camera.height <= 0.0F) camera.height = 10.0F;

    // an orthographic near plane may be negative; there is no projective divide.
    if (camera.far_plane <= camera.near_plane) {
        camera.near_plane = -100.0F;
        camera.far_plane  = 100.0F;
    }

    return camera;
}

void _nya_render3d_begin_with(NYA_Window* window, f32_4x4 view_projection) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    /*
     * Every 3D pipeline except the overlay tests depth and cannot bind in a pass without a depth target.
     * Caught here instead of as a driver validation error far away.
     */
    nya_assert(window->render_system.draw_batch.target_depth != nullptr,
               "the current render target has no depth buffer; it was created NYA_RENDER_TEXTURE_DEPTH_NONE, which only render2d may draw into");

    // queued 2D goes behind the scene, which is the ordering contract in render3d.h.
    nya_render2d_flush(window);

    // a second begin changes camera mid-frame; what was recorded belongs to the old one.
    if (batch->active) {
        nya_render3d_flush(window);
        _nya_render3d_playback(window);
    }

    batch->view_projection = view_projection;
    batch->active          = true;

    // the camera's frustum now; the cascades follow it the first time something is drawn.
    _nya_render3d_frustum_build(&batch->passes[0], view_projection);

    batch->passes_ready = false;
    batch->shadow_valid = false;

    /* Light and material reset every begin, so a frame never depends on the last. */
    // an occlusion buffer from a camera that moved would hide visible geometry. see nya_render3d_occlusion.
    batch->occlusion = nullptr;

    batch->light = _nya_render3d_default_light();

    batch->material = (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 1.0F, .reflectance = 0.5F };

    batch->blend        = NYA_RENDER3D_BLEND_ALPHA;
    batch->casts_shadow = true;
}

b8 _nya_render3d_object_begin(NYA_Window* window, NYA_Color color, f32x3 center, f32 radius, u32 vertices, u32 indices,
                              SDL_GPUTexture* texture, SDL_GPUSampler* sampler) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (!batch->active) return false;

    _nya_render3d_passes_prepare(window);

    u8 passes = _nya_render3d_passes_seeing(window, center, radius);

    if (passes == 0) return false;

    /*
     * Which stream records this draw. See _nya_render3d_stream_transparent: a fire particle is born at exactly
     * alpha one, so for its first tick it used to go into the opaque stream and write depth over the plume behind
     * it instead of adding to it. Two new particles every 45 ms punched a square hole in the flame for a tick
     * each, which is what flickered.
     */
    batch->transparent_active = _nya_render3d_stream_transparent(color, batch->blend)
                             && nya_render_feature_enabled(window, NYA_RENDER_FEATURE_TRANSPARENCY);

    if (!_nya_render3d_reserve(window, vertices, indices, texture, sampler)) return false;

    NYA_Render3DStream*        stream = _nya_render3d_stream(batch);
    const NYA_Render3DSegment* open   = &batch->segments[batch->segment_count];

    u32 segment_objects = batch->transparent_active ? open->transparent_objects : open->opaque_objects;

    // a neighbour in the same segment seen by the same passes grows instead, so a surface of triangles stays one object.
    if (stream->object_count > segment_objects && stream->objects[stream->object_count - 1].passes == passes) return true;

    stream->objects[stream->object_count++] = (NYA_Render3DObject){ .first_index = stream->index_count, .passes = passes };

    return true;
}

b8 _nya_render3d_reserve(NYA_Window* window, u32 vertices, u32 indices, SDL_GPUTexture* texture, SDL_GPUSampler* sampler) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (vertices > NYA_RENDER3D_MAX_VERTICES || indices > NYA_RENDER3D_MAX_INDICES) {
        // larger than an empty batch: dropped and counted, not drawn partially.
        batch->frame_dropped_draws++;
        return false;
    }

    /* A different texture ends the segment, as a different material does. */
    if (batch->texture != texture || batch->sampler != sampler) {
        nya_render3d_flush(window);

        batch->texture = texture;
        batch->sampler = sampler;
    }

    /* Both streams share one capacity because they share one GPU buffer. */
    u32 staged_vertices = batch->opaque.vertex_count + batch->transparent.vertex_count;
    u32 staged_indices  = batch->opaque.index_count + batch->transparent.index_count;

    if (staged_vertices + vertices > NYA_RENDER3D_MAX_VERTICES || staged_indices + indices > NYA_RENDER3D_MAX_INDICES) {
        nya_render3d_flush(window);
        _nya_render3d_playback(window);
    }

    return true;
}

NYA_Render3DStream* _nya_render3d_stream(NYA_Render3DBatch* batch) {
    return batch->transparent_active ? &batch->transparent : &batch->opaque;
}

void _nya_render3d_segment_open(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    nya_assert(batch->segment_count < NYA_RENDER3D_MAX_SEGMENTS);

    batch->segments[batch->segment_count] = (NYA_Render3DSegment){
        .opaque_objects      = batch->opaque.object_count,
        .transparent_objects = batch->transparent.object_count,
        .first_group         = batch->mesh_group_count,
        .first_decal         = render->decals_gpu.count,
    };
}

void _nya_render3d_segment_close(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;
    NYA_Render3DSegment*    open   = &batch->segments[batch->segment_count];

    nya_assert(batch->mesh_group_count >= open->first_group);

    open->group_count = batch->mesh_group_count - open->first_group;

    // switching decals off mid-scene empties them.
    open->decal_count   = render->decals_gpu.count > open->first_decal ? render->decals_gpu.count - open->first_decal : 0;
    open->decal_texture = render->decals_gpu.texture;

    // the switches are applied once here, where a segment's state is frozen, rather than on every draw.
    b8 textured = nya_render_feature_enabled(window, NYA_RENDER_FEATURE_TEXTURES);

    open->texture  = textured ? batch->texture : nullptr;
    open->sampler  = textured ? batch->sampler : nullptr;
    open->material = batch->material;
    open->blend    = batch->blend;

    // refraction is a reflection of the scene behind, and zero here keeps the glass pass and its capture out of
    // the playback entirely.
    if (!nya_render_feature_enabled(window, NYA_RENDER_FEATURE_REFLECTIONS)) open->material.refraction = 0.0F;

    // no depth test means the overlay pipeline, which neither tests nor writes: the scene draws in the order it
    // was recorded. A pipeline variant per switch would be a second build of every pipeline for the same picture.
    open->depth = nya_render_feature_enabled(window, NYA_RENDER_FEATURE_DEPTH_TEST) ? batch->depth : NYA_RENDER3D_DEPTH_OVERLAY;

    open->casts_shadow = batch->casts_shadow;

    batch->segment_uniforms[batch->segment_count] = _nya_render3d_shading_uniform(window);

    batch->segment_count++;
    batch->segment_count_worst = nya_max(batch->segment_count_worst, batch->segment_count);

    // full: the scene so far draws now, which opens the next segment itself.
    if (batch->segment_count == NYA_RENDER3D_MAX_SEGMENTS) {
        _nya_render3d_playback(window);
        return;
    }

    _nya_render3d_segment_open(window);
}
