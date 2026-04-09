#include "SDL3/SDL_gpu.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLRARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if defined(NYA_ASSET_BACKEND_FS) && defined(NYA_ASSET_BACKEND_BLOB)
#error "Only one asset backend can be defined at a time."
#elif !defined(NYA_ASSET_BACKEND_FS) && !defined(NYA_ASSET_BACKEND_BLOB)
#error "An asset backend must be defined. Define either NYA_ASSET_BACKEND_FS or NYA_ASSET_BACKEND_BLOB."
#endif

#ifdef NYA_ASSET_BACKEND_BLOB
#include "../../../assets/assets.c"

NYA_INTERNAL void _nya_asset_load_raw_from_blob(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
#define _nya_asset_load_raw_from_backend   _nya_asset_load_raw_from_blob
#define _nya_asset_unload_raw_from_backend nya_unused
#endif // NYA_ASSET_BACKEND_BLOB

#ifdef NYA_ASSET_BACKEND_FS
NYA_INTERNAL void _nya_asset_load_raw_from_filesystem(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
NYA_INTERNAL void _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset);
#define _nya_asset_load_raw_from_backend   _nya_asset_load_raw_from_filesystem
#define _nya_asset_unload_raw_from_backend _nya_asset_unload_raw_from_filesystem

#define _NYA_ASSET_RELOAD_GRACE_FRAMES 5
NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time);

void _nya_asset_reload_process(NYA_Event* event);
#endif // NYA_ASSET_BACKEND_FS

void _nya_asset_loading_process(NYA_Event* event);
void _nya_asset_unloading_process(NYA_Event* event);

NYA_INTERNAL NYA_AssetHandle _nya_asset_pick_correct_compiled_shader(NYA_AssetHandle source_shader, OUT SDL_GPUShaderFormat* out_format);

SDL_GPUVertexAttribute vertex_attributes[] = {
  {
   .location    = 0,
   .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
   .offset      = nya_offsetof(NYA_Vertex,           position),
   .buffer_slot = 0,
   },
  {
   .location    = 1,
   .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
   .offset      = nya_offsetof(NYA_Vertex,                                 color),
   .buffer_slot = 0,
   },
  {
   .location    = 2,
   .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
   .offset      = nya_offsetof(NYA_Vertex,                                                normals),
   .buffer_slot = 0,
   },
  {
   .location    = 3,
   .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
   .offset      = nya_offsetof(NYA_Vertex,uv),
   .buffer_slot = 0,
   },
};

SDL_GPUVertexBufferDescription vertex_buffer_description = {
  .slot               = 0,
  .input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX,
  .instance_step_rate = 0,
  .pitch              = sizeof(NYA_Vertex),
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_asset_init(void) {
  NYA_App* app = nya_app_get();

  app->asset_system = (NYA_AssetSystem){
    .allocator = nya_arena_create(.name = "asset_system_allocator"),
  };

  app->asset_system.assets          = nya_dict_create(app->asset_system.allocator, NYA_Asset);
  app->asset_system.loading_queue   = nya_array_create(app->asset_system.allocator, NYA_AssetLoadParameters);
  app->asset_system.unloading_queue = nya_array_create(app->asset_system.allocator, NYA_AssetHandle);

  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_FRAME_ENDED,
      .fn         = nya_callback(_nya_asset_unloading_process),
  });

  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_FRAME_ENDED,
      .fn         = nya_callback(_nya_asset_loading_process),
  });

#ifdef NYA_ASSET_BACKEND_FS
  app->asset_system.reload_queue = nya_array_create(app->asset_system.allocator, NYA_AssetHandle);
  nya_event_hook_register((NYA_EventHook){
      .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
      .event_type = NYA_EVENT_FRAME_ENDED,
      .fn         = nya_callback(_nya_asset_reload_process),
  });
#endif // NYA_ASSET_BACKEND_FS

  nya_info("Asset system initialized.");
}

void nya_system_asset_deinit(void) {
  NYA_App* app = nya_app_get();

  // submit all assets to be unloaded
  nya_dict_foreach_value(app->asset_system.assets, asset) {
    if (asset && asset->status != NYA_ASSET_STATUS_UNLOADED) { /**/
      nya_array_push_back(app->asset_system.unloading_queue, asset->handle);
    }
  }
  _nya_asset_unloading_process(nullptr); // process unloading immediately

  nya_array_destroy(app->asset_system.loading_queue);
  nya_array_destroy(app->asset_system.unloading_queue);
#ifdef NYA_ASSET_BACKEND_FS
  nya_array_destroy(app->asset_system.reload_queue);
#endif // NYA_ASSET_BACKEND_FS

  nya_dict_destroy(app->asset_system.assets);

  nya_arena_destroy(app->asset_system.allocator);

  nya_info("Asset system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * ASSET FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Asset* nya_asset_get(NYA_AssetHandle handle) {
  nya_assert(handle != nullptr);

  NYA_AssetSystem* system = &nya_app_get()->asset_system;
  NYA_Asset*       asset  = nya_dict_get(system->assets, handle);

#ifdef NYA_ASSET_BACKEND_FS
  if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) {
    u64 file_modification_time = 0;
    _nya_asset_get_modification_time(asset, &file_modification_time);

    if (file_modification_time > asset->source_modification_time) { /**/
      if (nya_array_contains(system->reload_queue, asset->handle)) return asset;

      asset->reload_grace_frames = _NYA_ASSET_RELOAD_GRACE_FRAMES;
      nya_array_push_back(system->reload_queue, asset->handle);
      nya_info("Asset marked for reload due to modification: %s", asset->handle);
    }
  }
#endif // NYA_ASSET_BACKEND_FS

  return asset;
}

void nya_asset_acquire(NYA_AssetHandle handle) {
  nya_assert(handle != nullptr);

  NYA_AssetSystem* system = &nya_app_get()->asset_system;

  NYA_Asset* asset = nya_dict_get(system->assets, handle);
  nya_assert(asset != nullptr, "Cannot acquire unloaed asset: %s", handle);

  atomic_fetch_add(&asset->reference_count, 1);
}

void nya_asset_release(NYA_AssetHandle handle) {
  nya_assert(handle != nullptr);

  NYA_AssetSystem* system = &nya_app_get()->asset_system;

  NYA_Asset* asset = nya_dict_get(system->assets, handle);
  nya_assert(asset != nullptr, "Cannot release unloaed asset: %s", handle);

  u64 prev_reference_count = atomic_fetch_sub(&asset->reference_count, 1);

  if (prev_reference_count == 1) nya_asset_unload(asset);
}

void nya_asset_load(NYA_AssetLoadParameters parameters) {
  NYA_AssetSystem* system = &nya_app_get()->asset_system;

  NYA_Asset* asset = nya_dict_get(system->assets, parameters.handle);
  if (asset != nullptr && asset->status != NYA_ASSET_STATUS_UNLOADED) return;

  NYA_Asset new_asset = (NYA_Asset){
    .handle          = parameters.handle,
    .status          = NYA_ASSET_STATUS_LOADING,
    .load_parameters = parameters,
    .reference_count = 0,
  };

  nya_dict_set(system->assets, parameters.handle, new_asset);
  nya_array_push_back(system->loading_queue, parameters);

  nya_info("Queuing asset for loading: %s", parameters.handle);
}

void nya_asset_unload(NYA_Asset* asset) {
  nya_assert(asset != nullptr);
  nya_assert(asset->reference_count == 0, "Cannot unload asset with non-zero reference count: %s", asset->handle);

  NYA_AssetSystem* system = &nya_app_get()->asset_system;

  nya_array_push_back(system->unloading_queue, asset->handle);

  nya_info("Queuing asset for unloading: %s", asset->handle);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#ifdef NYA_ASSET_BACKEND_BLOB
NYA_INTERNAL void _nya_asset_load_raw_from_blob(NYA_AssetHandle path, OUT NYA_Asset* out_asset) {
  nya_assert(path != nullptr);

  for (u64 asset_header_index = 0; asset_header_index < NYA_ASSET_BLOB_HEADER_COUNT; asset_header_index++) {
    NYA_AssetBlobHeader asset_header = NYA_ASSET_BLOB_HEADER[asset_header_index];
    if (!nya_string_equals(asset_header.path, path)) continue;

    out_asset->as_text.data = (u8*)NYA_ASSET_BLOB + asset_header.start;
    out_asset->as_text.size = asset_header.size;

    return;
  }

  nya_panic("Asset not found in blob: %s", path);
  nya_unreachable();
}
#endif // NYA_ASSET_BACKEND_BLOB

#ifdef NYA_ASSET_BACKEND_FS
NYA_INTERNAL void _nya_asset_load_raw_from_filesystem(NYA_CString path, OUT NYA_Asset* out_asset) {
  nya_assert(path != nullptr);

  if (!nya_filesystem_exists(path)) {
    nya_panic("Asset not found in filesystem: %s", path);
    nya_unreachable();
  }

  NYA_App*   app   = nya_app_get();
  NYA_Arena* arena = app->asset_system.allocator;

  NYA_String* content = nya_string_create(arena);
  NYA_EXPECT(nya_file_read(path, content));
  nya_string_shrink_to_fit(content);
  u8* data = content->items;
  u64 size = content->length;
  nya_arena_free(content->arena, content, sizeof(NYA_String));

  u64 modification_time = 0;
  NYA_EXPECT(nya_filesystem_last_modified(path, &modification_time));

  out_asset->as_text.data             = data;
  out_asset->as_text.size             = size;
  out_asset->source_modification_time = modification_time;
}

NYA_INTERNAL void _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset) {
  nya_assert(asset != nullptr);

  NYA_App*   app   = nya_app_get();
  NYA_Arena* arena = app->asset_system.allocator;

  nya_arena_free(arena, asset->as_text.data, asset->as_text.size);

  asset->as_text.data = nullptr;
  asset->as_text.size = 0;
}

NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time) {
  NYA_Result result;

  switch (asset->type) {
    case NYA_ASSET_TYPE_TEXT: {
      result = nya_filesystem_last_modified(asset->handle, out_modification_time);
    } break;

    case NYA_ASSET_TYPE_SHADER_VERTEX:
    case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
      result = nya_filesystem_last_modified(asset->as_shader.compiled_handle, out_modification_time);
    } break;

    case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
      u64 vertex_shader_modification_time   = 0;
      u64 fragment_shader_modification_time = 0;

      result = nya_filesystem_last_modified(asset->load_parameters.as_graphics_pipeline.vertex_shader_handle, &vertex_shader_modification_time);
      if (result.error == NYA_ERROR_NONE) {
        result = nya_filesystem_last_modified(asset->load_parameters.as_graphics_pipeline.fragment_shader_handle, &fragment_shader_modification_time);
      }

      if (result.error == NYA_ERROR_NONE) *out_modification_time = nya_max(vertex_shader_modification_time, fragment_shader_modification_time);
    } break;

    default: {
      *out_modification_time = 0;
      return true;
    } break;
  }

  if (result.error != NYA_ERROR_NONE) nya_warn("Loaded asset missing from filesystem: %s", asset->handle);
  return result.error == NYA_ERROR_NONE;
}
#endif // NYA_ASSET_BACKEND_FS

void _nya_asset_loading_process(NYA_Event* event) {
  nya_unused(event);

  NYA_AssetSystem*  system        = &nya_app_get()->asset_system;
  NYA_RenderSystem* render_system = &nya_app_get()->render_system;

  nya_array_foreach (system->loading_queue, parameters) {
    nya_info("Loading asset: %s", parameters->handle);

    NYA_Asset* asset = nya_dict_get(system->assets, parameters->handle);
    nya_assert(asset != nullptr);

    switch (parameters->type) {
      case NYA_ASSET_TYPE_TEXT: {
        _nya_asset_load_raw_from_backend(parameters->handle, asset);
        asset->type   = NYA_ASSET_TYPE_TEXT;
        asset->status = NYA_ASSET_STATUS_LOADED;
      } break;

      case NYA_ASSET_TYPE_SHADER_VERTEX:
      case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
        SDL_GPUShaderFormat format;
        NYA_AssetHandle     compiled_shader_handle = _nya_asset_pick_correct_compiled_shader(parameters->handle, &format);
        _nya_asset_load_raw_from_backend(compiled_shader_handle, asset);

        SDL_GPUShaderStage stage;
        if (parameters->type == NYA_ASSET_TYPE_SHADER_VERTEX) {
          stage = SDL_GPU_SHADERSTAGE_VERTEX;
        } else {
          stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        }

        SDL_GPUShader* shader = SDL_CreateGPUShader(
            render_system->gpu_device,
            &(SDL_GPUShaderCreateInfo){
                .code_size            = asset->as_text.size,
                .code                 = asset->as_text.data,
                .entrypoint           = "main",
                .format               = format,
                .stage                = stage,
                .num_samplers         = parameters->as_shader.num_samplers,
                .num_storage_textures = parameters->as_shader.num_storage_textures,
                .num_storage_buffers  = parameters->as_shader.num_storage_buffers,
                .num_uniform_buffers  = parameters->as_shader.num_uniform_buffers,
            }
        );
        nya_assert(shader != nullptr, "Failed to compile shader: %s", parameters->handle);
        _nya_asset_unload_raw_from_backend(asset); // unload raw text code

        asset->type                      = NYA_ASSET_TYPE_SHADER_VERTEX;
        asset->status                    = NYA_ASSET_STATUS_LOADED;
        asset->as_shader.compiled_handle = compiled_shader_handle;
        asset->as_shader.format          = format;
        asset->as_shader.shader          = shader;
      } break;

      case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
        NYA_Asset* vertex_shader_asset   = nya_asset_get(parameters->as_graphics_pipeline.vertex_shader_handle);
        NYA_Asset* fragment_shader_asset = nya_asset_get(parameters->as_graphics_pipeline.fragment_shader_handle);
        nya_assert(vertex_shader_asset != nullptr);
        nya_assert(fragment_shader_asset != nullptr);

        SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
          .target_info = {
            .num_color_targets = 1,
            .color_target_descriptions = (SDL_GPUColorTargetDescription[]){
              { .format = SDL_GetGPUSwapchainTextureFormat(render_system->gpu_device, parameters->as_graphics_pipeline.window->sdl_window)},
            },
          },
          .primitive_type                                = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
          .vertex_shader                                 = vertex_shader_asset->as_shader.shader,
          .fragment_shader                               = fragment_shader_asset->as_shader.shader,
          .rasterizer_state.fill_mode                    = SDL_GPU_FILLMODE_FILL,
          .rasterizer_state.cull_mode                    = SDL_GPU_CULLMODE_NONE,
          .vertex_input_state.num_vertex_buffers         = 1,
          .vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_description,
          .vertex_input_state.num_vertex_attributes      = 2,
          .vertex_input_state.vertex_attributes          = vertex_attributes,
        };
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(render_system->gpu_device, &pipelineCreateInfo);
        nya_assert(pipeline != nullptr);

        asset->type                          = NYA_ASSET_TYPE_GRAPHICS_PIPELINE;
        asset->status                        = NYA_ASSET_STATUS_LOADED;
        asset->as_graphics_pipeline.pipeline = pipeline;
      } break;

      default: {
        nya_panic("Unsupported asset type for loading: %d", parameters->type);
      } break;
    }
  }

  nya_array_clear(system->loading_queue);
}

void _nya_asset_unloading_process(NYA_Event* event) {
  nya_unused(event);

  NYA_AssetSystem*  system        = &nya_app_get()->asset_system;
  NYA_RenderSystem* render_system = &nya_app_get()->render_system;

  nya_array_foreach (system->unloading_queue, handle) {
    nya_info("Unloading asset: %s", *handle);

    NYA_Asset* asset = nya_dict_get(system->assets, *handle);
    nya_assert(asset != nullptr);

    switch (asset->type) {
      case NYA_ASSET_TYPE_TEXT: {
        _nya_asset_unload_raw_from_backend(asset);
        asset->status = NYA_ASSET_STATUS_UNLOADED;
      } break;

      case NYA_ASSET_TYPE_SHADER_VERTEX:
      case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
        SDL_ReleaseGPUShader(render_system->gpu_device, asset->as_shader.shader);
        nya_arena_free(system->allocator, (void*)asset->as_shader.compiled_handle, strlen(asset->as_shader.compiled_handle));
        asset->status = NYA_ASSET_STATUS_UNLOADED;
      } break;

      case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
        SDL_ReleaseGPUGraphicsPipeline(render_system->gpu_device, asset->as_graphics_pipeline.pipeline);
        asset->status = NYA_ASSET_STATUS_UNLOADED;
      } break;

      default: {
        nya_panic("Unsupported asset type for unloading: %d", asset->type);
      } break;
    }
  }

  nya_array_clear(system->unloading_queue);
}

#ifdef NYA_ASSET_BACKEND_FS
void _nya_asset_reload_process(NYA_Event* event) {
  nya_unused(event);
  NYA_AssetSystem* system = &nya_app_get()->asset_system;

  NYA_AssetHandleArray postponed = nya_array_create_on_stack(system->allocator, NYA_AssetHandle);

  nya_array_foreach (system->reload_queue, handle) {
    NYA_Asset* asset = nya_dict_get(system->assets, *handle);
    nya_assert(asset != nullptr);

    u64 file_modification_time = 0;
    _nya_asset_get_modification_time(asset, &file_modification_time);

    if (asset->reload_grace_frames > 0) {
      nya_info("Postponing reload of asset (grace period): %s", *handle);
      asset->reload_grace_frames--;
      nya_array_push_back(&postponed, *handle);
      continue;
    }

    if (file_modification_time != asset->source_modification_time) {
      nya_info("Postponing reload of asset (still written to): %s", *handle);
      asset->source_modification_time = file_modification_time;
      nya_array_push_back(&postponed, *handle);
    } else {
      nya_info("Reloading asset: %s", *handle);
      nya_array_push_back(system->unloading_queue, *handle);
      nya_array_push_back(system->loading_queue, asset->load_parameters);

      if (asset->type == NYA_ASSET_TYPE_SHADER_VERTEX || asset->type == NYA_ASSET_TYPE_SHADER_FRAGMENT) {
        nya_dict_foreach_value(system->assets, other_asset) {
          if (other_asset->type != NYA_ASSET_TYPE_GRAPHICS_PIPELINE) continue;

          if (other_asset->load_parameters.as_graphics_pipeline.vertex_shader_handle == asset->handle ||
              other_asset->load_parameters.as_graphics_pipeline.fragment_shader_handle == asset->handle) {
            if (!nya_array_contains(system->reload_queue, other_asset->handle)) {
              nya_array_push_back(&postponed, other_asset->handle);
              nya_info("Also marking graphics pipeline for reload due to shader modification: %s", other_asset->handle);
            }
          }
        }
      }
    }
  }

  nya_array_clear(system->reload_queue);

  nya_array_foreach (&postponed, handle) nya_array_push_back(system->reload_queue, *handle);
  nya_array_destroy_on_stack(&postponed);
}
#endif // NYA_ASSET_BACKEND_FS

NYA_INTERNAL NYA_AssetHandle _nya_asset_pick_correct_compiled_shader(NYA_AssetHandle source_shader, OUT SDL_GPUShaderFormat* out_format) {
  nya_assert(nya_string_contains(source_shader, "/shader/source/"));

  NYA_AssetSystem* system = &nya_app_get()->asset_system;

  NYA_String* compiled_shader_path = nya_string_from(system->allocator, source_shader);
  nya_string_replace(compiled_shader_path, "/shader/source/", "/shader/compiled/");
  nya_string_strip_suffix(compiled_shader_path, ".hlsl");

  switch (NYA_OS_CURRENT) {
    case NYA_OS_WINDOWS: {
      nya_string_extend(compiled_shader_path, ".dxil");
      *out_format = SDL_GPU_SHADERFORMAT_DXIL;
    } break;

    case NYA_OS_LINUX: {
      nya_string_extend(compiled_shader_path, ".spv");
      *out_format = SDL_GPU_SHADERFORMAT_SPIRV;
    } break;

    case NYA_OS_MAC: {
      nya_string_extend(compiled_shader_path, ".msl");
      *out_format = SDL_GPU_SHADERFORMAT_MSL;
    } break;

    default: {
      nya_panic("Unsupported OS for picking compiled shader: %d", NYA_OS_CURRENT);
    } break;
  }

  NYA_CString handle = nya_arena_alloc(system->allocator, compiled_shader_path->length + 1);
  nya_memcpy(handle, compiled_shader_path->items, compiled_shader_path->length);
  handle[compiled_shader_path->length] = '\0';

  nya_string_destroy(compiled_shader_path);

  return handle;
}
