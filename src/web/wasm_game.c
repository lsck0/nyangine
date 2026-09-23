/**
 * @file wasm_game.c
 *
 * The game slice of the web port: the engine's 2D drawing — a cleared background and one textured sprite
 * — on a real WebGL2 canvas, driven by emscripten_set_main_loop. Where wasm_ui.c proved the immediate
 * UI runs client-side against the HTML presenter, this proves the engine's *GPU* 2D path runs against a
 * browser, by standing up the SDL_GPU → GLES3 shim (src/nyangine/renderer/gpu_gles) and issuing the exact
 * SDL_GPU command stream the 2D renderer's flush issues.
 *
 * WHY THIS DEMO CALLS THE SDL_GPU API RATHER THAN nya_render2d_*
 *
 * The stage's ideal is to draw through the public nya_render2d_* calls. Two things in the engine as it
 * stands today block compiling that path under emscripten, and both are recorded as blockers for the next
 * stage rather than papered over here:
 *
 *   1. render2d.c's projection and camera math call into math_matrix.c, whose nya_matrix_create is
 *      overloaded on f16 and f32 — and on wasm base_types.h widens f16 to a plain float (clang rejects
 *      _Float16 for wasm32), so the two overloads collapse to one signature and the file will not
 *      compile. The 2D flush needs nya_matrix_orthographic from exactly that file.
 *   2. The non-headless renderer.c brings the whole window renderer up in one ~700-line function that
 *      creates the 3D, post, shadow and 2D pipelines together and is steeped in the asset system (it
 *      resolves pipelines and textures through nya_asset_*). There is no seam to bring up only the 2D
 *      batch without dragging 3D and the asset blob in.
 *
 * So this demo drives the shim through the identical call sequence render2d.c's flush uses — acquire a
 * command buffer, wait+acquire the swapchain, begin a render pass that CLEARs, bind the pipeline, push the
 * projection as a vertex uniform, bind the vertex and index buffers, bind the fragment sampler, draw
 * indexed, end the pass, submit — with the engine's real NYA_Vertex2D layout, the engine's real compiled
 * batch2d/textured GLSL ES shaders, and the engine's exact orthographic matrix. Everything below the
 * SDL_GPU line is the shipping shim; only the ~20 lines that record the batch are the demo's own.
 *
 * WHAT node CAN AND CANNOT SEE
 *
 * A headless node run has no canvas, so the shim gets no WebGL2 context and issues no GL — but it still
 * records every entry point in call order. nyangine_game_selfcheck() runs one frame and asserts that the
 * recorded sequence is exactly the 2D frame's, returning 1 on success. In a browser the same frame runs
 * with a live context and actually clears + draws. See web/game.html.
 * */

#include <emscripten/emscripten.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// The full header graph, for NYA_Vertex2D and the SDL_GPU types (renderer.h includes SDL3/SDL_gpu.h).
// Headers only: nothing GPU, physics or SDL is compiled here, exactly as wasm_ui.c takes them.
#define NYA_HEADLESS
#include "nyangine/nyangine.h"

// The os backend first: page/time/random for a module with no OS under it, as wasm_demo/wasm_ui do.
#include "nyangine/os/os_wasm.c"

// ── base: the arena → logging leaves the shim and this demo reach (same set wasm_ui.c links). ──
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

// ── the SDL_GPU → GLES3 shim: the whole point. Its functions ARE SDL_CreateGPUDevice and the rest, so in
// this one translation unit the calls below resolve to the shim, not to a vendored SDL. ──
#include "nyangine/renderer/gpu_gles/gpu_gles.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE 2D BATCH — the engine's real vertex, drawn as one textured quad
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One checkerboard sprite: four NYA_Vertex2D corners, two triangles. The layout is the engine's own. */
#define SPRITE_TEXTURE_SIZE 64
#define QUAD_VERTEX_COUNT   4
#define QUAD_INDEX_COUNT    6

typedef struct {
    SDL_GPUDevice*           device;
    SDL_GPUGraphicsPipeline* pipeline;
    SDL_GPUBuffer*           vertex_buffer;
    SDL_GPUBuffer*           index_buffer;
    SDL_GPUTransferBuffer*   vertex_transfer;
    SDL_GPUTransferBuffer*   index_transfer;
    SDL_GPUTexture*          texture;
    SDL_GPUSampler*          sampler;
    b8                       ready;
    u64                      frame;
} GameState;

NYA_INTERNAL GameState GAME = { 0 };

/** Reads one embedded GLSL ES source file (baked in with --embed-file) into an arena buffer. */
NYA_INTERNAL u8* read_shader(NYA_Arena* arena, NYA_ConstCString path, OUT u32* out_size) {
    FILE* file = fopen(path, "rb");
    nya_assert(file != nullptr, "gpu_gles demo: embedded shader %s missing.", path);

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

/** Writes an NYA_Vertex2D corner: position in pixels, uv in 0..1, colour as RGBA bytes (the engine's layout). */
NYA_INTERNAL void write_vertex(NYA_Vertex2D* vertex, f32 x, f32 y, f32 u, f32 v) {
    vertex->x = x;
    vertex->y = y;
    vertex->u = u;
    vertex->v = v;
    vertex->color[0] = 255;
    vertex->color[1] = 255;
    vertex->color[2] = 255;
    vertex->color[3] = 255; // white tint: textured.frag multiplies texel by colour, so the checker shows as-is
}

/** Builds the shim's device, the batch2d+textured pipeline, the sprite texture, its sampler and the buffers. */
NYA_INTERNAL void setup(void) {
    NYA_Arena* arena = nya_arena_create(.name = "wasm_game_setup");

    GAME.device = SDL_CreateGPUDevice(SDL_GetGPUShaderFormats(nullptr), false, "gles");
    // The one window: the shim binds the page's canvas, so any non-null handle claims it.
    (void)SDL_ClaimWindowForGPUDevice(GAME.device, (SDL_Window*)&GAME);

    // ── shaders: the engine's own compiled GLSL ES 300, embedded at build time ──
    u32 vertex_size = 0, fragment_size = 0;
    u8* vertex_src   = read_shader(arena, "shaders/batch2d.vert.glsl", &vertex_size);
    u8* fragment_src = read_shader(arena, "shaders/textured.frag.glsl", &fragment_size);

    SDL_GPUShader* vertex_shader = SDL_CreateGPUShader(GAME.device, &(SDL_GPUShaderCreateInfo){
        .code               = vertex_src,
        .code_size          = vertex_size,
        .entrypoint         = "main",
        .format             = SDL_GPU_SHADERFORMAT_PRIVATE,
        .stage              = SDL_GPU_SHADERSTAGE_VERTEX,
        .num_uniform_buffers = 1, // type_Uniforms { projection }
    });
    SDL_GPUShader* fragment_shader = SDL_CreateGPUShader(GAME.device, &(SDL_GPUShaderCreateInfo){
        .code         = fragment_src,
        .code_size    = fragment_size,
        .entrypoint   = "main",
        .format       = SDL_GPU_SHADERFORMAT_PRIVATE,
        .stage        = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = 1, // sampler2D
    });

    // ── the pipeline: NYA_Vertex2D's exact layout, alpha blending, one RGBA8 target ──
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

    GAME.pipeline = SDL_CreateGPUGraphicsPipeline(GAME.device, &(SDL_GPUGraphicsPipelineCreateInfo){
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

    // ── the sprite texture: a generated checkerboard, uploaded through the transfer-buffer path ──
    GAME.texture = SDL_CreateGPUTexture(GAME.device, &(SDL_GPUTextureCreateInfo){
        .type                 = SDL_GPU_TEXTURETYPE_2D,
        .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width                = SPRITE_TEXTURE_SIZE,
        .height               = SPRITE_TEXTURE_SIZE,
        .layer_count_or_depth = 1,
        .num_levels           = 1,
    });
    GAME.sampler = SDL_CreateGPUSampler(GAME.device, &(SDL_GPUSamplerCreateInfo){
        .min_filter     = SDL_GPU_FILTER_NEAREST,
        .mag_filter     = SDL_GPU_FILTER_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    });

    u32 texture_bytes = SPRITE_TEXTURE_SIZE * SPRITE_TEXTURE_SIZE * 4;
    SDL_GPUTransferBuffer* texture_transfer = SDL_CreateGPUTransferBuffer(GAME.device, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = texture_bytes,
    });
    u8* texels = (u8*)SDL_MapGPUTransferBuffer(GAME.device, texture_transfer, true);
    make_checkerboard(texels, SPRITE_TEXTURE_SIZE);
    SDL_UnmapGPUTransferBuffer(GAME.device, texture_transfer);

    // ── the geometry buffers: 4 vertices, 6 indices; content is refreshed each frame in the tick ──
    GAME.vertex_buffer = SDL_CreateGPUBuffer(GAME.device, &(SDL_GPUBufferCreateInfo){
        .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
        .size  = sizeof(NYA_Vertex2D) * QUAD_VERTEX_COUNT,
    });
    GAME.index_buffer = SDL_CreateGPUBuffer(GAME.device, &(SDL_GPUBufferCreateInfo){
        .usage = SDL_GPU_BUFFERUSAGE_INDEX,
        .size  = sizeof(u32) * QUAD_INDEX_COUNT,
    });
    GAME.vertex_transfer = SDL_CreateGPUTransferBuffer(GAME.device, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = sizeof(NYA_Vertex2D) * QUAD_VERTEX_COUNT,
    });
    GAME.index_transfer = SDL_CreateGPUTransferBuffer(GAME.device, &(SDL_GPUTransferBufferCreateInfo){
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = sizeof(u32) * QUAD_INDEX_COUNT,
    });

    // The index buffer never changes: two triangles over the quad, uploaded once inside a copy pass.
    u32* indices = (u32*)SDL_MapGPUTransferBuffer(GAME.device, GAME.index_transfer, true);
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    indices[3] = 0; indices[4] = 2; indices[5] = 3;
    SDL_UnmapGPUTransferBuffer(GAME.device, GAME.index_transfer);

    SDL_GPUCommandBuffer* upload = SDL_AcquireGPUCommandBuffer(GAME.device);
    SDL_GPUCopyPass*      copy   = SDL_BeginGPUCopyPass(upload);
    SDL_UploadToGPUTexture(copy,
        &(SDL_GPUTextureTransferInfo){ .transfer_buffer = texture_transfer, .offset = 0 },
        &(SDL_GPUTextureRegion){ .texture = GAME.texture, .w = SPRITE_TEXTURE_SIZE, .h = SPRITE_TEXTURE_SIZE, .d = 1 },
        false);
    SDL_UploadToGPUBuffer(copy,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = GAME.index_transfer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = GAME.index_buffer, .offset = 0, .size = sizeof(u32) * QUAD_INDEX_COUNT },
        false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(upload);
    SDL_ReleaseGPUTransferBuffer(GAME.device, texture_transfer);

    nya_arena_destroy(arena);
    GAME.ready = true;
}

/**
 * One frame, exactly the sequence render2d.c's flush issues: upload this frame's quad, then acquire →
 * wait swapchain → begin pass (clear) → bind pipeline → push projection → bind buffers → bind sampler →
 * draw → end → submit. The quad is a quarter-inset rectangle over the current swapchain, so a resize is
 * followed correctly.
 * */
NYA_INTERNAL void draw_frame(void) {
    if (!GAME.ready) return;
    GAME.frame++;

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(GAME.device);

    SDL_GPUTexture* swapchain = nullptr;
    u32             width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, (SDL_Window*)&GAME, &swapchain, &width, &height)) {
        SDL_SubmitGPUCommandBuffer(command_buffer);
        return;
    }
    if (width == 0) width = 640;
    if (height == 0) height = 480;

    // This frame's quad, in pixels: a centered square a quarter in from every edge, uv 0..1.
    f32 inset_x = (f32)width * 0.25F;
    f32 inset_y = (f32)height * 0.25F;
    f32 left = inset_x, right = (f32)width - inset_x;
    f32 top = inset_y, bottom = (f32)height - inset_y;

    NYA_Vertex2D* vertices = (NYA_Vertex2D*)SDL_MapGPUTransferBuffer(GAME.device, GAME.vertex_transfer, true);
    write_vertex(&vertices[0], left,  top,    0.0F, 0.0F);
    write_vertex(&vertices[1], right, top,    1.0F, 0.0F);
    write_vertex(&vertices[2], right, bottom, 1.0F, 1.0F);
    write_vertex(&vertices[3], left,  bottom, 0.0F, 1.0F);
    SDL_UnmapGPUTransferBuffer(GAME.device, GAME.vertex_transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command_buffer);
    SDL_UploadToGPUBuffer(copy,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = GAME.vertex_transfer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = GAME.vertex_buffer, .offset = 0, .size = sizeof(NYA_Vertex2D) * QUAD_VERTEX_COUNT },
        false);
    SDL_EndGPUCopyPass(copy);

    // The clear: a dark slate, so the sprite is visible against it.
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command_buffer,
        &(SDL_GPUColorTargetInfo){
            .texture     = swapchain,
            .load_op     = SDL_GPU_LOADOP_CLEAR,
            .store_op    = SDL_GPU_STOREOP_STORE,
            .clear_color = { 0.06F, 0.07F, 0.10F, 1.0F },
        },
        1, nullptr);

    SDL_BindGPUGraphicsPipeline(pass, GAME.pipeline);

    // The engine's orthographic matrix for (left=0, right=w, top=0, bottom=h): row-major, top-left origin,
    // y-up clip — which is WebGL2's clip too, so it needs no flip. Pushed to vertex uniform slot 0, the
    // binding the shim bound `type_Uniforms` to. (See nya_matrix_orthographic; replicated here because
    // math_matrix.c cannot compile on wasm — see the file comment.)
    f32 w = (f32)width, h = (f32)height;
    f32 projection[16] = {
        2.0F / w, 0.0F,      0.0F, -1.0F,
        0.0F,    -2.0F / h,  0.0F,  1.0F,
        0.0F,     0.0F,      1.0F,  0.0F,
        0.0F,     0.0F,      0.0F,  1.0F,
    };
    SDL_PushGPUVertexUniformData(command_buffer, 0, projection, sizeof(projection));

    SDL_BindGPUVertexBuffers(pass, 0, &(SDL_GPUBufferBinding){ .buffer = GAME.vertex_buffer, .offset = 0 }, 1);
    SDL_BindGPUIndexBuffer(pass, &(SDL_GPUBufferBinding){ .buffer = GAME.index_buffer, .offset = 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_BindGPUFragmentSamplers(pass, 0, &(SDL_GPUTextureSamplerBinding){ .texture = GAME.texture, .sampler = GAME.sampler }, 1);

    SDL_DrawGPUIndexedPrimitives(pass, QUAD_INDEX_COUNT, 1, 0, 0, 0);

    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SELF-CHECK — proves the frame drove the shim in the SDL_GPU order, even with no GL (node)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The entry points one 2D frame must issue, in order. draw_frame's copy pass precedes the render pass. */
NYA_INTERNAL NYA_ConstCString EXPECTED[] = {
    "SDL_AcquireGPUCommandBuffer",
    "SDL_WaitAndAcquireGPUSwapchainTexture",
    "SDL_MapGPUTransferBuffer",
    "SDL_UnmapGPUTransferBuffer",
    "SDL_BeginGPUCopyPass",
    "SDL_UploadToGPUBuffer",
    "SDL_EndGPUCopyPass",
    "SDL_BeginGPURenderPass",
    "SDL_BindGPUGraphicsPipeline",
    "SDL_PushGPUVertexUniformData",
    "SDL_BindGPUVertexBuffers",
    "SDL_BindGPUIndexBuffer",
    "SDL_BindGPUFragmentSamplers",
    "SDL_DrawGPUIndexedPrimitives",
    "SDL_EndGPURenderPass",
    "SDL_SubmitGPUCommandBuffer",
};

/**
 * Runs setup (once) and one frame with the trace reset around it, then asserts the recorded shim call
 * sequence is exactly the 2D frame's. Returns 1 on a clean sequence, 0 otherwise. Callable from node with
 * ccall('nyangine_game_selfcheck','number',[],[]) — the headless proof that the shim ran the frame right.
 * */
EMSCRIPTEN_KEEPALIVE
int nyangine_game_selfcheck(void) {
    if (!GAME.ready) setup();

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
 * Sets the batch up, runs the self-check once (so every load — browser or node — proves the sequence),
 * then hands the frame to emscripten_set_main_loop for the browser. Under node the loop callback may not
 * be pumped, but the self-check already ran a full frame.
 * */
int main(void) {
    setup();
    (void)nyangine_game_selfcheck();

    // 0 fps = drive from requestAnimationFrame; do not block (simulate_infinite_loop = false), so the
    // module instantiation returns and node can call exports.
    emscripten_set_main_loop(tick, 0, 0);
    return 0;
}
