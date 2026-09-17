/**
 * @file render3d_decal.c
 * */
#include "assets/shader/uniforms.h"

#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_internal.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What a grid is remembered by: everything the drape depends on and nothing it does not. */
typedef struct {
    f32 center[3];
    f32 size[3];
    f32 rotation;
} _NYA_Render3DDecalBox;

/** A draped grid: where each vertex landed and which way the surface faced, row by row. */
typedef struct {
    f32 points[NYA_RENDER3D_DECAL_VERTICES][3];
    f32 normals[NYA_RENDER3D_DECAL_VERTICES][3];

    /** Zero where the probe found nothing or the surface is too steep, which cuts the decal off there. */
    u8 landed[NYA_RENDER3D_DECAL_VERTICES];
} _NYA_Render3DDecalGrid;

/** The staging, grids and ceiling, made the first time they are needed. False when the arena refused. */
NYA_INTERNAL b8 _nya_render3d_decals_ensure(NYA_Render3DDecalsGPU* gpu);

/** The grid for `box`, draped now or remembered from an earlier frame. Null when the cache refused it. */
NYA_INTERNAL const _NYA_Render3DDecalGrid* _nya_render3d_decal_grid(NYA_Render3DDecalsGPU* gpu, NYA_Render3DDecalProbe probe, const _NYA_Render3DDecalBox* box);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render3d_decals_set(NYA_Window* window, NYA_Render3DDecals decals) {
    nya_assert(window != nullptr);

    // clamped rather than asserted, as it comes from a hand edited config.
    decals.lift = nya_max(decals.lift, 0.0F);

    window->render_system.decals = decals;

    if (!decals.enabled) {
        _nya_render3d_decals_release(window);
        return;
    }

    if (nya_asset_status(NYA_RENDER3D_PIPELINE_DECAL) != NYA_ASSET_STATUS_UNLOADED) return;

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
        .handle    = NYA_ASSET_SHADER_MESH3D_DECAL_FRAG,
        // the decal's texture at t0 and the shadow map at t1, as the textured mesh pipeline binds them.
        .as_shader = { .num_samplers = 2, .num_uniform_buffers = 1 },
    }), "while queueing the decal fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
        .handle               = NYA_RENDER3D_PIPELINE_DECAL,
        .as_graphics_pipeline = {
            .window                 = window,
            .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
            .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_DECAL_FRAG,
            .blend                  = NYA_BLEND_ALPHA,
            .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
            // tested against the ground it lies on, never written, so decals stack in the order drawn.
            .depth_test      = true,
            .cull_back_faces = true,
        },
    }), "while queueing the decal pipeline");
}

NYA_Render3DDecals nya_render3d_decals(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.decals;
}

void nya_render3d_decal_probe_set(NYA_Window* window, NYA_CallbackHandle probe, void* user_data) {
    nya_assert(window != nullptr);

    NYA_Render3DDecalsGPU* gpu = &window->render_system.decals_gpu;

    gpu->probe           = probe;
    gpu->probe_user_data = user_data;
    gpu->probe_generation++;
}

void nya_render3d_decal(NYA_Window* window, NYA_Render3DDecal decal) {
    nya_assert(window != nullptr);
    nya_assert(decal.size.x > 0.0F && decal.size.y > 0.0F && decal.size.z > 0.0F, "a decal needs a box with volume");

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DDecalsGPU*  gpu    = &render->decals_gpu;

    if (!render->decals.enabled || !render->mesh_batch.active || decal.texture == nullptr) return;

    NYA_Render3DDecalProbe probe = nya_callback_get(gpu->probe);
    if (probe == nullptr) return;

    if (gpu->frame_count >= NYA_RENDER3D_DECAL_MAX) {
        render->mesh_batch.frame_dropped_draws++;
        return;
    }

    if (!_nya_render3d_decals_ensure(gpu)) return;

    // one texture per draw call: a different one ends the segment the staged decals belong to.
    if (gpu->count > 0 && gpu->texture != decal.texture) nya_render3d_flush(window);

    const _NYA_Render3DDecalBox box = {
        .center   = { decal.center.x, decal.center.y, decal.center.z },
        .size     = { decal.size.x, decal.size.y, decal.size.z },
        .rotation = decal.rotation,
    };

    const _NYA_Render3DDecalGrid* grid = _nya_render3d_decal_grid(gpu, probe, &box);
    if (grid == nullptr) return;

    u32 columns = nya_max(decal.columns, (u8)1);
    u32 rows    = nya_max(decal.rows, (u8)1);

    f32 cell_x = (f32)(decal.cell % columns);
    f32 cell_y = (f32)((decal.cell / columns) % rows);

    f32 lift = render->decals.lift > 0.0F ? render->decals.lift : NYA_RENDER3D_DECAL_LIFT;

    NYA_Vertex3D* out = &gpu->vertices[(u64)gpu->count * NYA_RENDER3D_DECAL_VERTICES];

    for (u32 row = 0; row <= NYA_RENDER3D_DECAL_GRID; row++) {
        for (u32 column = 0; column <= NYA_RENDER3D_DECAL_GRID; column++) {
            u32 i = (row * (NYA_RENDER3D_DECAL_GRID + 1)) + column;

            f32x3 point = { grid->points[i][0], grid->points[i][1], grid->points[i][2] };

            // zero where nothing was found: the shader cuts the decal where the interpolated normal shortens.
            f32x3 normal = (f32x3){ grid->normals[i][0], grid->normals[i][1], grid->normals[i][2] } * (f32)grid->landed[i];

            f32x2 uv = {
                (cell_x + ((f32)column / (f32)NYA_RENDER3D_DECAL_GRID)) / (f32)columns,
                (cell_y + ((f32)row / (f32)NYA_RENDER3D_DECAL_GRID)) / (f32)rows,
            };

            out[i] = nya_vertex3d(point + (normal * lift), decal.color, normal, uv);
        }
    }

    gpu->texture = decal.texture;
    gpu->count++;
    gpu->frame_count++;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_render3d_decals_ensure(NYA_Render3DDecalsGPU* gpu) {
    if (gpu->arena != nullptr) return true;

    // sized for what it holds: a frame of staging and a grid per decal, a little over 200 KiB at the defaults.
    gpu->arena = nya_arena_create(.name = "decals", .region_size = nya_kibyte_to_byte(512));
    if (gpu->arena == nullptr) return false;

    gpu->vertices = nya_arena_alloc(gpu->arena, (u64)NYA_RENDER3D_DECAL_MAX * NYA_RENDER3D_DECAL_VERTICES * sizeof(NYA_Vertex3D));

    gpu->grids = nya_cache_create(
        gpu->arena,
        _NYA_Render3DDecalGrid,
        .name         = "decal_grids",
        .capacity     = NYA_RENDER3D_DECAL_MAX,
        .key_size_max = sizeof(_NYA_Render3DDecalBox),
        // a mark that stays put is hit every frame; a moving shadow leaves grids nothing asks for again.
        .eviction     = NYA_CACHE_EVICTION_LEAST_RECENT,
    );

    static b8 registered = false;

    // the first window's count. every window's shares the same capacity.
    if (!registered) nya_ceiling_register("decals", NYA_RENDER3D_DECAL_MAX, &gpu->frame_count);
    registered = true;

    return gpu->vertices != nullptr && gpu->grids != nullptr;
}

const _NYA_Render3DDecalGrid* _nya_render3d_decal_grid(NYA_Render3DDecalsGPU* gpu, NYA_Render3DDecalProbe probe, const _NYA_Render3DDecalBox* box) {
    _NYA_Render3DDecalGrid* grid = nya_cache_get(gpu->grids, box, sizeof(*box), gpu->probe_generation);
    if (grid != nullptr) return grid;

    void*     slot     = nullptr;
    NYA_Error inserted = nya_cache_insert(gpu->grids, box, sizeof(*box), gpu->probe_generation, &slot);
    if (!inserted.ok) return nullptr;

    grid = slot;

    f32 cosine = cosf(box->rotation);
    f32 sine   = sinf(box->rotation);

    f32x3 across = (f32x3){ cosine, 0.0F, -sine } * box->size[0];
    f32x3 along  = (f32x3){ sine, 0.0F, cosine } * box->size[2];
    f32x3 down   = { 0.0F, -box->size[1], 0.0F };

    f32x3 center = { box->center[0], box->center[1], box->center[2] };

    for (u32 row = 0; row <= NYA_RENDER3D_DECAL_GRID; row++) {
        for (u32 column = 0; column <= NYA_RENDER3D_DECAL_GRID; column++) {
            u32 i = (row * (NYA_RENDER3D_DECAL_GRID + 1)) + column;

            f32 u = ((f32)column / (f32)NYA_RENDER3D_DECAL_GRID) - 0.5F;
            f32 v = ((f32)row / (f32)NYA_RENDER3D_DECAL_GRID) - 0.5F;

            // from the top face, through the whole box.
            f32x3 origin = center + (across * u) + (along * v) - (down * 0.5F);

            f32x3 point  = origin + (down * 0.5F);
            f32x3 normal = { 0.0F, 1.0F, 0.0F };

            b8 hit = probe(origin, down, gpu->probe_user_data, &point, &normal);

            grid->landed[i] = hit && normal.y >= NYA_RENDER3D_DECAL_STEEPEST;

            // a miss stays at the middle of the box, where its cell collapses instead of stretching to the probe's end.
            if (!hit) point = origin + (down * 0.5F);

            grid->points[i][0]  = point.x;
            grid->points[i][1]  = point.y;
            grid->points[i][2]  = point.z;
            grid->normals[i][0] = normal.x;
            grid->normals[i][1] = normal.y;
            grid->normals[i][2] = normal.z;
        }
    }

    return grid;
}

#if NYA_HEADLESS_ENABLED

void _nya_render3d_decals_release(NYA_Window* window) {
    NYA_Render3DDecalsGPU* gpu = &window->render_system.decals_gpu;

    if (gpu->arena != nullptr) nya_arena_destroy(gpu->arena);

    *gpu = (NYA_Render3DDecalsGPU){ .probe = gpu->probe, .probe_user_data = gpu->probe_user_data, .probe_generation = gpu->probe_generation };
}

#else

void _nya_render3d_decals_release(NYA_Window* window) {
    NYA_Render3DDecalsGPU* gpu        = &window->render_system.decals_gpu;
    SDL_GPUDevice*         gpu_device = nya_app_get()->render_system.gpu_device;

    if (gpu->vertex_buffer != nullptr) nya_gpu_buffer_release(gpu_device, gpu->vertex_buffer);
    if (gpu->transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, gpu->transfer_buffer);
    if (gpu->index_buffer != nullptr) nya_gpu_buffer_release(gpu_device, gpu->index_buffer);
    if (gpu->arena != nullptr) nya_arena_destroy(gpu->arena);

    *gpu = (NYA_Render3DDecalsGPU){ .probe = gpu->probe, .probe_user_data = gpu->probe_user_data, .probe_generation = gpu->probe_generation };
}

void _nya_render3d_decals_upload(NYA_Window* window, SDL_GPUCopyPass* copy_pass) {
    NYA_Render3DDecalsGPU* gpu        = &window->render_system.decals_gpu;
    SDL_GPUDevice*         gpu_device = nya_app_get()->render_system.gpu_device;

    if (gpu->count == 0) return;

    u32 vertex_size = (u32)((u64)NYA_RENDER3D_DECAL_MAX * NYA_RENDER3D_DECAL_VERTICES * sizeof(NYA_Vertex3D));
    u32 index_size  = (u32)((u64)NYA_RENDER3D_DECAL_MAX * NYA_RENDER3D_DECAL_INDICES * sizeof(u16));

    if (gpu->vertex_buffer == nullptr) {
        gpu->vertex_buffer   = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vertex_size });
        gpu->transfer_buffer = nya_gpu_transfer_buffer_create(
            gpu_device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = vertex_size }
        );
        gpu->index_buffer = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_size });
    }

    if (gpu->vertex_buffer == nullptr || gpu->transfer_buffer == nullptr || gpu->index_buffer == nullptr) {
        nya_log_error("Could not create the decal buffers: %s", SDL_GetError());
        return;
    }

    if (!gpu->indices_uploaded) {
        SDL_GPUTransferBuffer* staging =
            nya_gpu_transfer_buffer_create(gpu_device, &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = index_size });

        if (staging == nullptr) return;

        u16* indices = SDL_MapGPUTransferBuffer(gpu_device, staging, false);
        nya_assert(indices != nullptr, "SDL_MapGPUTransferBuffer() failed for the decal indices: %s", SDL_GetError());

        const u32 side = NYA_RENDER3D_DECAL_GRID + 1;

        // two triangles a cell, wound counter-clockwise seen from above.
        for (u32 decal = 0; decal < NYA_RENDER3D_DECAL_MAX; decal++) {
            u32 base = decal * NYA_RENDER3D_DECAL_VERTICES;

            for (u32 row = 0; row < NYA_RENDER3D_DECAL_GRID; row++) {
                for (u32 column = 0; column < NYA_RENDER3D_DECAL_GRID; column++) {
                    u16 a = (u16)(base + (row * side) + column);
                    u16 b = (u16)(a + 1);
                    u16 c = (u16)(a + side + 1);
                    u16 d = (u16)(a + side);

                    *indices++ = a;
                    *indices++ = c;
                    *indices++ = b;
                    *indices++ = a;
                    *indices++ = d;
                    *indices++ = c;
                }
            }
        }

        SDL_UnmapGPUTransferBuffer(gpu_device, staging);

        SDL_UploadToGPUBuffer(copy_pass, &(SDL_GPUTransferBufferLocation){ .transfer_buffer = staging },
                              &(SDL_GPUBufferRegion){ .buffer = gpu->index_buffer, .size = index_size }, false);

        window->render_system.frame_stats.uploads++;
        window->render_system.frame_stats.upload_bytes += index_size;

        // released once the copy has run.
        nya_gpu_transfer_buffer_release(gpu_device, staging);

        gpu->indices_uploaded = true;
    }

    u32 vertex_count = gpu->count * NYA_RENDER3D_DECAL_VERTICES;

    NYA_Vertex3D* mapped = SDL_MapGPUTransferBuffer(gpu_device, gpu->transfer_buffer, true);
    nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed for the decals: %s", SDL_GetError());
    nya_memcpy(mapped, gpu->vertices, (u64)vertex_count * sizeof(NYA_Vertex3D));
    SDL_UnmapGPUTransferBuffer(gpu_device, gpu->transfer_buffer);

    u32 upload_size = vertex_count * (u32)sizeof(NYA_Vertex3D);

    SDL_UploadToGPUBuffer(copy_pass, &(SDL_GPUTransferBufferLocation){ .transfer_buffer = gpu->transfer_buffer },
                          &(SDL_GPUBufferRegion){ .buffer = gpu->vertex_buffer, .size = upload_size }, true);

    window->render_system.frame_stats.uploads++;
    window->render_system.frame_stats.upload_bytes += upload_size;
}

void _nya_render3d_decals_draw(NYA_Window* window, const NYA_Render3DSegment* segment, const struct NYA_ShaderMesh3DUniform* uniform) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render3DBatch*      batch  = &render->mesh_batch;
    NYA_Render3DDecalsGPU*  gpu    = &render->decals_gpu;

    if (segment->decal_count == 0 || !gpu->indices_uploaded) return;

    // still loading on the first frames, like a textured mesh.
    NYA_Render3DTextureBinding texture  = nya_render3d_texture_resolve(segment->decal_texture);
    NYA_Asset*                 pipeline = nya_asset_get(NYA_RENDER3D_PIPELINE_DECAL);

    if (texture.texture == nullptr || pipeline == nullptr || pipeline->status != NYA_ASSET_STATUS_LOADED) return;

    SDL_GPUGraphicsPipeline* build = _nya_render_pipeline(window, pipeline);
    if (build == nullptr) return;

    SDL_BindGPUGraphicsPipeline(render->render_pass, build);
    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = gpu->vertex_buffer }, 1);
    SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = gpu->index_buffer }, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    if (!_nya_render3d_bind_samplers(window, texture.texture, texture.sampler)) return;

    SDL_PushGPUVertexUniformData(render->render_commands, 0, &batch->view_projection, sizeof(batch->view_projection));
    SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, sizeof(*uniform));

    // every decal's grid has its own slot of the index pattern, so the segment's run starts at its first decal's.
    SDL_DrawGPUIndexedPrimitives(render->render_pass, segment->decal_count * NYA_RENDER3D_DECAL_INDICES, 1,
                                 segment->first_decal * NYA_RENDER3D_DECAL_INDICES, 0, 0);

    batch->frame_draw_calls++;
    batch->frame_vertices += segment->decal_count * NYA_RENDER3D_DECAL_VERTICES;
}

#endif // NYA_HEADLESS_ENABLED
