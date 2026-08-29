#include "assets/shader/uniforms.h"

#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_internal.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Room for `vertices` more vertices and `indices` more indices, flushing first if needed; false only when
 * the request cannot fit even an empty batch (a primitive larger than NYA_RENDER3D_MAX_VERTICES). The
 * texture is passed in here rather than set separately so no caller can forget it: a primitive queued
 * after a textured mesh would otherwise draw against its atlas at UV zero — the 3D demo's particle dust
 * sat exactly there in the draw order. Null is the untextured pipeline.
 * */
NYA_INTERNAL b8 _nya_render3d_reserve(NYA_Window* window, u32 vertices, u32 indices, SDL_GPUTexture* texture, SDL_GPUSampler* sampler);

/**
 * Appends one vertex and answers where it landed. `uv` is zero for every generated primitive — the
 * untextured pipeline never reads it; only nya_render3d_mesh passes a real one.
 * */
NYA_INTERNAL u32 _nya_render3d_vertex(NYA_Render3DBatch* batch, f32x3 position, f32x3 normal, NYA_Color color, f32x2 uv);

/** Two triangles over four corners, wound counter-clockwise seen from outside. */

/** Fills in whatever the caller left at zero, so a `{ 0 }` camera still renders something. */
NYA_INTERNAL NYA_Camera3DPerspective  _nya_render3d_camera_defaults(NYA_Camera3DPerspective camera);
NYA_INTERNAL NYA_Camera3DOrthographic _nya_render3d_camera_orthographic_defaults(NYA_Camera3DOrthographic camera);

/** Shared tail of both begin functions: flush 2D, store the view-projection, reset light and material. */
NYA_INTERNAL void _nya_render3d_begin_with(NYA_Window* window, f32_4x4 view_projection, f32x3 eye);

/** Creates the shadow map and its depth buffer if they are not there yet. False when the GPU refused. */
NYA_INTERNAL b8 _nya_render3d_shadow_ensure(NYA_Window* window);

/**
 * The shadow texture's side, holding the cascades as a two-by-two atlas, always twice the per-cascade
 * size regardless of cascade count — one cascade wastes three quarters of the texture (four megabytes at
 * the default size), the price of the count being a compile-time knob rather than a resized texture.
 * An atlas rather than a texture array: one binding, one sampler, no array-texture support needed across
 * backends. The cost is a cascade's filter kernel reaching into its neighbour at a quadrant edge, which
 * the inset in mesh3d_shadow guards against.
 * */
#define _NYA_RENDER3D_SHADOW_ATLAS_SIZE (NYA_RENDER3D_SHADOW_MAP_SIZE * 2)

/** A quad with real texture coordinates and a texture bound. The billboard path; see its note there. */
NYA_INTERNAL void _nya_render3d_quad_textured(
    NYA_Window*     window,
    f32x3           a,
    f32x3           b,
    f32x3           c,
    f32x3           d,
    SDL_GPUTexture* texture,
    SDL_GPUSampler* sampler,
    NYA_Color       color
);

/** Whichever staging stream the primitives are currently writing into. See NYA_Render3DStream. */
NYA_INTERNAL NYA_Render3DStream* _nya_render3d_stream(NYA_Render3DBatch* batch) __attr_no_discard;

/** Selects that stream from a primitive's colour. Called before the reserve, not after. */
NYA_INTERNAL void _nya_render3d_route(NYA_Render3DBatch* batch, NYA_Color color);

/** Orders the transparent stream's triangles back to front. Nothing to do for fewer than two. */
NYA_INTERNAL void _nya_render3d_sort_transparent(NYA_Render3DBatch* batch, f32x3 eye);


/** Extracts the six inward-facing clip planes of `batch->view_projection`. Once per pass, not per draw. */
NYA_INTERNAL void _nya_render3d_frustum_build(NYA_Render3DBatch* batch);

/**
 * Whether a bounding sphere is on the visible side of every frustum plane.
 *
 * A sphere rather than a box: one dot product per plane against six for a box's furthest corner, it is
 * invariant under rotation, and it is *conservative* — it can say yes to something actually outside
 * (a wasted primitive) but never no to something inside (a hole in the picture).
 * */
NYA_INTERNAL b8 _nya_render3d_visible(const NYA_Render3DBatch* batch, f32x3 center, f32 radius) __attr_no_discard;

/**
 * Copies the current colour target into the refraction capture, creating or resizing it as needed.
 * False when there is nothing to capture from — no render texture target, no device, failed allocation
 * — in which case glass falls back to ordinary blending rather than sampling a stale frame.
 * */
NYA_INTERNAL b8 _nya_render3d_refraction_capture(NYA_Window* window);

/** The registered mesh for `handle`, or null. Linear over a small table; see the note on the array. */
NYA_INTERNAL NYA_Render3DRegisteredMesh* _nya_render3d_registered(NYA_Render3DBatch* batch, NYA_ConstCString handle) __attr_no_discard;

/**
 * Creates a GPU vertex buffer and a transfer buffer already filled with `vertices`. False on failure.
 * Deliberately does *not* perform the copy: that needs an open command buffer, and this is reachable
 * from outside a frame. See NYA_Render3DRegisteredMesh.pending_upload.
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

/** The group for `handle` this pass, appending one if it is the first copy. Null when the table is full. */
NYA_INTERNAL NYA_Render3DMeshGroup* _nya_render3d_mesh_group(NYA_Render3DBatch* batch, NYA_ConstCString handle, b8 transparent);

/** The fragment uniform block, built from the batch's light, material and shadow state. */
NYA_INTERNAL struct NYA_ShaderMesh3DUniform _nya_render3d_shading_uniform(const NYA_Render3DBatch* batch) __attr_no_discard;

/** Binds the shadow map, and the base colour before it when there is one. Skipped during a shadow pass. */
NYA_INTERNAL b8 _nya_render3d_bind_samplers(NYA_Window* window, SDL_GPUTexture* texture, SDL_GPUSampler* sampler);

/** Draws the CPU-baked triangles. The path everything generated per frame goes through. */
NYA_INTERNAL void _nya_render3d_flush_immediate(NYA_Window* window, const struct NYA_ShaderMesh3DUniform* uniform);

/** Draws the queued mesh groups, one instanced call per mesh part. See NYA_Render3DInstance. */
NYA_INTERNAL void _nya_render3d_flush_instanced(NYA_Window* window, const struct NYA_ShaderMesh3DUniform* uniform);

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

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    camera = _nya_render3d_camera_defaults(camera);

    // Nesting inside the shadow pass is a no-op: it has already installed the light's matrix, and
    // overwriting it with the camera's would map what the camera sees, not the light. The camera is
    // still recorded so screen rays can be cast during the pass.
    if (batch->shadow_pass_active) {
        batch->camera          = camera;
        batch->camera_is_ortho = false;
        batch->camera_valid    = true;
        return;
    }

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    // From the target, not the window, so a render texture of a different shape is not stretched.
    // Guarded because a minimised window reports zero and an aspect of zero asserts in the projection.
    f32 aspect = target_height > 0 ? (f32)target_width / (f32)target_height : 1.0F;

    f32_4x4 projection = nya_matrix_perspective(camera.fov_y, aspect, camera.near_plane, camera.far_plane);
    f32_4x4 view       = nya_matrix_look_at(camera.position, camera.target, camera.up);

    _nya_render3d_begin_with(window, projection * view, camera.position);

    // Kept so nya_render3d_screen_ray can unproject without inverting a matrix: it rebuilds the ray
    // from the camera basis directly, which is both cheaper and better conditioned.
    window->render_system.mesh_batch.camera          = camera;
    window->render_system.mesh_batch.camera_is_ortho = false;
    window->render_system.mesh_batch.camera_valid    = true;
}

void nya_render3d_begin_orthographic(NYA_Window* window, NYA_Camera3DOrthographic camera) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    camera = _nya_render3d_camera_orthographic_defaults(camera);

    if (batch->shadow_pass_active) {
        batch->camera_orthographic = camera;
        batch->camera_is_ortho     = true;
        batch->camera_valid        = true;
        return;
    }

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    f32 aspect = target_height > 0 ? (f32)target_width / (f32)target_height : 1.0F;

    f32_4x4 projection = nya_matrix_orthographic_3d(camera.height, aspect, camera.near_plane, camera.far_plane);
    f32_4x4 view       = nya_matrix_look_at(camera.position, camera.target, camera.up);

    _nya_render3d_begin_with(window, projection * view, camera.position);

    window->render_system.mesh_batch.camera_orthographic = camera;
    window->render_system.mesh_batch.camera_is_ortho     = true;
    window->render_system.mesh_batch.camera_valid        = true;
}

void nya_render3d_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;
    if (!batch->active) return;


    // Drawn now, so anything render2d puts down afterwards lands in front of it. The 2D pipelines do
    // not test depth, so "in front" is decided purely by which flush happened last.
    nya_render3d_flush(window);

    // The shadow pass is still active: ending here would clear `active` and stop everything drawn
    // after this point reaching the shadow map. The pass is ended by nya_render3d_shadow_end.
    if (batch->shadow_pass_active) return;

    batch->active = false;

    // The shadow map expires with the frame that filled it. Cleared here rather than at the next
    // shadow pass's start, so a frame that stops calling nya_render3d_shadow_begin draws unshadowed
    // immediately instead of being lit by a map of geometry no longer there.
    batch->shadow_valid = false;

    // And with it the record of which cascades ran, so a frame filling fewer than the last does not
    // leave the shader indexing a matrix from the previous frame's deeper cascade.
    batch->shadow_cascade_count = 0;
}

b8 nya_render3d_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.active;
}

void nya_render3d_sky_draw(NYA_Window* window, NYA_Render3DSky sky) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // No camera, no basis to shade a ray from. Silent rather than asserted: a layer that draws a sky is
    // usually the same one that sets the camera, and the order between them is already checked by
    // everything else refusing to draw.
    if (!batch->active) return;

    /*
     * Never into a shadow map — a real bug, not a precaution. A shadow pass sets `active` through the same
     * path the scene pass does, so the sky drew a fullscreen triangle into each cascade through a *2D*
     * pipeline built for the swapchain's colour format, into an R32_FLOAT depth target: its red channel
     * landed as depth everywhere uncovered — around 0.3 to 0.9 instead of the 1.0 meaning "nothing here" —
     * so roughly half the scene read as occluded and picked up a shadow nothing cast, three times a frame
     * with three cascades. The sky is the one thing in a scene that is *not* geometry, so the pass-agnostic
     * rule does not reach it: it occludes nothing and belongs in no depth map.
     */
    if (batch->shadow_pass_active) return;

    u32 target_width  = 0;
    u32 target_height = 0;

    nya_render2d_target_size(window, &target_width, &target_height);

    if (target_width == 0 || target_height == 0) return;

    // The camera basis, rebuilt the way nya_render3d_screen_ray rebuilds it — cheaper and better
    // conditioned than inverting the view-projection, and shared so the sky and the picker agree which
    // way a pixel looks.
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    // Zero means unspecified throughout, matching every other options struct here. The defaults are a
    // clear midday sky, so a caller who fills in nothing but a direction gets a sky rather than black.
    NYA_Color zenith  = sky.zenith.a > 0.0F ? sky.zenith : (NYA_Color){ 0.28F, 0.51F, 0.85F, 1.0F };
    NYA_Color horizon = sky.horizon.a > 0.0F ? sky.horizon : (NYA_Color){ 0.72F, 0.84F, 0.96F, 1.0F };
    NYA_Color ground  = sky.ground.a > 0.0F ? sky.ground : (NYA_Color){ 0.16F, 0.18F, 0.24F, 1.0F };
    NYA_Color sun     = sky.sun_color.a > 0.0F ? sky.sun_color : (NYA_Color){ 1.0F, 0.96F, 0.84F, 1.0F };

    f32x3 sun_direction = sky.sun_direction;

    if (nya_vector_length(sun_direction) < NYA_EPSILON) sun_direction = (f32x3){ 0.0F, 1.0F, 0.0F };

    sun_direction = nya_vector_normalize(sun_direction);

    // Half a degree by default, which is life-size. The shader wants the cosine of the radius, because
    // comparing that against a dot product is exact and needs no inverse trigonometry per pixel.
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

    // Anything the 3D batch has queued goes out first: the sky writes no depth, so geometry drawn before
    // it would be painted over, and the procedural draw below binds a different pipeline regardless.
    nya_render3d_flush(window);

    nya_render2d_procedural(window, NYA_RENDER3D_PIPELINE_SKY, 3, &uniform, sizeof(uniform));
}

void nya_render3d_outline_set(NYA_Window* window, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Batch state like the material is, so a change has to flush first — the outline is decided per draw
    // call and queued instances have already been recorded under the previous setting.
    if (thickness != batch->outline_thickness) nya_render3d_flush(window);

    batch->outline_thickness = nya_max(thickness, 0.0F);
    batch->outline_color     = color;
}

void nya_render3d_blend_set(NYA_Window* window, NYA_Render3DBlend blend) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Batch state, so a change flushes: it selects the pipeline, and the pipeline is per draw call.
    if (blend != batch->blend) nya_render3d_flush(window);

    batch->blend = blend;
}

void nya_render3d_depth_set(NYA_Window* window, NYA_Render3DDepth depth) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Batch state, so a change flushes: it selects the pipeline, and the pipeline is per draw call.
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

    if (!batch->active) return;

    f32x2 half = size * 0.5F;

    // The bounding sphere is the quad's half-diagonal, which is the same whichever way it is spun — the
    // rotation is in the view plane, so it cannot move a corner further out than this.
    if (!_nya_render3d_visible(batch, center, nya_vector_length(half))) {
        batch->frame_culled++;
        return;
    }

    // The camera's right and up, rebuilt the way nya_render3d_screen_ray rebuilds them: two axes in the
    // view plane so the quad faces the viewer from every angle — axes from anywhere but the camera flip
    // the quad inside out as it turns (see nya_particles_draw's note). During a *shadow* pass these are
    // the light's, since the shadow pass installs the light's camera through the same fields — a
    // billboard casts the shadow of a quad facing the light.
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    // Spun in the view plane, not about a world axis: turning the axes themselves keeps the quad flat to
    // the camera while it rotates — rotating the finished corners about anything else would tip it out.
    f32 cosine = cosf(rotation);
    f32 sine   = sinf(rotation);

    f32x3 axis_x = (right * cosine) + (up * sine);
    f32x3 axis_y = (up * cosine) - (right * sine);

    f32x3 offset_x = axis_x * half.x;
    f32x3 offset_y = axis_y * half.y;

    // Wound so the face normal comes out along the view axis, toward the camera — the quad is single
    // sided and the pipelines cull back faces, so this decides whether it is visible at all.
    //
    // The texture is resolved here rather than passed down as a handle: the batch compares *bound
    // textures* to decide when to flush, not handles, so two handles naming the same image can merge
    // into one draw call. A handle naming nothing, or something still loading, draws untextured.
    _nya_render3d_quad_textured(
        window,
        center - offset_x - offset_y,
        center - offset_x + offset_y,
        center + offset_x + offset_y,
        center + offset_x - offset_y,
        texture.texture,
        texture.sampler,
        color
    );
}

NYA_Render3DTextureBinding nya_render3d_texture_resolve(NYA_ConstCString texture_handle) {
    if (texture_handle == nullptr) return (NYA_Render3DTextureBinding){ 0 };

    // Resolved to a *bound texture* rather than kept as a handle, since the batch compares bound
    // textures to decide when to flush, not handles. A handle naming nothing, or something still
    // loading, resolves to nothing and draws untextured — same answer nya_render3d_mesh gives.
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
    // The convenience form: one lookup per call, which is right for a handful and wrong for a crowd.
    nya_render3d_billboard_resolved(window, nya_render3d_texture_resolve(texture_handle), center, size, rotation, color);
}

void nya_render3d_light_set(NYA_Window* window, NYA_Render3DLight light) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Part of the fragment uniform, which is per draw call — so what is already queued has to be
    // drawn under the light it was queued with.
    nya_render3d_flush(window);

    if (light.direction.x == 0.0F && light.direction.y == 0.0F && light.direction.z == 0.0F) light.direction = (f32x3){ 0.0F, -1.0F, 0.0F };
    if (light.intensity <= 0.0F) light.intensity = 1.0F;

    batch->light = light;
}

NYA_Render3DLight nya_render3d_light(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.light;
}

/**
 * The light a pass installs when the caller has not chosen one. Extracted because two places need the
 * same answer: nya_render3d_begin, which resets the light every frame so appearance cannot depend on
 * what the last frame set, and the shadow pass, which must build its matrix from the *same* direction —
 * two copies would be two chances for the shadow to land where the light is not coming from.
 * */
NYA_INTERNAL NYA_Render3DLight _nya_render3d_default_light(void) {
    return (NYA_Render3DLight){
        .direction = NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT,
        .color     = NYA_COLOR_WHITE,
        // High, because the shading model is cartoon: this is the darkest shade an object reaches and a
        // flat colour must still read as itself there. See NYA_Render3DLight.ambient.
        .ambient   = 0.6F,
        .intensity = 1.0F,
    };
}

void nya_render3d_shadow_begin(NYA_Window* window, NYA_Render3DShadow shadow) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    nya_assert(!batch->shadow_pass_active, "nya_render3d_shadow_begin does not nest; end the current pass first");
    nya_assert(!batch->active, "the shadow pass has to come before nya_render3d_begin, not inside it");

    // Zero strength is how a caller turns shadows off without removing the calls, so it draws nothing at
    // all rather than filling a map nothing will sample.
    if (shadow.strength <= 0.0F) return;

    if (render->render_pass == nullptr) return;
    if (!_nya_render3d_shadow_ensure(window)) return;

    // The cascade index decides how wide this pass's volume is: `extent` names the *nearest* cascade, and
    // each one after it is NYA_RENDER3D_SHADOW_CASCADE_RATIO times wider over the same resolution, so the
    // near map is dense where the camera is and the far one covers the distance. Clamped rather than
    // asserted: a caller looping past the compiled-in count just refills the last cascade (a wasted pass)
    // rather than writing past the end of the arrays.
    u32 cascade = nya_min(shadow.cascade, (u32)(NYA_RENDER3D_SHADOW_CASCADES - 1));

    shadow.extent = nya_render3d_cascade_extent(shadow.extent, cascade);

    if (shadow.depth <= 0.0F) shadow.depth = shadow.extent * 4.0F;
    if (shadow.bias <= 0.0F) shadow.bias = NYA_RENDER3D_SHADOW_BIAS;

    shadow.strength = nya_clamp(shadow.strength, 0.0F, 1.0F);

    // The light the *scene* will be lit by, which is not always the one currently on the batch:
    // nya_render3d_begin resets the light to the default every frame, so a caller that has not set one
    // yet has a zeroed light here while the scene pass is about to use the default. Taking the default in
    // that case keeps the shadow and the shading agreeing on where the sun is.
    NYA_Render3DLight light = batch->light;

    b8 light_is_unset = light.direction.x == 0.0F && light.direction.y == 0.0F && light.direction.z == 0.0F;

    if (light_is_unset) light = _nya_render3d_default_light();

    f32x3 direction, right, up;
    nya_render3d_light_basis(light.direction, &direction, &right, &up);

    nya_unused(right);

    // A directional light has no position, so one is invented: back along the light by half the depth,
    // far enough that the whole volume is in front of it. `direction` is the way light travels, so
    // backing off means subtracting it.
    f32x3 eye = shadow.center - (direction * (shadow.depth * 0.5F));

    // Orthographic, because a directional light's rays are parallel. Aspect one: the map is square.
    f32_4x4 projection = nya_matrix_orthographic_3d(shadow.extent * 2.0F, 1.0F, 0.01F, shadow.depth);
    f32_4x4 view       = nya_matrix_look_at(eye, shadow.center, up);

    batch->shadow          = shadow;
    batch->shadow_cascade  = cascade;

    batch->shadow_view_projection[cascade] = projection * view;
    batch->shadow_cascade_extent[cascade]  = shadow.extent;

    // The highest cascade index reached this frame plus one, so a caller running fewer than the maximum
    // gets exactly the ones it filled and the shader indexes none of the stale entries beyond them.
    if (cascade + 1 > batch->shadow_cascade_count) batch->shadow_cascade_count = cascade + 1;

    // Anything the 2D batch has queued belongs to the window, not to a thousand pixel square depth map.
    _nya_render2d_pass_suspend(window);

    render->render_pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture = batch->shadow_color,
            // White is the far plane in this encoding: depth is in [0, 1] and anything the light did not
            // see has to compare as further away than every surface, or the whole map reads as occluded.
            .clear_color = (SDL_FColor){ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },

            // Only the first cascade of the frame clears, since the clear covers the whole atlas and a
            // load op has no notion of the viewport set afterwards; later cascades LOAD instead.
            .load_op  = cascade == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
            .store_op = SDL_GPU_STOREOP_STORE,
        },
        1,
        &(SDL_GPUDepthStencilTargetInfo){
            .texture          = batch->shadow_depth,
            .clear_depth      = 1.0F,

            // Cleared every cascade, unlike the colour: a cascade's depth test is only ever against its
            // own geometry, and keeping the previous cascade's depths would reject everything behind them.
            .load_op          = SDL_GPU_LOADOP_CLEAR,
            .store_op         = SDL_GPU_STOREOP_DONT_CARE,
            .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
            .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
        }
    );

    if (render->render_pass == nullptr) {
        nya_log_error("SDL_BeginGPURenderPass() failed for the shadow pass: %s", SDL_GetError());
        _nya_render2d_pass_resume(window);
        return;
    }

    // The viewport restricts this pass to its quadrant of the atlas. A viewport rather than a scissor,
    // because it has to *transform* as well as clip: clip space still spans the whole target, and only
    // the viewport maps that onto a quarter of it — a scissor would draw the cascade at full size and
    // then throw away three quarters of it.
    SDL_SetGPUViewport(
        render->render_pass,
        &(SDL_GPUViewport){
            .x         = (f32)((cascade % 2) * NYA_RENDER3D_SHADOW_MAP_SIZE),
            .y         = (f32)((cascade / 2) * NYA_RENDER3D_SHADOW_MAP_SIZE),
            .w         = (f32)NYA_RENDER3D_SHADOW_MAP_SIZE,
            .h         = (f32)NYA_RENDER3D_SHADOW_MAP_SIZE,
            .min_depth = 0.0F,
            .max_depth = 1.0F,
        }
    );

    batch->shadow_pass_active = true;

    // The batch is brought up exactly as the scene pass brings it up, with the light's matrix — every
    // draw call below goes through the same vertex path and uniform slot, so a game's draw function
    // needs no idea which pass it is in.
    _nya_render3d_begin_with(window, batch->shadow_view_projection[cascade], eye);

    // Put back the light the matrix was built from: _nya_render3d_begin_with resets it, right for the
    // scene pass but wrong here, since the shadow map was already positioned by `light`.
    batch->light = light;
}

void nya_render3d_shadow_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    if (!batch->shadow_pass_active) return;

    // Flushed while the shadow pipeline is still selected, or the last run of triangles would be drawn
    // into the map by the scene shader.
    nya_render3d_flush(window);

    batch->active             = false;
    batch->shadow_pass_active = false;
    batch->shadow_valid       = true;

    if (render->render_pass != nullptr) SDL_EndGPURenderPass(render->render_pass);

    render->render_pass = nullptr;

    // Back to whatever the 2D batch was aimed at, which is the window or the bloom target.
    _nya_render2d_pass_resume(window);
}

b8 nya_render3d_shadow_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.mesh_batch.shadow_valid;
}

b8 _nya_render3d_shadow_ensure(NYA_Window* window) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (batch->shadow_color != nullptr && batch->shadow_depth != nullptr) return true;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;
    if (gpu_device == nullptr) return false;

    // Single sampled, unlike every other target here: a multisampled target would need resolving before
    // it could be read, and there is nothing to antialias in a depth map — averaging two distances
    // describes neither surface. The filter in the shader is what softens the edges instead.
    batch->shadow_color = SDL_CreateGPUTexture(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
            .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = _NYA_RENDER3D_SHADOW_ATLAS_SIZE,
            .height               = _NYA_RENDER3D_SHADOW_ATLAS_SIZE,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = SDL_GPU_SAMPLECOUNT_1,
        }
    );

    if (batch->shadow_color == nullptr) {
        nya_log_error("SDL_CreateGPUTexture() failed for the shadow map: %s", SDL_GetError());
        return false;
    }

    batch->shadow_depth = SDL_CreateGPUTexture(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = nya_app_get()->render_system.depth_format,
            .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            .width                = _NYA_RENDER3D_SHADOW_ATLAS_SIZE,
            .height               = _NYA_RENDER3D_SHADOW_ATLAS_SIZE,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = SDL_GPU_SAMPLECOUNT_1,
        }
    );

    if (batch->shadow_depth == nullptr) {
        nya_log_error("SDL_CreateGPUTexture() failed for the shadow map's depth buffer: %s", SDL_GetError());

        SDL_ReleaseGPUTexture(gpu_device, batch->shadow_color);
        batch->shadow_color = nullptr;
        return false;
    }

    nya_log_debug("Shadow atlas created at %dx%d: %d cascades of %dx%d.", _NYA_RENDER3D_SHADOW_ATLAS_SIZE, _NYA_RENDER3D_SHADOW_ATLAS_SIZE,
              NYA_RENDER3D_SHADOW_CASCADES, NYA_RENDER3D_SHADOW_MAP_SIZE, NYA_RENDER3D_SHADOW_MAP_SIZE);

    return true;
}

void nya_render3d_point_light_add(NYA_Window* window, NYA_Render3DPointLight light) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Dropped loudly rather than quietly: a light past the budget that simply did not appear would look
    // like a light with the wrong colour or position, debugged for the wrong reason.
    if (batch->point_light_count >= NYA_RENDER3D_MAX_POINT_LIGHTS) {
        nya_log_warn("A fifth point light was added and dropped; NYA_RENDER3D_MAX_POINT_LIGHTS is %d.", NYA_RENDER3D_MAX_POINT_LIGHTS);
        return;
    }

    // The whole set is a fragment uniform, so anything already queued was lit by the previous set.
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

    // Clamped rather than asserted: these are values a designer tunes, frequently by dragging a
    // slider past its end, and a scene that clamps is better than one that aborts.
    material.metallic  = nya_clamp(material.metallic, 0.0F, 1.0F);
    material.roughness = nya_clamp(material.roughness, 0.02F, 1.0F);

    // Not clamped to one: a value above it is how an emissive surface is lifted past a bloom threshold,
    // which is the whole reason to have it. Clamped below, because negative emission is not a darkness.
    material.emission = nya_max(material.emission, 0.0F);

    material.edge = nya_clamp(material.edge, 0.0F, 1.0F);

    // Zero is not a reflectance anyone means; it is a field nobody filled in. The glTF default.
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

    // Culled before a single corner is rotated, and tested here rather than inside nya_render3d_quad: a
    // cube tested once is one sphere test, tested per face is six. The sphere is the half-diagonal,
    // rotation-invariant, so a turning cube needs no re-derivation. See _nya_render3d_visible.
    NYA_Render3DBatch* cube_batch = &window->render_system.mesh_batch;

    if (!_nya_render3d_visible(cube_batch, center, nya_vector_length(half))) {
        cube_batch->frame_culled++;
        return;
    }

    // The eight corners, rotated and translated on the CPU: a model matrix would be a uniform, a uniform
    // is per draw call, and a draw call per cube is exactly what this exists to avoid.
    f32x3 corners[8];
    for (u32 i = 0; i < 8; i++) {
        f32x3 local = {
            (i & 1) ? half.x : -half.x,
            (i & 2) ? half.y : -half.y,
            (i & 4) ? half.z : -half.z,
        };

        corners[i] = center + nya_quaternion_rotate(rotation, local);
    }

    // Counter-clockwise seen from outside, so back-face culling keeps the faces you can see. Getting
    // a winding backwards makes exactly one face of the cube vanish, which reads as a hole.
    nya_render3d_quad(window, corners[0], corners[2], corners[3], corners[1], color); // -z
    nya_render3d_quad(window, corners[5], corners[7], corners[6], corners[4], color); // +z
    nya_render3d_quad(window, corners[4], corners[6], corners[2], corners[0], color); // -x
    nya_render3d_quad(window, corners[1], corners[3], corners[7], corners[5], color); // +x
    nya_render3d_quad(window, corners[0], corners[1], corners[5], corners[4], color); // -y
    nya_render3d_quad(window, corners[6], corners[7], corners[3], corners[2], color); // +y
}

void nya_render3d_cube_outline(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    // One test for the whole outline, as the cube does. Its twelve edges are lines, and each of those
    // would otherwise test again — twelve answers to a question the shape has already answered.
    NYA_Render3DBatch* outline_batch = &window->render_system.mesh_batch;

    if (!_nya_render3d_visible(outline_batch, center, nya_vector_length(size * 0.5F) + thickness)) {
        outline_batch->frame_culled++;
        return;
    }

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

    // The twelve edges of a cube, as pairs of corner indices. A table rather than three nested loops
    // because the loop form is write-only: nobody can check it by eye against a drawing.
    static const u32 edges[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, // along x
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, // along y
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }, // along z
    };

    for (u32 i = 0; i < 12; i++) nya_render3d_line(window, corners[edges[i][0]], corners[edges[i][1]], thickness, color);
}

void nya_render3d_sphere(NYA_Window* window, f32x3 center, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    // The cheapest cull in the renderer: a sphere primitive already *is* its own bounding sphere, and
    // it is also the most expensive primitive to emit — NYA_RENDER3D_SPHERE_SEGMENTS squared quads.
    NYA_Render3DBatch* sphere_batch = &window->render_system.mesh_batch;

    if (!_nya_render3d_visible(sphere_batch, center, radius)) {
        sphere_batch->frame_culled++;
        return;
    }

    if (radius <= 0.0F) return;

    const u32 segments = NYA_RENDER3D_SPHERE_SEGMENTS;
    const u32 rings    = NYA_RENDER3D_SPHERE_SEGMENTS / 2;

    // Quads rather than triangles, and the poles degenerate into quads with two coincident corners.
    // Special-casing the poles into triangle fans would save a handful of vertices and add a branch
    // to a loop that is already the cheapest part of drawing a sphere.
    for (u32 ring = 0; ring < rings; ring++) {
        f32 phi_0 = (f32)M_PI * ((f32)ring / (f32)rings);
        f32 phi_1 = (f32)M_PI * ((f32)(ring + 1) / (f32)rings);

        for (u32 segment = 0; segment < segments; segment++) {
            f32 theta_0 = 2.0F * (f32)M_PI * ((f32)segment / (f32)segments);
            f32 theta_1 = 2.0F * (f32)M_PI * ((f32)(segment + 1) / (f32)segments);

            // On a unit sphere the position and the normal are the same vector, which is the one
            // place in this file where the normal does not have to be worked out separately.
            f32x3 a = { sinf(phi_0) * cosf(theta_0), cosf(phi_0), sinf(phi_0) * sinf(theta_0) };
            f32x3 b = { sinf(phi_0) * cosf(theta_1), cosf(phi_0), sinf(phi_0) * sinf(theta_1) };
            f32x3 c = { sinf(phi_1) * cosf(theta_1), cosf(phi_1), sinf(phi_1) * sinf(theta_1) };
            f32x3 d = { sinf(phi_1) * cosf(theta_0), cosf(phi_1), sinf(phi_1) * sinf(theta_0) };

            // Routed here rather than through nya_render3d_quad, because a sphere is the one primitive
            // that emits its own vertices — its normals are the surface's, not the quad's face normal.
            _nya_render3d_route(sphere_batch, color);

            if (!_nya_render3d_reserve(window, 4, 6, nullptr, nullptr)) return;

            NYA_Render3DStream* stream = _nya_render3d_stream(sphere_batch);

            u32 base = stream->vertex_count;

            (void)_nya_render3d_vertex(sphere_batch, center + (a * radius), a, color, f32x2_zero);
            (void)_nya_render3d_vertex(sphere_batch, center + (b * radius), b, color, f32x2_zero);
            (void)_nya_render3d_vertex(sphere_batch, center + (c * radius), c, color, f32x2_zero);
            (void)_nya_render3d_vertex(sphere_batch, center + (d * radius), d, color, f32x2_zero);

            stream->indices[stream->index_count++] = base + 0;
            stream->indices[stream->index_count++] = base + 1;
            stream->indices[stream->index_count++] = base + 2;
            stream->indices[stream->index_count++] = base + 0;
            stream->indices[stream->index_count++] = base + 2;
            stream->indices[stream->index_count++] = base + 3;
        }
    }
}

void nya_render3d_plane(NYA_Window* window, f32x3 center, f32x2 size, NYA_Color color) {
    nya_assert(window != nullptr);

    f32x2 half = size * 0.5F;

    // Culled like the other shapes. It was not, along with the line and the grid, so a scene drawing a
    // large ground plane paid for it from every angle including the ones facing away from it.
    NYA_Render3DBatch* plane_batch = &window->render_system.mesh_batch;

    if (!_nya_render3d_visible(plane_batch, center, nya_vector_length(half))) {
        plane_batch->frame_culled++;
        return;
    }

    nya_render3d_quad(
        window,
        center + (f32x3){ -half.x, 0.0F, half.y },
        center + (f32x3){ half.x, 0.0F, half.y },
        center + (f32x3){ half.x, 0.0F, -half.y },
        center + (f32x3){ -half.x, 0.0F, -half.y },
        color
    );
}

void nya_render3d_triangle(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, NYA_Color color) {
    nya_assert(window != nullptr);

    /*
     * Culled even though a triangle is only three vertices: worth it because a surface built from
     * thousands of them has most behind the camera on any given frame. The test is six dot products
     * against ~190 bytes of vertex writes plus upload share, paying for itself above roughly one hit in
     * twenty. The bounding sphere is the circumcircle's, loose for a long thin triangle — the safe
     * direction.
     */
    NYA_Render3DBatch* triangle_batch = &window->render_system.mesh_batch;

    // Before the reserve, so the capacity check and the emit both see the stream this belongs in.
    _nya_render3d_route(triangle_batch, color);

    f32x3 centroid = (a + b + c) / 3.0F;

    f32 radius = nya_max(nya_vector_length(a - centroid), nya_max(nya_vector_length(b - centroid), nya_vector_length(c - centroid)));

    if (!_nya_render3d_visible(triangle_batch, centroid, radius)) {
        triangle_batch->frame_culled++;
        return;
    }

    if (!_nya_render3d_reserve(window, 3, 3, nullptr, nullptr)) return;

    NYA_Render3DStream* stream = _nya_render3d_stream(triangle_batch);

    NYA_Render3DBatch* batch = triangle_batch;
    nya_unused(batch);

    /*
     * The face normal, from the winding, shared by all three vertices — not even a choice, since three
     * points describe a plane. Not normalized away from zero: a degenerate triangle (coincident or
     * collinear points) gives a zero cross product and nya_vector_normalize hands back zero, shading as
     * unlit rather than undefined, cheaper than a length-and-branch check on every triangle of a surface.
     */
    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    u32 base = stream->vertex_count;

    (void)_nya_render3d_vertex(triangle_batch, a, normal, color, f32x2_zero);
    (void)_nya_render3d_vertex(triangle_batch, b, normal, color, f32x2_zero);
    (void)_nya_render3d_vertex(triangle_batch, c, normal, color, f32x2_zero);

    stream->indices[stream->index_count++] = base + 0;
    stream->indices[stream->index_count++] = base + 1;
    stream->indices[stream->index_count++] = base + 2;
}

void _nya_render3d_quad_textured(
    NYA_Window*     window,
    f32x3           a,
    f32x3           b,
    f32x3           c,
    f32x3           d,
    SDL_GPUTexture* texture,
    SDL_GPUSampler* sampler,
    NYA_Color       color
) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    _nya_render3d_route(batch, color);

    // The texture reaches the reserve, which is what makes a change to it flush and what selects the
    // textured pipeline at draw time. Every other primitive passes null here, which is why the textured
    // 3D pipelines had become unreachable before this existed.
    if (!_nya_render3d_reserve(window, 4, 6, texture, sampler)) return;

    NYA_Render3DStream* stream = _nya_render3d_stream(batch);

    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    u32 base = stream->vertex_count;

    // Corners run bottom-left, top-left, top-right, bottom-right, and the uvs follow: v grows *downward*
    // in a texture and upward in the quad, so the first corner takes v = 1. Reversed, it flips every
    // sprite vertically.
    (void)_nya_render3d_vertex(batch, a, normal, color, (f32x2){ 0.0F, 1.0F });
    (void)_nya_render3d_vertex(batch, b, normal, color, (f32x2){ 0.0F, 0.0F });
    (void)_nya_render3d_vertex(batch, c, normal, color, (f32x2){ 1.0F, 0.0F });
    (void)_nya_render3d_vertex(batch, d, normal, color, (f32x2){ 1.0F, 1.0F });

    stream->indices[stream->index_count++] = base + 0;
    stream->indices[stream->index_count++] = base + 1;
    stream->indices[stream->index_count++] = base + 2;
    stream->indices[stream->index_count++] = base + 0;
    stream->indices[stream->index_count++] = base + 2;
    stream->indices[stream->index_count++] = base + 3;
}

void nya_render3d_quad(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Routed here rather than in the shapes above it: every generated primitive bottoms out in a quad or
    // a triangle, so deciding the stream at the two places that actually write vertices covers all of
    // them and cannot fall out of step with a shape that forgets to ask.
    _nya_render3d_route(batch, color);

    if (!_nya_render3d_reserve(window, 4, 6, nullptr, nullptr)) return;

    NYA_Render3DStream* stream = _nya_render3d_stream(batch);

    // One normal for the whole quad, from its first triangle — flat shading, deliberately: every
    // primitive here has hard edges, and a shared vertex normal would round a cube's corners into
    // something that looks like a bad sphere. A smooth-shaded mesh brings its own normals instead.
    f32x3 normal = nya_vector_normalize(nya_vector_cross(b - a, c - a));

    u32 base = stream->vertex_count;

    (void)_nya_render3d_vertex(batch, a, normal, color, f32x2_zero);
    (void)_nya_render3d_vertex(batch, b, normal, color, f32x2_zero);
    (void)_nya_render3d_vertex(batch, c, normal, color, f32x2_zero);
    (void)_nya_render3d_vertex(batch, d, normal, color, f32x2_zero);

    stream->indices[stream->index_count++] = base + 0;
    stream->indices[stream->index_count++] = base + 1;
    stream->indices[stream->index_count++] = base + 2;
    stream->indices[stream->index_count++] = base + 0;
    stream->indices[stream->index_count++] = base + 2;
    stream->indices[stream->index_count++] = base + 3;
}

void nya_render3d_line(NYA_Window* window, f32x3 from, f32x3 to, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    f32x3 along  = to - from;
    f32   length = nya_vector_length(along);

    if (length < NYA_EPSILON) return;
    if (thickness <= 0.0F) thickness = 0.02F;

    // Culled once for the whole prism rather than six times for its faces — matters most for the grid,
    // nothing but lines. The sphere is the segment's midpoint and half its length, plus thickness so a
    // line seen end-on is not clipped away.
    NYA_Render3DBatch* line_batch = &window->render_system.mesh_batch;

    if (!_nya_render3d_visible(line_batch, (from + to) * 0.5F, (length * 0.5F) + thickness)) {
        line_batch->frame_culled++;
        return;
    }

    f32x3 forward = along / length;

    // Any two vectors perpendicular to the line will do, so one is built by crossing against whichever
    // world axis the line is *least* aligned with — a fixed axis breaks when the line is parallel to it,
    // collapsing the prism exactly when the line is vertical, the most common case a grid has.
    f32x3 reference = fabsf(forward.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 right = nya_vector_normalize(nya_vector_cross(forward, reference)) * (thickness * 0.5F);
    f32x3 up    = nya_vector_normalize(nya_vector_cross(forward, right)) * (thickness * 0.5F);

    f32x3 corners[8] = {
        from - right - up, from + right - up, from - right + up, from + right + up,
        to - right - up,   to + right - up,   to - right + up,   to + right + up,
    };

    nya_render3d_quad(window, corners[0], corners[2], corners[3], corners[1], color);
    nya_render3d_quad(window, corners[5], corners[7], corners[6], corners[4], color);
    nya_render3d_quad(window, corners[4], corners[6], corners[2], corners[0], color);
    nya_render3d_quad(window, corners[1], corners[3], corners[7], corners[5], color);
    nya_render3d_quad(window, corners[0], corners[1], corners[5], corners[4], color);
    nya_render3d_quad(window, corners[6], corners[7], corners[3], corners[2], color);
}

/**
 * Draws a skinned mesh through `palette`. See core_skeleton.h for where a palette comes from. Its own
 * draw call, not part of the batch, and un-instanced: vertices move every frame and each by a different
 * matrix, so nothing can be baked into a shared buffer or shared between two instances — a second copy of
 * a character is a second pose. Props, which there are thousands of, still go through the retained
 * instanced path; characters, a handful, do not.
 * */
/*
 * The shader's bone cap and the engine's must agree, and they are written down separately: uniforms.h is
 * shared with the shaders, which cannot include engine headers, so the number lives in both places. This
 * is what stops them drifting — a mismatch would size the uniform block differently from what the shader
 * indexes, which reads as garbage geometry rather than as a build error.
 */
static_assert(NYA_SHADER_SKIN_MAX_BONES == NYA_SKELETON_MAX_BONES,
              "the shader's bone palette and NYA_SKELETON_MAX_BONES have drifted apart");

void nya_render3d_skinned_mesh(NYA_Window* window, NYA_ConstCString handle, const f32_4x4* palette, u32 bone_count, f32_4x4 model,
                               NYA_Color tint) {
    nya_assert(window != nullptr);

    if (handle == nullptr || palette == nullptr || bone_count == 0) return;

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (!batch->active) return;

    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)handle);

    // Not loaded yet is not an error; see the same case in nya_render3d_mesh.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_MESH) return;

    if (asset->as_mesh.skinned_vertices == nullptr || asset->as_mesh.skeleton == nullptr) {
        nya_log_error("'%s' is not a skinned mesh; draw it with nya_render3d_mesh.", handle);
        return;
    }

    if (asset->as_mesh.vertex_count == 0) return;

    // The batch is issued first: this draw binds a different pipeline and vertex layout, and whatever
    // the batch has queued was built for the one it is about to replace.
    nya_render3d_flush(window);

    NYA_RenderSystemWindow* render = &window->render_system;

    // The geometry is uploaded once and kept, under the same registry the retained meshes use — its
    // vertex buffer never changes at all, since the *pose* is what changes and travels as a uniform.
    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(batch, handle);

    if (registered == nullptr) {
        for (u32 i = 0; i < NYA_RENDER3D_MAX_REGISTERED_MESHES; i++) {
            if (batch->registered_meshes[i].handle != nullptr) continue;

            registered = &batch->registered_meshes[i];
            break;
        }

        if (registered == nullptr) {
            nya_log_error("No room to hold the skinned mesh '%s'; raise NYA_RENDER3D_MAX_REGISTERED_MESHES.", handle);
            return;
        }

        SDL_GPUBuffer*         buffer   = nullptr;
        SDL_GPUTransferBuffer* transfer = nullptr;

        u32 size = asset->as_mesh.vertex_count * (u32)sizeof(NYA_VertexSkinned3D);

        if (!_nya_render3d_vertex_buffer_stage(asset->as_mesh.skinned_vertices, size, handle, &buffer, &transfer)) return;

        *registered = (NYA_Render3DRegisteredMesh){
            .handle         = handle,
            .vertices       = buffer,
            .vertex_count   = asset->as_mesh.vertex_count,
            .pending_upload = transfer,
            .pending_size   = size,
        };
    }

    _nya_render3d_registered_flush_upload(window, registered);

    // The depth-only variant during a shadow pass, the scene one otherwise. A layer's on_render runs
    // again for each cascade through this same path — here the repetition is wanted: same geometry,
    // same palette, a different matrix and pipeline is exactly what casting a shadow is.
    b8 shadow = batch->shadow_pass_active;

    NYA_ConstCString pipeline_handle = shadow ? NYA_RENDER3D_PIPELINE_SKINNED_SHADOW : NYA_RENDER3D_PIPELINE_SKINNED;

    NYA_Asset* pipeline = nya_asset_get((NYA_AssetHandle)pipeline_handle);

    if (pipeline == nullptr || pipeline->status != NYA_ASSET_STATUS_LOADED) return;

    // The shadow map is created *before* anything is bound, not as a side effect of binding samplers:
    // _nya_render3d_bind_samplers creates it on demand, and creating it opens a pass, which discards the
    // pipeline and vertex buffer bindings made before it. That surfaced here as "Missing fragment
    // sampler binding" rather than as anything mentioning a pass. Only the scene pass samples the map.
    if (!shadow && batch->shadow_color == nullptr && !_nya_render3d_shadow_ensure(window)) return;

    // The pass is checked *here*, after everything that can disturb it and before anything is bound.
    // Uploading the vertex buffer and creating the shadow map both suspend and resume the render pass
    // for a copy, which replaces the pass handle — checking before and binding after reads a stale
    // null, a segfault inside SDL with no mention of a pass. From here to the draw, nothing may touch it.
    if (render->render_pass == nullptr) return;

    // The palette, flattened to three rows a bone. Built here rather than kept this way, since composing
    // a pose needs whole matrix multiplies. The copy is at most 64 bones, once per skinned draw.
    struct NYA_ShaderSkinUniform skin = { 0 };

    u32 bones = bone_count < NYA_SHADER_SKIN_MAX_BONES ? bone_count : NYA_SHADER_SKIN_MAX_BONES;

    for (u32 b = 0; b < bones; b++) {
        // The model transform folded into each bone rather than sent separately: exactly equivalent since
        // `model * bone * vertex` associates either way, and it costs the vertex stage nothing — a
        // multiply per bone on the CPU (at most 64) instead of per vertex in the shader.
        f32_4x4 placed = model * palette[b];

        for (u32 row = 0; row < 3; row++) {
            for (u32 column = 0; column < 4; column++) skin.bones[b][row][column] = placed[row][column];
        }
    }

    // Every bone the shader can index has to mean something, because a vertex weighted to a bone the
    // palette never filled would be multiplied by zeroes and collapse to the origin.
    for (u32 b = bones; b < NYA_SHADER_SKIN_MAX_BONES; b++) {
        // The model transform rather than the identity: a vertex weighted to a bone the caller never
        // filled then sits with the rest of the model instead of at the world origin.
        for (u32 row = 0; row < 3; row++) {
            for (u32 column = 0; column < 4; column++) skin.bones[b][row][column] = model[row][column];
        }
    }

    skin.tint_r = tint.r;
    skin.tint_g = tint.g;
    skin.tint_b = tint.b;
    skin.tint_a = tint.a;

    SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline->as_graphics_pipeline.pipeline);
    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = registered->vertices, .offset = 0 }, 1);

    // The light's matrix in a shadow pass, the camera's otherwise: batch->view_projection is set to the
    // cascade's matrix for the duration of the pass, letting one draw path serve both. See
    // nya_render3d_shadow_begin.
    SDL_PushGPUVertexUniformData(render->render_commands, 0, &batch->view_projection, sizeof(batch->view_projection));
    SDL_PushGPUVertexUniformData(render->render_commands, 1, &skin, sizeof(skin));

    // Shading and the shadow map are the scene pass's business only: mesh3d_shadow.frag.hlsl declares
    // no uniform and no sampler, so pushing either here binds a slot the pipeline does not declare — a
    // validation error rather than a no-op.
    if (!shadow) {
        struct NYA_ShaderMesh3DUniform shading = _nya_render3d_shading_uniform(batch);

        SDL_PushGPUFragmentUniformData(render->render_commands, 0, &shading, sizeof(shading));

        // The shadow map, bound here rather than through _nya_render3d_bind_samplers: that helper
        // returns early, reporting success having bound nothing, whenever a shadow pass is active — and
        // the draw then fails validation with "Missing fragment sampler binding". mesh3d.frag.hlsl
        // declares one sampler regardless of texturing, so exactly one must be bound here.
        SDL_GPUSampler* shadow_sampler = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR);

        if (batch->shadow_color == nullptr || shadow_sampler == nullptr) return;

        SDL_BindGPUFragmentSamplers(
            render->render_pass,
            0,
            &(SDL_GPUTextureSamplerBinding){ .texture = batch->shadow_color, .sampler = shadow_sampler },
            1
        );
    }

    SDL_DrawGPUPrimitives(render->render_pass, registered->vertex_count, 1, 0, 0);

    batch->frame_draw_calls++;
}

void nya_render3d_mesh(NYA_Window* window, NYA_ConstCString handle, f32x3 center, f32x3 scale, NYA_Quaternion rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    if (handle == nullptr) return;

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    /*
     * Resolved through the LOD chain before anything else looks at it.
     *
     * A mesh with no chain comes back unchanged, so this is inert until something registers one and no
     * call site has to know. Squared distance, so no square root, and taken from whichever camera the
     * batch is currently running — the same expression the transparent sort uses for its own eye.
     */
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

    /*
     * Registered geometry is already on the GPU, so it skips everything the asset path does here.
     *
     * Checked first for the reason nya_render3d_mesh_bounds checks first: it is the cheaper answer, and a
     * caller who registered under a handle also naming an asset means their own geometry.
     */
    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(batch, handle);

    NYA_Asset* asset = nullptr;

    if (registered != nullptr) {
        // Registered at startup, outside any frame, so its copy is still waiting. This is the first draw
        // inside one; see NYA_Render3DRegisteredMesh.pending_upload.
        _nya_render3d_registered_flush_upload(window, registered);
    } else {
        asset = nya_asset_get((NYA_AssetHandle)handle);

        /*
         * Nothing to draw yet is not an error.
         *
         * nya_asset_load queues, and the load lands at the end of the frame, so the first frame after
         * asking for a model has no model. Drawing nothing for a frame is the right answer; asserting here
         * would make every caller order its loads against its draws by hand.
         */
        if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_MESH) return;

        if (asset->as_mesh.part_count == 0 || asset->as_mesh.vertex_count == 0) return;
    }

    /*
     * Records an instance rather than emitting vertices. This used to walk the model's vertices, transform
     * each on the CPU, and append to the immediate batch every frame (twice once a shadow pass existed) —
     * for a thousand-triangle prop drawn ten times, sixty thousand vertex transforms and nearly four
     * megabytes uploaded per frame, for geometry unchanged since it was read off disk. Now the model is
     * uploaded once and a draw appends eighty bytes (model matrix + tint): ten copies is one draw call and
     * eight hundred bytes. The immediate batch is still right for geometry generated fresh each frame —
     * terrain from a heightmap, a particle, a debug line — which has no reuse for an instance buffer to
     * exploit. The split is *authored and reused* versus *generated and discarded*.
     */
    f32x3 bounds_min;
    f32x3 bounds_max;

    if (nya_render3d_mesh_bounds(window, handle, &bounds_min, &bounds_max)) {
        f32x3 extent = (bounds_max - bounds_min) * scale * 0.5F;
        f32x3 middle = (bounds_max + bounds_min) * scale * 0.5F;

        // A sphere around the scaled bounds, so the rotation needs no part in it: a box's half-diagonal
        // is the same whichever way it is turned, so this costs nothing per frame for a spinning object.
        // Looser than a rotated box — a long thin model gets a sphere its diagonal long — loose being
        // the safe direction.
        f32x3 world_center = center + nya_quaternion_rotate(rotation, middle);

        if (!_nya_render3d_visible(batch, world_center, nya_vector_length(extent))) {
            batch->frame_culled++;
            return;
        }
    }

    // Uploaded on first use rather than at load time, because this needs a copy pass and the asset system
    // has no frame to hang one on. Kept afterwards for the life of the asset. Registered geometry was
    // uploaded when it was registered and has nothing to do here.
    if (asset != nullptr && asset->as_mesh.gpu_vertices == nullptr && !_nya_render3d_mesh_upload(window, asset)) return;

    if (batch->instance_count >= NYA_RENDER3D_MAX_INSTANCES) {
        // Counted and dropped rather than grown. See NYA_RENDER3D_MAX_INSTANCES.
        batch->frame_dropped_draws++;
        return;
    }

    // Same rule the primitives go by: the tint's alpha decides which pass this copy belongs in.
    NYA_Render3DMeshGroup* group = _nya_render3d_mesh_group(batch, handle, color.a < 1.0F);

    if (group == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    /*
     * Appended to the *end* of the instance array, valid only because a new group is appended at the end
     * too. A group's instances must be contiguous — the draw call names a first instance and a count —
     * so adding to a non-last group would shift everything after it. _nya_render3d_mesh_group therefore
     * only ever returns the last group or a new one; see its definition.
     */
    batch->instances[batch->instance_count] = (NYA_Render3DInstance){
        .model = nya_matrix_transform(center, nya_quaternion_to_matrix3(rotation), scale),
        .tint  = color,
    };

    // The group remembers its furthest copy, ordering transparent groups against each other: groups draw
    // back to front, so the one reaching deepest goes first. Instances within a group are sorted too,
    // since groups that interleave in depth cannot be ordered as a whole.
    f32x3 eye    = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 offset = center - eye;

    f32 depth = nya_vector_dot(offset, offset);

    if (group->instance_count == 0 || depth > group->depth) group->depth = depth;

    batch->instance_count++;
    group->instance_count++;
}

b8 nya_render3d_mesh_register(NYA_Window* window, NYA_ConstCString handle, const NYA_Vertex3D* vertices, u32 vertex_count) {
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

    // Anything queued goes out before the previous buffer is released: this frame may have already
    // queued draws of the *old* geometry, and releasing its buffer with those pending is a use after
    // free — SDL defers release past submitted work, but not past work still sitting in the CPU batch.
    nya_render3d_flush(window);

    NYA_Render3DRegisteredMesh* slot = _nya_render3d_registered(batch, handle);

    if (slot == nullptr) {
        for (u32 i = 0; i < NYA_RENDER3D_MAX_REGISTERED_MESHES; i++) {
            if (batch->registered_meshes[i].handle == nullptr) {
                slot = &batch->registered_meshes[i];
                break;
            }
        }
    }

    if (slot == nullptr) {
        nya_log_error("No room to register the mesh '%s'; raise NYA_RENDER3D_MAX_REGISTERED_MESHES.", handle);
        return false;
    }

    SDL_GPUBuffer*         buffer   = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;

    // The previous buffers are kept until the new ones exist, so a failed re-registration leaves the mesh
    // drawing its old geometry rather than nothing at all.
    if (!_nya_render3d_vertex_buffer_stage(vertices, (u32)(vertex_count * sizeof(NYA_Vertex3D)), handle, &buffer, &transfer)) return false;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    if (slot->vertices != nullptr) SDL_ReleaseGPUBuffer(gpu_device, slot->vertices);

    // A pending copy that never happened, from a registration replaced before it was ever drawn.
    if (slot->pending_upload != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, slot->pending_upload);

    f32x3 min = vertices[0].position;
    f32x3 max = min;

    for (u32 i = 1; i < vertex_count; i++) {
        f32x3 position = vertices[i].position;

        min = (f32x3){ nya_min(min.x, position.x), nya_min(min.y, position.y), nya_min(min.z, position.z) };
        max = (f32x3){ nya_max(max.x, position.x), nya_max(max.y, position.y), nya_max(max.z, position.z) };
    }

    *slot = (NYA_Render3DRegisteredMesh){
        .handle         = handle,
        .vertices       = buffer,
        .vertex_count   = vertex_count,
        .pending_upload = transfer,
        .pending_size   = (u32)(vertex_count * sizeof(NYA_Vertex3D)),
        .bounds_min     = min,
        .bounds_max     = max,
    };

    // Attempted immediately, harmlessly skipped when there is no frame: a caller registering inside a
    // frame gets the copy now, one registering at startup gets it on the first draw instead.
    _nya_render3d_registered_flush_upload(window, slot);

    nya_log_debug("Registered '%s': %u vertices, %u KiB, kept until it is released or replaced.", handle, vertex_count,
              (u32)(vertex_count * sizeof(NYA_Vertex3D)) / 1024);

    return true;
}

void nya_render3d_mesh_release(NYA_Window* window, NYA_ConstCString handle) {
    nya_assert(window != nullptr);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    NYA_Render3DRegisteredMesh* slot = _nya_render3d_registered(batch, handle);
    if (slot == nullptr) return;

    // Same reason the registration flushes: this frame may have queued draws of it already.
    nya_render3d_flush(window);

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    if (slot->vertices != nullptr) SDL_ReleaseGPUBuffer(gpu_device, slot->vertices);
    if (slot->pending_upload != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, slot->pending_upload);

    *slot = (NYA_Render3DRegisteredMesh){ 0 };
}

b8 nya_render3d_mesh_bounds(NYA_Window* window, NYA_ConstCString handle, OUT f32x3* out_min, OUT f32x3* out_max) {
    nya_assert(window != nullptr);
    nya_assert(out_min != nullptr && out_max != nullptr);

    if (handle == nullptr) return false;

    // Registered geometry first: cheaper (two copies rather than a walk) and more specific — a caller
    // registering under a handle that also names an asset gets their own geometry.
    NYA_Render3DRegisteredMesh* registered = _nya_render3d_registered(&window->render_system.mesh_batch, handle);

    if (registered != nullptr) {
        *out_min = registered->bounds_min;
        *out_max = registered->bounds_max;

        return true;
    }

    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)handle);

    // The same three conditions nya_render3d_mesh treats as "nothing to draw yet", and for the same
    // reason: a queued load lands at the end of the frame, so the first frame after asking has no mesh.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_MESH) return false;

    if (asset->as_mesh.vertex_count == 0) return false;

    // Walked once and remembered on the asset. This used to walk every call, reasoning bounds are asked
    // for when fitting to a model rather than per frame — frustum culling made that false, since a model
    // drawn ten times costs twenty walks a frame across both passes. The cache lives on the asset and is
    // zeroed by the load, so a hot reload recomputes it rather than keeping the previous model's.
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

    // Thin relative to the cell, so the grid reads as lines rather than as a lattice of bars, and
    // scales with the cell so a grid of metres and a grid of centimetres look the same.
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

    // `camera_valid`, not `active` — see NYA_Render3DBatch.camera_valid. A click arrives during on_event
    // and a camera is set during on_render, one phase later, so asking whether a scene is currently open
    // always answers no from the place that actually needs a ray. The camera to un-project through is the
    // one the player was looking through, which is last frame's.
    if (!batch->camera_valid) return (NYA_Render3DRay){ .direction = { 0.0F, 0.0F, -1.0F } };

    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    if (target_width == 0 || target_height == 0) return (NYA_Render3DRay){ .direction = { 0.0F, 0.0F, -1.0F } };

    // Rebuilt from the camera basis rather than by inverting the view-projection: both are correct, but
    // this is cheaper and much better conditioned. A perspective matrix is nearly singular for a small
    // near plane, and its inverse loses precision exactly where a picker is used — near the camera, where
    // the ray origin is.
    f32x3 eye     = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = batch->camera_is_ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = batch->camera_is_ortho ? batch->camera_orthographic.up : batch->camera.up;

    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    // Normalized device coordinates: -1..+1 across the target, with y flipped because screen y grows
    // downward and clip space y grows up.
    f32 ndc_x = ((screen.x / (f32)target_width) * 2.0F) - 1.0F;
    f32 ndc_y = 1.0F - ((screen.y / (f32)target_height) * 2.0F);

    f32 aspect = (f32)target_width / (f32)target_height;

    if (batch->camera_is_ortho) {
        // Parallel rays: the direction is constant and the *origin* is what the pixel chooses. That
        // is the whole difference from perspective, and getting it backwards gives a picker that
        // works in the centre of the screen and nowhere else.
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
    };
}

void nya_render3d_flush(NYA_Window* window) {
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    if (batch->opaque.index_count == 0 && batch->transparent.index_count == 0 && batch->instance_count == 0) return;

    // No pass to draw into: the window is occluded or minimised. Dropped rather than held for a
    // frame that may never come, which would draw stale geometry once it returned.
    if (render->render_pass == nullptr) {
        batch->opaque      = (NYA_Render3DStream){ .vertices = batch->opaque.vertices, .indices = batch->opaque.indices };
        batch->transparent = (NYA_Render3DStream){ .vertices = batch->transparent.vertices, .indices = batch->transparent.indices };

        batch->instance_count   = 0;
        batch->mesh_group_count = 0;
        return;
    }

    // One uniform block for both paths, built once. The light, the material and the shadow state are
    // batch state — anything that changes them flushes first — so every draw about to be issued, immediate
    // or instanced, is lit identically. Building it twice would invite the two to drift.
    struct NYA_ShaderMesh3DUniform uniform = _nya_render3d_shading_uniform(batch);

    _nya_render3d_flush_immediate(window, &uniform);
    _nya_render3d_flush_instanced(window, &uniform);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_ShaderMesh3DUniform _nya_render3d_shading_uniform(const NYA_Render3DBatch* batch) {
    struct NYA_ShaderMesh3DUniform uniform = {
        // Negated on the way in, so the shader receives a vector pointing from the surface toward
        // the light. The public field is the direction light travels, which is what a caller thinks
        // in; doing the flip here means exactly one place can get it wrong.
        .light_direction_x = -batch->light.direction.x,
        .light_direction_y = -batch->light.direction.y,
        .light_direction_z = -batch->light.direction.z,
        .ambient           = batch->light.ambient,

        .light_color_r = batch->light.color.r,
        .light_color_g = batch->light.color.g,
        .light_color_b = batch->light.color.b,
        .intensity     = batch->light.intensity,

        .camera_x = batch->camera_is_ortho ? batch->camera_orthographic.position.x : batch->camera.position.x,
        .camera_y = batch->camera_is_ortho ? batch->camera_orthographic.position.y : batch->camera.position.y,
        .camera_z = batch->camera_is_ortho ? batch->camera_orthographic.position.z : batch->camera.position.z,
        .metallic = batch->material.metallic,

        .roughness   = batch->material.roughness,
        .reflectance = batch->material.reflectance,
        .emission    = batch->material.emission,

        .point_light_count = (f32)batch->point_light_count,

        .edge = batch->material.edge,

        /*
         * Zero strength unless a pass actually ran this frame.
         *
         * The shader returns fully lit for zero and skips the lookup entirely, so a frame that never
         * called nya_render3d_shadow_begin samples nothing rather than sampling a stale or empty map.
         */
        .shadow_strength = batch->shadow_valid ? batch->shadow.strength : 0.0F,
        // One *cascade's* texel, not the atlas's — easy to get backwards. The shader offsets its kernel
        // in cascade-local UV (zero to one across one cascade) and only divides by the atlas split
        // afterwards, when it folds the result into a quadrant, so the texel handed over here is a texel
        // of the cascade. Passing the atlas's would shrink the kernel to a quarter of its intended width
        // and turn a soft contact shadow into a hard one.
        .shadow_texel = 1.0F / (f32)NYA_RENDER3D_SHADOW_MAP_SIZE,
        .shadow_bias  = batch->shadow.bias,

        .cascade_count = (f32)batch->shadow_cascade_count,
    };

    for (u32 i = 0; i < NYA_RENDER3D_SHADOW_CASCADES; i++) {
        uniform.light_view_projection[i] = batch->shadow_view_projection[i];
        uniform.cascade_extent[i]        = batch->shadow_cascade_extent[i];
    }

    // Copied field by field rather than memcpy'd over the structs: NYA_Render3DPointLight is a public
    // struct with its own layout (f32x3 is sixteen bytes wide, not twelve) and the uniform block is a wire
    // format that must match the HLSL exactly — named fields is the only version that stays correct when
    // either side gains a field.
    for (u32 i = 0; i < batch->point_light_count; i++) {
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

    // Normalized here rather than trusting the setter, because the setter is not the only way in:
    // nya_render3d_begin installs the default directly.
    f32x3 direction = nya_vector_normalize((f32x3){ uniform.light_direction_x, uniform.light_direction_y, uniform.light_direction_z });

    uniform.light_direction_x = direction.x;
    uniform.light_direction_y = direction.y;
    uniform.light_direction_z = direction.z;

    return uniform;
}

b8 _nya_render3d_bind_samplers(NYA_Window* window, SDL_GPUTexture* texture, SDL_GPUSampler* sampler) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    /*
     * The bindings, in the order each shader declares them. The shadow pass binds nothing: its fragment
     * shader samples no texture at all. The scene shaders each take the shadow map, and the textured one
     * takes a base colour before it — so the untextured pipeline declares one sampler and the textured
     * pipeline two, and the shadow map's register differs between them, which is why mesh3d_shadow takes
     * its texture as a parameter instead of reading a global. A shadow map is bound even when no pass has
     * run, because a declared sampler must have *something* bound; `shadow_strength` being zero is what
     * stops the shader reading it.
     */
    if (batch->shadow_pass_active) return true;

    // The map has to exist even for a scene that never casts a shadow: a declared sampler must have a
    // *valid* texture bound, and binding null is the same fault as binding too few. Created here if
    // nothing has created it yet; its contents do not matter, since `shadow_strength` is zero in that case
    // and the shader returns before sampling. One texture for a scene that does not use it, against a
    // driver-level crash for the same scene.
    if (batch->shadow_color == nullptr && !_nya_render3d_shadow_ensure(window)) return false;

    SDL_GPUSampler* shadow_sampler = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR);

    if (texture != nullptr) {
        SDL_BindGPUFragmentSamplers(
            render->render_pass,
            0,
            (SDL_GPUTextureSamplerBinding[]){
                { .texture = texture, .sampler = sampler },
                { .texture = batch->shadow_color, .sampler = shadow_sampler },
            },
            2
        );
    } else {
        SDL_BindGPUFragmentSamplers(
            render->render_pass,
            0,
            &(SDL_GPUTextureSamplerBinding){ .texture = batch->shadow_color, .sampler = shadow_sampler },
            1
        );
    }

    return true;
}

void _nya_render3d_flush_immediate(NYA_Window* window, const struct NYA_ShaderMesh3DUniform* uniform) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    NYA_Render3DStream* opaque      = &batch->opaque;
    NYA_Render3DStream* transparent = &batch->transparent;

    if (opaque->index_count == 0 && transparent->index_count == 0) return;

    // Sorted before the upload, since the sort rewrites the indices about to be copied. The eye is the
    // camera's during a scene pass and the light's during a shadow pass, which is what
    // _nya_render3d_begin_with installed — the light's is the right one there because a shadow map is a
    // depth buffer, and depth buffers do not care about blend order. Wasted work in that pass, not wrong;
    // skipping it is a nicety not worth the branch.
    f32x3 eye = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;

    // Additive geometry is not sorted, because addition does not care: the order two additive surfaces
    // draw in cannot change the result, the whole reason they need no depth write either. Sorting them
    // would be a qsort over every particle in the system, twice a frame per pass, for an ordering nothing
    // can observe.
    if (batch->blend != NYA_RENDER3D_BLEND_ADDITIVE) _nya_render3d_sort_transparent(batch, eye);

    /*
     * Two pipelines, chosen the same way, differing only in whether they write depth: a translucent
     * surface must *test* depth — it is still behind the wall in front of it — and must not *write* it, or
     * the nearer of two translucent panes would stop the further one being drawn at all. The overlay case
     * takes both streams, not just the transparent one: geometry is routed to opaque or transparent by its
     * alpha, and a gizmo is usually drawn solid, so overriding only the transparent handle would leave the
     * ordinary case — an opaque handle inside the object it moves — still hidden by it.
     */
    NYA_ConstCString opaque_handle = batch->shadow_pass_active                      ? NYA_RENDER3D_PIPELINE_SHADOW
                                   : batch->depth == NYA_RENDER3D_DEPTH_OVERLAY     ? NYA_RENDER3D_PIPELINE_OVERLAY
                                   : batch->texture != nullptr                      ? NYA_RENDER3D_PIPELINE_MESH_TEXTURED
                                                                                    : NYA_RENDER3D_PIPELINE_MESH;

    /*
     * The shadow pass draws translucent geometry through its own pipeline, unchanged — a decision, not a
     * fallthrough: a translucent surface casting a solid shadow is wrong, casting none is also wrong, and
     * solid is the one this renderer can express (the map holds a depth, not a transmittance), which reads
     * better than a window casting no shadow at all.
     *
     * Glass is asked for by the material and granted only if there is something to capture: a caller sets
     * `refraction` and gets refraction where the frame is composed through a render texture, and ordinary
     * blending where it is not (see _nya_render3d_refraction_capture) — nothing fails, the surface just
     * stops bending what is behind it. Never during a shadow pass: a depth map has no colour to refract.
     */
    b8 wants_glass = !batch->shadow_pass_active && batch->material.refraction > 0.0F && transparent->index_count > 0;

    /*
     * The *intent* is resolved here; the capture itself happens after the opaque draw — it has to, since
     * the capture is a copy of the colour target and what glass needs to see is the opaque scene,
     * including whatever this very flush is about to draw into it. Both pipelines are looked up now so the
     * decision below chooses between two things that already exist, rather than an asset lookup mid-pass.
     *
     * Additive geometry casts nothing: everything translucent used to go through the shadow pipeline during
     * a shadow pass, right for glass but plainly wrong for what additive blending is *for* — fire, sparks
     * and glow are light being emitted, and light does not cast a shadow. A flame throwing a solid black
     * silhouette on the ground is the artefact this avoids.
     */
    b8 additive_shadow = batch->shadow_pass_active && batch->blend == NYA_RENDER3D_BLEND_ADDITIVE;

    NYA_ConstCString transparent_handle;

    if (batch->shadow_pass_active) {
        transparent_handle = NYA_RENDER3D_PIPELINE_SHADOW;
    } else if (batch->depth == NYA_RENDER3D_DEPTH_OVERLAY) {
        // Ahead of the blend cases, and above them: an overlay is an overlay whatever it blends
        // like, and a gizmo that additively blended its way back behind the scene would be a gizmo
        // that is sometimes grabbable.
        transparent_handle = NYA_RENDER3D_PIPELINE_OVERLAY;
    } else if (batch->blend == NYA_RENDER3D_BLEND_ADDITIVE) {
        transparent_handle = batch->texture != nullptr ? NYA_RENDER3D_PIPELINE_ADDITIVE_TEXTURED : NYA_RENDER3D_PIPELINE_ADDITIVE;
    } else {
        transparent_handle = batch->texture != nullptr ? NYA_RENDER3D_PIPELINE_TRANSPARENT_TEXTURED : NYA_RENDER3D_PIPELINE_TRANSPARENT;
    }

    NYA_Asset* opaque_pipeline      = nya_asset_get((NYA_AssetHandle)opaque_handle);
    NYA_Asset* transparent_pipeline = nya_asset_get((NYA_AssetHandle)transparent_handle);

    NYA_Asset* glass_pipeline = wants_glass ? nya_asset_get((NYA_AssetHandle)NYA_RENDER3D_PIPELINE_GLASS) : nullptr;

    if (glass_pipeline != nullptr && glass_pipeline->status != NYA_ASSET_STATUS_LOADED) glass_pipeline = nullptr;

    b8 opaque_ready      = opaque_pipeline != nullptr && opaque_pipeline->status == NYA_ASSET_STATUS_LOADED;
    b8 transparent_ready = transparent_pipeline != nullptr && transparent_pipeline->status == NYA_ASSET_STATUS_LOADED;

    if (!opaque_ready && !transparent_ready && glass_pipeline == nullptr) {
        // Still loading, which is normal for the first frames of a run. Dropped, so the geometry does
        // not accumulate until the pipeline arrives and then all appear at once.
        opaque->vertex_count      = 0;
        opaque->index_count       = 0;
        transparent->vertex_count = 0;
        transparent->index_count  = 0;
        return;
    }

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // Both streams into one buffer, opaque first: sharing it keeps the pair costing no more VRAM than the
    // single stream did, and the transparent run's indices are stored relative to its own first vertex, so
    // the draw's vertex offset rebases them without anything having to be rewritten.
    u32 opaque_vertices      = opaque->vertex_count;
    u32 transparent_vertices = transparent->vertex_count;
    u32 opaque_indices       = opaque->index_count;
    u32 transparent_indices  = transparent->index_count;

    u32 vertex_upload_size = (u32)((opaque_vertices + transparent_vertices) * sizeof(NYA_Vertex3D));
    u32 index_upload_size  = (u32)((opaque_indices + transparent_indices) * sizeof(u32));

    NYA_Vertex3D* mapped = SDL_MapGPUTransferBuffer(gpu_device, batch->transfer_buffer, true);
    nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed for the 3D batch: %s", SDL_GetError());
    nya_memcpy(mapped, opaque->vertices, opaque_vertices * sizeof(NYA_Vertex3D));
    nya_memcpy(mapped + opaque_vertices, transparent->vertices, transparent_vertices * sizeof(NYA_Vertex3D));
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->transfer_buffer);

    u32* mapped_indices = SDL_MapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer, true);
    nya_assert(mapped_indices != nullptr, "SDL_MapGPUTransferBuffer() failed for the 3D batch's indices: %s", SDL_GetError());
    nya_memcpy(mapped_indices, opaque->indices, opaque_indices * sizeof(u32));
    nya_memcpy(mapped_indices + opaque_indices, transparent->indices, transparent_indices * sizeof(u32));
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer);

    // A copy pass cannot run inside a render pass, so the render pass is closed and reopened around
    // it — reusing render2d's suspend and resume, which already know how to reattach the colour
    // target, the depth target and the scissor. Two batches, one pass, one set of rules about it.
    _nya_render2d_pass_suspend(window);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);

    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->vertex_buffer, .offset = 0, .size = vertex_upload_size },
        true
    );

    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->index_transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->index_buffer, .offset = 0, .size = index_upload_size },
        true
    );

    SDL_EndGPUCopyPass(copy_pass);

    _nya_render2d_pass_resume(window);

    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer, .offset = 0 }, 1);
    SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer, .offset = 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    if (!_nya_render3d_bind_samplers(window, batch->texture, batch->sampler)) {
        opaque->vertex_count      = 0;
        opaque->index_count       = 0;
        transparent->vertex_count = 0;
        transparent->index_count  = 0;
        return;
    }

    SDL_PushGPUVertexUniformData(render->render_commands, 0, &batch->view_projection, sizeof(batch->view_projection));
    SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));

    // Opaque first, then transparent, never the other way round: the opaque pass fills the depth buffer,
    // which lets the transparent pass test against solid geometry it is behind. Reversed, every translucent
    // surface would blend over the background and then have the opaque geometry paint over the result.
    if (opaque_indices > 0 && opaque_ready) {
        SDL_BindGPUGraphicsPipeline(render->render_pass, opaque_pipeline->as_graphics_pipeline.pipeline);
        SDL_DrawGPUIndexedPrimitives(render->render_pass, opaque_indices, 1, 0, 0, 0);

        batch->frame_draw_calls++;
    }

    if (transparent_indices > 0 && !additive_shadow) {
        /*
         * The capture, taken here: after the opaque half of this flush and before the transparent half.
         * Everything already in the colour target — earlier flushes, and the opaque draw just above — is
         * what the glass will see; anything drawn after it will not be, the honest limit of a single
         * capture and why glass behind glass shows an unrefracted backdrop. It can decline, and then the
         * surface falls back to the pipeline it would have used anyway.
         *
         * The shadow map is ensured here rather than relied on: this branch binds `shadow_color` directly,
         * and it only ever worked because _nya_render3d_bind_samplers ran earlier in the same flush and
         * created the map as a side effect — an ordering dependency nothing states and nothing enforces.
         * Reorder the flush and the binding becomes null, a driver fault rather than a visible mistake.
         */
        b8 glass = glass_pipeline != nullptr && _nya_render3d_shadow_ensure(window) && _nya_render3d_refraction_capture(window);

        if (glass) {
            // Everything rebound, because the capture suspended and resumed the pass: a resume begins a
            // *new* render pass with no pipeline, no vertex buffers, no samplers and no pushed uniforms.
            // Rebinding only the samplers, which the first version did, leaves the draw reading a vertex
            // buffer nothing bound — an empty screen on a forgiving driver, a fault on a strict one.
            SDL_BindGPUGraphicsPipeline(render->render_pass, glass_pipeline->as_graphics_pipeline.pipeline);

            SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer, .offset = 0 }, 1);
            SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer, .offset = 0 },
                                   SDL_GPU_INDEXELEMENTSIZE_32BIT);

            // The capture at t0 and the shadow map at t1, which is the order this shader declares them —
            // and not the order the other two do. See mesh3d_glass.frag.hlsl.
            SDL_GPUSampler* linear = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR);

            SDL_BindGPUFragmentSamplers(
                render->render_pass,
                0,
                (SDL_GPUTextureSamplerBinding[]){
                    { .texture = batch->refraction_capture, .sampler = linear },
                    { .texture = batch->shadow_color, .sampler = linear },
                },
                2
            );

            struct NYA_ShaderGlassUniform glass_uniform = {
                .texel_x    = batch->refraction_width > 0 ? 1.0F / (f32)batch->refraction_width : 0.0F,
                .texel_y    = batch->refraction_height > 0 ? 1.0F / (f32)batch->refraction_height : 0.0F,
                .refraction = batch->material.refraction,
                .blur       = batch->material.blur,
            };

            SDL_PushGPUVertexUniformData(render->render_commands, 0, &batch->view_projection, sizeof(batch->view_projection));
            SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));
            SDL_PushGPUFragmentUniformData(render->render_commands, 1, &glass_uniform, sizeof(glass_uniform));
        } else if (transparent_ready) {
            SDL_BindGPUGraphicsPipeline(render->render_pass, transparent_pipeline->as_graphics_pipeline.pipeline);
        }

        if (glass || transparent_ready) {
            // The vertex offset is what rebases this stream's indices onto the second half of the buffer.
            SDL_DrawGPUIndexedPrimitives(render->render_pass, transparent_indices, 1, opaque_indices, (s32)opaque_vertices, 0);

            batch->frame_draw_calls++;
        }
    }

    batch->frame_vertices += opaque_vertices + transparent_vertices;
    batch->frame_indices  += opaque_indices + transparent_indices;

    opaque->vertex_count      = 0;
    opaque->index_count       = 0;
    transparent->vertex_count = 0;
    transparent->index_count  = 0;
}

void _nya_render3d_sort_transparent(NYA_Render3DBatch* batch, f32x3 eye) {
    NYA_Render3DStream* stream = &batch->transparent;

    u32 triangles = stream->index_count / 3;

    // One triangle is already in order with itself, and zero has nothing to order.
    if (triangles < 2) return;

    for (u32 i = 0; i < triangles; i++) {
        u32 first = i * 3;

        f32x3 a = stream->vertices[stream->indices[first + 0]].position;
        f32x3 b = stream->vertices[stream->indices[first + 1]].position;
        f32x3 c = stream->vertices[stream->indices[first + 2]].position;

        f32x3 offset = ((a + b + c) / 3.0F) - eye;

        batch->sort_keys[i] = (NYA_Render3DSortKey){ .depth = nya_vector_dot(offset, offset), .first = first };
    }

    nya_render3d_sort_keys(batch->sort_keys, batch->sort_keys_scratch, triangles);

    // Walked backwards, because the radix pass sorts ascending and this wants furthest first so nearer
    // surfaces blend over the ones behind them. Reversing here is free; sorting descending is not.
    for (u32 i = 0; i < triangles; i++) {
        u32 source = batch->sort_keys[triangles - 1 - i].first;

        batch->sorted_indices[(i * 3) + 0] = stream->indices[source + 0];
        batch->sorted_indices[(i * 3) + 1] = stream->indices[source + 1];
        batch->sorted_indices[(i * 3) + 2] = stream->indices[source + 2];
    }

    // Copied back rather than swapping the pointers, so the stream's array is always the one the emitters
    // wrote into and there is no question of which of two buffers is current.
    nya_memcpy(stream->indices, batch->sorted_indices, (u64)triangles * 3 * sizeof(u32));
}

void _nya_render3d_flush_instanced(NYA_Window* window, const struct NYA_ShaderMesh3DUniform* uniform) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;

    if (batch->instance_count == 0) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    /*
     * Instances ordered within each transparent group, before the upload that freezes them. Ordering the
     * groups against each other is not enough alone: a group is many copies of one mesh at many depths,
     * and drawing twenty translucent copies in spawn order blends them in an order unrelated to where they
     * are. Only the transparent groups — sorting an opaque run would be work the depth buffer already does,
     * and would break up runs the driver may otherwise coalesce.
     */
    f32x3 eye = batch->camera_is_ortho ? batch->camera_orthographic.position : batch->camera.position;

    for (u32 g = 0; g < batch->mesh_group_count; g++) {
        const NYA_Render3DMeshGroup* group = &batch->mesh_groups[g];

        if (!group->transparent || group->instance_count < 2) continue;

        for (u32 i = 0; i < group->instance_count; i++) {
            const NYA_Render3DInstance* instance = &batch->instances[group->first_instance + i];

            /*
             * The translation is the matrix's fourth *column*, which `m[row][3]` reads.
             *
             * The indices are (row, column) whatever the storage order is — the column-major layout only
             * decides where those elements sit in memory, not how they are addressed.
             */
            f32x3 offset = (f32x3){ instance->model[0][3], instance->model[1][3], instance->model[2][3] } - eye;

            batch->sort_keys[i] = (NYA_Render3DSortKey){ .depth = nya_vector_dot(offset, offset), .first = i };
        }

        nya_render3d_sort_keys(batch->sort_keys, batch->sort_keys_scratch, group->instance_count);

        // Backwards, for the reason the triangle sort is: ascending radix, furthest-first draw order.
        for (u32 i = 0; i < group->instance_count; i++) {
            u32 source                 = batch->sort_keys[group->instance_count - 1 - i].first;
            batch->sorted_instances[i] = batch->instances[group->first_instance + source];
        }

        nya_memcpy(&batch->instances[group->first_instance], batch->sorted_instances,
                   (u64)group->instance_count * sizeof(NYA_Render3DInstance));
    }

    u32 instance_upload_size = (u32)(batch->instance_count * sizeof(NYA_Render3DInstance));

    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, batch->instance_transfer_buffer, true);
    nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed for the 3D instance stream: %s", SDL_GetError());
    nya_memcpy(mapped, batch->instances, instance_upload_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->instance_transfer_buffer);

    // One copy pass for the whole frame's instances, regardless of how many meshes they cover. Same
    // suspend and resume the immediate path uses; see the note there.
    _nya_render2d_pass_suspend(window);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);

    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->instance_transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->instance_buffer, .offset = 0, .size = instance_upload_size },
        true
    );

    SDL_EndGPUCopyPass(copy_pass);

    _nya_render2d_pass_resume(window);

    /*
     * A draw call per mesh *part*, not per mesh and not per copy: a part is the unit that has a texture,
     * and a texture is a binding, so a model with two materials is two draws however many copies there
     * are — the cost scales with distinct *materials* on screen rather than with how much geometry is.
     *
     * The groups run in two ordered passes: opaque in call order, then transparent back to front. Call
     * order is right for opaque — the depth buffer sorts it, and leaving the order alone keeps consecutive
     * draws of one mesh together — while transparent has to be ordered explicitly, since blending is not
     * commutative. Built as a list of indices rather than by sorting the groups themselves: a group names
     * a run of the instance array by offset, and moving the structs would leave those offsets describing
     * somebody else's instances.
     */
    u32 order[NYA_RENDER3D_MAX_MESH_GROUPS];
    u32 order_count = 0;

    for (u32 g = 0; g < batch->mesh_group_count; g++) {
        if (!batch->mesh_groups[g].transparent) order[order_count++] = g;
    }

    u32 opaque_groups = order_count;

    for (u32 g = 0; g < batch->mesh_group_count; g++) {
        if (batch->mesh_groups[g].transparent) order[order_count++] = g;
    }

    // Insertion sort over the transparent tail, furthest first: at most NYA_RENDER3D_MAX_MESH_GROUPS
    // entries and usually a handful, where a qsort's setup costs more than the sort, and an almost-sorted
    // list — which a scene redrawn each frame produces — is where insertion sort is at its best.
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

        if (group->instance_count == 0) continue;

        // A group names either registered geometry or an asset, resolved into one shape here: registered
        // geometry is a single untextured part covering all its vertices, so the loop below runs once for
        // it. Expressing it that way rather than branching keeps the draw, outline pass and bindings in
        // one place for both sources.
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

            // Unloaded between being queued and being drawn — a hot reload landing mid-frame. Skipped
            // rather than drawn from a buffer that has been released underneath us.
            if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_mesh.gpu_vertices == nullptr) continue;

            mesh_vertices = asset->as_mesh.gpu_vertices;
            parts         = asset->as_mesh.parts;
            part_count    = asset->as_mesh.part_count;
        }

        for (u32 p = 0; p < part_count; p++) {
            const NYA_MeshPart* part = &parts[p];

            if (part->vertex_count == 0) continue;

            SDL_GPUTexture* texture = (asset != nullptr && part->texture >= 0) ? asset->as_mesh.textures[part->texture] : nullptr;
            SDL_GPUSampler* sampler = texture != nullptr ? _nya_render_sampler_for(asset->as_mesh.filter) : nullptr;

            // Four ways this can go, and the shadow pass short-circuits all of them: a translucent mesh
            // casts a solid shadow, for the reason the immediate path's note gives — the map holds a depth
            // rather than a transmittance, and no shadow at all reads worse than a solid one.
            NYA_ConstCString pipeline_handle;

            if (batch->shadow_pass_active) {
                pipeline_handle = NYA_RENDER3D_PIPELINE_INSTANCED_SHADOW;
            } else if (group->transparent) {
                pipeline_handle = texture != nullptr ? NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT_TEXTURED
                                                     : NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT;
            } else {
                pipeline_handle = texture != nullptr ? NYA_RENDER3D_PIPELINE_INSTANCED_TEXTURED : NYA_RENDER3D_PIPELINE_INSTANCED;
            }

            NYA_Asset* pipeline_asset = nya_asset_get((NYA_AssetHandle)pipeline_handle);
            if (pipeline_asset == nullptr || pipeline_asset->status != NYA_ASSET_STATUS_LOADED) continue;

            /*
             * The ink first, then the model over it — order is the whole mechanism. The expanded shell is
             * drawn with front faces culled, so what survives is its *back*, sitting behind the model
             * everywhere except where it pokes out past the silhouette; drawing the model second covers
             * the rest, leaving a band. Skipped entirely during a shadow pass: letting outline ink into the
             * depth map would make every object cast a shadow slightly larger than itself.
             *
             * No ink on a translucent mesh either: the outline pipeline writes depth deliberately, so a
             * nearer object's line occludes a further one, and a depth-writing pass mid-transparent-half
             * would stop everything behind the outlined mesh being drawn — a line around something you can
             * see through is also not what an outline means.
             */
            if (batch->outline_thickness > 0.0F && !batch->shadow_pass_active && !group->transparent) {
                NYA_Asset* outline_asset = nya_asset_get((NYA_AssetHandle)NYA_RENDER3D_PIPELINE_OUTLINE);

                if (outline_asset != nullptr && outline_asset->status == NYA_ASSET_STATUS_LOADED) {
                    struct NYA_ShaderOutlineUniform outline = {
                        .color_r   = batch->outline_color.r,
                        .color_g   = batch->outline_color.g,
                        .color_b   = batch->outline_color.b,
                        .color_a   = batch->outline_color.a,
                        .thickness = batch->outline_thickness,
                    };

                    SDL_BindGPUGraphicsPipeline(render->render_pass, outline_asset->as_graphics_pipeline.pipeline);

                    SDL_BindGPUVertexBuffers(
                        render->render_pass,
                        0,
                        (SDL_GPUBufferBinding[]){
                            { .buffer = mesh_vertices, .offset = 0 },
                            { .buffer = batch->instance_buffer, .offset = (u32)(group->first_instance * sizeof(NYA_Render3DInstance)) },
                        },
                        2
                    );

                    // Slot 1, beside the view-projection the shared vertex path already pushes at slot 0.
                    SDL_PushGPUVertexUniformData(render->render_commands, 0, &batch->view_projection, sizeof(batch->view_projection));
                    SDL_PushGPUVertexUniformData(render->render_commands, 1, &outline, sizeof(outline));

                    SDL_DrawGPUPrimitives(render->render_pass, part->vertex_count, group->instance_count, part->first_vertex, 0);

                    batch->frame_draw_calls++;
                }
            }

            SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline_asset->as_graphics_pipeline.pipeline);

            // Two streams: the mesh at slot 0, the frame's instances at slot 1. The instance binding
            // starts at this group's first instance rather than zero, so the draw's own first-instance
            // parameter stays zero — keeps the arithmetic in one place and avoids the corner where a
            // backend applies first_instance to the buffer index but not to SV_InstanceID.
            SDL_BindGPUVertexBuffers(
                render->render_pass,
                0,
                (SDL_GPUBufferBinding[]){
                    { .buffer = mesh_vertices, .offset = 0 },
                    { .buffer = batch->instance_buffer, .offset = (u32)(group->first_instance * sizeof(NYA_Render3DInstance)) },
                },
                2
            );

            if (!_nya_render3d_bind_samplers(window, texture, sampler)) break;

            // Pushed per part rather than once before the loop: the outline pass above binds its own
            // pipeline and pushes its own vertex uniforms, so anything set before the loop is no longer
            // what this draw would read. A handful of bytes per part against a bug class where turning the
            // outline on silently moved every mesh in the scene.
            SDL_PushGPUVertexUniformData(render->render_commands, 0, &batch->view_projection, sizeof(batch->view_projection));
            SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));

            // Not indexed: the mesh loader de-indexes (one vertex per triangle corner), so an index
            // buffer here would be the identity permutation — four bytes per vertex to say vertex i is
            // vertex i. The part's run of vertices is a plain first-vertex and count instead.
            SDL_DrawGPUPrimitives(render->render_pass, part->vertex_count, group->instance_count, part->first_vertex, 0);

            batch->frame_draw_calls++;
            batch->frame_vertices += part->vertex_count * group->instance_count;
        }

        batch->frame_instances += group->instance_count;
    }

    batch->instance_count   = 0;
    batch->mesh_group_count = 0;
}

NYA_Render3DMeshGroup* _nya_render3d_mesh_group(NYA_Render3DBatch* batch, NYA_ConstCString handle, b8 transparent) {
    /*
     * Only the last group, or a new one: a group names a *contiguous* run of the instance array, since
     * that is what a draw call takes, and instances are only ever appended at the end. Matching against an
     * earlier group would hand back one whose run has already been closed off by a later group, so every
     * mesh after it would draw the wrong transforms. The cost is that draws alternating between two meshes
     * produce a group each time rather than two — a draw call per alternation, fixed by drawing a scene
     * grouped by model, which every renderer rewards and this one now measures.
     */
    if (batch->mesh_group_count > 0) {
        NYA_Render3DMeshGroup* last = &batch->mesh_groups[batch->mesh_group_count - 1];

        // The transparency has to match as well as the mesh: a group is one draw call and the two draw
        // through different pipelines, so a translucent copy landing in an opaque group would be drawn
        // with depth writing on — invisible until two of them overlapped.
        if (last->handle == handle && last->transparent == transparent) return last;
    }

    if (batch->mesh_group_count >= NYA_RENDER3D_MAX_MESH_GROUPS) return nullptr;

    NYA_Render3DMeshGroup* group = &batch->mesh_groups[batch->mesh_group_count];

    *group = (NYA_Render3DMeshGroup){
        .handle         = handle,
        .first_instance = batch->instance_count,
        .instance_count = 0,
        .transparent    = transparent,

        // Overwritten by the first instance. Zero would sort a group in front of everything, which for a
        // group that never receives one is harmless and for one that does is immediately corrected.
        .depth = 0.0F,
    };

    batch->mesh_group_count++;

    return group;
}

b8 _nya_render3d_refraction_capture(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;
    NYA_Render2DBatch*      target = &render->draw_batch;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;
    if (gpu_device == nullptr) return false;

    /*
     * Only from a render texture — the whole constraint on refraction. The capture is a copy of the
     * *resolved* colour target, and the target only resolves when a pass ends: _nya_render2d_pass_resume
     * resolves on every reopen for a render texture (read back the moment it is ended) and defers it to
     * the last pass of the frame for the window, so mid-frame there is a resolved image to copy for the
     * first and not the second. Refusing here rather than copying a stale one: a frame-old backdrop inside
     * a moving pane of glass is far more obviously wrong than glass that is merely not refracting.
     */
    if (!target->target_is_texture || target->target_texture == nullptr) return false;

    u32 width  = target->target_width;
    u32 height = target->target_height;

    if (width == 0 || height == 0) return false;

    // Recreated rather than resized, because a GPU texture has no resize. A window being dragged between
    // monitors is the case that exercises this, and it happens rarely enough that the churn is fine.
    if (batch->refraction_capture != nullptr && (batch->refraction_width != width || batch->refraction_height != height)) {
        SDL_ReleaseGPUTexture(gpu_device, batch->refraction_capture);

        batch->refraction_capture = nullptr;
    }

    if (batch->refraction_capture == nullptr) {
        batch->refraction_capture = SDL_CreateGPUTexture(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type   = SDL_GPU_TEXTURETYPE_2D,
                .format = SDL_GetGPUSwapchainTextureFormat(gpu_device, window->sdl_window),

                // A colour target as well as a sampler, because SDL_BlitGPUTexture writes into it through
                // a render pass of its own rather than through a copy pass.
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,

                // Single sampled: it is a copy of an already resolved image, and there is nothing left to
                // antialias. Multisampling it would also make it unsamplable without a second resolve.
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

    /*
     * The pass is suspended around the blit, which is what makes the copy see a resolved image.
     *
     * Ending the pass is the resolve. Blitting inside it would be reading the target while it is bound for
     * writing, which is undefined at best and a driver fault at worst.
     */
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

    _nya_render2d_pass_resume(window);

    return true;
}

NYA_Render3DRegisteredMesh* _nya_render3d_registered(NYA_Render3DBatch* batch, NYA_ConstCString handle) {
    if (handle == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_RENDER3D_MAX_REGISTERED_MESHES; i++) {
        if (batch->registered_meshes[i].handle == handle) return &batch->registered_meshes[i];
    }

    return nullptr;
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

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = size });

    if (buffer == nullptr) {
        nya_log_error("Could not create a vertex buffer for '%s': %s", label, SDL_GetError());
        return false;
    }

    /*
     * A transfer buffer per registration, kept until the copy happens rather than released here.
     *
     * None of this needs a command buffer — creating buffers and mapping staging memory are device
     * operations — which is what lets registration work from anywhere. Only the copy is a frame's work.
     */
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(gpu_device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size });

    if (transfer == nullptr) {
        nya_log_error("Could not create a transfer buffer for '%s': %s", label, SDL_GetError());
        SDL_ReleaseGPUBuffer(gpu_device, buffer);
        return false;
    }

    void* staging = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);

    if (staging == nullptr) {
        nya_log_error("Could not map the transfer buffer for '%s': %s", label, SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);
        SDL_ReleaseGPUBuffer(gpu_device, buffer);
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

    // No frame yet. Left pending, so the next draw inside one performs it — which is the frame after a
    // registration that happened during startup.
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

    _nya_render2d_pass_resume(window);

    // "As soon as it is safe to do so", which already defers past the copy just queued.
    SDL_ReleaseGPUTransferBuffer(nya_app_get()->render_system.gpu_device, mesh->pending_upload);

    mesh->pending_upload = nullptr;
    mesh->pending_size   = 0;
}

b8 _nya_render3d_mesh_upload(NYA_Window* window, NYA_Asset* asset) {
    NYA_RenderSystemWindow* render     = &window->render_system;
    SDL_GPUDevice*          gpu_device = nya_app_get()->render_system.gpu_device;

    u32 vertex_count = asset->as_mesh.vertex_count;
    u32 size         = (u32)(vertex_count * sizeof(NYA_Vertex3D));

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = size });

    if (buffer == nullptr) {
        nya_log_error("Could not create a vertex buffer for the mesh '%s': %s", asset->handle, SDL_GetError());
        return false;
    }

    // A transfer buffer for this upload alone, released as soon as it lands: the batch's shared transfer
    // buffer is sized for the immediate path and a model can be larger than it. This happens once per mesh
    // for the life of the process, the right trade against sizing a permanent buffer for the largest model
    // anyone might ever load.
    SDL_GPUTransferBuffer* transfer =
        SDL_CreateGPUTransferBuffer(gpu_device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size });

    if (transfer == nullptr) {
        nya_log_error("Could not create a transfer buffer for the mesh '%s': %s", asset->handle, SDL_GetError());
        SDL_ReleaseGPUBuffer(gpu_device, buffer);
        return false;
    }

    NYA_Vertex3D* staging = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);

    if (staging == nullptr) {
        nya_log_error("Could not map the transfer buffer for the mesh '%s': %s", asset->handle, SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);
        SDL_ReleaseGPUBuffer(gpu_device, buffer);
        return false;
    }

    // Written part by part, so each part's material colour can be baked into its vertices. The
    // alternative, a per-part uniform, would push a colour that never changes every frame; baking it costs
    // nothing at runtime and leaves the instance tint a plain multiply on top — what the immediate path
    // did, except it redid the multiply on every vertex of every frame.
    for (u32 p = 0; p < asset->as_mesh.part_count; p++) {
        const NYA_MeshPart* part = &asset->as_mesh.parts[p];

        for (u32 i = 0; i < part->vertex_count; i++) {
            u32 source = part->first_vertex + i;

            staging[source] = (NYA_Vertex3D){
                .position = asset->as_mesh.positions[source],
                .color    = part->base_color,
                .normals  = asset->as_mesh.normals[source],

                // Zero when the model has no UV set, which samples one texel and is the right answer for
                // a model that was never textured.
                .uv = asset->as_mesh.uvs != nullptr ? asset->as_mesh.uvs[source] : f32x2_zero,
            };
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

    _nya_render2d_pass_resume(window);

    // "As soon as it is safe to do so", which already defers past the copy above — the same contract
    // nya_render_texture_destroy relies on. No wait is needed and adding one would stall the frame.
    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);

    asset->as_mesh.gpu_vertices     = buffer;
    asset->as_mesh.gpu_vertex_count = vertex_count;

    nya_log_debug("Uploaded '%s' to the GPU: %u vertices, %u KiB, kept for the life of the asset.", asset->handle, vertex_count, size / 1024);

    return true;
}

void _nya_render3d_frustum_build(NYA_Render3DBatch* batch) {
    /*
     * The standard plane extraction: each clip plane is a sum or difference of two rows of the matrix. A
     * point is inside the left plane when its clip x is at least -w, i.e. when `(row3 + row0) · v >= 0`,
     * and the same reasoning gives the other five; `m[i][j]` reads rows out of the column-major matrix,
     * indexed (row, column) whatever the storage order is. Near is row 2 alone rather than `row3 + row2`,
     * because this projection maps depth onto [0, 1] rather than [-1, 1] (see nya_matrix_perspective) — the
     * [-1, 1] form here would put the near plane at the camera's own position and cull nothing.
     *
     * The planes are normalized here, once, and that is the whole reason this is a separate function. An
     * unnormalized plane still gives the right *sign*, so a visibility test can use one — but comparing
     * against a radius then needs the plane's length, and taking it in the test means a square root per
     * plane per primitive. Six per triangle, on a surface made of thousands, dwarfs the writes the test
     * exists to avoid; it measured as a net loss on a scene where nothing was off screen. Six square roots
     * per *pass* instead — the test below is then six multiply-adds and a compare.
     */
    f32_4x4 m = batch->view_projection;

    f32x4 row0 = { m[0][0], m[0][1], m[0][2], m[0][3] };
    f32x4 row1 = { m[1][0], m[1][1], m[1][2], m[1][3] };
    f32x4 row2 = { m[2][0], m[2][1], m[2][2], m[2][3] };
    f32x4 row3 = { m[3][0], m[3][1], m[3][2], m[3][3] };

    batch->frustum[0] = row3 + row0;  // left
    batch->frustum[1] = row3 - row0;  // right
    batch->frustum[2] = row3 + row1;  // bottom
    batch->frustum[3] = row3 - row1;  // top
    batch->frustum[4] = row2;         // near
    batch->frustum[5] = row3 - row2;  // far

    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = batch->frustum[i];

        f32 length = nya_vector_length((f32x3){ plane[0], plane[1], plane[2] });

        // A degenerate plane cannot come out of a well formed projection. Left alone if one does, which
        // makes the test answer "visible" — the safe direction.
        if (length < NYA_EPSILON) continue;

        batch->frustum[i] = plane / length;
    }
}

b8 _nya_render3d_visible(const NYA_Render3DBatch* batch, f32x3 center, f32 radius) {
    for (u32 i = 0; i < 6; i++) {
        f32x4 plane = batch->frustum[i];

        f32x3 normal = { plane[0], plane[1], plane[2] };

        // Normalized by _nya_render3d_frustum_build, so this is a true signed distance and the radius
        // needs no scaling. That is the whole of the cost model: six multiply-adds and a compare.
        f32 distance = nya_vector_dot(normal, center) + plane[3];

        if (distance < -radius) return false;
    }

    /*
     * Only for what survived the frustum, and only when a buffer was set.
     *
     * The order is the cost model. The frustum test is six multiply-adds; an occlusion query projects
     * eight corners and scans a rectangle, and it can only ever reject a subset of what the frustum
     * already keeps. Running the expensive one on primitives the cheap one throws away would be
     * paying for an answer nobody reads.
     */
    if (batch->occlusion == nullptr) return true;

    if (!nya_occlusion_test(batch->occlusion, center, radius)) return true;

    ((NYA_Render3DBatch*)batch)->frame_occluded++;
    return false;
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
    // Zero is not a value anyone means for any of these; it is a field nobody filled in. Sixty
    // degrees is the usual game default, and y is up in a 3D scene — see physics3d.h, which
    // makes the same choice for the same reason.
    if (camera.up.x == 0.0F && camera.up.y == 0.0F && camera.up.z == 0.0F) camera.up = (f32x3){ 0.0F, 1.0F, 0.0F };
    if (camera.fov_y <= 0.0F) camera.fov_y = (f32)M_PI / 3.0F;
    if (camera.near_plane <= 0.0F) camera.near_plane = 0.1F;
    if (camera.far_plane <= camera.near_plane) camera.far_plane = 1000.0F;

    return camera;
}

NYA_Camera3DOrthographic _nya_render3d_camera_orthographic_defaults(NYA_Camera3DOrthographic camera) {
    if (camera.up.x == 0.0F && camera.up.y == 0.0F && camera.up.z == 0.0F) camera.up = (f32x3){ 0.0F, 1.0F, 0.0F };
    if (camera.height <= 0.0F) camera.height = 10.0F;

    // Unlike the perspective one, an orthographic near plane may sit behind the camera — there is no
    // projective divide, so nothing degenerates at zero and negative is meaningful.
    if (camera.far_plane <= camera.near_plane) {
        camera.near_plane = -100.0F;
        camera.far_plane  = 100.0F;
    }

    return camera;
}

void _nya_render3d_begin_with(NYA_Window* window, f32_4x4 view_projection, f32x3 eye) {
    nya_unused(eye);

    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    // Everything render2d has queued belongs behind the scene, so it is drawn before the scene is.
    // This is the whole of the ordering contract described in render3d.h.
    nya_render2d_flush(window);

    // A second begin without an end is a caller changing camera mid-frame, which is legal and costs
    // a draw call — but what is already queued belongs to the old one.
    if (batch->active) nya_render3d_flush(window);

    batch->view_projection = view_projection;
    batch->active          = true;

    // Once per pass, because the planes are a property of the camera. Every draw between here and the
    // matching end tests against these rather than re-deriving them. See _nya_render3d_frustum_build.
    _nya_render3d_frustum_build(batch);

    /*
     * The light and material are reset on every begin rather than persisted across frames.
     *
     * Persisting them would make a frame's appearance depend on what the previous frame happened to
     * set last, which is the class of bug that only shows up after a layer is reordered. A scene that
     * wants its own light sets it immediately after begin, which is one line and is visible.
     */
    // Cleared for the same reason the light is: a buffer filled for a camera that has since moved
    // would keep hiding geometry that is now in plain sight. See nya_render3d_occlusion.
    batch->occlusion = nullptr;

    batch->light = _nya_render3d_default_light();

    batch->material = (NYA_Render3DMaterial){ .metallic = 0.0F, .roughness = 1.0F, .reflectance = 0.5F };

    // Off, for the same reason the light and material are reset: a frame's appearance must not depend on
    // what the previous one happened to leave behind.
    batch->outline_thickness = 0.0F;
    batch->outline_color     = (NYA_Color){ 0.0F, 0.0F, 0.0F, 1.0F };
    batch->blend             = NYA_RENDER3D_BLEND_ALPHA;
}

b8 _nya_render3d_reserve(NYA_Window* window, u32 vertices, u32 indices, SDL_GPUTexture* texture, SDL_GPUSampler* sampler) {
    NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    if (vertices > NYA_RENDER3D_MAX_VERTICES || indices > NYA_RENDER3D_MAX_INDICES) {
        // Larger than an empty batch can hold, so flushing would not help. Dropped and counted rather
        // than partially drawn, which would be geometry with a bite out of it.
        batch->frame_dropped_draws++;
        return false;
    }

    /*
     * A different texture ends the run, exactly as a different material does.
     *
     * Before the capacity check rather than after: this flush empties the batch, so whatever the
     * capacity question would have answered a moment ago is no longer the question.
     */
    if (batch->texture != texture || batch->sampler != sampler) {
        nya_render3d_flush(window);

        batch->texture = texture;
        batch->sampler = sampler;
    }

    /*
     * The two streams share one capacity, because they share one GPU buffer.
     *
     * Checking them separately would let a frame stage more geometry in total than the buffer they are
     * uploaded into can hold, which is a silent overrun rather than a flush.
     */
    u32 staged_vertices = batch->opaque.vertex_count + batch->transparent.vertex_count;
    u32 staged_indices  = batch->opaque.index_count + batch->transparent.index_count;

    if (staged_vertices + vertices > NYA_RENDER3D_MAX_VERTICES || staged_indices + indices > NYA_RENDER3D_MAX_INDICES) {
        nya_render3d_flush(window);

        staged_vertices = batch->opaque.vertex_count + batch->transparent.vertex_count;
        staged_indices  = batch->opaque.index_count + batch->transparent.index_count;
    }

    // The flush may have found no pipeline and dropped everything, which leaves the counters at zero
    // and this check passing — which is correct, since there is then room.
    return staged_vertices + vertices <= NYA_RENDER3D_MAX_VERTICES && staged_indices + indices <= NYA_RENDER3D_MAX_INDICES;
}

NYA_Render3DStream* _nya_render3d_stream(NYA_Render3DBatch* batch) {
    return batch->transparent_active ? &batch->transparent : &batch->opaque;
}

void _nya_render3d_route(NYA_Render3DBatch* batch, NYA_Color color) {
    /*
     * The colour's alpha decides the stream, and that is the only signal there is.
     *
     * A caller that wants a fully opaque surface written into the sorted stream — a glass pane with an
     * alpha of one, say — does not have a way to ask, and does not need one: an opaque surface sorts
     * correctly through the depth buffer whichever stream it is in.
     *
     * Compared against one rather than against zero. Alpha zero is invisible and could be skipped
     * entirely, but skipping it would make a fade-out stop drawing one frame early, which reads as a pop.
     */
    batch->transparent_active = color.a < 1.0F;
}

u32 _nya_render3d_vertex(NYA_Render3DBatch* batch, f32x3 position, f32x3 normal, NYA_Color color, f32x2 uv) {
    NYA_Render3DStream* stream = _nya_render3d_stream(batch);

    u32 index = stream->vertex_count;

    stream->vertices[index] = (NYA_Vertex3D){
        .position = position,
        .color    = color,
        .normals  = normal,
        .uv       = uv,
    };

    stream->vertex_count++;

    return index;
}

