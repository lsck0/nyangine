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

    /**
     * The three FBOs the off-screen 3D path needs, created lazily on first use and reused for the life of
     * the device (WebGL2 has no per-pass framebuffer object in the SDL_GPU sense — attachments are just
     * re-pointed each pass):
     *   - render_fbo:  a render pass whose colour/depth targets are real textures, not the swapchain.
     *   - resolve_fbo: the single-sample destination a RESOLVE store op blits the multisample colour into.
     *   - blit_read/blit_draw: the read+draw pair SDL_BlitGPUTexture wraps around a texture→texture copy.
     */
    GLuint render_fbo;
    GLuint resolve_fbo;
    GLuint blit_read_fbo;
    GLuint blit_draw_fbo;
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
    GLuint id;   // a GL texture object, unless this is a renderbuffer or the default target
    GLuint rbo;  // a GL renderbuffer object, non-zero when the target is multisampled or non-samplable
    u32    width;
    u32    height;
    b8     is_default_target; // fbo 0, not a real GL texture object

    // Render-target/depth state, filled by SDL_CreateGPUTexture; a plain 2D sampler texture leaves these zeroed.
    b8     is_renderbuffer;   // backed by `rbo` (multisample colour/depth, or a non-sampled attachment)
    b8     is_color_target;   // usable as a colour attachment
    b8     is_depth;          // a depth (or depth-stencil) target
    b8     has_stencil;       // the depth format also carries stencil (D24_S8, D32F_S8)
    u32    sample_count;      // 1, 2, 4 or 8 — the MSAA sample count the target was created with
    GLenum gl_attachment;     // GL_DEPTH_ATTACHMENT / GL_DEPTH_STENCIL_ATTACHMENT for a depth target, else 0
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

    // Off-screen pass bookkeeping for the MSAA resolve; zeroed for the swapchain (fbo 0) path, which resolves nothing.
    b8              offscreen;             // bound a real FBO, not fbo 0
    u32             num_color_targets;
    SDL_GPUTexture* color_target[2];       // the colour attachments, in order (index 1 is the normal buffer)
    SDL_GPUTexture* resolve_target[2];     // where each colour attachment resolves, or null for no resolve
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

/**
 * The GL sized internal format, upload format and type for one SDL_GPU texture format, and whether it is a
 * depth (and depth-stencil) format. Only the formats the 2D + 3D + shadow + post path actually create are
 * handled; anything else falls back to RGBA8 with a warning, so an unforeseen target still links a texture
 * rather than crashing. WebGL2's renderable-format set is narrower than desktop GL: a single-channel colour
 * target maps to R8 (core-renderable) rather than R16_UNORM, which WebGL2 cannot render to without an
 * extension — a browser difference the shadow map's precision tolerates.
 * */
NYA_INTERNAL void _nya_gles_texture_format(SDL_GPUTextureFormat format, OUT GLenum* internal, OUT GLenum* upload, OUT GLenum* type,
                                           OUT b8* is_depth, OUT b8* has_stencil) {
    *is_depth = false;
    *has_stencil = false;
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: // WebGL2 has no renderable BGRA8; the shim stores RGBA8 either way.
            *internal = GL_RGBA8; *upload = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
        case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
        case SDL_GPU_TEXTUREFORMAT_R16_UNORM: // the shadow map: R16 is not WebGL2-renderable, so R8 stands in.
            *internal = GL_R8; *upload = GL_RED; *type = GL_UNSIGNED_BYTE; return;
        case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
            *internal = GL_R16F; *upload = GL_RED; *type = GL_HALF_FLOAT; return;
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
            *internal = GL_RGBA16F; *upload = GL_RGBA; *type = GL_HALF_FLOAT; return;
        case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
            *internal = GL_DEPTH_COMPONENT16; *upload = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; *is_depth = true; return;
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
            *internal = GL_DEPTH_COMPONENT24; *upload = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; *is_depth = true; return;
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
            *internal = GL_DEPTH_COMPONENT32F; *upload = GL_DEPTH_COMPONENT; *type = GL_FLOAT; *is_depth = true; return;
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
            *internal = GL_DEPTH24_STENCIL8; *upload = GL_DEPTH_STENCIL; *type = GL_UNSIGNED_INT_24_8; *is_depth = true; *has_stencil = true; return;
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
            *internal = GL_DEPTH32F_STENCIL8; *upload = GL_DEPTH_STENCIL; *type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; *is_depth = true; *has_stencil = true; return;
        default:
            nya_log_warn("gpu_gles: texture format %d unhandled, treating as RGBA8.", (int)format);
            *internal = GL_RGBA8; *upload = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
    }
}

/** How many samples an SDL sample-count enum names: SDL_GPU_SAMPLECOUNT_1/2/4/8 → 1/2/4/8. */
NYA_INTERNAL u32 _nya_gles_sample_count(SDL_GPUSampleCount count) {
    switch (count) {
        case SDL_GPU_SAMPLECOUNT_2: return 2;
        case SDL_GPU_SAMPLECOUNT_4: return 4;
        case SDL_GPU_SAMPLECOUNT_8: return 8;
        default:                    return 1;
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

    // The canvas the page created; under node there is none, so this returns <= 0 and the shim runs trace-only, recording the sequence with no GL.
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
    // The engine keys shader selection on this; with no GLSL flag in SDL's enum, the shim reports PRIVATE and the wasm build feeds it compiled GLSL ES.
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

    GLenum internal = 0, upload = 0, type = 0;
    b8     is_depth = false, has_stencil = false;
    _nya_gles_texture_format(createinfo->format, &internal, &upload, &type, &is_depth, &has_stencil);

    u32 samples = _nya_gles_sample_count(createinfo->sample_count);

    b8 sampler_use = (createinfo->usage & SDL_GPU_TEXTUREUSAGE_SAMPLER) != 0;
    b8 color_use   = (createinfo->usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) != 0;
    b8 depth_use   = (createinfo->usage & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) != 0;

    texture->sample_count   = samples;
    texture->is_color_target = color_use;
    texture->is_depth        = is_depth || depth_use;
    texture->has_stencil     = has_stencil;
    if (texture->is_depth) {
        texture->gl_attachment = texture->has_stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    }

    // A renderbuffer when the attachment is multisampled or a depth target with no sampling; everything else is a GL texture a later pass can read.
    texture->is_renderbuffer = samples > 1 || (texture->is_depth && !sampler_use);

    if (!device->gl_ok) return texture; // headless (node): record the shape, issue no GL.

    if (texture->is_renderbuffer) {
        glGenRenderbuffers(1, &texture->rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, texture->rbo);
        if (samples > 1) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)samples, internal, (GLsizei)createinfo->width, (GLsizei)createinfo->height);
        } else {
            glRenderbufferStorage(GL_RENDERBUFFER, internal, (GLsizei)createinfo->width, (GLsizei)createinfo->height);
        }
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        return texture;
    }

    glGenTextures(1, &texture->id);
    glBindTexture(GL_TEXTURE_2D, texture->id);
    // A colour/depth target is glTexStorage2D (immutable, complete before attachment); a sampler texture keeps glTexImage2D so upload can respecify it.
    if (color_use || depth_use) {
        glTexStorage2D(GL_TEXTURE_2D, 1, internal, (GLsizei)createinfo->width, (GLsizei)createinfo->height);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, (GLsizei)createinfo->width, (GLsizei)createinfo->height, 0, upload, type, nullptr);
    }
    // Defaults; a bound sampler object overrides these at draw time.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

void SDL_ReleaseGPUTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture) {
    _nya_gles_trace("SDL_ReleaseGPUTexture");
    if (texture == nullptr) return;
    if (!device->gl_ok) return;
    if (texture->id != 0) glDeleteTextures(1, &texture->id);
    if (texture->rbo != 0) glDeleteRenderbuffers(1, &texture->rbo);
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

/** True for a character that can appear inside a GLSL identifier, so a token match can require a word boundary. */
NYA_INTERNAL b8 _nya_gles_is_ident_char(GLchar c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/**
 * Renames a fragment shader's input varyings so the program links against the vertex stage.
 *
 * SPIRV-Cross names HLSL stage IO by semantic with a direction prefix — the vertex stage writes
 * `out_var_COLOR0`, the fragment stage reads `in_var_COLOR0` — and its GLSL ES target emits neither with a
 * `layout(location=)`. GLES3 then matches varyings by name, the two names differ, and glLinkProgram fails
 * ("FRAGMENT varying in_var_COLOR0 does not match any VERTEX varying"). Individually each shader is valid,
 * which is why the offline validation and the headless (no-GL) self-checks never caught it; only a real
 * link does. Rewriting the fragment's `in_var_` tokens to `out_var_` makes the names agree: a fragment
 * stage's only `in_var_*` are the varyings the vertex stage produced (its vertex-attribute inputs live in
 * the vertex stage, and are bound by location, not name), so the rename is total and safe. The match is
 * anchored at a word boundary so an identifier that merely ends in "in_var_" is left alone, and it is done
 * on a device-arena copy so the caller's source bytes are untouched.
 * */
NYA_INTERNAL const GLchar* _nya_gles_fragment_varying_fix(NYA_Arena* arena, const GLchar* source, GLint length, OUT GLint* out_length) {
    static const GLchar needle[] = "in_var_";
    static const GLchar repl[]   = "out_var_";
    u64 needle_len = sizeof(needle) - 1;
    u64 repl_len   = sizeof(repl) - 1;

    // repl is one byte longer than needle, so 2x the source (plus terminator) is a safe over-allocation that avoids a counting pass.
    GLchar* buffer = (GLchar*)nya_arena_alloc(arena, (u64)length * 2 + 1);

    u64 w = 0;
    for (GLint i = 0; i < length;) {
        b8 boundary = (i == 0) || !_nya_gles_is_ident_char(source[i - 1]);
        if (boundary && (u64)(length - i) >= needle_len && memcmp(source + i, needle, needle_len) == 0) {
            memcpy(buffer + w, repl, repl_len);
            w += repl_len;
            i += (GLint)needle_len;
        } else {
            buffer[w++] = source[i++];
        }
    }
    buffer[w]   = 0;
    *out_length = (GLint)w;
    return buffer;
}

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

        // The fragment stage's `in_var_` varyings will not link against the vertex stage's `out_var_`; normalise them here (see the helper above).
        if (shader->stage == GL_FRAGMENT_SHADER) {
            source = _nya_gles_fragment_varying_fix(device->arena, source, length, &length);
        }

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

        // The `type_Uniforms` block → binding 0, matching the slot the engine pushes to; no layout(binding=) in the source, so the shim assigns it here.
        GLuint block = glGetUniformBlockIndex(pipeline->program, "type_Uniforms");
        if (block != GL_INVALID_INDEX) glUniformBlockBinding(pipeline->program, block, 0);

        // Each sampler2D uniform → the next texture unit; names are synthesised (`_29`), so bind by reflecting the active uniforms, not a fixed name.
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

/** Attaches one colour/depth target (texture or renderbuffer) to the currently bound FBO at `attach_point`. */
NYA_INTERNAL void _nya_gles_attach(GLenum attach_point, SDL_GPUTexture* texture, u32 mip_level) {
    if (texture == nullptr) return;
    if (texture->is_renderbuffer) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, attach_point, GL_RENDERBUFFER, texture->rbo);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attach_point, GL_TEXTURE_2D, texture->id, (GLint)mip_level);
    }
}

SDL_GPURenderPass* SDL_BeginGPURenderPass(SDL_GPUCommandBuffer* command_buffer, const SDL_GPUColorTargetInfo* color_target_infos,
                                          Uint32 num_color_targets, const SDL_GPUDepthStencilTargetInfo* depth_stencil_target_info) {
    _nya_gles_trace("SDL_BeginGPURenderPass");

    SDL_GPUDevice*     device = command_buffer->device;
    SDL_GPURenderPass* pass   = (SDL_GPURenderPass*)nya_arena_alloc(device->arena, sizeof(SDL_GPURenderPass));
    nya_memset(pass, 0, sizeof(SDL_GPURenderPass));
    pass->device = device;
    pass->cmd    = command_buffer;

    // Off-screen when the first colour target is a real texture or a depth target is bound; the 2D swapchain path passes the default target and no depth, staying on fbo 0.
    b8 color_is_default = num_color_targets > 0 && color_target_infos[0].texture != nullptr && color_target_infos[0].texture->is_default_target;
    pass->offscreen     = (num_color_targets > 0 && !color_is_default) || depth_stencil_target_info != nullptr;

    // The target's size: a real colour target's own dimensions, else the depth target's, else the swapchain's.
    if (num_color_targets > 0 && color_target_infos[0].texture != nullptr && !color_is_default) {
        pass->target_width  = color_target_infos[0].texture->width;
        pass->target_height = color_target_infos[0].texture->height;
    } else if (depth_stencil_target_info != nullptr && depth_stencil_target_info->texture != nullptr) {
        pass->target_width  = depth_stencil_target_info->texture->width;
        pass->target_height = depth_stencil_target_info->texture->height;
    } else {
        pass->target_width  = device->default_target->width;
        pass->target_height = device->default_target->height;
    }

    if (!pass->offscreen) {
        // ── the swapchain: fbo 0, exactly as the 2D path has always driven it ──
        if (device->gl_ok) {
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

    // ── an off-screen target: bind the device's render FBO and re-point its attachments this pass ──
    pass->num_color_targets = num_color_targets > 2 ? 2 : num_color_targets;
    for (u32 i = 0; i < pass->num_color_targets; i++) {
        pass->color_target[i] = color_target_infos[i].texture;
        // A RESOLVE store op on a multisampled colour target sends it to resolve_texture at SDL_EndGPURenderPass.
        b8 resolve = (color_target_infos[i].store_op == SDL_GPU_STOREOP_RESOLVE || color_target_infos[i].store_op == SDL_GPU_STOREOP_RESOLVE_AND_STORE);
        pass->resolve_target[i] = (resolve && color_target_infos[i].texture != nullptr && color_target_infos[i].texture->sample_count > 1)
                                      ? color_target_infos[i].resolve_texture
                                      : nullptr;
    }

    if (!device->gl_ok) return pass; // headless: the attachment/resolve shape is recorded; no GL is issued.

    if (device->render_fbo == 0) glGenFramebuffers(1, &device->render_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, device->render_fbo);

    GLenum draw_buffers[2] = { GL_NONE, GL_NONE };
    for (u32 i = 0; i < pass->num_color_targets; i++) {
        _nya_gles_attach(GL_COLOR_ATTACHMENT0 + i, color_target_infos[i].texture, color_target_infos[i].mip_level);
        draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    // Detach a second slot a previous pass left bound, so a single-target pass does not inherit it.
    if (pass->num_color_targets < 2) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    glDrawBuffers((GLsizei)pass->num_color_targets, draw_buffers);

    // Depth (or depth-stencil), attached at the point the format asked for; the other one is cleared off.
    if (depth_stencil_target_info != nullptr && depth_stencil_target_info->texture != nullptr) {
        SDL_GPUTexture* depth = depth_stencil_target_info->texture;
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        _nya_gles_attach(depth->gl_attachment != 0 ? depth->gl_attachment : GL_DEPTH_ATTACHMENT, depth, depth_stencil_target_info->mip_level);
    } else {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        nya_log_error("gpu_gles: off-screen framebuffer incomplete (0x%x); the render pass will not draw.", (unsigned)status);
    }

    glViewport(0, 0, (GLsizei)pass->target_width, (GLsizei)pass->target_height);
    glDisable(GL_SCISSOR_TEST);

    // Per-attachment clears with glClearBuffer*, the right tool for a multi-attachment FBO, unlike the single glClearColor the swapchain path uses.
    for (u32 i = 0; i < pass->num_color_targets; i++) {
        if (color_target_infos[i].load_op == SDL_GPU_LOADOP_CLEAR) {
            SDL_FColor  c    = color_target_infos[i].clear_color;
            const GLfloat rgba[4] = { c.r, c.g, c.b, c.a };
            glClearBufferfv(GL_COLOR, (GLint)i, rgba);
        }
    }
    if (depth_stencil_target_info != nullptr && depth_stencil_target_info->texture != nullptr
        && depth_stencil_target_info->load_op == SDL_GPU_LOADOP_CLEAR) {
        if (depth_stencil_target_info->texture->has_stencil) {
            glClearBufferfi(GL_DEPTH_STENCIL, 0, depth_stencil_target_info->clear_depth, (GLint)depth_stencil_target_info->clear_stencil);
        } else {
            const GLfloat depth_value = depth_stencil_target_info->clear_depth;
            glClearBufferfv(GL_DEPTH, 0, &depth_value);
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
    // The built-in 2D pipelines declare no fragment uniform blocks; a custom shape shader's block needs its own binding (TODO), so push to a separate range that cannot collide with the vertex block.
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
            // Integer-typed attributes read as normalized floats in the shader (UBYTE4_NORM), which normalized=GL_TRUE does.
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
    _nya_gles_trace("SDL_EndGPURenderPass");

    SDL_GPUDevice* device = render_pass->device;
    if (!device->gl_ok || !render_pass->offscreen) return; // the swapchain draws already executed; nothing to resolve.

    // A RESOLVE store op's multisample renderbuffer is blitted down to its resolve texture via glBlitFramebuffer: read FBO on the MSAA attachment, draw FBO on the resolve texture, GL_NEAREST.
    for (u32 i = 0; i < render_pass->num_color_targets; i++) {
        SDL_GPUTexture* source  = render_pass->color_target[i];
        SDL_GPUTexture* resolve = render_pass->resolve_target[i];
        if (source == nullptr || resolve == nullptr) continue;

        if (device->resolve_fbo == 0) glGenFramebuffers(1, &device->resolve_fbo);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, device->render_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + i);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, device->resolve_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve->id, 0);
        const GLenum draw_one = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw_one);

        GLsizei w = (GLsizei)nya_min(source->width, resolve->width);
        GLsizei h = (GLsizei)nya_min(source->height, resolve->height);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // Leave fbo 0 current, so the swapchain 2D path that follows finds the state it expects.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BLIT & CAPABILITY QUERIES — the texture→texture copy the post/refraction path issues, and the sample-count
 * and format probes the renderer uses to pick an MSAA level and a depth format
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Binds one texture or renderbuffer as GL_COLOR_ATTACHMENT0 of the given framebuffer, or fbo 0 for the swapchain. */
NYA_INTERNAL void _nya_gles_blit_bind(GLenum framebuffer_target, GLuint fbo, SDL_GPUTexture* texture, u32 mip_level) {
    if (texture != nullptr && texture->is_default_target) {
        glBindFramebuffer(framebuffer_target, 0); // the swapchain: blit straight to/from the default framebuffer.
        return;
    }
    glBindFramebuffer(framebuffer_target, fbo);
    if (texture == nullptr) return;
    if (texture->is_renderbuffer) {
        glFramebufferRenderbuffer(framebuffer_target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, texture->rbo);
    } else {
        glFramebufferTexture2D(framebuffer_target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->id, (GLint)mip_level);
    }
}

void SDL_BlitGPUTexture(SDL_GPUCommandBuffer* command_buffer, const SDL_GPUBlitInfo* info) {
    _nya_gles_trace("SDL_BlitGPUTexture");

    SDL_GPUDevice* device = command_buffer->device;
    if (!device->gl_ok) return; // headless: the copy is recorded in the trace; no GL runs.

    if (device->blit_read_fbo == 0) glGenFramebuffers(1, &device->blit_read_fbo);
    if (device->blit_draw_fbo == 0) glGenFramebuffers(1, &device->blit_draw_fbo);

    _nya_gles_blit_bind(GL_READ_FRAMEBUFFER, device->blit_read_fbo, info->source.texture, info->source.mip_level);
    if (info->source.texture == nullptr || !info->source.texture->is_default_target) glReadBuffer(GL_COLOR_ATTACHMENT0);

    _nya_gles_blit_bind(GL_DRAW_FRAMEBUFFER, device->blit_draw_fbo, info->destination.texture, info->destination.mip_level);
    if (info->destination.texture == nullptr || !info->destination.texture->is_default_target) {
        const GLenum draw_one = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw_one);
    }

    if (info->load_op == SDL_GPU_LOADOP_CLEAR) {
        SDL_FColor c = info->clear_color;
        glClearColor(c.r, c.g, c.b, c.a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // The source rectangle into the destination (y-flipped if the flip mode asks); NEAREST unless a smooth downscale is requested, which a resolve never is.
    GLint  sx0 = (GLint)info->source.x, sy0 = (GLint)info->source.y;
    GLint  sx1 = sx0 + (GLint)info->source.w, sy1 = sy0 + (GLint)info->source.h;
    GLint  dx0 = (GLint)info->destination.x, dy0 = (GLint)info->destination.y;
    GLint  dx1 = dx0 + (GLint)info->destination.w, dy1 = dy0 + (GLint)info->destination.h;
    GLenum filter = info->filter == SDL_GPU_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;

    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, filter);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool SDL_GPUTextureSupportsSampleCount(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUSampleCount sample_count) {
    nya_unused(format);
    _nya_gles_trace("SDL_GPUTextureSupportsSampleCount");

    u32 want = _nya_gles_sample_count(sample_count);
    if (want <= 1) return true; // single-sampled is always available.

    if (!device->gl_ok) {
        // Headless (node): report a fixed 4x ceiling so the MSAA pick is deterministic without GL; a browser answers from GL_MAX_SAMPLES below.
        return want <= 4;
    }

    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    return (GLint)want <= max_samples;
}

bool SDL_GPUTextureSupportsFormat(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUTextureType type, SDL_GPUTextureUsageFlags usage) {
    nya_unused(device), nya_unused(type), nya_unused(usage);
    _nya_gles_trace("SDL_GPUTextureSupportsFormat");

    // The shim reports support for exactly the formats _nya_gles_texture_format handles; only 2D targets exist, so 3D/array/cube is unsupported.
    if (type != SDL_GPU_TEXTURETYPE_2D) return false;
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
        case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
        case SDL_GPU_TEXTUREFORMAT_R16_UNORM:
        case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
        case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
        case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
        case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
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
