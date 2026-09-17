#include "assets/shader/uniforms.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* The cartoon passes' pipelines, queued the first time their feature runs. */
#define _NYA_POST_PIPELINE_OCCLUSION       "nya_post_occlusion_pipeline"
#define _NYA_POST_PIPELINE_OCCLUSION_APPLY "nya_post_occlusion_apply_pipeline"
#define _NYA_POST_PIPELINE_INK             "nya_post_ink_pipeline"
#define _NYA_POST_PIPELINE_ANTIALIAS       "nya_post_antialias_pipeline"
#define _NYA_POST_PIPELINE_BLUR            "nya_post_depth_of_field_blur_pipeline"
#define _NYA_POST_PIPELINE_FOCUS           "nya_post_depth_of_field_pipeline"
#define _NYA_POST_PIPELINE_SPEED_LINES     "nya_post_speed_lines_pipeline"
#define _NYA_POST_PIPELINE_DEBUG           "nya_post_debug_pipeline"

/** Near black with a little blue, what NYA_PostInk.color falls back to. */
#define _NYA_POST_INK_COLOR ((NYA_Color){ 0.08F, 0.07F, 0.10F, 1.0F })

/** What a built-in pass binds, in this order from t0. A caller's pass has none and draws through the 2D batch. */
typedef enum {
    _NYA_POST_INPUT_SOURCE  = 1 << 0,
    _NYA_POST_INPUT_NORMALS = 1 << 1,
    _NYA_POST_INPUT_HALF    = 1 << 2,
    _NYA_POST_INPUT_BLUR    = 1 << 3,
} _NYA_PostInput;

/** One pass as nya_post_end runs it. */
typedef struct {
    NYA_ConstCString pipeline;

    /** A caller's pass's second image. See NYA_PostPass.texture. */
    NYA_ConstCString texture;

    const void* uniform;
    u32         uniform_size;

    NYA_Color tint;

    /** _NYA_PostInput bits. */
    u32 inputs;

    /** Draws into this half resolution target instead of the next image in the chain. */
    NYA_RenderTexture* target;
} _NYA_PostStep;

/** The built-in passes one frame can queue: occlusion twice, ink, depth of field twice, antialiasing, the debug view. */
#define _NYA_POST_BUILT_IN_MAX 7

/** Whether any option on the window reads the scene normal buffer. */
NYA_INTERNAL b8 _nya_post_wants_normals(const NYA_RenderSystemWindow* render) {
    return render->post_ink.enabled || render->post_ambient_occlusion.enabled || render->post_debug_view != NYA_POST_DEBUG_VIEW_NONE
        || render->post_depth_of_field.focus == NYA_POST_FOCUS_DISTANCE;
}

/** Whether any option on the window draws the half resolution occlusion. Every debug view binds it. */
NYA_INTERNAL b8 _nya_post_wants_half(const NYA_RenderSystemWindow* render) {
    return render->post_ambient_occlusion.enabled || render->post_debug_view != NYA_POST_DEBUG_VIEW_NONE;
}

/** Whether `pass` can draw this frame: its pipeline, and its texture if it names one, have loaded. */
NYA_INTERNAL b8 _nya_post_pass_ready(const NYA_PostPass* pass) {
    // cast because the asset API takes a mutable handle while only reading it. See nya_render2d_texture.
    if (pass->pipeline == nullptr || nya_asset_status((NYA_CString)pass->pipeline) != NYA_ASSET_STATUS_LOADED) return false;

    return pass->texture == nullptr || nya_asset_status((NYA_CString)pass->texture) == NYA_ASSET_STATUS_LOADED;
}

/**
 * Rebuilds the targets if the window size or the options' needs changed. False when there is nothing usable to draw
 * into.
 */
NYA_INTERNAL b8 _nya_post_targets_ensure(NYA_Window* window, NYA_PostChain* chain) {
    const u32 width  = window->screen_width;
    const u32 height = window->screen_height;

    // Minimised or mid resize. The GPU will not make a target of no size, and the caller falls back
    // to drawing straight to the window.
    if (width == 0 || height == 0) return false;

    const NYA_RenderSystemWindow* render = &window->render_system;

    // the normal buffer only while a pass reads it, so turning the passes off gives its memory back.
    NYA_RenderTextureOptions scene = chain->scene;
    scene.normals                  = _nya_post_wants_normals(render) && scene.depth == NYA_RENDER_TEXTURE_DEPTH_ATTACHED;

    const NYA_RenderTextureOptions* built = &chain->targets[0].options;

    // current also means the renderer's sample count, so a changed MSAA setting rebuilds the normal buffer with it.
    if (!nya_render_texture_is_current(&chain->targets[0], width, height) || built->depth != scene.depth || built->normals != scene.normals) {
        nya_render_texture_destroy(&chain->targets[0]);
        nya_render_texture_destroy(&chain->targets[1]);

        // the scene multisampled like the window, so a 3D scene occludes itself and its edges are smoothed as they
        // would be on the swapchain.
        chain->targets[0] = nya_render_texture_create_with(window, width, height, scene);
        chain->width      = width;
        chain->height     = height;
    }

    // rounded up, so an odd size still covers its last row.
    u32 half_width  = (width + 1) / 2;
    u32 half_height = (height + 1) / 2;

    b8 half_matches = chain->half.width == half_width && chain->half.height == half_height;

    if (!_nya_post_wants_half(render) || !half_matches) nya_render_texture_destroy(&chain->half);

    // single sampled: it is only ever filled by one fullscreen pass, which a multisampled companion would not improve.
    if (_nya_post_wants_half(render) && chain->half.width == 0) {
        chain->half = nya_render_texture_create_with(
            window, half_width, half_height, (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_NONE, .single_sampled = true }
        );
    }

    // its own, since the debug view reads the occlusion after depth of field has run.
    b8 wants_blur   = render->post_depth_of_field.focus != NYA_POST_FOCUS_OFF;
    b8 blur_matches = chain->blur.width == half_width && chain->blur.height == half_height;

    if (!wants_blur || !blur_matches) nya_render_texture_destroy(&chain->blur);

    if (wants_blur && chain->blur.width == 0) {
        chain->blur = nya_render_texture_create_with(
            window, half_width, half_height, (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_NONE, .single_sampled = true }
        );
    }

    return true;
}

/**
 * Makes the between passes target when `needed` and releases it when not. Single sampled and without depth: it only
 * takes fullscreen passes, which have no edges to smooth and nothing to occlude.
 */
NYA_INTERNAL void _nya_post_intermediate_ensure(NYA_Window* window, NYA_PostChain* chain, b8 needed) {
    if (!needed) {
        nya_render_texture_destroy(&chain->targets[1]);
        return;
    }

    if (nya_render_texture_is_current(&chain->targets[1], chain->width, chain->height)) return;

    nya_render_texture_destroy(&chain->targets[1]);
    chain->targets[1] = nya_render_texture_create_with(
        window,
        chain->width,
        chain->height,
        (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_NONE, .single_sampled = true }
    );
}

/** Draws `source` over the whole window through `pass`, into whatever target is currently bound. */
NYA_INTERNAL void _nya_post_draw_pass(NYA_Window* window, const NYA_RenderTexture* source, const _NYA_PostStep* pass) {
    // Zeroed alpha means the caller left `tint` unset, which should be opaque white rather than invisible.
    NYA_Color tint = pass->tint;
    if (tint.a == 0) tint = NYA_COLOR_WHITE;

    nya_render2d_shader_begin(window, pass->pipeline);

    if (pass->texture != nullptr) (void)nya_render2d_shader_set_texture(window, pass->texture);

    if (pass->uniform != nullptr && pass->uniform_size > 0) {
        nya_render2d_shader_set_uniform(window, pass->uniform, pass->uniform_size);
    }

    nya_render2d_render_texture(window, source, 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height, tint);

    nya_render2d_shader_end(window);
}

/** Draws a built-in pass as one fullscreen triangle, binding the inputs it names. */
NYA_INTERNAL void _nya_post_draw_step(NYA_Window* window, const NYA_PostChain* chain, u32 source, const _NYA_PostStep* step) {
    SDL_GPUTexture* textures[4];
    u32             count = 0;

    // a scene without normals gets the image in their place. only tilt shift asks then, and it never samples them.
    SDL_GPUTexture* normals = chain->targets[0].normal_texture != nullptr ? chain->targets[0].normal_texture : chain->targets[source].texture;

    if (step->inputs & _NYA_POST_INPUT_SOURCE) textures[count++] = chain->targets[source].texture;
    if (step->inputs & _NYA_POST_INPUT_NORMALS) textures[count++] = normals;
    if (step->inputs & _NYA_POST_INPUT_HALF) textures[count++] = chain->half.texture;
    if (step->inputs & _NYA_POST_INPUT_BLUR) textures[count++] = chain->blur.texture;

    nya_render2d_fullscreen(window, step->pipeline, textures, count, step->uniform, step->uniform_size);
}

/**
 * Whether a cartoon pipeline is loaded, queueing it the first time it is asked for. Paired with the procedural
 * vertex stage every window already loads.
 * */
NYA_INTERNAL b8 _nya_post_pipeline_ready(NYA_Window* window, NYA_ConstCString pipeline, NYA_AssetHandle fragment, u32 samplers, b8 half) {
    NYA_AssetStatus status = nya_asset_status((NYA_CString)pipeline);

    if (status != NYA_ASSET_STATUS_UNLOADED) return status == NYA_ASSET_STATUS_LOADED;

    NYA_Error shader = nya_asset_load((NYA_AssetLoadParameters){
        .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
        .handle    = fragment,
        .as_shader = { .num_samplers = samplers, .num_uniform_buffers = 1 },
    });

    NYA_Error loaded = shader.ok ? nya_asset_load((NYA_AssetLoadParameters){
        .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
        .handle               = (NYA_CString)pipeline,
        .as_graphics_pipeline = {
            .window                 = window,
            .vertex_shader_handle   = NYA_ASSET_SHADER_PROCEDURAL_VERT,
            .fragment_shader_handle = fragment,
            .vertex_layout          = NYA_VERTEX_LAYOUT_2D,

            // over whatever the window already holds, as every chain pass is. the occlusion replaces its target.
            .blend          = half ? NYA_BLEND_NONE : NYA_BLEND_ALPHA,
            .single_sampled = half,
        },
    }) : shader;

    // logged once: the status is no longer unloaded, so the pass is skipped quietly from here on.
    if (!loaded.ok) nya_log_error("Could not queue the post pipeline '%s': %s", pipeline, (NYA_ConstCString)loaded.message);

    return false;
}

/** The camera and fog the scene was drawn with, for rebuilding positions from the normal buffer. */
NYA_INTERNAL struct NYA_ShaderSceneView _nya_post_scene_view(const NYA_Window* window, const NYA_PostChain* chain) {
    const NYA_Render3DBatch* batch = &window->render_system.mesh_batch;

    b8 ortho = batch->camera_is_ortho;

    f32x3 eye     = ortho ? batch->camera_orthographic.position : batch->camera.position;
    f32x3 target  = ortho ? batch->camera_orthographic.target : batch->camera.target;
    f32x3 up_hint = ortho ? batch->camera_orthographic.up : batch->camera.up;

    // the basis nya_render3d_screen_ray and the sky rebuild, so all three agree.
    f32x3 forward = nya_vector_normalize(target - eye);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, up_hint));
    f32x3 up      = nya_vector_cross(right, forward);

    return (struct NYA_ShaderSceneView){
        .right_x = right.x,
        .right_y = right.y,
        .right_z = right.z,
        .tangent = ortho ? 0.0F : tanf(batch->camera.fov_y * 0.5F),

        .up_x   = up.x,
        .up_y   = up.y,
        .up_z   = up.z,
        .aspect = (f32)chain->width / (f32)chain->height,

        .forward_x   = forward.x,
        .forward_y   = forward.y,
        .forward_z   = forward.z,
        .half_height = ortho ? batch->camera_orthographic.height * 0.5F : 0.0F,

        .eye_x       = eye.x,
        .eye_y       = eye.y,
        .eye_z       = eye.z,
        .fog_density = batch->fog.density,

        .fog_height_falloff = batch->fog.height_falloff,
        .fog_height_base    = batch->fog.height_base,
        .texel_x            = 1.0F / (f32)chain->width,
        .texel_y            = 1.0F / (f32)chain->height,
    };
}

/** The depth of field block with every zero field replaced by its default. */
NYA_INTERNAL struct NYA_ShaderDepthOfFieldUniform _nya_post_depth_of_field_uniform(NYA_PostDepthOfField options, const NYA_PostChain* chain) {
    b8 distance = options.focus == NYA_POST_FOCUS_DISTANCE;

    f32 band  = options.band > 0.0F ? options.band : NYA_POST_DEPTH_OF_FIELD_BAND;
    f32 range = options.focus_range > 0.0F ? options.focus_range : NYA_POST_DEPTH_OF_FIELD_RANGE;

    return (struct NYA_ShaderDepthOfFieldUniform){
        .texel_x = 1.0F / (f32)chain->width,
        .texel_y = 1.0F / (f32)chain->height,
        .radius  = options.radius > 0.0F ? options.radius : NYA_POST_DEPTH_OF_FIELD_RADIUS,
        .focus   = (f32)options.focus,

        .band_center = 0.5F + options.band_offset,
        .band        = band,
        .falloff     = options.falloff > 0.0F ? options.falloff : (distance ? range * 2.0F : band * 3.0F),
        .layers      = (f32)(options.layers > 0 ? options.layers : NYA_POST_DEPTH_OF_FIELD_LAYERS),

        .focus_distance = options.focus_distance > 0.0F ? options.focus_distance : NYA_POST_DEPTH_OF_FIELD_DISTANCE,
        .focus_range    = range,
    };
}

/** The ink block with every zero field replaced by its default. */
NYA_INTERNAL struct NYA_ShaderInkUniform _nya_post_ink_uniform(NYA_PostInk ink, struct NYA_ShaderSceneView view) {
    NYA_Color color = ink.color.a > 0.0F ? ink.color : _NYA_POST_INK_COLOR;

    f32 crease     = ink.crease > 0.0F ? ink.crease : NYA_POST_INK_CREASE;
    f32 fade_start = ink.fade_start > 0.0F ? ink.fade_start : NYA_POST_INK_FADE_START;
    f32 fade_end   = ink.fade_end > 0.0F ? ink.fade_end : NYA_POST_INK_FADE_END;

    return (struct NYA_ShaderInkUniform){
        .view          = view,
        .color_r       = color.r,
        .color_g       = color.g,
        .color_b       = color.b,
        .color_a       = color.a,
        .width         = ink.width > 0.0F ? ink.width : NYA_POST_INK_WIDTH,
        .crease_cosine = cosf(crease * (f32)M_PI / 180.0F),
        .fade_start    = fade_start,
        .fade_end      = nya_max(fade_end, fade_start + 1.0F),
    };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_post_begin(NYA_Window* window, NYA_PostChain* chain) {
    nya_assert(window != nullptr);
    nya_assert(chain != nullptr);

    chain->capturing = false;

    if (!_nya_post_targets_ensure(window, chain)) return false;

    chain->scene_index = 0;
    chain->capturing   = true;

    // Transparent, not a colour: whatever was drawn to the window before this is underneath and the
    // chain composites over it. See the header.
    nya_render_texture_begin(window, &chain->targets[chain->scene_index], NYA_COLOR_TRANSPARENT);

    return true;
}

void nya_post_end(NYA_Window* window, NYA_PostChain* chain, const NYA_PostPass* passes, u32 pass_count) {
    nya_assert(window != nullptr);
    nya_assert(chain != nullptr);
    nya_assert(passes != nullptr || pass_count == 0);

    if (!chain->capturing) return;

    const NYA_RenderSystemWindow* render = &window->render_system;

    // read before the capture ends: a 3D draw attached the normal buffer, and without one there is nothing to ink.
    b8 scene = chain->targets[0].options.normals && render->draw_batch.target_normal_written;

    nya_render_texture_end(window);
    chain->capturing = false;

    /*
     * The built-in passes, queued only for what is on. Their uniforms live here until the passes below have run.
     */
    _NYA_PostStep before[_NYA_POST_BUILT_IN_MAX];
    // speed lines, then the debug view.
    _NYA_PostStep after[2];

    u32 before_count = 0;
    u32 after_count  = 0;

    struct NYA_ShaderSceneView                view      = scene ? _nya_post_scene_view(window, chain) : (struct NYA_ShaderSceneView){ 0 };
    struct NYA_ShaderAmbientOcclusionUniform  occlusion = { 0 };
    struct NYA_ShaderInkUniform               ink       = { 0 };
    struct NYA_ShaderAntialiasUniform         antialias = { 0 };
    struct NYA_ShaderDepthOfFieldUniform      focus     = { 0 };
    struct NYA_ShaderSpeedLinesUniform        lines     = { 0 };
    struct NYA_ShaderSceneDebugUniform        debug     = { 0 };

    const NYA_PostAmbientOcclusion* occlusion_options = &render->post_ambient_occlusion;

    if (scene && _nya_post_wants_half(render)) {
        occlusion = (struct NYA_ShaderAmbientOcclusionUniform){
            .view       = view,
            .radius     = occlusion_options->radius > 0.0F ? occlusion_options->radius : NYA_POST_OCCLUSION_RADIUS,
            .strength   = occlusion_options->strength > 0.0F ? occlusion_options->strength : NYA_POST_OCCLUSION_STRENGTH,
            .band       = occlusion_options->band > 0.0F ? occlusion_options->band : NYA_POST_OCCLUSION_BAND,
            .min_radius = occlusion_options->min_radius > 0.0F ? occlusion_options->min_radius : NYA_POST_OCCLUSION_MIN_RADIUS,
        };

        if (_nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_OCCLUSION, NYA_ASSET_SHADER_EFFECT_OCCLUSION_FRAG, 1, true)) {
            before[before_count++] = (_NYA_PostStep){
                .pipeline     = _NYA_POST_PIPELINE_OCCLUSION,
                .uniform      = &occlusion,
                .uniform_size = sizeof(occlusion),
                .inputs       = _NYA_POST_INPUT_NORMALS,
                .target       = &chain->half,
            };
        }
    }

    // half written this frame, or its content is a previous frame's.
    b8 half_ready = before_count > 0;

    if (scene && half_ready && occlusion_options->enabled
        && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_OCCLUSION_APPLY, NYA_ASSET_SHADER_EFFECT_OCCLUSION_APPLY_FRAG, 3, false)) {
        before[before_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_OCCLUSION_APPLY,
            .uniform      = &occlusion,
            .uniform_size = sizeof(occlusion),
            .inputs       = _NYA_POST_INPUT_SOURCE | _NYA_POST_INPUT_NORMALS | _NYA_POST_INPUT_HALF,
        };
    }

    if (scene) ink = _nya_post_ink_uniform(render->post_ink, view);

    if (scene && render->post_ink.enabled && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_INK, NYA_ASSET_SHADER_EFFECT_INK_FRAG, 2, false)) {
        before[before_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_INK,
            .uniform      = &ink,
            .uniform_size = sizeof(ink),
            .inputs       = _NYA_POST_INPUT_SOURCE | _NYA_POST_INPUT_NORMALS,
        };
    }

    // distance focus needs the normal buffer this frame; tilt shift works on any image.
    const NYA_PostDepthOfField* focus_options = &render->post_depth_of_field;

    b8 focus_on = focus_options->focus == NYA_POST_FOCUS_TILT_SHIFT || (scene && focus_options->focus == NYA_POST_FOCUS_DISTANCE);

    if (focus_on && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_BLUR, NYA_ASSET_SHADER_EFFECT_DEPTH_OF_FIELD_BLUR_FRAG, 2, true)
        && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_FOCUS, NYA_ASSET_SHADER_EFFECT_DEPTH_OF_FIELD_FRAG, 3, false)) {
        focus = _nya_post_depth_of_field_uniform(*focus_options, chain);

        before[before_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_BLUR,
            .uniform      = &focus,
            .uniform_size = sizeof(focus),
            .inputs       = _NYA_POST_INPUT_SOURCE | _NYA_POST_INPUT_NORMALS,
            .target       = &chain->blur,
        };

        before[before_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_FOCUS,
            .uniform      = &focus,
            .uniform_size = sizeof(focus),
            .inputs       = _NYA_POST_INPUT_SOURCE | _NYA_POST_INPUT_NORMALS | _NYA_POST_INPUT_BLUR,
        };
    }

    const NYA_PostAntialias* antialias_options = &render->post_antialias;

    if (antialias_options->enabled
        && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_ANTIALIAS, NYA_ASSET_SHADER_EFFECT_ANTIALIAS_FRAG, 1, false)) {
        antialias = (struct NYA_ShaderAntialiasUniform){
            .texel_x   = 1.0F / (f32)chain->width,
            .texel_y   = 1.0F / (f32)chain->height,
            .subpixel  = antialias_options->subpixel > 0.0F ? antialias_options->subpixel : NYA_POST_ANTIALIAS_SUBPIXEL,
            .threshold = antialias_options->threshold > 0.0F ? antialias_options->threshold : NYA_POST_ANTIALIAS_THRESHOLD,
        };

        before[before_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_ANTIALIAS,
            .uniform      = &antialias,
            .uniform_size = sizeof(antialias),
            .inputs       = _NYA_POST_INPUT_SOURCE,
        };
    }

    const NYA_PostSpeedLines* lines_options = &render->post_speed_lines;

    if (lines_options->amount > 0.0F
        && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_SPEED_LINES, NYA_ASSET_SHADER_EFFECT_SPEED_LINES_FRAG, 1, false)) {
        NYA_Color color = lines_options->color.a > 0.0F ? lines_options->color : NYA_COLOR_WHITE;

        lines = (struct NYA_ShaderSpeedLinesUniform){
            .center_x = 0.5F + lines_options->center_x,
            .center_y = 0.5F + lines_options->center_y,
            .aspect   = (f32)chain->width / (f32)chain->height,
            // stepped, so each drawing holds for a few frames, and wrapped so the float keeps its precision in long runs.
            .frame = fmodf(floorf(nya_app_get()->frame_stats.uptime_s * NYA_POST_SPEED_LINES_RATE), 997.0F),

            .amount       = lines_options->amount,
            .density      = nya_max(roundf(lines_options->density > 0.0F ? lines_options->density : NYA_POST_SPEED_LINES_DENSITY), 1.0F),
            .clear_radius = lines_options->clear_radius > 0.0F ? lines_options->clear_radius : NYA_POST_SPEED_LINES_CLEAR_RADIUS,
            .pixel        = 1.0F / (f32)chain->height,

            .color_r = color.r,
            .color_g = color.g,
            .color_b = color.b,
            .color_a = color.a,
        };

        after[after_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_SPEED_LINES,
            .uniform      = &lines,
            .uniform_size = sizeof(lines),
            .inputs       = _NYA_POST_INPUT_SOURCE,
        };
    }

    if (scene && half_ready && render->post_debug_view != NYA_POST_DEBUG_VIEW_NONE
        && _nya_post_pipeline_ready(window, _NYA_POST_PIPELINE_DEBUG, NYA_ASSET_SHADER_EFFECT_SCENE_DEBUG_FRAG, 3, false)) {
        const NYA_Render3DBatch* batch = &render->mesh_batch;

        debug = (struct NYA_ShaderSceneDebugUniform){ .ink = ink, .view = (f32)render->post_debug_view };

        // cascades whose pass ran at some point. nya_render3d_end resets the frame's count before this reads it.
        for (u32 i = 0; i < NYA_RENDER3D_SHADOW_CASCADES && batch->shadow_cascade_extent[i] > 0.0F; i++) {
            debug.light_view_projection[i] = batch->shadow_view_projection[i];
            debug.cascade_count           += 1.0F;
        }

        after[after_count++] = (_NYA_PostStep){
            .pipeline     = _NYA_POST_PIPELINE_DEBUG,
            .uniform      = &debug,
            .uniform_size = sizeof(debug),
            .inputs       = _NYA_POST_INPUT_SOURCE | _NYA_POST_INPUT_NORMALS | _NYA_POST_INPUT_HALF,
        };
    }

    nya_assert(before_count <= _NYA_POST_BUILT_IN_MAX);

    /*
     * A caller's pass whose pipeline has not finished loading is skipped rather than drawn. The built-in ones were
     * only queued once loaded.
     */
    u32 usable = after_count;

    for (u32 i = 0; i < before_count; i++) usable += before[i].target != nullptr ? 0 : 1;

    for (u32 i = 0; i < pass_count; i++) {
        if (_nya_post_pass_ready(&passes[i])) usable++;
    }

    // only a chain of two or more passes has a between, and one pass costs no second target.
    _nya_post_intermediate_ensure(window, chain, usable > 1);

    // Nothing to run: put the captured scene back on the window so the frame is not simply lost.
    if (usable == 0) {
        nya_render2d_render_texture(window, &chain->targets[chain->scene_index], 0.0F, 0.0F, (f32)window->screen_width,
                                    (f32)window->screen_height, NYA_COLOR_WHITE);
        return;
    }

    u32 source = chain->scene_index;
    u32 run    = 0;

    u32 total = before_count + pass_count + after_count;

    for (u32 i = 0; i < total; i++) {
        _NYA_PostStep step;

        if (i < before_count) {
            step = before[i];
        } else if (i < before_count + pass_count) {
            const NYA_PostPass* pass = &passes[i - before_count];

            if (!_nya_post_pass_ready(pass)) continue;

            step = (_NYA_PostStep){
                .pipeline     = pass->pipeline,
                .texture      = pass->texture,
                .uniform      = pass->uniform,
                .uniform_size = pass->uniform_size,
                .tint         = pass->tint,
            };
        } else {
            step = after[i - before_count - pass_count];
        }

        // the occlusion and the blur go to their own targets and leave the chain's image where it was.
        if (step.target != nullptr) {
            nya_render_texture_begin(window, step.target, NYA_COLOR_TRANSPARENT);
            _nya_post_draw_step(window, chain, source, &step);
            nya_render_texture_end(window);
            continue;
        }

        run++;

        // The last surviving pass draws to the window; the rest ping-pong into the other target.
        if (run == usable) {
            if (step.inputs == 0) _nya_post_draw_pass(window, &chain->targets[source], &step);
            else _nya_post_draw_step(window, chain, source, &step);

            return;
        }

        const u32 destination = source ^ 1U;

        nya_render_texture_begin(window, &chain->targets[destination], NYA_COLOR_TRANSPARENT);

        if (step.inputs == 0) _nya_post_draw_pass(window, &chain->targets[source], &step);
        else _nya_post_draw_step(window, chain, source, &step);

        nya_render_texture_end(window);

        source = destination;
    }
}

void nya_post_chain_destroy(NYA_PostChain* chain) {
    if (chain == nullptr) return;

    for (u32 i = 0; i < nya_carray_length(chain->targets); i++) nya_render_texture_destroy(&chain->targets[i]);

    nya_render_texture_destroy(&chain->half);
    nya_render_texture_destroy(&chain->blur);

    // the scene options are the caller's choice, not state, so they survive.
    *chain = (NYA_PostChain){ .scene = chain->scene };
}

void nya_post_ink_set(NYA_Window* window, NYA_PostInk ink) {
    nya_assert(window != nullptr);

    ink.color = (NYA_Color){
        nya_clamp(ink.color.r, 0.0F, 1.0F),
        nya_clamp(ink.color.g, 0.0F, 1.0F),
        nya_clamp(ink.color.b, 0.0F, 1.0F),
        nya_clamp(ink.color.a, 0.0F, 1.0F),
    };

    // past sixteen pixels the eight taps no longer meet, and a line breaks into dots.
    ink.width      = nya_clamp(ink.width, 0.0F, 16.0F);
    ink.crease     = nya_clamp(ink.crease, 0.0F, 179.0F);
    ink.fade_start = nya_max(ink.fade_start, 0.0F);
    ink.fade_end   = nya_max(ink.fade_end, 0.0F);

    window->render_system.post_ink = ink;
}

NYA_PostInk nya_post_ink(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.post_ink;
}

void nya_post_ambient_occlusion_set(NYA_Window* window, NYA_PostAmbientOcclusion occlusion) {
    nya_assert(window != nullptr);

    occlusion.radius     = nya_clamp(occlusion.radius, 0.0F, 16.0F);
    occlusion.strength   = nya_clamp(occlusion.strength, 0.0F, 1.0F);
    occlusion.band       = nya_clamp(occlusion.band, 0.0F, 1.0F);
    // the shader reaches forty pixels at most.
    occlusion.min_radius = nya_clamp(occlusion.min_radius, 0.0F, 40.0F);

    window->render_system.post_ambient_occlusion = occlusion;
}

NYA_PostAmbientOcclusion nya_post_ambient_occlusion(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.post_ambient_occlusion;
}

void nya_post_antialias_set(NYA_Window* window, NYA_PostAntialias antialias) {
    nya_assert(window != nullptr);

    antialias.subpixel  = nya_clamp(antialias.subpixel, 0.0F, 1.0F);
    antialias.threshold = nya_clamp(antialias.threshold, 0.0F, 1.0F);

    window->render_system.post_antialias = antialias;
}

NYA_PostAntialias nya_post_antialias(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.post_antialias;
}

void nya_post_depth_of_field_set(NYA_Window* window, NYA_PostDepthOfField depth_of_field) {
    nya_assert(window != nullptr);

    // a config file can hold any number.
    if ((u32)depth_of_field.focus >= NYA_POST_FOCUS_COUNT) depth_of_field.focus = NYA_POST_FOCUS_OFF;

    depth_of_field.band_offset    = nya_clamp(depth_of_field.band_offset, -0.5F, 0.5F);
    depth_of_field.band           = nya_clamp(depth_of_field.band, 0.0F, 0.5F);
    depth_of_field.focus_distance = nya_max(depth_of_field.focus_distance, 0.0F);
    depth_of_field.focus_range    = nya_max(depth_of_field.focus_range, 0.0F);
    depth_of_field.falloff        = nya_max(depth_of_field.falloff, 0.0F);
    depth_of_field.radius         = nya_clamp(depth_of_field.radius, 0.0F, NYA_POST_DEPTH_OF_FIELD_RADIUS_MAX);
    depth_of_field.layers         = nya_min(depth_of_field.layers, 16U);

    window->render_system.post_depth_of_field = depth_of_field;
}

NYA_PostDepthOfField nya_post_depth_of_field(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.post_depth_of_field;
}

void nya_post_speed_lines_set(NYA_Window* window, NYA_PostSpeedLines speed_lines) {
    nya_assert(window != nullptr);

    speed_lines.amount       = nya_clamp(speed_lines.amount, 0.0F, 1.0F);
    speed_lines.center_x     = nya_clamp(speed_lines.center_x, -0.5F, 0.5F);
    speed_lines.center_y     = nya_clamp(speed_lines.center_y, -0.5F, 0.5F);
    speed_lines.density      = nya_clamp(speed_lines.density, 0.0F, 1024.0F);
    speed_lines.clear_radius = nya_clamp(speed_lines.clear_radius, 0.0F, 2.0F);

    window->render_system.post_speed_lines = speed_lines;
}

NYA_PostSpeedLines nya_post_speed_lines(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.post_speed_lines;
}

void nya_post_debug_view_set(NYA_Window* window, NYA_PostDebugView view) {
    nya_assert(window != nullptr);

    // a config file can hold any number.
    window->render_system.post_debug_view = (u32)view < NYA_POST_DEBUG_VIEW_COUNT ? view : NYA_POST_DEBUG_VIEW_NONE;
}

NYA_PostDebugView nya_post_debug_view(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.post_debug_view;
}
