/**
 * @file gpu_gles.c
 *
 * A WebGL2 / GLES3 implementation of the slice of the SDL_GPU API that the engine's 2D renderer
 * touches, so the same render2d command stream that runs on Vulkan/D3D12/Metal natively can run in a
 * browser with no change to a line of engine `.c` or to SDL_gpu.h.
 *
 * WHY THIS CAN EXIST AT ALL
 *
 * SDL_gpu.h declares every handle type as an opaque forward declaration — `typedef struct X X;` with no
 * body — and only the enums and the info/description structs are fully defined there. In a native build
 * SDL owns the bodies and the functions. In the emscripten build no SDL library is linked (the wasm
 * command compiles a chosen leaf set of engine `.c` files and includes the SDL headers for their
 * declarations only), so there is no symbol to collide with: this file is free to give the opaque
 * handles bodies that hold GLES state and to define the SDL_GPU functions the 2D path calls. To the
 * engine source and to SDL_gpu.h nothing changed; the linker simply finds these definitions instead of
 * a vendored library's.
 *
 * WHAT IS AND IS NOT HERE
 *
 * Implemented: the ~two dozen entry points the 2D flush and frame bring-up reach — device and window,
 * buffers and transfer buffers, textures and samplers, shader compile and pipeline link, the command
 * buffer, the render pass (clear + binds + uniform push + indexed draw), the copy pass (buffer and
 * texture upload) and submit. Everything else in SDL_gpu.h that the 2D path never calls (compute, blit,
 * 3D depth resolve, storage buffers, indirect draw, download) is either absent or a logged-TODO stub.
 *
 * NO COMMAND BUFFERS ON THE GL TIMELINE
 *
 * WebGL2 has no command buffers and no copy passes: a GL call executes when it is made. So the command
 * buffer and the copy pass here are bookkeeping handles, and `SDL_UploadToGPU*` does the glTexSubImage /
 * glBufferSubData immediately rather than recording it. `SDL_Submit*` is a `glFlush`, and a fence is a
 * no-op the caller may wait on trivially. The render pass carries the small amount of state a GL draw
 * needs that the SDL_GPU model attaches to the pass and pipeline rather than to global GL state (the
 * bound pipeline, vertex buffer and index buffer), and applies it at the draw call.
 *
 * BINDINGS
 *
 * The compiled GLSL ES the engine feeds (see the assets/shader/compiled tree) has no `layout(binding=)`:
 * SPIRV-Cross emits a named uniform block (`type_Uniforms`) and a synthesised sampler name (`_29`). So
 * the shim assigns binding points itself at link time — the vertex uniform block to binding 0 (matching
 * the `slot_index` the engine pushes vertex uniforms to), and each `sampler2D` uniform to the next
 * texture unit in declared order — by reflection on the linked program, never by a hard-coded name
 * beyond the block name the generator fixes.
 *
 * The whole file is `#if OS_WASM`; on native it is empty.
 * */

#include "nyangine/base/base_basic.h"

#if OS_WASM

#include "nyangine/renderer/gpu_gles/gpu_gles.h"

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_memory.h"

#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <GLES3/gl3.h>

#include <string.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLE BODIES — the opaque SDL_GPU types, given GLES state
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many texture-sampler pairs and uniform blocks one 2D pipeline can carry. Two of each is plenty. */
#define NYA_GLES_MAX_SAMPLERS 4
#define NYA_GLES_MAX_UNIFORMS 4
#define NYA_GLES_MAX_ATTRIBUTES 8

typedef struct {
    GLuint   location;
    GLint    size;       // component count: 2 for float2, 4 for ubyte4/float4
    GLenum   type;       // GL_FLOAT, GL_UNSIGNED_BYTE, ...
    GLboolean normalized;
    GLuint   offset;     // byte offset within a vertex
    GLuint   buffer_slot;
} _NYA_GLESAttribute;

struct SDL_GPUDevice {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl;
    b8          gl_ok;
    NYA_Arena*  arena;

    /** Uniform buffers, one per push slot, created lazily. Vertex slot 0 lives at binding 0. */
    GLuint vertex_ubo[NYA_GLES_MAX_UNIFORMS];
    GLuint fragment_ubo[NYA_GLES_MAX_UNIFORMS];

    /** The one sentinel that stands for the default framebuffer (fbo 0), handed back as the swapchain. */
    struct SDL_GPUTexture* default_target;
};

struct SDL_GPUBuffer {
    GLuint id;
    GLenum target;   // GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER
    u32    size;
    b8     storage_ready;
};

struct SDL_GPUTransferBuffer {
    u8* staging;
    u32 size;
};

struct SDL_GPUTexture {
    GLuint id;
    u32    width;
    u32    height;
    b8     is_default_target; // fbo 0, not a real GL texture object
};

struct SDL_GPUSampler {
    GLuint id;
};

struct SDL_GPUShader {
    GLuint id;
    GLenum stage;    // GL_VERTEX_SHADER / GL_FRAGMENT_SHADER
    u32    num_samplers;
    u32    num_uniform_buffers;
};

struct SDL_GPUGraphicsPipeline {
    GLuint program;

    _NYA_GLESAttribute attributes[NYA_GLES_MAX_ATTRIBUTES];
    u32                attribute_count;
    GLsizei            vertex_pitch;

    // blend, captured from color target 0
    b8     blend_enabled;
    GLenum src_color, dst_color, color_op;
    GLenum src_alpha, dst_alpha, alpha_op;

    b8     depth_test;
    GLenum primitive; // GL_TRIANGLES / GL_LINES
};

struct SDL_GPUCommandBuffer {
    SDL_GPUDevice* device;
};

struct SDL_GPURenderPass {
    SDL_GPUDevice*                 device;
    SDL_GPUCommandBuffer*          cmd;
    const SDL_GPUGraphicsPipeline* pipeline;

    GLuint vertex_buffer;
    u32    vertex_offset;

    GLuint index_buffer;
    GLenum index_type;   // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
    u32    index_size;   // 2 or 4

    u32 target_width;
    u32 target_height;
};

struct SDL_GPUCopyPass {
    SDL_GPUDevice* device;
};

struct SDL_GPUFence {
    b8 signalled;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHIM-ONLY STATE: the trace and the single device, for headless verification
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_GLES_TRACE_MAX 256

typedef struct {
    NYA_ConstCString entries[NYA_GLES_TRACE_MAX];
    u32              count;
    b8               context_ok;
} _NYA_GLESGlobal;

NYA_INTERNAL _NYA_GLESGlobal _gles = { 0 };

/** Records one shim entry point by name; saturates rather than wrapping, so the first frame stays readable. */
NYA_INTERNAL void _nya_gles_trace(NYA_ConstCString name) {
    if (_gles.count < NYA_GLES_TRACE_MAX) _gles.entries[_gles.count] = name;
    _gles.count++;
}

b8 nya_gpu_gles_context_ok(void) {
    return _gles.context_ok;
}

u32 nya_gpu_gles_trace_count(void) {
    return _gles.count;
}

NYA_ConstCString nya_gpu_gles_trace_at(u32 index) {
    if (index >= _gles.count || index >= NYA_GLES_TRACE_MAX) return "";
    return _gles.entries[index];
}

void nya_gpu_gles_trace_reset(void) {
    _gles.count = 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENUM TRANSLATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_gles_vertex_format(SDL_GPUVertexElementFormat format, OUT GLint* size, OUT GLenum* type, OUT GLboolean* normalized) {
    switch (format) {
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT:  *size = 1; *type = GL_FLOAT;         *normalized = GL_FALSE; return;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2: *size = 2; *type = GL_FLOAT;         *normalized = GL_FALSE; return;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3: *size = 3; *type = GL_FLOAT;         *normalized = GL_FALSE; return;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4: *size = 4; *type = GL_FLOAT;         *normalized = GL_FALSE; return;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM: *size = 4; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE;  return;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM: *size = 2; *type = GL_UNSIGNED_BYTE; *normalized = GL_TRUE;  return;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM:  *size = 4; *type = GL_BYTE;          *normalized = GL_TRUE;  return;
        default:
            nya_log_warn("gpu_gles: unhandled vertex element format %d, treating as float4.", (int)format);
            *size = 4; *type = GL_FLOAT; *normalized = GL_FALSE;
            return;
    }
}

NYA_INTERNAL GLenum _nya_gles_blend_factor(SDL_GPUBlendFactor factor) {
    switch (factor) {
        case SDL_GPU_BLENDFACTOR_ZERO:                     return GL_ZERO;
        case SDL_GPU_BLENDFACTOR_ONE:                      return GL_ONE;
        case SDL_GPU_BLENDFACTOR_SRC_COLOR:                return GL_SRC_COLOR;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR:      return GL_ONE_MINUS_SRC_COLOR;
        case SDL_GPU_BLENDFACTOR_DST_COLOR:                return GL_DST_COLOR;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR:      return GL_ONE_MINUS_DST_COLOR;
        case SDL_GPU_BLENDFACTOR_SRC_ALPHA:                return GL_SRC_ALPHA;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:      return GL_ONE_MINUS_SRC_ALPHA;
        case SDL_GPU_BLENDFACTOR_DST_ALPHA:                return GL_DST_ALPHA;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA:      return GL_ONE_MINUS_DST_ALPHA;
        case SDL_GPU_BLENDFACTOR_CONSTANT_COLOR:           return GL_CONSTANT_COLOR;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
        case SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE:       return GL_SRC_ALPHA_SATURATE;
        default:                                           return GL_ONE;
    }
}

NYA_INTERNAL GLenum _nya_gles_blend_op(SDL_GPUBlendOp op) {
    switch (op) {
        case SDL_GPU_BLENDOP_ADD:              return GL_FUNC_ADD;
        case SDL_GPU_BLENDOP_SUBTRACT:         return GL_FUNC_SUBTRACT;
        case SDL_GPU_BLENDOP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case SDL_GPU_BLENDOP_MIN:              return GL_MIN;
        case SDL_GPU_BLENDOP_MAX:              return GL_MAX;
        default:                               return GL_FUNC_ADD;
    }
}

NYA_INTERNAL GLenum _nya_gles_filter(SDL_GPUFilter filter) {
    return filter == SDL_GPU_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
}

NYA_INTERNAL GLenum _nya_gles_address(SDL_GPUSamplerAddressMode mode) {
    switch (mode) {
        case SDL_GPU_SAMPLERADDRESSMODE_REPEAT:          return GL_REPEAT;
        case SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
        default:                                         return GL_CLAMP_TO_EDGE;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DEVICE & WINDOW
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUDevice* SDL_CreateGPUDevice(SDL_GPUShaderFormat format_flags, bool debug_mode, const char* name) {
    nya_unused(format_flags), nya_unused(debug_mode), nya_unused(name);
    _nya_gles_trace("SDL_CreateGPUDevice");

    NYA_Arena* arena = nya_arena_create(.name = "gpu_gles_device");

    SDL_GPUDevice* device = (SDL_GPUDevice*)nya_arena_alloc(arena, sizeof(SDL_GPUDevice));
    nya_memset(device, 0, sizeof(SDL_GPUDevice));
    device->arena = arena;

    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2; // WebGL2 == GLES3
    attrs.minorVersion = 0;
    attrs.alpha        = false;
    attrs.depth        = true;
    attrs.antialias    = false;

    // The canvas the page created. Under node there is no canvas, so this returns <= 0 and the shim
    // runs in trace-only mode: the sequence is still recorded, no GL is issued.
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl = emscripten_webgl_create_context("#canvas", &attrs);
    if (gl > 0 && emscripten_webgl_make_context_current(gl) == EMSCRIPTEN_RESULT_SUCCESS) {
        device->gl        = gl;
        device->gl_ok     = true;
        _gles.context_ok  = true;
        nya_log_info("gpu_gles: WebGL2 context created and made current.");
    } else {
        device->gl_ok    = false;
        _gles.context_ok = false;
        nya_log_warn("gpu_gles: no WebGL2 context (headless/node). Shim runs in trace-only mode.");
    }

    device->default_target = (SDL_GPUTexture*)nya_arena_alloc(arena, sizeof(SDL_GPUTexture));
    nya_memset(device->default_target, 0, sizeof(SDL_GPUTexture));
    device->default_target->is_default_target = true;

    return device;
}

void SDL_DestroyGPUDevice(SDL_GPUDevice* device) {
    _nya_gles_trace("SDL_DestroyGPUDevice");
    if (device == nullptr) return;
    if (device->gl_ok) emscripten_webgl_destroy_context(device->gl);
    nya_arena_destroy(device->arena); // frees the device itself, allocated from this arena
}

bool SDL_ClaimWindowForGPUDevice(SDL_GPUDevice* device, SDL_Window* window) {
    nya_unused(window);
    _nya_gles_trace("SDL_ClaimWindowForGPUDevice");
    // The context is bound to the page's one canvas already; there is nothing per-window to claim.
    return device != nullptr;
}

SDL_GPUShaderFormat SDL_GetGPUShaderFormats(SDL_GPUDevice* device) {
    nya_unused(device);
    _nya_gles_trace("SDL_GetGPUShaderFormats");
    // The engine keys shader selection on this. There is no GLSL flag in SDL's enum, so the shim reports
    // PRIVATE — its own marker — and the wasm build feeds this backend the compiled GLSL ES source.
    return SDL_GPU_SHADERFORMAT_PRIVATE;
}

SDL_GPUTextureFormat SDL_GetGPUSwapchainTextureFormat(SDL_GPUDevice* device, SDL_Window* window) {
    nya_unused(device), nya_unused(window);
    _nya_gles_trace("SDL_GetGPUSwapchainTextureFormat");
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BUFFERS & TRANSFER BUFFERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUBuffer* SDL_CreateGPUBuffer(SDL_GPUDevice* device, const SDL_GPUBufferCreateInfo* createinfo) {
    _nya_gles_trace("SDL_CreateGPUBuffer");

    SDL_GPUBuffer* buffer = (SDL_GPUBuffer*)nya_arena_alloc(device->arena, sizeof(SDL_GPUBuffer));
    nya_memset(buffer, 0, sizeof(SDL_GPUBuffer));
    buffer->size   = createinfo->size;
    buffer->target = (createinfo->usage & SDL_GPU_BUFFERUSAGE_INDEX) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;

    if (device->gl_ok) {
        glGenBuffers(1, &buffer->id);
        glBindBuffer(buffer->target, buffer->id);
        glBufferData(buffer->target, (GLsizeiptr)createinfo->size, nullptr, GL_DYNAMIC_DRAW);
        buffer->storage_ready = true;
    }
    return buffer;
}

void SDL_ReleaseGPUBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer) {
    _nya_gles_trace("SDL_ReleaseGPUBuffer");
    if (buffer == nullptr) return;
    if (device->gl_ok && buffer->id != 0) glDeleteBuffers(1, &buffer->id);
}

SDL_GPUTransferBuffer* SDL_CreateGPUTransferBuffer(SDL_GPUDevice* device, const SDL_GPUTransferBufferCreateInfo* createinfo) {
    _nya_gles_trace("SDL_CreateGPUTransferBuffer");

    SDL_GPUTransferBuffer* transfer = (SDL_GPUTransferBuffer*)nya_arena_alloc(device->arena, sizeof(SDL_GPUTransferBuffer));
    nya_memset(transfer, 0, sizeof(SDL_GPUTransferBuffer));
    transfer->size    = createinfo->size;
    transfer->staging = (u8*)nya_arena_alloc(device->arena, createinfo->size); // CPU staging, engine allocator
    return transfer;
}

void SDL_ReleaseGPUTransferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBuffer* transfer_buffer) {
    nya_unused(device), nya_unused(transfer_buffer);
    _nya_gles_trace("SDL_ReleaseGPUTransferBuffer");
    // Staging lives in the device arena; it is reclaimed when the device is destroyed.
}

void* SDL_MapGPUTransferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBuffer* transfer_buffer, bool cycle) {
    nya_unused(device), nya_unused(cycle);
    _nya_gles_trace("SDL_MapGPUTransferBuffer");
    return transfer_buffer->staging; // a pointer into staging; no GPU mapping needed
}

void SDL_UnmapGPUTransferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBuffer* transfer_buffer) {
    nya_unused(device), nya_unused(transfer_buffer);
    _nya_gles_trace("SDL_UnmapGPUTransferBuffer");
    // Staging is persistent; nothing to flush until an upload copies from it.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TEXTURES & SAMPLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUTexture* SDL_CreateGPUTexture(SDL_GPUDevice* device, const SDL_GPUTextureCreateInfo* createinfo) {
    _nya_gles_trace("SDL_CreateGPUTexture");

    SDL_GPUTexture* texture = (SDL_GPUTexture*)nya_arena_alloc(device->arena, sizeof(SDL_GPUTexture));
    nya_memset(texture, 0, sizeof(SDL_GPUTexture));
    texture->width  = createinfo->width;
    texture->height = createinfo->height;

    if (createinfo->format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
        nya_log_warn("gpu_gles: SDL_CreateGPUTexture format %d not RGBA8; the 2D path only needs RGBA8. TODO other formats.",
                     (int)createinfo->format);
    }

    if (device->gl_ok) {
        glGenTextures(1, &texture->id);
        glBindTexture(GL_TEXTURE_2D, texture->id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)createinfo->width, (GLsizei)createinfo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // Defaults; a bound sampler object overrides these at draw time.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return texture;
}

void SDL_ReleaseGPUTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture) {
    _nya_gles_trace("SDL_ReleaseGPUTexture");
    if (texture == nullptr) return;
    if (device->gl_ok && texture->id != 0) glDeleteTextures(1, &texture->id);
}

SDL_GPUSampler* SDL_CreateGPUSampler(SDL_GPUDevice* device, const SDL_GPUSamplerCreateInfo* createinfo) {
    _nya_gles_trace("SDL_CreateGPUSampler");

    SDL_GPUSampler* sampler = (SDL_GPUSampler*)nya_arena_alloc(device->arena, sizeof(SDL_GPUSampler));
    nya_memset(sampler, 0, sizeof(SDL_GPUSampler));

    if (device->gl_ok) {
        glGenSamplers(1, &sampler->id);
        glSamplerParameteri(sampler->id, GL_TEXTURE_MIN_FILTER, (GLint)_nya_gles_filter(createinfo->min_filter));
        glSamplerParameteri(sampler->id, GL_TEXTURE_MAG_FILTER, (GLint)_nya_gles_filter(createinfo->mag_filter));
        glSamplerParameteri(sampler->id, GL_TEXTURE_WRAP_S, (GLint)_nya_gles_address(createinfo->address_mode_u));
        glSamplerParameteri(sampler->id, GL_TEXTURE_WRAP_T, (GLint)_nya_gles_address(createinfo->address_mode_v));
    }
    return sampler;
}

void SDL_ReleaseGPUSampler(SDL_GPUDevice* device, SDL_GPUSampler* sampler) {
    _nya_gles_trace("SDL_ReleaseGPUSampler");
    if (sampler == nullptr) return;
    if (device->gl_ok && sampler->id != 0) glDeleteSamplers(1, &sampler->id);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHADERS & PIPELINES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUShader* SDL_CreateGPUShader(SDL_GPUDevice* device, const SDL_GPUShaderCreateInfo* createinfo) {
    _nya_gles_trace("SDL_CreateGPUShader");

    SDL_GPUShader* shader = (SDL_GPUShader*)nya_arena_alloc(device->arena, sizeof(SDL_GPUShader));
    nya_memset(shader, 0, sizeof(SDL_GPUShader));
    shader->stage               = (createinfo->stage == SDL_GPU_SHADERSTAGE_VERTEX) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    shader->num_samplers        = createinfo->num_samplers;
    shader->num_uniform_buffers = createinfo->num_uniform_buffers;

    if (device->gl_ok) {
        shader->id = glCreateShader(shader->stage);
        const GLchar* source = (const GLchar*)createinfo->code; // GLSL ES 300 source bytes
        GLint         length = (GLint)createinfo->code_size;
        glShaderSource(shader->id, 1, &source, &length);
        glCompileShader(shader->id);

        GLint compiled = 0;
        glGetShaderiv(shader->id, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char  log[1024] = { 0 };
            GLsizei written = 0;
            glGetShaderInfoLog(shader->id, sizeof(log) - 1, &written, log);
            nya_log_error("gpu_gles: shader compile failed (%s stage):\n%s",
                          shader->stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        }
    }
    return shader;
}

void SDL_ReleaseGPUShader(SDL_GPUDevice* device, SDL_GPUShader* shader) {
    _nya_gles_trace("SDL_ReleaseGPUShader");
    if (shader == nullptr) return;
    if (device->gl_ok && shader->id != 0) glDeleteShader(shader->id);
}

SDL_GPUGraphicsPipeline* SDL_CreateGPUGraphicsPipeline(SDL_GPUDevice* device, const SDL_GPUGraphicsPipelineCreateInfo* createinfo) {
    _nya_gles_trace("SDL_CreateGPUGraphicsPipeline");

    SDL_GPUGraphicsPipeline* pipeline = (SDL_GPUGraphicsPipeline*)nya_arena_alloc(device->arena, sizeof(SDL_GPUGraphicsPipeline));
    nya_memset(pipeline, 0, sizeof(SDL_GPUGraphicsPipeline));

    // ── vertex layout: captured now, replayed at each draw as glVertexAttribPointer ──
    const SDL_GPUVertexInputState* input = &createinfo->vertex_input_state;
    pipeline->vertex_pitch = (input->num_vertex_buffers > 0) ? (GLsizei)input->vertex_buffer_descriptions[0].pitch : 0;

    u32 count = input->num_vertex_attributes;
    if (count > NYA_GLES_MAX_ATTRIBUTES) count = NYA_GLES_MAX_ATTRIBUTES;
    pipeline->attribute_count = count;
    for (u32 i = 0; i < count; i++) {
        const SDL_GPUVertexAttribute* attribute = &input->vertex_attributes[i];
        _NYA_GLESAttribute*           out       = &pipeline->attributes[i];
        out->location    = attribute->location;
        out->offset      = attribute->offset;
        out->buffer_slot = attribute->buffer_slot;
        _nya_gles_vertex_format(attribute->format, &out->size, &out->type, &out->normalized);
    }

    // ── blend: from color target 0, else opaque ──
    if (createinfo->target_info.num_color_targets > 0) {
        const SDL_GPUColorTargetBlendState* blend = &createinfo->target_info.color_target_descriptions[0].blend_state;
        pipeline->blend_enabled = blend->enable_blend;
        pipeline->src_color = _nya_gles_blend_factor(blend->src_color_blendfactor);
        pipeline->dst_color = _nya_gles_blend_factor(blend->dst_color_blendfactor);
        pipeline->color_op  = _nya_gles_blend_op(blend->color_blend_op);
        pipeline->src_alpha = _nya_gles_blend_factor(blend->src_alpha_blendfactor);
        pipeline->dst_alpha = _nya_gles_blend_factor(blend->dst_alpha_blendfactor);
        pipeline->alpha_op  = _nya_gles_blend_op(blend->alpha_blend_op);
    }

    pipeline->depth_test = createinfo->depth_stencil_state.enable_depth_test;
    pipeline->primitive  = (createinfo->primitive_type == SDL_GPU_PRIMITIVETYPE_LINELIST ||
                            createinfo->primitive_type == SDL_GPU_PRIMITIVETYPE_LINESTRIP)
                               ? GL_LINES
                               : GL_TRIANGLES;

    if (device->gl_ok) {
        SDL_GPUShader* vertex_shader   = createinfo->vertex_shader;
        SDL_GPUShader* fragment_shader = createinfo->fragment_shader;

        pipeline->program = glCreateProgram();
        glAttachShader(pipeline->program, vertex_shader->id);
        glAttachShader(pipeline->program, fragment_shader->id);
        glLinkProgram(pipeline->program);

        GLint linked = 0;
        glGetProgramiv(pipeline->program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char    log[1024] = { 0 };
            GLsizei written   = 0;
            glGetProgramInfoLog(pipeline->program, sizeof(log) - 1, &written, log);
            nya_log_error("gpu_gles: program link failed:\n%s", log);
            return pipeline;
        }

        // The vertex uniform block SPIRV-Cross names `type_Uniforms` → binding 0, matching the slot the
        // engine pushes vertex uniforms to (see SDL_PushGPUVertexUniformData). No layout(binding=) exists
        // in the source, so the shim assigns it here.
        GLuint block = glGetUniformBlockIndex(pipeline->program, "type_Uniforms");
        if (block != GL_INVALID_INDEX) glUniformBlockBinding(pipeline->program, block, 0);

        // Each sampler2D uniform → the next texture unit in declared order. The names are synthesised
        // (`_29`), so bind by reflecting the active uniforms rather than by a fixed name.
        glUseProgram(pipeline->program);
        GLint uniform_count = 0;
        glGetProgramiv(pipeline->program, GL_ACTIVE_UNIFORMS, &uniform_count);
        GLint unit = 0;
        for (GLint i = 0; i < uniform_count; i++) {
            char    uname[64] = { 0 };
            GLsizei len       = 0;
            GLint   usize     = 0;
            GLenum  utype     = 0;
            glGetActiveUniform(pipeline->program, (GLuint)i, sizeof(uname) - 1, &len, &usize, &utype, uname);
            if (utype == GL_SAMPLER_2D) {
                GLint location = glGetUniformLocation(pipeline->program, uname);
                if (location >= 0) glUniform1i(location, unit++);
            }
        }
    }
    return pipeline;
}

void SDL_ReleaseGPUGraphicsPipeline(SDL_GPUDevice* device, SDL_GPUGraphicsPipeline* graphics_pipeline) {
    _nya_gles_trace("SDL_ReleaseGPUGraphicsPipeline");
    if (graphics_pipeline == nullptr) return;
    if (device->gl_ok && graphics_pipeline->program != 0) glDeleteProgram(graphics_pipeline->program);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * COMMAND BUFFER, COPY PASS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUCommandBuffer* SDL_AcquireGPUCommandBuffer(SDL_GPUDevice* device) {
    _nya_gles_trace("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCommandBuffer* cmd = (SDL_GPUCommandBuffer*)nya_arena_alloc(device->arena, sizeof(SDL_GPUCommandBuffer));
    cmd->device = device;
    return cmd;
}

NYA_INTERNAL bool _nya_gles_acquire_swapchain(SDL_GPUCommandBuffer* command_buffer, SDL_Window* window, SDL_GPUTexture** swapchain_texture,
                                              Uint32* swapchain_texture_width, Uint32* swapchain_texture_height) {
    nya_unused(window);
    SDL_GPUDevice* device = command_buffer->device;

    int width = 0, height = 0;
    if (device->gl_ok) {
        emscripten_webgl_get_drawing_buffer_size(device->gl, &width, &height);
    }
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;

    device->default_target->width  = (u32)width;
    device->default_target->height = (u32)height;

    if (swapchain_texture != nullptr) *swapchain_texture = device->default_target;
    if (swapchain_texture_width != nullptr) *swapchain_texture_width = (u32)width;
    if (swapchain_texture_height != nullptr) *swapchain_texture_height = (u32)height;
    return true;
}

bool SDL_AcquireGPUSwapchainTexture(SDL_GPUCommandBuffer* command_buffer, SDL_Window* window, SDL_GPUTexture** swapchain_texture,
                                    Uint32* swapchain_texture_width, Uint32* swapchain_texture_height) {
    _nya_gles_trace("SDL_AcquireGPUSwapchainTexture");
    return _nya_gles_acquire_swapchain(command_buffer, window, swapchain_texture, swapchain_texture_width, swapchain_texture_height);
}

bool SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer* command_buffer, SDL_Window* window, SDL_GPUTexture** swapchain_texture,
                                           Uint32* swapchain_texture_width, Uint32* swapchain_texture_height) {
    _nya_gles_trace("SDL_WaitAndAcquireGPUSwapchainTexture");
    return _nya_gles_acquire_swapchain(command_buffer, window, swapchain_texture, swapchain_texture_width, swapchain_texture_height);
}

SDL_GPUCopyPass* SDL_BeginGPUCopyPass(SDL_GPUCommandBuffer* command_buffer) {
    _nya_gles_trace("SDL_BeginGPUCopyPass");
    SDL_GPUCopyPass* copy = (SDL_GPUCopyPass*)nya_arena_alloc(command_buffer->device->arena, sizeof(SDL_GPUCopyPass));
    copy->device = command_buffer->device;
    return copy;
}

void SDL_UploadToGPUBuffer(SDL_GPUCopyPass* copy_pass, const SDL_GPUTransferBufferLocation* source, const SDL_GPUBufferRegion* destination, bool cycle) {
    nya_unused(cycle);
    _nya_gles_trace("SDL_UploadToGPUBuffer");
    SDL_GPUDevice* device = copy_pass->device;
    if (!device->gl_ok) return;

    SDL_GPUBuffer* buffer = destination->buffer;
    glBindBuffer(buffer->target, buffer->id);
    glBufferSubData(buffer->target, (GLintptr)destination->offset, (GLsizeiptr)destination->size, source->transfer_buffer->staging + source->offset);
}

void SDL_UploadToGPUTexture(SDL_GPUCopyPass* copy_pass, const SDL_GPUTextureTransferInfo* source, const SDL_GPUTextureRegion* destination, bool cycle) {
    nya_unused(cycle);
    _nya_gles_trace("SDL_UploadToGPUTexture");
    SDL_GPUDevice* device = copy_pass->device;
    if (!device->gl_ok) return;

    SDL_GPUTexture* texture = destination->texture;
    glBindTexture(GL_TEXTURE_2D, texture->id);
    glTexSubImage2D(GL_TEXTURE_2D, (GLint)destination->mip_level, (GLint)destination->x, (GLint)destination->y,
                    (GLsizei)destination->w, (GLsizei)destination->h, GL_RGBA, GL_UNSIGNED_BYTE,
                    source->transfer_buffer->staging + source->offset);
}

void SDL_EndGPUCopyPass(SDL_GPUCopyPass* copy_pass) {
    nya_unused(copy_pass);
    _nya_gles_trace("SDL_EndGPUCopyPass");
    // Uploads already ran on the GL timeline; nothing to close.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RENDER PASS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPURenderPass* SDL_BeginGPURenderPass(SDL_GPUCommandBuffer* command_buffer, const SDL_GPUColorTargetInfo* color_target_infos,
                                          Uint32 num_color_targets, const SDL_GPUDepthStencilTargetInfo* depth_stencil_target_info) {
    nya_unused(depth_stencil_target_info);
    _nya_gles_trace("SDL_BeginGPURenderPass");

    SDL_GPUDevice*     device = command_buffer->device;
    SDL_GPURenderPass* pass   = (SDL_GPURenderPass*)nya_arena_alloc(device->arena, sizeof(SDL_GPURenderPass));
    nya_memset(pass, 0, sizeof(SDL_GPURenderPass));
    pass->device = device;
    pass->cmd    = command_buffer;

    pass->target_width  = device->default_target->width;
    pass->target_height = device->default_target->height;

    if (device->gl_ok) {
        // The 2D path renders to the swapchain: fbo 0. (A render-to-texture target would bind a real FBO;
        // that is deferred with the rest of the 3D/post path — see the blocker list.)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, (GLsizei)pass->target_width, (GLsizei)pass->target_height);
        glDisable(GL_SCISSOR_TEST);

        if (num_color_targets > 0 && color_target_infos[0].load_op == SDL_GPU_LOADOP_CLEAR) {
            SDL_FColor c = color_target_infos[0].clear_color;
            glClearColor(c.r, c.g, c.b, c.a);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
    }
    return pass;
}

void SDL_BindGPUGraphicsPipeline(SDL_GPURenderPass* render_pass, SDL_GPUGraphicsPipeline* graphics_pipeline) {
    _nya_gles_trace("SDL_BindGPUGraphicsPipeline");
    render_pass->pipeline = graphics_pipeline;
    if (!render_pass->device->gl_ok) return;

    glUseProgram(graphics_pipeline->program);

    if (graphics_pipeline->blend_enabled) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(graphics_pipeline->src_color, graphics_pipeline->dst_color, graphics_pipeline->src_alpha, graphics_pipeline->dst_alpha);
        glBlendEquationSeparate(graphics_pipeline->color_op, graphics_pipeline->alpha_op);
    } else {
        glDisable(GL_BLEND);
    }

    if (graphics_pipeline->depth_test) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
}

void SDL_BindGPUVertexBuffers(SDL_GPURenderPass* render_pass, Uint32 first_slot, const SDL_GPUBufferBinding* bindings, Uint32 num_bindings) {
    nya_unused(first_slot);
    _nya_gles_trace("SDL_BindGPUVertexBuffers");
    if (num_bindings == 0) return;
    render_pass->vertex_buffer = bindings[0].buffer->id;
    render_pass->vertex_offset = bindings[0].offset;
}

void SDL_BindGPUIndexBuffer(SDL_GPURenderPass* render_pass, const SDL_GPUBufferBinding* binding, SDL_GPUIndexElementSize index_element_size) {
    _nya_gles_trace("SDL_BindGPUIndexBuffer");
    render_pass->index_buffer = binding->buffer->id;
    if (index_element_size == SDL_GPU_INDEXELEMENTSIZE_16BIT) {
        render_pass->index_type = GL_UNSIGNED_SHORT;
        render_pass->index_size = 2;
    } else {
        render_pass->index_type = GL_UNSIGNED_INT;
        render_pass->index_size = 4;
    }
}

void SDL_BindGPUFragmentSamplers(SDL_GPURenderPass* render_pass, Uint32 first_slot, const SDL_GPUTextureSamplerBinding* texture_sampler_bindings, Uint32 num_bindings) {
    _nya_gles_trace("SDL_BindGPUFragmentSamplers");
    if (!render_pass->device->gl_ok) return;

    for (Uint32 i = 0; i < num_bindings; i++) {
        GLuint unit = first_slot + i; // the pipeline set sampler N to unit N in declared order
        glActiveTexture(GL_TEXTURE0 + unit);
        SDL_GPUTexture* texture = texture_sampler_bindings[i].texture;
        SDL_GPUSampler* sampler = texture_sampler_bindings[i].sampler;
        glBindTexture(GL_TEXTURE_2D, texture != nullptr ? texture->id : 0);
        glBindSampler(unit, sampler != nullptr ? sampler->id : 0);
    }
}

NYA_INTERNAL void _nya_gles_push_uniform(SDL_GPUCommandBuffer* command_buffer, GLuint* ubo_slots, Uint32 slot_index, const void* data, Uint32 length) {
    SDL_GPUDevice* device = command_buffer->device;
    if (!device->gl_ok) return;
    if (slot_index >= NYA_GLES_MAX_UNIFORMS) {
        nya_log_warn("gpu_gles: uniform slot %u out of range.", (unsigned)slot_index);
        return;
    }
    if (ubo_slots[slot_index] == 0) glGenBuffers(1, &ubo_slots[slot_index]);

    glBindBuffer(GL_UNIFORM_BUFFER, ubo_slots[slot_index]);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)length, data, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, slot_index, ubo_slots[slot_index]);
}

void SDL_PushGPUVertexUniformData(SDL_GPUCommandBuffer* command_buffer, Uint32 slot_index, const void* data, Uint32 length) {
    _nya_gles_trace("SDL_PushGPUVertexUniformData");
    // Vertex uniform slot N → UBO binding N. The pipeline bound `type_Uniforms` to binding 0.
    _nya_gles_push_uniform(command_buffer, command_buffer->device->vertex_ubo, slot_index, data, length);
}

void SDL_PushGPUFragmentUniformData(SDL_GPUCommandBuffer* command_buffer, Uint32 slot_index, const void* data, Uint32 length) {
    _nya_gles_trace("SDL_PushGPUFragmentUniformData");
    // The built-in 2D pipelines declare no fragment uniform blocks; this is reached only by a custom
    // shape shader. Its block would need its own binding assigned at link time — TODO with custom 2D
    // shaders. For now push to a separate binding range so it cannot collide with the vertex block.
    _nya_gles_push_uniform(command_buffer, command_buffer->device->fragment_ubo, slot_index, data, length);
}

void SDL_SetGPUViewport(SDL_GPURenderPass* render_pass, const SDL_GPUViewport* viewport) {
    _nya_gles_trace("SDL_SetGPUViewport");
    if (!render_pass->device->gl_ok) return;
    glViewport((GLint)viewport->x, (GLint)viewport->y, (GLsizei)viewport->w, (GLsizei)viewport->h);
    glDepthRangef(viewport->min_depth, viewport->max_depth);
}

void SDL_SetGPUScissor(SDL_GPURenderPass* render_pass, const SDL_Rect* scissor) {
    _nya_gles_trace("SDL_SetGPUScissor");
    if (!render_pass->device->gl_ok) return;
    glEnable(GL_SCISSOR_TEST);
    // GL's scissor origin is bottom-left; the engine's rects are top-left, so flip y against the target.
    GLint y = (GLint)render_pass->target_height - (scissor->y + scissor->h);
    glScissor(scissor->x, y, scissor->w, scissor->h);
}

NYA_INTERNAL void _nya_gles_setup_attributes(SDL_GPURenderPass* render_pass) {
    const SDL_GPUGraphicsPipeline* pipeline = render_pass->pipeline;
    glBindBuffer(GL_ARRAY_BUFFER, render_pass->vertex_buffer);
    for (u32 i = 0; i < pipeline->attribute_count; i++) {
        const _NYA_GLESAttribute* a = &pipeline->attributes[i];
        glEnableVertexAttribArray(a->location);
        const void* pointer = (const void*)(uintptr_t)(render_pass->vertex_offset + a->offset);
        if (a->type == GL_FLOAT) {
            glVertexAttribPointer(a->location, a->size, a->type, a->normalized, pipeline->vertex_pitch, pointer);
        } else {
            // Integer-typed attributes still read as normalized floats in the shader (UBYTE4_NORM), which
            // is what glVertexAttribPointer with normalized=GL_TRUE does.
            glVertexAttribPointer(a->location, a->size, a->type, a->normalized, pipeline->vertex_pitch, pointer);
        }
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, render_pass->index_buffer);
}

void SDL_DrawGPUIndexedPrimitives(SDL_GPURenderPass* render_pass, Uint32 num_indices, Uint32 num_instances, Uint32 first_index, Sint32 vertex_offset, Uint32 first_instance) {
    nya_unused(first_instance);
    _nya_gles_trace("SDL_DrawGPUIndexedPrimitives");
    if (!render_pass->device->gl_ok || render_pass->pipeline == nullptr) return;

    if (vertex_offset != 0) {
        nya_log_warn("gpu_gles: non-zero vertex_offset %d unsupported (WebGL2 has no BaseVertex draw); the 2D path uses 0.", (int)vertex_offset);
    }

    _nya_gles_setup_attributes(render_pass);

    const void* offset = (const void*)(uintptr_t)((u64)first_index * render_pass->index_size);
    if (num_instances > 1) {
        glDrawElementsInstanced(render_pass->pipeline->primitive, (GLsizei)num_indices, render_pass->index_type, offset, (GLsizei)num_instances);
    } else {
        glDrawElements(render_pass->pipeline->primitive, (GLsizei)num_indices, render_pass->index_type, offset);
    }
}

void SDL_DrawGPUPrimitives(SDL_GPURenderPass* render_pass, Uint32 num_vertices, Uint32 num_instances, Uint32 first_vertex, Uint32 first_instance) {
    nya_unused(first_instance);
    _nya_gles_trace("SDL_DrawGPUPrimitives");
    if (!render_pass->device->gl_ok || render_pass->pipeline == nullptr) return;

    _nya_gles_setup_attributes(render_pass);
    if (num_instances > 1) {
        glDrawArraysInstanced(render_pass->pipeline->primitive, (GLint)first_vertex, (GLsizei)num_vertices, (GLsizei)num_instances);
    } else {
        glDrawArrays(render_pass->pipeline->primitive, (GLint)first_vertex, (GLsizei)num_vertices);
    }
}

void SDL_EndGPURenderPass(SDL_GPURenderPass* render_pass) {
    nya_unused(render_pass);
    _nya_gles_trace("SDL_EndGPURenderPass");
    // No deferred pass to resolve on GL; the draws already executed.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SUBMIT & FENCES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

bool SDL_SubmitGPUCommandBuffer(SDL_GPUCommandBuffer* command_buffer) {
    _nya_gles_trace("SDL_SubmitGPUCommandBuffer");
    if (command_buffer->device->gl_ok) glFlush();
    return true;
}

SDL_GPUFence* SDL_SubmitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer* command_buffer) {
    _nya_gles_trace("SDL_SubmitGPUCommandBufferAndAcquireFence");
    if (command_buffer->device->gl_ok) glFlush();
    SDL_GPUFence* fence = (SDL_GPUFence*)nya_arena_alloc(command_buffer->device->arena, sizeof(SDL_GPUFence));
    fence->signalled = true; // GL has no explicit fence here; a submitted buffer is done as far as the caller waits
    return fence;
}

bool SDL_WaitForGPUFences(SDL_GPUDevice* device, bool wait_all, SDL_GPUFence* const* fences, Uint32 num_fences) {
    nya_unused(device), nya_unused(wait_all), nya_unused(fences), nya_unused(num_fences);
    _nya_gles_trace("SDL_WaitForGPUFences");
    return true; // immediate: the work was flushed at submit
}

void SDL_ReleaseGPUFence(SDL_GPUDevice* device, SDL_GPUFence* fence) {
    nya_unused(device), nya_unused(fence);
    _nya_gles_trace("SDL_ReleaseGPUFence");
}

#endif // OS_WASM
