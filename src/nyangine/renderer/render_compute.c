/**
 * @file render_compute.c
 * */
#include "nyangine/nyangine.h"

#if !OS_WASM

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The compiled artifact for a `.comp` source handle in the format `device` accepts, and that format out.
 * The compiled shaders are named `<stem>.comp.<ext>` beside the sources' `<stem>.comp.hlsl`, one extension
 * per backend; the loader picks by what the device takes, the same rule the graphics shaders follow. The
 * returned string lives in `arena`.
 * */
NYA_INTERNAL NYA_CString _nya_gpu_compute_compiled_path(NYA_Arena* arena, SDL_GPUDevice* device, NYA_ConstCString source, OUT SDL_GPUShaderFormat* out_format) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_gpu_compute_supported(SDL_GPUDevice* device) {
    nya_assert(device != nullptr);

    // The one capability every effect here needs: a texture it can write from a compute shader. The
    // offscreen and software backends do not all have it, and this is a cheap, honest probe for it.
    return SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
}

SDL_GPUComputePipeline* nya_gpu_compute_pipeline_create(SDL_GPUDevice* device, NYA_GPUComputePipelineDesc desc) {
    nya_assert(device != nullptr);
    nya_assert(desc.shader != nullptr);

    if (!nya_gpu_compute_supported(device)) {
        nya_log_warn("Compute is unavailable on this GPU device; '%s' will not load.", desc.shader);
        return nullptr;
    }

    // A short-lived arena for the compiled bytes, which SDL copies into the pipeline, so it is freed here.
    NYA_Arena* arena = nya_arena_create(.name = "compute_pipeline_load");

    SDL_GPUShaderFormat format;
    NYA_CString         compiled = _nya_gpu_compute_compiled_path(arena, device, desc.shader, &format);

    u8*       code      = nullptr;
    u64       code_size = 0;
    NYA_Error read      = nya_asset_read(arena, compiled, &code, &code_size);
    if (!read.ok) {
        nya_log_error("Could not read compiled compute shader '%s': %s", compiled, read.message);
        nya_arena_destroy(arena);
        return nullptr;
    }

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(
        device,
        &(SDL_GPUComputePipelineCreateInfo){
            .code_size                     = code_size,
            .code                          = code,
            .entrypoint                    = "main",
            .format                        = format,
            .num_samplers                  = desc.num_samplers,
            .num_readonly_storage_textures = desc.num_readonly_storage_textures,
            .num_readonly_storage_buffers  = desc.num_readonly_storage_buffers,
            .num_readwrite_storage_textures = desc.num_readwrite_storage_textures,
            .num_readwrite_storage_buffers = desc.num_readwrite_storage_buffers,
            .num_uniform_buffers           = desc.num_uniform_buffers,
            .threadcount_x                 = nya_max(desc.threadcount_x, 1U),
            .threadcount_y                 = nya_max(desc.threadcount_y, 1U),
            .threadcount_z                 = nya_max(desc.threadcount_z, 1U),
        }
    );

    nya_arena_destroy(arena);

    if (pipeline == nullptr) {
        nya_log_error("Could not create compute pipeline for '%s': %s", desc.shader, SDL_GetError());
        return nullptr;
    }

    return pipeline;
}

void nya_gpu_compute_pipeline_release(SDL_GPUDevice* device, SDL_GPUComputePipeline* pipeline) {
    if (pipeline == nullptr) return;

    SDL_ReleaseGPUComputePipeline(device, pipeline);
}

void nya_gpu_compute_dispatch(SDL_GPUCommandBuffer* commands, const NYA_GPUComputeDispatch* dispatch) {
    nya_assert(commands != nullptr);
    nya_assert(dispatch != nullptr);
    nya_assert(dispatch->pipeline != nullptr);

    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(
        commands,
        dispatch->readwrite_textures, dispatch->readwrite_texture_count,
        dispatch->readwrite_buffers, dispatch->readwrite_buffer_count
    );
    nya_assert(pass != nullptr, "SDL_BeginGPUComputePass() failed: %s", SDL_GetError());

    SDL_BindGPUComputePipeline(pass, dispatch->pipeline);

    if (dispatch->sampler_count > 0) {
        SDL_BindGPUComputeSamplers(pass, 0, dispatch->samplers, dispatch->sampler_count);
    }
    if (dispatch->readonly_texture_count > 0) {
        SDL_BindGPUComputeStorageTextures(pass, 0, dispatch->readonly_textures, dispatch->readonly_texture_count);
    }
    if (dispatch->readonly_buffer_count > 0) {
        SDL_BindGPUComputeStorageBuffers(pass, 0, dispatch->readonly_buffers, dispatch->readonly_buffer_count);
    }

    // Compute uniforms are pushed onto the command buffer, not the pass, but always inside one.
    if (dispatch->uniform != nullptr && dispatch->uniform_size > 0) {
        SDL_PushGPUComputeUniformData(commands, 0, dispatch->uniform, dispatch->uniform_size);
    }

    SDL_DispatchGPUCompute(pass, nya_max(dispatch->groupcount_x, 1U), nya_max(dispatch->groupcount_y, 1U),
                           nya_max(dispatch->groupcount_z, 1U));

    SDL_EndGPUComputePass(pass);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_CString _nya_gpu_compute_compiled_path(NYA_Arena* arena, SDL_GPUDevice* device, NYA_ConstCString source, OUT SDL_GPUShaderFormat* out_format) {
    nya_assert(nya_string_contains(source, "/shader/source/"), "a compute shader handle names a source path");

    NYA_String* path = nya_string_from(arena, source);
    nya_string_replace(path, "/shader/source/", "/shader/compiled/");
    nya_string_strip_suffix(path, ".hlsl");

    // By what the device accepts, not by OS, exactly as the graphics shader loader picks; see
    // _nya_asset_pick_correct_compiled_shader.
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        nya_string_extend(path, ".spv");
        *out_format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        nya_string_extend(path, ".dxil");
        *out_format = SDL_GPU_SHADERFORMAT_DXIL;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        nya_string_extend(path, ".msl");
        *out_format = SDL_GPU_SHADERFORMAT_MSL;
    } else {
        nya_log_panic("The GPU device accepts none of the compiled shader formats (SPIR-V, DXIL, MSL).");
    }

    return nya_string_to_cstring(arena, path);
}

#endif // !OS_WASM
