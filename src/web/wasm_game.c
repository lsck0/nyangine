/**
 * @file wasm_game.c
 *
 * The game slice of the web port: the engine's REAL 2D drawing module — a cleared background and one
 * textured sprite — on a WebGL2 canvas, driven by emscripten_set_main_loop. Where wasm_ui.c proved the
 * immediate UI runs client-side against the HTML presenter, this proves the engine's own nya_render2d_*
 * path runs against a browser, on top of the SDL_GPU → GLES3 shim (src/nyangine/renderer/gpu_gles).
 *
 * WHAT DRIVES THE FRAME
 *
 * The sprite is drawn with nya_render2d_texture and nya_render2d_flush — the shipping 2D module compiled
 * verbatim (render2d.c, render_sort.c, render_camera.c) — not a hand-written SDL_GPU command stream. The
 * flush builds the orthographic projection with nya_matrix_orthographic, batches the quad, uploads it in a
 * copy pass and issues the indexed draw, exactly as it does on native. Everything below the SDL_GPU line
 * is the shim; everything above it is the engine.
 *
 * THE 2D-ONLY BRING-UP SEAM
 *
 * The native renderer.c stands the whole window up in one function that creates the 3D, post, shadow and
 * 2D pipelines together and resolves them through the asset system. None of that compiles under emscripten
 * (no SDL library, no asset blob, the 3D bring-up drags box3d/ufbx), and render2d needs almost none of it:
 * a 2D frame wants the batch's GPU buffers, two pipelines built from the GLSL ES shaders, and a device to
 * push the projection through. So this file supplies a 2D-only bring-up — game_bringup() below — the way
 * wasm_ui.c supplies the app and window backend core_app.c/core_window.c would: the real state render2d
 * reads, minus the SDL/asset machinery. render2d.c itself is compiled UNCHANGED, so the native build is
 * byte-identical; the seam lives entirely here and in the small asset/render backend this file defines.
 *
 * The backend answers the five engine functions render2d reaches into on the draw path —
 * nya_asset_get, nya_asset_graphics_pipeline, nya_asset_is_missing, nya_asset_missing_report and
 * _nya_render_sampler_for — from a tiny table this file registers the shim-built pipelines and the sprite
 * texture into. It is a wasm backend, not a stub: the pipelines and the texture are the real GPU resources
 * the batch binds; there is simply no on-disk asset system underneath them here.
 *
 * WHAT node CAN AND CANNOT SEE
 *
 * A headless node run has no canvas, so the shim gets no WebGL2 context and issues no GL — but it still
 * records every entry point in call order. nyangine_game_selfcheck() runs one render2d frame and asserts
 * that the recorded shim sequence is exactly the one the real 2D flush issues, returning 1 on success. In
 * a browser the same frame runs with a live context and actually clears + draws. Only a browser can
 * confirm the pixels; node confirms the engine drove the shim in the right order. See web/game.html.
 * */

#include <emscripten/emscripten.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// The full header graph, for NYA_Vertex2D, NYA_App/NYA_Window and the SDL_GPU types (renderer.h includes
// SDL3/SDL_gpu.h). Headers only: nothing GPU, physics or SDL is compiled from a vendored library here.
#define NYA_HEADLESS
#include "nyangine/nyangine.h"

// The os backend first: page/time/random for a module with no OS under it, as wasm_ui/wasm_demo do.
#include "nyangine/os/os_wasm.c"

// ── base: the arena → logging leaves the shim, the 2D module and this demo reach (as wasm_ui.c links). ──
#include "nyangine/base/base_arena.c"
#include "nyangine/base/base_backtrace.c"
#include "nyangine/base/base_ceiling.c"
#include "nyangine/base/base_clock.c"
#include "nyangine/base/base_error.c"
#include "nyangine/base/base_hash.c"
#include "nyangine/base/base_logging.c"
#include "nyangine/base/base_object.c"
#include "nyangine/base/base_reflection.c"
#include "nyangine/base/base_string.c"
#include "nyangine/base/base_types.c"

// ── math: the 2D flush's projection (math_matrix, now that it compiles on wasm — NYA_F16_IS_F32), the
// vectors shapes are built in, and the rectangle overlap/union the range merge sorts by. ──
#include "nyangine/math/math_matrix.c"
#include "nyangine/math/math_shapes.c"
#include "nyangine/math/math_vector.c"

// ── the SDL_GPU → GLES3 shim: in this one translation unit the SDL_Create* calls below and the ones the
// 2D flush issues resolve to the shim, not to a vendored SDL. ──
#include "nyangine/renderer/gpu_gles/gpu_gles.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE APP + WINDOW + ASSET/RENDER BACKEND — what core_app.c, core_window.c, core_asset.c and renderer.c
 * would supply, minus SDL and the on-disk asset system. The twin of wasm_ui.c's backend section.
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What nya_app_get returns: the single app instance, exactly as core_app.h declares it (core_app.c's twin). */
NYA_App _NYA_APP_INSTANCE;

NYA_App* nya_app_get(void) {
    nya_assert(_NYA_APP_INSTANCE.initialized);
    return &_NYA_APP_INSTANCE;
}

/** The one window the 2D batch lives on. Its render_system holds the batch, the open pass and the command buffer. */
NYA_INTERNAL NYA_Window WINDOW = {
    .handle        = { .index = 1, .generation = 1 },
    .screen_width  = 640,
    .screen_height = 480,
};

NYA_Window* nya_window_get(NYA_WindowHandle window) {
    if (window.index == WINDOW.handle.index && window.generation == WINDOW.handle.generation) return &WINDOW;
    return nullptr;
}

/** The handle the sprite is registered under. Any string will do: this file both writes and reads the table. */
#define SPRITE_TEXTURE_HANDLE "wasm_sprite"

/**
 * The whole asset table: the two 2D pipelines the flush resolves by handle, and the one sprite texture the
 * demo draws. render2d compares pipeline handles by the NYA_RENDER2D_PIPELINE_* literal, but nya_asset_get
 * matches by text so the registration need not share the literal's address across includes.
 * */
NYA_INTERNAL NYA_Asset _ASSET_TABLE[3] = { 0 };
NYA_INTERNAL u32       _ASSET_COUNT    = 0;

/** Registers one already-built asset under `handle`, returning the slot so the caller can fill its GPU resource. */
NYA_INTERNAL NYA_Asset* game_asset_register(NYA_ConstCString handle, NYA_AssetType type) {
    nya_assert(_ASSET_COUNT < nya_carray_length(_ASSET_TABLE), "the wasm asset table is full");

    NYA_Asset* asset = &_ASSET_TABLE[_ASSET_COUNT++];
    *asset           = (NYA_Asset){ .type = type, .handle = (NYA_AssetHandle)handle, .status = NYA_ASSET_STATUS_LOADED };
    return asset;
}

/*
 * The four asset-system entry points and the one render helper the 2D draw path reaches. Answered here for
 * the pre-built table, the way os_wasm.c answers the os interfaces: no queue, no reference counting, no
 * loading — every handle the demo uses is registered and LOADED before the first frame.
 */

NYA_Asset* nya_asset_get(NYA_AssetHandle handle) {
    if (handle == nullptr) return nullptr;

    for (u32 i = 0; i < _ASSET_COUNT; i++) {
        if (strcmp(_ASSET_TABLE[i].handle, handle) == 0) return &_ASSET_TABLE[i];
    }
    return nullptr;
}

SDL_GPUGraphicsPipeline* nya_asset_graphics_pipeline(NYA_Asset* asset, SDL_GPUSampleCount sample_count, b8 normals, b8 face_culling) {
    // one build per handle on wasm: the swapchain is single sampled with no normal buffer, so there are no variants.
    nya_unused(sample_count), nya_unused(normals), nya_unused(face_culling);

    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;
    return asset->as_graphics_pipeline.variants[0].pipeline;
}

b8 nya_asset_is_missing(NYA_AssetHandle handle) {
    // missing means "draw a placeholder for it": absent, or loaded-and-failed. Everything here is registered LOADED.
    return nya_asset_get(handle) == nullptr;
}

void nya_asset_missing_report(NYA_ConstCString handle) {
    nya_log_warn("wasm_game: asset '%s' is missing.", handle != nullptr ? handle : "(null)");
}

// _nya_render_sampler_for is the last engine function the draw path reaches; it is NYA_INTERNAL, declared by
// render_internal.h (included through render2d.c below) and defined in the bring-up section, once GAME exists.

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE 2D MODULE — the engine's real batch, sort and camera, compiled verbatim on top of the backend above.
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// render_glyph_atlas.c defines NYA_Glyph and NYA_GlyphGrid, which render2d.c's font atlas uses. The
// engine's native unity build includes it in the same TU; the same holds here so render2d.c parses. Its
// bodies (and render2d's whole text/font path) reach TTF and the on-disk cache, which no wasm slice links
// — but nothing this file calls reaches them either, so wasm-ld's dead-code pass drops the lot. The 2D
// sprite path needs no glyphs; this is only here to let the file compile.
#include "nyangine/renderer/render_glyph_atlas.c"

#include "nyangine/renderer/render_camera.c"
#include "nyangine/renderer/render_sort.c"
#include "nyangine/renderer/render2d.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BRING-UP — the 2D-only slice of what renderer.c's window bring-up does, plus the shim pipelines/texture
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define SPRITE_TEXTURE_SIZE 64

typedef struct {
    SDL_GPUDevice*  device;
    SDL_GPUSampler* sampler;
    b8              ready;
    u64             frame;
} GameState;

NYA_INTERNAL GameState GAME = { 0 };

/**
 * The render system's sampler for a filter. Native keeps one per NYA_TextureFilter on the render system; the
 * demo needs only the sprite's, so it hands back the one shim sampler the bring-up made. Never null after
 * bring-up. NYA_INTERNAL to match render_internal.h's declaration.
 * */
NYA_INTERNAL SDL_GPUSampler* _nya_render_sampler_for(NYA_TextureFilter filter) {
    nya_unused(filter);
    return GAME.sampler;
}

/**
 * render2d's custom-shader draws (nya_render2d_procedural/_fullscreen) resolve their pipeline through this;
 * the sprite path never touches them, so wasm-ld drops the callers and this body with them. Defined only so
 * the NYA_INTERNAL declaration render_internal.h makes is not left dangling — it is never actually called.
 * */
NYA_INTERNAL SDL_GPUGraphicsPipeline* _nya_render_pipeline(NYA_Window* window, NYA_Asset* asset) {
    nya_unused(window), nya_unused(asset);
    return nullptr;
}

/** Reads one embedded GLSL ES source file (baked in with --embed-file) into an arena buffer. */
NYA_INTERNAL u8* read_shader(NYA_Arena* arena, NYA_ConstCString path, OUT u32* out_size) {
    FILE* file = fopen(path, "rb");
    nya_assert(file != nullptr, "wasm_game: embedded shader %s missing.", path);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    u8* buffer = (u8*)nya_arena_alloc(arena, (u64)size + 1);
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    buffer[read] = 0;
    *out_size    = (u32)read;
    return buffer;
}

/** Fills a size×size RGBA8 checkerboard into `pixels`: two flat colours, so the sprite is unmistakable. */
NYA_INTERNAL void make_checkerboard(OUT u8* pixels, u32 size) {
    for (u32 y = 0; y < size; y++) {
        for (u32 x = 0; x < size; x++) {
            b8  light = ((x / 8) + (y / 8)) % 2 == 0;
            u8* p     = &pixels[(y * size + x) * 4];
            p[0] = light ? 240 : 40;
            p[1] = light ? 90 : 40;
            p[2] = light ? 40 : 90;
            p[3] = 255;
        }
    }
}

/**
 * Builds one 2D pipeline from the engine's own compiled GLSL ES shaders through the shim: batch2d.vert
 * paired with `fragment_path`, the NYA_Vertex2D layout, alpha blending and one RGBA8 target — the same
 * description NYA_VERTEX_LAYOUT_2D produces on native. `num_samplers` is 0 for the shape shader, 1 for the
 * textured one, matching what the fragment stage declares.
 * */
NYA_INTERNAL SDL_GPUGraphicsPipeline* build_pipeline(NYA_Arena* arena, NYA_ConstCString fragment_path, u32 num_samplers) {
    u32 vertex_size = 0, fragment_size = 0;
    u8* vertex_src   = read_shader(arena, "shaders/batch2d.vert.glsl", &vertex_size);
    u8* fragment_src = read_shader(arena, fragment_path, &fragment_size);

    SDL_GPUShader* vertex_shader = SDL_CreateGPUShader(GAME.device, &(SDL_GPUShaderCreateInfo){
        .code                = vertex_src,
        .code_size           = vertex_size,
        .entrypoint          = "main",
        .format              = SDL_GPU_SHADERFORMAT_PRIVATE,
        .stage               = SDL_GPU_SHADERSTAGE_VERTEX,
        .num_uniform_buffers = 1, // type_Uniforms { projection }, pushed per flush
    });
    SDL_GPUShader* fragment_shader = SDL_CreateGPUShader(GAME.device, &(SDL_GPUShaderCreateInfo){
        .code         = fragment_src,
        .code_size    = fragment_size,
        .entrypoint   = "main",
        .format       = SDL_GPU_SHADERFORMAT_PRIVATE,
        .stage        = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = num_samplers,
    });

    SDL_GPUVertexBufferDescription buffer_desc = {
        .slot       = 0,
        .pitch      = sizeof(NYA_Vertex2D),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    // Locations match batch2d.vert: 0 POSITION (float2), 1 COLOR0 (ubyte4_norm), 2 TEXCOORD0 (float2).
    SDL_GPUVertexAttribute attributes[] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,      .offset = offsetof(NYA_Vertex2D, x) },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(NYA_Vertex2D, color) },
        { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,      .offset = offsetof(NYA_Vertex2D, u) },
    };

    SDL_GPUColorTargetDescription color_target = {
        .format      = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .blend_state = {
            .enable_blend          = true,
            .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
            .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .color_blend_op        = SDL_GPU_BLENDOP_ADD,
            .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
            .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
        },
    };

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(GAME.device, &(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader   = vertex_shader,
        .fragment_shader = fragment_shader,
        .primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .vertex_input_state = {
            .vertex_buffer_descriptions = &buffer_desc,
            .num_vertex_buffers         = 1,
            .vertex_attributes          = attributes,
            .num_vertex_attributes      = 3,
        },
        .target_info = {
            .color_target_descriptions = &color_target,
            .num_color_targets         = 1,
        },
    });

    SDL_ReleaseGPUShader(GAME.device, vertex_shader);
    SDL_ReleaseGPUShader(GAME.device, fragment_shader);
    return pipeline;
}

/** Creates the sprite's shim texture, uploads a generated checkerboard into it, and registers it as an asset. */
NYA_INTERNAL void build_sprite_texture(NYA_Arena* arena) {
    nya_unused(arena);

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(GAME.device, &(SDL_GPUTextureCreateInfo){
        .type                 = SDL_GPU_TEXTURETYPE_2D,
        .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width                = SPRITE_TEXTURE_SIZE,
        .height               = SPRITE_TEXTURE_SIZE,
        .layer_count_or_depth = 1,
        .num_levels           = 1,
    });

    u32                    texture_bytes = SPRITE_TEXTURE_SIZE * SPRITE_TEXTURE_SIZE * 4;
    SDL_GPUTransferBuffer* transfer      = SDL_CreateGPUTransferBuffer(GAME.device, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = texture_bytes,
    });
    u8* texels = (u8*)SDL_MapGPUTransferBuffer(GAME.device, transfer, true);
    make_checkerboard(texels, SPRITE_TEXTURE_SIZE);
    SDL_UnmapGPUTransferBuffer(GAME.device, transfer);

    SDL_GPUCommandBuffer* upload = SDL_AcquireGPUCommandBuffer(GAME.device);
    SDL_GPUCopyPass*      copy   = SDL_BeginGPUCopyPass(upload);
    SDL_UploadToGPUTexture(copy,
        &(SDL_GPUTextureTransferInfo){ .transfer_buffer = transfer, .offset = 0 },
        &(SDL_GPUTextureRegion){ .texture = texture, .w = SPRITE_TEXTURE_SIZE, .h = SPRITE_TEXTURE_SIZE, .d = 1 },
        false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(upload);
    SDL_ReleaseGPUTransferBuffer(GAME.device, transfer);

    NYA_Asset* asset = game_asset_register(SPRITE_TEXTURE_HANDLE, NYA_ASSET_TYPE_TEXTURE);
    asset->as_texture.texture = texture;
    asset->as_texture.width   = SPRITE_TEXTURE_SIZE;
    asset->as_texture.height  = SPRITE_TEXTURE_SIZE;
    asset->as_texture.filter  = NYA_TEXTURE_FILTER_NEAREST;
}

/**
 * The 2D-only bring-up: the device, the app's render system, the batch's GPU + CPU buffers (the 2D slice of
 * renderer.c's window bring-up), the two pipelines and the sprite. After this the real nya_render2d_* API
 * draws through the batch the same way it does on native.
 * */
NYA_INTERNAL void game_bringup(void) {
    NYA_Arena* arena = nya_arena_create(.name = "wasm_game");

    GAME.device = SDL_CreateGPUDevice(SDL_GetGPUShaderFormats(nullptr), false, "gles");
    // The one window: the shim binds the page's canvas, so any non-null handle claims it.
    (void)SDL_ClaimWindowForGPUDevice(GAME.device, (SDL_Window*)&WINDOW);

    // The app render system render2d reads through: nya_app_get()->render_system.gpu_device in the flush,
    // and the arena the batch's staging lives in for the life of the module.
    _NYA_APP_INSTANCE.render_system.gpu_device = GAME.device;
    _NYA_APP_INSTANCE.render_system.allocator  = arena;
    _NYA_APP_INSTANCE.initialized              = true;

    // one sampler, handed to every textured draw through _nya_render_sampler_for. NEAREST, so the checker is crisp.
    GAME.sampler = SDL_CreateGPUSampler(GAME.device, &(SDL_GPUSamplerCreateInfo){
        .min_filter     = SDL_GPU_FILTER_NEAREST,
        .mag_filter     = SDL_GPU_FILTER_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    });

    // ── the batch's buffers: the 2D slice of renderer.c's per-window bring-up, sized by the same limits ──
    NYA_Render2DBatch* batch       = &WINDOW.render_system.draw_batch;
    u32                buffer_size = (u32)(NYA_RENDER2D_MAX_VERTICES * sizeof(NYA_Vertex2D));
    u32                index_size  = (u32)((u64)NYA_RENDER2D_MAX_INDICES * sizeof(u32));

    *batch = (NYA_Render2DBatch){ 0 };

    batch->vertex_buffer   = SDL_CreateGPUBuffer(GAME.device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = buffer_size });
    batch->index_buffer    = SDL_CreateGPUBuffer(GAME.device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_size });
    batch->transfer_buffer = SDL_CreateGPUTransferBuffer(GAME.device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = buffer_size });
    batch->index_transfer_buffer = SDL_CreateGPUTransferBuffer(GAME.device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = index_size });

    // from the module arena: lives as long as the page, rewritten every frame.
    batch->vertices = nya_arena_alloc(arena, NYA_RENDER2D_MAX_VERTICES * sizeof(NYA_Vertex2D));
    batch->indices  = nya_arena_alloc(arena, (u64)NYA_RENDER2D_MAX_INDICES * sizeof(u32));
    batch->ranges   = nya_arena_alloc(arena, NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDrawRange));
    batch->draws    = nya_arena_alloc(arena, NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDraw));

    // ── the two 2D pipelines, registered by the handle the flush resolves them through ──
    game_asset_register(NYA_RENDER2D_PIPELINE_SHAPES, NYA_ASSET_TYPE_GRAPHICS_PIPELINE)
        ->as_graphics_pipeline.variants[0].pipeline = build_pipeline(arena, "shaders/shape.frag.glsl", 0);
    game_asset_register(NYA_RENDER2D_PIPELINE_TEXTURED, NYA_ASSET_TYPE_GRAPHICS_PIPELINE)
        ->as_graphics_pipeline.variants[0].pipeline = build_pipeline(arena, "shaders/textured.frag.glsl", 1);

    build_sprite_texture(arena);

    GAME.ready = true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ONE FRAME — through the public nya_render2d_* API
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One frame: acquire the command buffer and swapchain, point the batch at them, open the CLEAR pass, draw
 * the sprite with nya_render2d_texture, flush, then end and submit. The flush suspends the clear pass, does
 * the copy upload, reopens the pass with LOAD and issues the indexed draw — the exact native 2D sequence.
 * The sprite is a quarter-inset square, so a resize is followed correctly.
 * */
NYA_INTERNAL void draw_frame(void) {
    if (!GAME.ready) return;
    GAME.frame++;

    NYA_RenderSystemWindow* render = &WINDOW.render_system;
    NYA_Render2DBatch*      batch  = &render->draw_batch;

    render->render_commands = SDL_AcquireGPUCommandBuffer(GAME.device);

    SDL_GPUTexture* swapchain = nullptr;
    u32             width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(render->render_commands, (SDL_Window*)&WINDOW, &swapchain, &width, &height)) {
        SDL_SubmitGPUCommandBuffer(render->render_commands);
        render->render_commands = nullptr;
        return;
    }
    if (width == 0) width = 640;
    if (height == 0) height = 480;

    WINDOW.screen_width  = width;
    WINDOW.screen_height = height;

    // The frame counters are per frame, zeroed here the way nya_render_begin does on native.
    batch->frame_flushes        = 0;
    batch->frame_vertices       = 0;
    batch->frame_indices        = 0;
    batch->frame_dropped_draws  = 0;
    batch->pending_flush_reason = NYA_RENDER2D_FLUSH_FRAME_END;
    for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) batch->frame_flush_reasons[i] = 0;

    // Point the batch at the swapchain: a single-sampled 2D target, no MSAA, depth or normal buffer. This is
    // what nya_render_begin sets on native; the flush's pass_resume reads exactly these fields.
    batch->target_texture      = swapchain;
    batch->target_msaa         = nullptr;
    batch->target_depth        = nullptr;
    batch->target_normal       = nullptr;
    batch->target_normal_msaa  = nullptr;
    batch->target_normal_written = false;
    batch->target_is_texture   = false;
    batch->resolve_pending     = false;
    batch->target_sample_count = SDL_GPU_SAMPLECOUNT_1;
    batch->target_width        = width;
    batch->target_height       = height;
    batch->camera              = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_NONE };
    render->render_pass_normals = false;

    // The clear: a dark slate, so the sprite is visible against it.
    render->render_pass = SDL_BeginGPURenderPass(render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture     = swapchain,
            .load_op     = SDL_GPU_LOADOP_CLEAR,
            .store_op    = SDL_GPU_STOREOP_STORE,
            .clear_color = { 0.06F, 0.07F, 0.10F, 1.0F },
        },
        1, nullptr);

    // A centered square a quarter in from every edge, in screen pixels, tinted white so the checker shows as-is.
    f32 inset_x = (f32)width * 0.25F;
    f32 inset_y = (f32)height * 0.25F;
    f32 size    = nya_min((f32)width - (inset_x * 2.0F), (f32)height - (inset_y * 2.0F));

    nya_render2d_texture_ex(&WINDOW, SPRITE_TEXTURE_HANDLE, (NYA_Render2DTexture){
        .x      = ((f32)width - size) * 0.5F,
        .y      = ((f32)height - size) * 0.5F,
        .width  = size,
        .height = size,
        .tint   = { 1.0F, 1.0F, 1.0F, 1.0F },
    });

    nya_render2d_flush(&WINDOW);

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;

    SDL_SubmitGPUCommandBuffer(render->render_commands);
    render->render_commands = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SELF-CHECK — proves one real render2d frame drove the shim in the SDL_GPU order, even with no GL (node)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The shim entry points one 2D frame issues, in order, when nya_render2d_texture is drawn through the batch:
 * the demo's acquire + clear pass, then the flush (map/unmap the vertex and index staging, suspend the pass,
 * copy-upload both buffers, reopen the pass — which reapplies the scissor — bind the buffers, then per draw
 * reapply the scissor, bind the pipeline, push the projection, bind the sampler and draw), then end + submit.
 * */
NYA_INTERNAL NYA_ConstCString EXPECTED[] = {
    "SDL_AcquireGPUCommandBuffer",
    "SDL_WaitAndAcquireGPUSwapchainTexture",
    "SDL_BeginGPURenderPass",             // the demo's CLEAR pass
    "SDL_MapGPUTransferBuffer",           // flush: vertices
    "SDL_UnmapGPUTransferBuffer",
    "SDL_MapGPUTransferBuffer",           // flush: indices
    "SDL_UnmapGPUTransferBuffer",
    "SDL_EndGPURenderPass",               // flush: suspend the clear pass
    "SDL_BeginGPUCopyPass",
    "SDL_UploadToGPUBuffer",              // vertices
    "SDL_UploadToGPUBuffer",              // indices
    "SDL_EndGPUCopyPass",
    "SDL_BeginGPURenderPass",             // flush: reopen the pass with LOAD
    "SDL_SetGPUScissor",                  // reopen reapplies the batch scissor
    "SDL_BindGPUVertexBuffers",
    "SDL_BindGPUIndexBuffer",
    "SDL_SetGPUScissor",                  // per-draw range scissor
    "SDL_BindGPUGraphicsPipeline",
    "SDL_PushGPUVertexUniformData",       // the orthographic projection
    "SDL_BindGPUFragmentSamplers",        // the sprite texture + sampler
    "SDL_DrawGPUIndexedPrimitives",
    "SDL_EndGPURenderPass",               // the demo ends the frame's pass
    "SDL_SubmitGPUCommandBuffer",
};

/**
 * Runs the bring-up (once) and one render2d frame with the trace reset around it, then asserts the recorded
 * shim call sequence is exactly the real 2D frame's, and that the frame produced one draw with geometry.
 * Returns 1 on success, 0 otherwise. Callable from node with ccall('nyangine_game_selfcheck','number',[],[]).
 * */
EMSCRIPTEN_KEEPALIVE
int nyangine_game_selfcheck(void) {
    if (!GAME.ready) game_bringup();

    nya_gpu_gles_trace_reset();
    draw_frame();

    u32 expected_count = (u32)nya_carray_length(EXPECTED);
    u32 actual_count   = nya_gpu_gles_trace_count();

    b8 ok = actual_count == expected_count;
    if (ok) {
        for (u32 i = 0; i < expected_count; i++) {
            if (strcmp(nya_gpu_gles_trace_at(i), EXPECTED[i]) != 0) {
                ok = false;
                nya_log_error("wasm_game selfcheck: call %u was '%s', expected '%s'.", i, nya_gpu_gles_trace_at(i), EXPECTED[i]);
            }
        }
    } else {
        nya_log_error("wasm_game selfcheck: recorded %u shim calls, expected %u.", actual_count, expected_count);
        for (u32 i = 0; i < actual_count; i++) nya_log_info("  [%u] %s", i, nya_gpu_gles_trace_at(i));
    }

    // the batch's own accounting, independent of the shim trace: one draw call, four vertices, six indices.
    NYA_Render2DFrameStats stats = nya_render2d_frame_stats(&WINDOW);
    if (stats.draw_calls != 1 || stats.vertices != 4 || stats.indices != 6) {
        ok = false;
        nya_log_error("wasm_game selfcheck: batch drew %u calls / %u vertices / %u indices, expected 1 / 4 / 6.",
                      stats.draw_calls, stats.vertices, stats.indices);
    }

    nya_log_info("wasm_game selfcheck: sequence %s; WebGL2 context %s.",
                 ok ? "OK" : "MISMATCH", nya_gpu_gles_context_ok() ? "live (drawing real pixels)" : "absent (trace-only, headless)");
    return ok ? 1 : 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENTRY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The per-tick callback emscripten_set_main_loop drives; each tick clears and draws the sprite. */
NYA_INTERNAL void tick(void) {
    draw_frame();
}

/**
 * Stands the 2D module up, runs the self-check once (so every load — browser or node — proves the sequence),
 * then hands the frame to emscripten_set_main_loop for the browser. Under node the loop callback may not be
 * pumped, but the self-check already ran a full render2d frame.
 * */
int main(void) {
    game_bringup();
    (void)nyangine_game_selfcheck();

    // 0 fps = drive from requestAnimationFrame; do not block (simulate_infinite_loop = false), so the
    // module instantiation returns and node can call exports.
    emscripten_set_main_loop(tick, 0, 0);
    return 0;
}
