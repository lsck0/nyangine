/**
 * @file render2d.c
 * */
#include "assets/shader/uniforms.h"

#include "nyangine-core/nyangine.h"

#include "nyangine-core/renderer/render_internal.h"

#include "genyarated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */


/**
 * Glyph atlases held at once, one per face and point size, evicted least recently used when full.
 *
 * gnyame's busiest frame wants a dozen: the UI's three typography sizes on two faces, the menu's title and body,
 * the HUD's two, the debug overlay's two, and whatever a shrink-to-fit label lands on. Every atlas is one texture
 * sized to the face's largest glyph, about 90 KB at body size, so twenty four costs a couple of megabytes and
 * leaves room for a second face without thrashing. Eviction keeps a miss cheap rather than fatal, so this number
 * is a memory budget, not a correctness bound.
 * */
#ifndef NYA_RENDER2D_FONT_CACHE_MAX
#define NYA_RENDER2D_FONT_CACHE_MAX 24
#endif

/** Longest derived font asset handle: a path, an '@', and a point size. */
#define NYA_RENDER2D_FONT_HANDLE_MAX 256

typedef struct NYA_FontAtlas NYA_FontAtlas;

struct NYA_FontAtlas {
    /** The path the face was loaded from. */
    NYA_ConstCString path;

    /** Point size this atlas was rasterised at. Part of the cache key, with the path. */
    f32 point_size;

    /**
     * The asset handle, the path plus the point size ("./assets/fonts/x.ttf@19"). Owned here because the
     * asset system keeps the pointer it is given.
     * */
    char handle[NYA_RENDER2D_FONT_HANDLE_MAX];

    SDL_GPUTexture* texture;

    /** Baseline to baseline. What to add to y for the next line. */
    f32 line_height;

    /** Top of the line box to the baseline, and baseline to the deepest descender (positive). */
    f32 ascent;
    f32 descent;

    // The lazily baked glyph table, keyed by glyph index (shaping outputs indices), baked on first use since SDL_ttf has no codepoint-to-index map.
    NYA_Glyph glyphs[NYA_RENDER2D_GLYPH_CAPACITY];

    /** The glyph index each slot holds. */
    u32 glyph_indices[NYA_RENDER2D_GLYPH_CAPACITY];

    /** Slot number plus one for each glyph index, zero for "not baked". Masked, so the size is a power of two. */
    u16 lookup[NYA_RENDER2D_GLYPH_LOOKUP];

    /** Slots used. Grows as glyphs are baked. */
    u32 glyph_count;

    /**
     * One byte of coverage per texel, `grid.atlas_width * grid.atlas_height`. Holds glyphs baked by a run until
     * the run uploads them. The shaders read one channel.
     * */
    u8* coverage;

    /**
     * One cell, reused for every glyph. Mapped with cycling, since each glyph's copy in a pass still owns the
     * previous contents until the command buffer runs.
     * */
    SDL_GPUTransferBuffer* transfer_buffer;

    NYA_GlyphGrid grid;

    /** Slots already on the GPU. Glyphs are never evicted, so the ones to upload are the slots from here on. */
    u32 uploaded_count;

    /** Whether running out of slots was logged, so a full atlas warns once instead of every frame. */
    b8 full_warned;

    /** Whether the glyphs are a distance field rather than coverage. */
    b8 sdf;
};

/** Appends one vertex with explicit uv. Callers reserve first, so this never checks for space. */
NYA_INTERNAL void _nya_render2d_vertex(NYA_Render2DBatch* batch, f32 x, f32 y, f32 u, f32 v, NYA_Color color);


/** Appends one triangle, by offsets from the first vertex of the shape being built. */
NYA_INTERNAL void _nya_render2d_triangle_indices(NYA_Render2DBatch* batch, u32 base, u32 a, u32 b, u32 c);

/** Packs a float colour into the four normalized bytes NYA_Vertex2D stores. */
NYA_INTERNAL void _nya_render2d_pack_color(NYA_Color color, OUT u8 out_rgba[4]);

/**
 * Flushes if the pending draw needs another pipeline or texture, then makes room for `count`. False
 * when the batch cannot draw at all, so a shape is queued whole or not at all.
 * */
/**
 * Flushes, recording why. The reason is set here because nya_render2d_flush is public and callers
 * should not have to name one.
 * */
NYA_INTERNAL void _nya_render2d_flush_for(NYA_Window* window, NYA_Render2DFlushReason reason);

/** The body both nya_render2d_textf variants share. */
NYA_INTERNAL void _nya_render2d_textf_va(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, va_list arguments);

NYA_INTERNAL b8 _nya_render2d_prepare(NYA_Window* window, NYA_ConstCString pipeline, SDL_GPUTexture* texture, SDL_GPUSampler* sampler, u32 vertex_count, u32 index_count);

/**
 * Draws the stand-in for a texture that is not there, and warns once for the handle.
 *
 * Returns true when it drew, so a caller can say `if (missing) { placeholder; return; }` in one line.
 * False while the asset is merely still loading, which is the ordinary case for a frame or two after a
 * load is queued and is not something to put magenta on the screen for.
 * */
NYA_INTERNAL b8 _nya_render2d_texture_placeholder(NYA_Window* window, NYA_ConstCString handle, f32 x, f32 y, f32 width, f32 height);

/** Lays a box out, drawing when `window` is non-null and only measuring when it is not. */
NYA_INTERNAL f32x2 _nya_render2d_text_box_layout(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params);

/** Queues one axis aligned textured quad. The shared tail of every rect, texture and glyph draw. */
NYA_INTERNAL void _nya_render2d_quad(NYA_Render2DBatch* batch, f32 x, f32 y, f32 width, f32 height, f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color);

/** Queues a quad from four positioned corners: top left, top right, bottom right, bottom left. */
NYA_INTERNAL void _nya_render2d_quad_corners(NYA_Render2DBatch* batch, const f32x2 corners[4], f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color);

/**
 * The corners of a rectangle centred on `center` and turned by `rotation`, in the order
 * _nya_render2d_quad_corners expects.
 * */
NYA_INTERNAL void _nya_render2d_rect_rotated_corners(f32x2 center, f32x2 size, f32 rotation, OUT f32x2 out_corners[4]);

/** Segments per quarter circle of a rounded corner: about one per two pixels of radius, at least two. */
NYA_INTERNAL u32 _nya_render2d_corner_segments(f32 radius);

/**
 * Appends the perimeter of a rounded rectangle `inset` inside its bounds, clockwise from the top left
 * corner: 4 * (segments + 1) vertices. Corner centres stay put while the inset is under the radius.
 * */
NYA_INTERNAL void _nya_render2d_rounded_perimeter(NYA_Render2DBatch* batch, NYA_Rectf bounds, f32 radius, f32 inset, u32 segments, NYA_Color color);

/** Closes and reopens the render pass around work that needs a copy pass. */
NYA_INTERNAL void _nya_render2d_pass_suspend(NYA_Window* window);

/** Pushes the batch's scissor state onto the current render pass, or clears it. */
NYA_INTERNAL void _nya_render2d_apply_scissor(NYA_Window* window);
NYA_INTERNAL void _nya_render2d_range_close(NYA_Window* window);
NYA_INTERNAL f32_4x4 _nya_render2d_range_projection(const NYA_Render2DDrawRange* range);
NYA_INTERNAL void _nya_render2d_range_apply_scissor(NYA_Window* window, const NYA_Render2DDrawRange* range);
NYA_INTERNAL void _nya_render2d_pass_resume(NYA_Window* window);

/**
 * Reopens the pass with or without the target's normal buffer, whichever the next draw's pipeline is built for.
 * Only a change reopens it, so a run of 3D or of 2D costs nothing. A target without the buffer always gets a pass
 * without it, and a shadow pass is left alone.
 * */
NYA_INTERNAL void _nya_render2d_pass_normals_set(NYA_Window* window, b8 normals);

/** Resolves the render texture's multisampled normal buffer, if a pass wrote it. Called with no pass open. */
NYA_INTERNAL void _nya_render2d_normals_resolve(NYA_Window* window);

/**
 * Builds the glyph atlas for a font asset, or returns the one already built. Null on failure.
 * */
NYA_INTERNAL NYA_FontAtlas* _nya_render2d_font_atlas(NYA_Window* window, NYA_ConstCString font_path, f32 point_size);

/** The glyph for a glyph index, rasterising it into a free cell if needed. */
NYA_INTERNAL const NYA_Glyph* _nya_render2d_glyph(NYA_FontAtlas* atlas, u32 glyph_index);

/**
 * The face an atlas was built from, or null.
 * */
NYA_INTERNAL TTF_Font* _nya_render2d_atlas_font(const NYA_FontAtlas* atlas) __attr_no_discard;

/** Uploads the cells baked since the last upload, one rect each. */
NYA_INTERNAL void _nya_render2d_atlas_upload(NYA_Window* window, NYA_FontAtlas* atlas);

/** The glyph cache's destructor: texture, transfer buffer and CPU coverage. Callers flush first. */
NYA_INTERNAL void _nya_render2d_atlas_destroy(void* value, void* user_data);

/** True the first time an atlas failure is reported for `handle`, so a per-frame failure prints one line. */
NYA_INTERNAL b8 _nya_render2d_atlas_warn_once(NYA_ConstCString handle) __attr_no_discard;


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Glyph atlases, keyed by font handle text and tagged with the font asset's generation, so a reloaded face
 * reads as stale. Created with the first atlas.
 * */
NYA_INTERNAL NYA_Cache* _nya_render2d_font_cache = nullptr;

/**
 * The fullest any atlas has been. Glyphs are never evicted, so this is the busiest atlas now.
 * NYA_RENDER2D_GLYPH_CAPACITY is per atlas, so the ceiling registry needs a single number to watch.
 * */
NYA_INTERNAL u32 _nya_render2d_glyph_count_worst = 0;

/**
 * Font handles an atlas failure was already reported for. Everything here is reached once per text draw, so a
 * failure that persists would otherwise print thousands of lines a minute. Hashes, not text: all this has to do is
 * tell one handle from another. Once full it stops reporting, which is the right trade for a diagnostic.
 * */
#define NYA_RENDER2D_FONT_WARNED_MAX 16
NYA_INTERNAL u64 _nya_render2d_font_warned[NYA_RENDER2D_FONT_WARNED_MAX] = { 0 };


/** The font nya_render2d_text and the measurements use, set by nya_render2d_font_set. */
NYA_INTERNAL NYA_ConstCString _nya_render2d_current_font = nullptr;

/** Point size of the current font; the two are one setting. */
NYA_INTERNAL f32 _nya_render2d_current_font_size = 0.0F;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render2d_shutdown(void) {
    // keyed by font, not window, so nothing per-window frees them.
    if (_nya_render2d_font_cache != nullptr) {
        nya_cache_destroy(_nya_render2d_font_cache);
        _nya_render2d_font_cache = nullptr;
    }

    _nya_render2d_current_font      = nullptr;
    _nya_render2d_glyph_count_worst = 0;
}

/**
 * The projection a range draws through, from its own target and camera. Per range because both change
 * mid-frame: a render texture differs in size from the window, and a world camera is set around the HUD.
 * */
NYA_INTERNAL f32_4x4 _nya_render2d_range_projection(const NYA_Render2DDrawRange* range) {
    f32_4x4 projection = nya_matrix_orthographic(0.0F, (f32)range->target_width, 0.0F, (f32)range->target_height);

    // The camera is a view matrix ahead of the projection; the translation centres it on the target so zoom happens around what's looked at.
    if (range->camera.kind == NYA_CAMERA2D_KIND_NONE) return projection;

    // both camera kinds reduce to these four numbers, so the flush does not care which it is.
    f32 a, b, c, d;
    nya_camera2d_basis(&range->camera, &a, &b, &c, &d);

    f32x2 position = nya_camera2d_position(&range->camera);

    f32 center_x = (f32)range->target_width * 0.5F;
    f32 center_y = (f32)range->target_height * 0.5F;

    f32 px = position[0];
    f32 py = position[1];

    f32_4x4 view = nya_matrix_create(
        (f32x4){ a, b, 0.0F, center_x - ((a * px) + (b * py)) },
        (f32x4){ c, d, 0.0F, center_y - ((c * px) + (d * py)) },
        (f32x4){ 0.0F, 0.0F, 1.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );

    return projection * view;
}

/** The range's own clip rectangle; ranges are replayed out of order. */
NYA_INTERNAL void _nya_render2d_range_apply_scissor(NYA_Window* window, const NYA_Render2DDrawRange* range) {
    NYA_RenderSystemWindow* render = &window->render_system;

    if (render->render_pass == nullptr) return;

    if (!range->scissor_active) {
        // the whole target. SDL has no "disable", so no clipping means clipping to everything.
        SDL_SetGPUScissor(render->render_pass,
                          &(SDL_Rect){ .x = 0, .y = 0, .w = (s32)range->target_width, .h = (s32)range->target_height });
        return;
    }

    SDL_SetGPUScissor(
        render->render_pass,
        &(SDL_Rect){ .x = range->scissor_x, .y = range->scissor_y, .w = range->scissor_width, .h = range->scissor_height }
    );
}

/**
 * Records everything queued since the last range as one draw, and starts the next.
 * */
NYA_INTERNAL void _nya_render2d_range_close(NYA_Window* window) {
    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    if (batch->index_count <= batch->range_first_index) return;
    if (batch->range_count >= NYA_RENDER2D_MAX_RANGES) return;

    NYA_Render2DDrawRange* range = &batch->ranges[batch->range_count];

    *range = (NYA_Render2DDrawRange){
        .layer       = batch->layer,
        .sequence    = batch->range_sequence,
        .first_index = batch->range_first_index,
        .index_count = batch->index_count - batch->range_first_index,

        // resolved now, so replay makes no decisions. shader mode belongs to this range.
        .pipeline = batch->shader_override != nullptr ? batch->shader_override : batch->pipeline,

        .texture        = batch->texture,
        .sampler        = batch->sampler,
        .shader_texture = batch->shader_override != nullptr ? batch->shader_texture : nullptr,

        .target_width  = batch->target_width,
        .target_height = batch->target_height,
        .camera        = batch->camera,

        .scissor_active = batch->scissor_active,
        .scissor_x      = batch->scissor_x,
        .scissor_y      = batch->scissor_y,
        .scissor_width  = batch->scissor_width,
        .scissor_height = batch->scissor_height,
    };

    // copied: the caller's struct is usually a stack local, gone by replay.
    if (batch->shader_uniform_size > 0 && batch->shader_uniform_size <= NYA_RENDER2D_RANGE_UNIFORM_MAX) {
        nya_memcpy(range->uniform, batch->shader_uniform, batch->shader_uniform_size);
        range->uniform_size = batch->shader_uniform_size;
    }

    f32 min_x = F32_MAX;
    f32 min_y = F32_MAX;
    f32 max_x = -F32_MAX;
    f32 max_y = -F32_MAX;

    for (u32 i = batch->range_first_vertex; i < batch->vertex_count; i++) {
        const NYA_Vertex2D* vertex = &batch->vertices[i];

        min_x = nya_min(min_x, vertex->x);
        min_y = nya_min(min_y, vertex->y);
        max_x = nya_max(max_x, vertex->x);
        max_y = nya_max(max_y, vertex->y);
    }

    range->bounds = min_x <= max_x ? (NYA_Rectf){ min_x, min_y, max_x - min_x, max_y - min_y } : (NYA_Rectf){ 0 };

    batch->range_count++;
    batch->range_sequence++;
    batch->range_first_index  = batch->index_count;
    batch->range_first_vertex = batch->vertex_count;
}

void nya_render2d_layer_set(NYA_Window* window, s32 layer) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    if (layer == batch->layer) return;

    // closes the range instead of drawing, so it is issued in layer order later.
    _nya_render2d_range_close(window);

    batch->layer = layer;
}

s32 nya_render2d_layer(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.draw_batch.layer;
}

void nya_render2d_flush(NYA_Window* window) {
    nya_trace_scope_fallback(NYA_TRACE_BATCH2D);

    // timed per call: the run count is the draw call count, and the total is what it costs.
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    // the open range becomes the last one, so the loop below is the only thing that draws.
    _nya_render2d_range_close(window);

    if (batch->range_count == 0) {
        batch->vertex_count       = 0;
        batch->index_count        = 0;
        batch->range_first_index  = 0;
        batch->range_first_vertex = 0;
        return;
    }

    // no pass: the window is occluded or minimised. dropped rather than drawn stale later.
    if (render->render_pass == nullptr) {
        batch->vertex_count       = 0;
        batch->index_count        = 0;
        batch->range_count        = 0;
        batch->range_first_index  = 0;
        batch->range_first_vertex = 0;
        batch->range_sequence     = 0;
        return;
    }

    // merged before the upload, which writes the indices in draw order.
    u32 draw_count = nya_render2d_ranges_merge(batch->ranges, batch->range_count, batch->draws);

    SDL_GPUDevice* gpu_device  = nya_app_get()->render_system.gpu_device;
    u32            upload_size = (u32)(batch->vertex_count * sizeof(NYA_Vertex2D));

    // unmapped after the copy so the driver may move the transfer buffer between frames.
    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, batch->transfer_buffer, true);
    nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed: %s", SDL_GetError());
    nya_memcpy(mapped, batch->vertices, upload_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->transfer_buffer);

    u32   index_upload_size = (u32)(batch->index_count * sizeof(u32));
    void* mapped_indices    = SDL_MapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer, true);
    nya_assert(mapped_indices != nullptr, "SDL_MapGPUTransferBuffer() failed for indices: %s", SDL_GetError());
    nya_render2d_draws_indices_write(batch->ranges, batch->draws, draw_count, batch->indices, mapped_indices);
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer);

    _nya_render2d_pass_suspend(window);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);
    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->vertex_buffer, .offset = 0, .size = upload_size },
        true
    );
    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->index_transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->index_buffer, .offset = 0, .size = index_upload_size },
        true
    );
    render->frame_stats.uploads      += 2;
    render->frame_stats.upload_bytes += (u64)upload_size + index_upload_size;

    SDL_EndGPUCopyPass(copy_pass);

    // reopened without the normal buffer, which no 2D pipeline is built for. see _nya_render2d_pass_normals_set.
    render->render_pass_normals = false;

    _nya_render2d_pass_resume(window);

    // the buffers are bound once, since every draw indexes the same upload.
    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer, .offset = 0 }, 1);
    SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer, .offset = 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    for (u32 i = 0; i < draw_count; i++) {
        const NYA_Render2DDraw*      draw  = &batch->draws[i];
        const NYA_Render2DDrawRange* range = &batch->ranges[draw->first_range];

        NYA_Asset* pipeline_asset = range->pipeline != nullptr ? nya_asset_get(range->pipeline) : nullptr;

        // still loading; skipped so one pipeline does not hold up the frame.
        SDL_GPUGraphicsPipeline* pipeline = nya_asset_graphics_pipeline(pipeline_asset, batch->target_sample_count, false, true);
        if (pipeline == nullptr) continue;

        // per range, since target and camera belong to the range.
        f32_4x4 range_projection = _nya_render2d_range_projection(range);

        _nya_render2d_range_apply_scissor(window, range);

        SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline);
        SDL_PushGPUVertexUniformData(render->render_commands, 0, &range_projection, sizeof(range_projection));

        // Only for a custom shader: the built-in pipelines declare no fragment uniforms, and pushing one is a validation error.
        if (range->uniform_size > 0) {
            SDL_PushGPUFragmentUniformData(render->render_commands, 0, range->uniform, range->uniform_size);
        }

        if (range->texture != nullptr) {
            SDL_BindGPUFragmentSamplers(
                render->render_pass,
                0,
                (SDL_GPUTextureSamplerBinding[]){
                    { .texture = range->texture, .sampler = range->sampler },
                    { .texture = range->shader_texture, .sampler = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR) },
                },
                range->shader_texture != nullptr ? 2 : 1
            );
        }

        SDL_DrawGPUIndexedPrimitives(render->render_pass, draw->index_count, 1, draw->first_index, 0, 0);

        batch->frame_flushes++;
    nya_trace_draws(1);
    }

    batch->frame_vertices += batch->vertex_count;
    batch->frame_indices  += batch->index_count;

    // consumed here so an unattributed flush (the frame end) still lands somewhere.
    batch->frame_flush_reasons[batch->pending_flush_reason % NYA_RENDER2D_FLUSH_REASON_COUNT]++;
    batch->pending_flush_reason = NYA_RENDER2D_FLUSH_FRAME_END;

    batch->vertex_count       = 0;
    batch->index_count        = 0;
    batch->range_count        = 0;
    batch->range_first_index  = 0;
    batch->range_first_vertex = 0;
    batch->range_sequence     = 0;
}

void _nya_render2d_textf_va(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, va_list arguments) {
    char text[NYA_RENDER2D_TEXT_MAX];

    // truncated: vsnprintf always terminates, and a HUD line this long is a bug worth seeing.
    (void)vsnprintf(text, sizeof(text), format, arguments);

    nya_render2d_text_with_font(window, font_path, point_size, text, x, y, color);
}

void _nya_render2d_flush_for(NYA_Window* window, NYA_Render2DFlushReason reason) {
    window->render_system.draw_batch.pending_flush_reason = reason;
    nya_render2d_flush(window);
}

/*
 * ─────────────────────────────────────────────────────────
 * SHAPES
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_rect(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, NYA_Color color) {
    nya_assert(window != nullptr);

    // two triangles, not indexed: an index buffer saves two vertices per quad and costs a second buffer.
    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // the shape shader ignores uvs; they exist so textured and untextured draws share a batch.
    _nya_render2d_quad(batch, x, y, width, height, 0.0F, 0.0F, 0.0F, 0.0F, color);
}

void nya_render2d_rect_gradient(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, const NYA_Color corners[4]) {
    nya_assert(window != nullptr && corners != nullptr);

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;
    u32                base  = batch->vertex_count;

    _nya_render2d_vertex(batch, x, y, 0.0F, 0.0F, corners[0]);
    _nya_render2d_vertex(batch, x + width, y, 0.0F, 0.0F, corners[1]);
    _nya_render2d_vertex(batch, x + width, y + height, 0.0F, 0.0F, corners[2]);
    _nya_render2d_vertex(batch, x, y + height, 0.0F, 0.0F, corners[3]);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
    _nya_render2d_triangle_indices(batch, base, 0, 2, 3);
}

void nya_render2d_rect_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) return;

    // clamped so a thick outline fills the rectangle instead of overlapping into a darker patch.
    f32 horizontal = nya_min(thickness, height * 0.5F);
    f32 vertical   = nya_min(thickness, width * 0.5F);

    // the corners are covered exactly once, which matters at any alpha below one.
    nya_render2d_rect(window, x, y, width, horizontal, color);
    nya_render2d_rect(window, x, y + height - horizontal, width, horizontal, color);
    nya_render2d_rect(window, x, y + horizontal, vertical, height - (horizontal * 2.0F), color);
    nya_render2d_rect(window, x + width - vertical, y + horizontal, vertical, height - (horizontal * 2.0F), color);
}

void nya_render2d_rect_rounded(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    if (width <= 0.0F || height <= 0.0F) return;

    radius = nya_clamp(radius, 0.0F, nya_min(width, height) * 0.5F);
    if (radius <= 0.0F) {
        nya_render2d_rect(window, x, y, width, height, color);
        return;
    }

    u32 segments  = _nya_render2d_corner_segments(radius);
    u32 perimeter = 4 * (segments + 1);

    // a fan from the centre, as nya_render2d_circle draws.
    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, perimeter + 1, perimeter * 3)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;
    u32                base  = batch->vertex_count;

    _nya_render2d_vertex(batch, x + (width * 0.5F), y + (height * 0.5F), 0.0F, 0.0F, color);
    _nya_render2d_rounded_perimeter(batch, (NYA_Rectf){ x, y, width, height }, radius, 0.0F, segments, color);

    for (u32 i = 0; i < perimeter; i++) _nya_render2d_triangle_indices(batch, base, 0, 1 + i, 1 + ((i + 1) % perimeter));
}

void nya_render2d_rect_rounded_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 radius, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (width <= 0.0F || height <= 0.0F || thickness <= 0.0F) return;

    radius    = nya_clamp(radius, 0.0F, nya_min(width, height) * 0.5F);
    thickness = nya_min(thickness, nya_min(width, height) * 0.5F);

    u32 segments  = _nya_render2d_corner_segments(radius);
    u32 perimeter = 4 * (segments + 1);

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, perimeter * 2, perimeter * 6)) return;

    NYA_Render2DBatch* batch  = &window->render_system.draw_batch;
    u32                base   = batch->vertex_count;
    NYA_Rectf          bounds = { x, y, width, height };

    // both rings sample the same angles, so vertex i outside pairs with vertex i inside.
    _nya_render2d_rounded_perimeter(batch, bounds, radius, 0.0F, segments, color);
    _nya_render2d_rounded_perimeter(batch, bounds, radius, thickness, segments, color);

    for (u32 i = 0; i < perimeter; i++) {
        u32 next = (i + 1) % perimeter;

        _nya_render2d_triangle_indices(batch, base, i, next, perimeter + i);
        _nya_render2d_triangle_indices(batch, base, next, perimeter + next, perimeter + i);
    }
}

u32 _nya_render2d_corner_segments(f32 radius) {
    return nya_clamp((u32)(radius * 0.5F), 2U, 16U);
}

void _nya_render2d_rounded_perimeter(NYA_Render2DBatch* batch, NYA_Rectf bounds, f32 radius, f32 inset, u32 segments, NYA_Color color) {
    // past the radius the corner is square, so its centre moves inward with the edges.
    f32 reach = nya_max(radius, inset);
    f32 ring  = nya_max(radius - inset, 0.0F);

    f32x2 centers[4] = {
        { bounds.x + reach, bounds.y + reach },
        { bounds.x + bounds.width - reach, bounds.y + reach },
        { bounds.x + bounds.width - reach, bounds.y + bounds.height - reach },
        { bounds.x + reach, bounds.y + bounds.height - reach },
    };

    f32 quarter = (f32)M_PI * 0.5F;

    for (u32 corner = 0; corner < 4; corner++) {
        // y points down, so starting at pi walks the top left corner from its left edge up to its top edge.
        f32 start = (f32)M_PI + (quarter * (f32)corner);

        for (u32 i = 0; i <= segments; i++) {
            f32 angle = start + (quarter * (f32)i / (f32)segments);
            _nya_render2d_vertex(batch, centers[corner][0] + (cosf(angle) * ring), centers[corner][1] + (sinf(angle) * ring), 0.0F, 0.0F, color);
        }
    }
}

void nya_render2d_rect_rotated(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    if (size.x <= 0.0F || size.y <= 0.0F) return;

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    f32x2 corners[4];
    _nya_render2d_rect_rotated_corners(center, size, rotation, corners);

    _nya_render2d_quad_corners(batch, corners, 0.0F, 0.0F, 0.0F, 0.0F, color);
}

void nya_render2d_rect_rotated_outline(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) return;
    if (size.x <= 0.0F || size.y <= 0.0F) return;

    f32x2 corners[4];
    _nya_render2d_rect_rotated_corners(center, size, rotation, corners);

    // four lines over the same corners the fill uses.
    for (u32 i = 0; i < 4; i++) nya_render2d_line(window, corners[i], corners[(i + 1) % 4], thickness, color);
}

void nya_render2d_polyline(NYA_Window* window, const f32x2* points, u32 count, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (points == nullptr || count < 2) return;
    if (thickness <= 0.0F) return;

    for (u32 i = 0; i + 1 < count; i++) nya_render2d_line(window, points[i], points[i + 1], thickness, color);
}

void nya_render2d_line(NYA_Window* window, f32x2 from, f32x2 to, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) return;

    f32x2 along  = to - from;
    f32   length = sqrtf((along[0] * along[0]) + (along[1] * along[1]));

    // a zero-length line has no direction; the normal would divide by zero.
    if (length <= 0.0F) return;

    // perpendicular scaled to half the thickness, so the quad straddles the segment.
    f32x2 normal = (f32x2){ -along[1] / length, along[0] / length } * (thickness * 0.5F);

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    f32x2 corners[4] = { from - normal, from + normal, to + normal, to - normal };

    _nya_render2d_quad_corners(batch, corners, 0.0F, 0.0F, 0.0F, 0.0F, color);
}

void nya_render2d_triangle(NYA_Window* window, f32x2 a, f32x2 b, f32x2 c, NYA_Color color) {
    nya_assert(window != nullptr);

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 3, 3)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, a[0], a[1], 0.0F, 0.0F, color);
    _nya_render2d_vertex(batch, b[0], b[1], 0.0F, 0.0F, color);
    _nya_render2d_vertex(batch, c[0], c[1], 0.0F, 0.0F, color);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
}

void nya_render2d_circle(NYA_Window* window, f32x2 center, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    if (radius <= 0.0F) return;

    // Segments scale with the radius (min 8); the cap grows from 64 up to 512, so large circles stay round without filling the batch.
    u32 ceiling  = (u32)nya_clamp((f32)NYA_RENDER2D_CIRCLE_SEGMENTS * (radius / 64.0F), (f32)NYA_RENDER2D_CIRCLE_SEGMENTS, 512.0F);
    u32 segments = (u32)(radius * 1.5F);
    segments     = nya_clamp(segments, 8U, ceiling);

    // a centre plus one vertex per rim point: 65 vertices for 64 segments instead of 192.
    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, segments + 1, segments * 3)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;
    f32            step  = (2.0F * (f32)M_PI) / (f32)segments;

    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, center[0], center[1], 0.0F, 0.0F, color);

    for (u32 i = 0; i < segments; i++) {
        f32 angle = step * (f32)i;
        _nya_render2d_vertex(batch, center[0] + (cosf(angle) * radius), center[1] + (sinf(angle) * radius), 0.0F, 0.0F, color);
    }

    // still a triangle list, so every shape shares one pipeline. the last segment closes back to the first.
    for (u32 i = 0; i < segments; i++) {
        u32 next = (i + 1) % segments;
        _nya_render2d_triangle_indices(batch, base, 0, 1 + i, 1 + next);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_camera_set(NYA_Window* window, NYA_Camera2DTopDown camera) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // shared with the headless build. see render_camera.c.
    camera = nya_camera2d_top_down_sanitized(camera);

    // Closes the range instead of flushing: camera and clip are per range, so queued geometry keeps its state and stays reorderable by layer.
    _nya_render2d_range_close(window);

    batch->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_TOP_DOWN, .as_top_down = camera };
}

void nya_render2d_camera_isometric_set(NYA_Window* window, NYA_Camera2DIsometric camera) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    camera = nya_camera2d_isometric_sanitized(camera);

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    batch->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_ISOMETRIC, .as_isometric = camera };
}

void nya_render2d_camera_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    batch->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_NONE };
}

NYA_Camera2D nya_render2d_camera_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.draw_batch.camera;
}

NYA_Camera2DTopDown nya_render2d_camera_top_down_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    return nya_camera2d_top_down_or_identity(window->render_system.draw_batch.camera);
}

f32x2 nya_render2d_screen_to_world(NYA_Window* window, f32x2 screen) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    return nya_camera2d_screen_to_world(&batch->camera, screen, batch->target_width, batch->target_height);
}

f32x2 nya_render2d_world_to_screen(NYA_Window* window, f32x2 world) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    return nya_camera2d_world_to_screen(&batch->camera, world, batch->target_width, batch->target_height);
}

/*
 * ─────────────────────────────────────────────────────────
 * TEXTURES
 * ─────────────────────────────────────────────────────────
 */

/** Full extent of the placeholder when the caller asked for a texture's own size and there is none. */
#define NYA_RENDER2D_PLACEHOLDER_SIZE 64.0F

/** How thick the cross over the placeholder is drawn, as a fraction of the shorter side. */
#define NYA_RENDER2D_PLACEHOLDER_BAR 0.16F

b8 _nya_render2d_texture_placeholder(NYA_Window* window, NYA_ConstCString handle, f32 x, f32 y, f32 width, f32 height) {
    nya_assert(window != nullptr);

    if (!nya_asset_is_missing((NYA_CString)handle)) return false;

    nya_asset_missing_report(handle);

    if (width <= 0.0F) width = NYA_RENDER2D_PLACEHOLDER_SIZE;
    if (height <= 0.0F) height = NYA_RENDER2D_PLACEHOLDER_SIZE;

    // Magenta and a black cross, unmistakable for art; drawn with the shape pipeline so it needs no GPU resource, upload or release of its own.
    nya_render2d_rect(window, x, y, width, height, (NYA_Color){ 1.0F, 0.0F, 1.0F, 1.0F });

    const f32 bar = nya_max(1.0F, nya_min(width, height) * NYA_RENDER2D_PLACEHOLDER_BAR);

    nya_render2d_line(window, (f32x2){ x, y }, (f32x2){ x + width, y + height }, bar, NYA_COLOR_BLACK);
    nya_render2d_line(window, (f32x2){ x + width, y }, (f32x2){ x, y + height }, bar, NYA_COLOR_BLACK);

    return true;
}

void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint) {
    // cast because nya_asset_get takes a mutable handle it only reads.
    if (_nya_render2d_texture_placeholder(window, texture_handle, x, y, 0.0F, 0.0F)) return;

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return;

    f32 width  = (f32)asset->as_texture.width;
    f32 height = (f32)asset->as_texture.height;

    nya_render2d_texture_rect(window, texture_handle, 0.0F, 0.0F, width, height, x, y, width, height, tint);
}

void nya_render2d_texture_ex(NYA_Window* window, NYA_ConstCString texture_handle, NYA_Render2DTexture params) {
    nya_assert(window != nullptr);

    if (_nya_render2d_texture_placeholder(window, texture_handle, params.x, params.y, params.width, params.height)) return;

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_texture.texture == nullptr) {
        window->render_system.draw_batch.frame_dropped_draws++;
        return;
    }

    f32 texture_width  = (f32)asset->as_texture.width;
    f32 texture_height = (f32)asset->as_texture.height;
    if (texture_width <= 0.0F || texture_height <= 0.0F) return;

    // zero means unset. see NYA_Render2DTexture.
    f32 source_width  = params.source_width > 0.0F ? params.source_width : texture_width;
    f32 source_height = params.source_height > 0.0F ? params.source_height : texture_height;

    f32 width  = params.width > 0.0F ? params.width : source_width;
    f32 height = params.height > 0.0F ? params.height : source_height;

    NYA_Color tint = params.tint;
    if (tint.r == 0.0F && tint.g == 0.0F && tint.b == 0.0F && tint.a == 0.0F) tint = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, asset->as_texture.texture, _nya_render_sampler_for(asset->as_texture.filter), 4, 6)) return;

    f32 u0 = params.source_x / texture_width;
    f32 v0 = params.source_y / texture_height;
    f32 u1 = (params.source_x + source_width) / texture_width;
    f32 v1 = (params.source_y + source_height) / texture_height;

    // mirroring swaps uvs. see NYA_Render2DTexture.flip_x.
    if (params.flip_x) { f32 swap = u0; u0 = u1; u1 = swap; }
    if (params.flip_y) { f32 swap = v0; v0 = v1; v1 = swap; }

    // Corners relative to the pivot, rotated, then moved to it, so `rotation` turns about `origin`.
    f32 left   = -params.origin[0];
    f32 top    = -params.origin[1];
    f32 right  = left + width;
    f32 bottom = top + height;

    f32x2 corners[4] = {
        { left, top },
        { right, top },
        { right, bottom },
        { left, bottom },
    };

    if (params.rotation != 0.0F) {
        f32 c = cosf(params.rotation);
        f32 s = sinf(params.rotation);

        for (u32 i = 0; i < 4; i++) {
            f32 cx = corners[i][0];
            f32 cy = corners[i][1];

            // clockwise on screen, because y grows down.
            corners[i] = (f32x2){ (cx * c) - (cy * s), (cx * s) + (cy * c) };
        }
    }

    for (u32 i = 0; i < 4; i++) corners[i] += (f32x2){ params.x, params.y };

    _nya_render2d_quad_corners(&window->render_system.draw_batch, corners, u0, v0, u1, v1, tint);
}

void nya_render2d_texture_rect(
    NYA_Window*      window,
    NYA_ConstCString texture_handle,
    f32              source_x,
    f32              source_y,
    f32              source_width,
    f32              source_height,
    f32              destination_x,
    f32              destination_y,
    f32              destination_width,
    f32              destination_height,
    NYA_Color        tint
) {
    nya_assert(window != nullptr);

    if (_nya_render2d_texture_placeholder(window, texture_handle, destination_x, destination_y, destination_width, destination_height)) return;

    // missing or still loading is normal right after a load. cast: nya_asset_get only reads the handle.
    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_texture.texture == nullptr) {
        window->render_system.draw_batch.frame_dropped_draws++;
        return;
    }

    f32 texture_width  = (f32)asset->as_texture.width;
    f32 texture_height = (f32)asset->as_texture.height;
    if (texture_width <= 0.0F || texture_height <= 0.0F) return;

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, asset->as_texture.texture, _nya_render_sampler_for(asset->as_texture.filter), 4, 6)) return;

    f32 u0 = source_x / texture_width;
    f32 v0 = source_y / texture_height;
    f32 u1 = (source_x + source_width) / texture_width;
    f32 v1 = (source_y + source_height) / texture_height;

    _nya_render2d_quad(&window->render_system.draw_batch, destination_x, destination_y, destination_width, destination_height, u0, v0, u1, v1, tint);
}

/*
 * ─────────────────────────────────────────────────────────
 * TEXT
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_font_set(NYA_ConstCString font_path, f32 point_size) {
    if (point_size <= 0.0F) point_size = NYA_RENDER2D_FONT_DEFAULT_SIZE;
    if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size) return;

    _nya_render2d_current_font      = font_path;
    _nya_render2d_current_font_size = point_size;
}

NYA_ConstCString nya_render2d_font_get(void) {
    return _nya_render2d_current_font;
}

f32 nya_render2d_font_size_get(void) {
    return _nya_render2d_current_font_size;
}

void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params) {
    nya_assert(window != nullptr);
    nya_assert(params.fill < NYA_NINE_SLICE_FILL_COUNT);

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);

    // missing or still loading, normal right after a load.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_TEXTURE) return;

    b8  region = params.source_width > 0.0F && params.source_height > 0.0F;
    f32 width  = region ? params.source_width : (f32)asset->as_texture.width;
    f32 height = region ? params.source_height : (f32)asset->as_texture.height;

    // A zero tint means white; nya_render2d_texture_rect passes colours through unchanged, so the substitution must happen here.
    NYA_Color tint = params.tint;
    if (tint.r == 0.0F && tint.g == 0.0F && tint.b == 0.0F && tint.a == 0.0F) tint = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };

    NYA_NineSliceAxis columns;
    NYA_NineSliceAxis rows;
    nya_render2d_nine_slice_axis(params.source_x, width, params.left, params.right, params.x, params.width, params.scale, params.fill, &columns);
    nya_render2d_nine_slice_axis(params.source_y, height, params.top, params.bottom, params.y, params.height, params.scale, params.fill, &rows);

    for (u32 row = 0; row < rows.count; row++) {
        for (u32 column = 0; column < columns.count; column++) {
            // see NYA_NineSlice.hollow.
            if (params.hollow && rows.middle[row] && columns.middle[column]) continue;

            nya_render2d_texture_rect(
                window,
                texture_handle,
                columns.source[column],
                rows.source[row],
                columns.source_size[column],
                rows.source_size[row],
                columns.destination[column],
                rows.destination[row],
                columns.destination_size[column],
                rows.destination_size[row],
                tint
            );
        }
    }
}

void nya_render2d_text(NYA_Window* window, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_render2d_text_with_font(window, _nya_render2d_current_font, _nya_render2d_current_font_size, text, x, y, color);
}

void nya_render2d_textf(NYA_Window* window, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    va_list arguments;
    va_start(arguments, format);
    _nya_render2d_textf_va(window, _nya_render2d_current_font, _nya_render2d_current_font_size, x, y, color, format, arguments);
    va_end(arguments);
}

void nya_render2d_textf_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    va_list arguments;
    va_start(arguments, format);
    _nya_render2d_textf_va(window, font_path, point_size, x, y, color, format, arguments);
    va_end(arguments);
}

/** The scratch a draw or a measure shapes into. */
NYA_INTERNAL NYA_TextRun _nya_render2d_run = { 0 };

/** Bakes every glyph a shaped run needs, then uploads the atlas once. */
NYA_INTERNAL void _nya_render2d_run_bake(NYA_Window* window, NYA_FontAtlas* atlas, const NYA_TextRun* run) {
    for (u32 i = 0; i < run->glyph_count; i++) (void)_nya_render2d_glyph(atlas, run->glyphs[i].glyph_index);

    _nya_render2d_atlas_upload(window, atlas);
}

/**
 * Emits one shaped glyph at `origin` plus its own position. Returns false when the batch is full.
 * */
NYA_INTERNAL b8 _nya_render2d_glyph_emit(NYA_Window* window, NYA_FontAtlas* atlas, const NYA_TextGlyph* shaped, f32 origin_x, f32 origin_y, NYA_Color color) {
    const NYA_Glyph* glyph = _nya_render2d_glyph(atlas, shaped->glyph_index);

    // Not bakeable (full atlas or no such glyph); the shaper already placed the next glyph, so the line keeps its width.
    if (glyph == nullptr) return true;

    // spaces have a position and no picture.
    if (glyph->width <= 0.0F || glyph->height <= 0.0F) return true;

    /* Coverage and distance fields use different pipelines and filters. */
    NYA_ConstCString pipeline = atlas->sdf ? NYA_RENDER2D_PIPELINE_TEXT_SDF : NYA_RENDER2D_PIPELINE_TEXT;
    NYA_TextureFilter filter  = atlas->sdf ? NYA_TEXTURE_FILTER_LINEAR : NYA_TEXTURE_FILTER_NEAREST;

    if (!_nya_render2d_prepare(window, pipeline, atlas->texture, _nya_render_sampler_for(filter), 4, 6)) {
        return false;
    }

    /* The shaper's sub-rectangle, folded into the cell's uv. */
    f32 texel_width  = 1.0F / (f32)atlas->grid.atlas_width;
    f32 texel_height = 1.0F / (f32)atlas->grid.atlas_height;

    f32 u0 = glyph->u0 + ((f32)shaped->source_x * texel_width);
    f32 v0 = glyph->v0 + ((f32)shaped->source_y * texel_height);
    f32 u1 = u0 + ((f32)shaped->width * texel_width);
    f32 v1 = v0 + ((f32)shaped->height * texel_height);

    // Snapped to whole pixels: off-grid nearest sampling drops or doubles columns and shimmers; only the destination is rounded, not the layout.
    _nya_render2d_quad(
        &window->render_system.draw_batch,
        roundf(origin_x + (f32)shaped->x),
        roundf(origin_y + (f32)shaped->y),
        (f32)shaped->width,
        (f32)shaped->height,
        u0,
        v0,
        u1,
        v1,
        color
    );

    return true;
}

void nya_render2d_text_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_assert(window != nullptr);

    if (text == nullptr || text[0] == '\0') return;

    NYA_FontAtlas* atlas = _nya_render2d_font_atlas(window, font_path, point_size);
    if (atlas == nullptr) return;

    /* Shaped once and kept; the rest is placement. The atlas's size has the default applied. */
    if (!nya_text_shape_with_font(font_path, atlas->point_size, text, 0, &_nya_render2d_run)) return;

    _nya_render2d_run_bake(window, atlas, &_nya_render2d_run);

    for (u32 i = 0; i < _nya_render2d_run.glyph_count; i++) {
        if (!_nya_render2d_glyph_emit(window, atlas, &_nya_render2d_run.glyphs[i], x, y, color)) return;
    }
}

f32x2 nya_render2d_text_box(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params) {
    nya_assert(window != nullptr);

    return _nya_render2d_text_box_layout(window, text, params);
}

f32x2 nya_render2d_text_box_measure(NYA_ConstCString text, NYA_Render2DTextBox params) {
    // a null window lays out without drawing, so a measure can never disagree with the draw.
    return _nya_render2d_text_box_layout(nullptr, text, params);
}

f32x2 nya_render2d_text_measure(NYA_ConstCString text) {
    return nya_render2d_text_measure_with_font(_nya_render2d_current_font, _nya_render2d_current_font_size, text);
}

f32x2 nya_render2d_text_measure_with_font(NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text) {
    if (text == nullptr) return f32x2_zero;

    /* The face, not the atlas. */
    return nya_text_measure_with_font(font_path, point_size, text, 0);
}

f32 nya_render2d_text_width(NYA_ConstCString text) {
    return nya_render2d_text_measure(text)[0];
}

f32 nya_render2d_text_height(NYA_ConstCString text) {
    return nya_render2d_text_measure(text)[1];
}

/* The vertical metrics, read from the current face. */
f32 nya_render2d_font_line_height(void) {
    return nya_text_line_height(nya_text_font_for(_nya_render2d_current_font, _nya_render2d_current_font_size));
}

f32 nya_render2d_font_ascent(void) {
    return nya_text_ascent(nya_text_font_for(_nya_render2d_current_font, _nya_render2d_current_font_size));
}

f32 nya_render2d_font_descent(void) {
    return nya_text_descent(nya_text_font_for(_nya_render2d_current_font, _nya_render2d_current_font_size));
}

f32 nya_render2d_font_height(void) {
    TTF_Font* font = nya_text_font_for(_nya_render2d_current_font, _nya_render2d_current_font_size);

    return nya_text_ascent(font) + nya_text_descent(font);
}

/*
 * ─────────────────────────────────────────────────────────
 * SHADERS
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_shader_begin(NYA_Window* window, NYA_ConstCString pipeline_handle) {
    nya_assert(window != nullptr);

    // shapes queued before this keep the pipeline they were queued with.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    window->render_system.draw_batch.shader_override = (NYA_CString)pipeline_handle;
}

void nya_render2d_shader_set_uniform(NYA_Window* window, const void* data, u32 size) {
    nya_assert(window != nullptr);
    nya_assert(data != nullptr || size == 0);
    nya_assert(size <= NYA_RENDER2D_MAX_UNIFORM_BYTES, "%u uniform bytes, past NYA_RENDER2D_MAX_UNIFORM_BYTES (%d)", size, NYA_RENDER2D_MAX_UNIFORM_BYTES);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // a uniform is per draw call, so what is queued goes out under the old value.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    // copied: the caller's struct is usually a compound literal, gone before the flush.
    if (size > 0) nya_memcpy(batch->shader_uniform, data, size);
    batch->shader_uniform_size = size;
}

b8 nya_render2d_shader_set_texture(NYA_Window* window, NYA_ConstCString texture_handle) {
    nya_assert(window != nullptr);
    nya_assert(texture_handle != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    nya_assert(batch->shader_override != nullptr, "nya_render2d_shader_set_texture needs a custom shader; call nya_render2d_shader_begin first");

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);

    SDL_GPUTexture* texture = nullptr;

    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) {
        if (asset->type == NYA_ASSET_TYPE_TEXTURE) texture = asset->as_texture.texture;
        if (asset->type == NYA_ASSET_TYPE_LUT) texture = asset->as_lut.texture;
    }

    if (texture == nullptr) return false;
    if (texture == batch->shader_texture) return true;

    // a binding is per draw call, so what is queued goes out with the old one.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_TEXTURE);

    batch->shader_texture = texture;

    return true;
}

void nya_render2d_shader_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    window->render_system.draw_batch.shader_override    = nullptr;
    // cleared with the shader so the next custom pipeline does not inherit these.
    window->render_system.draw_batch.shader_uniform_size = 0;
    window->render_system.draw_batch.shader_texture      = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * SCISSOR
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_procedural(NYA_Window* window, NYA_ConstCString pipeline_handle, u32 vertex_count, const void* uniform_data, u32 uniform_size) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (pipeline_handle == nullptr || vertex_count == 0) return;

    NYA_Asset* asset = nya_asset_get((NYA_CString)pipeline_handle);

    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) {
        batch->frame_dropped_draws++;
        return;
    }

    // what is queued was queued for another pipeline.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    if (render->render_pass == nullptr || render->render_commands == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    // the sky draws inside the 3D pass, where the normal buffer is attached; a 2D fullscreen effect does not.
    _nya_render2d_pass_normals_set(window, render->mesh_batch.active);

    SDL_GPUGraphicsPipeline* pipeline = _nya_render_pipeline(window, asset);

    if (pipeline == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline);

    if (uniform_data != nullptr && uniform_size > 0) {
        SDL_PushGPUVertexUniformData(render->render_commands, 0, uniform_data, uniform_size);
        SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform_data, uniform_size);
    }

    SDL_DrawGPUPrimitives(render->render_pass, (Uint32)vertex_count, 1, 0, 0);

    batch->frame_flushes++;
    nya_trace_draws(1);
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    // The cached pipeline is cleared: this draw bound another behind the batch's back, so it must not skip rebinding.
    batch->pipeline = nullptr;
    batch->texture  = nullptr;
    batch->sampler  = nullptr;
}

void nya_render2d_fullscreen(
    NYA_Window*            window,
    NYA_ConstCString       pipeline_handle,
    SDL_GPUTexture* const* textures,
    u32                    texture_count,
    const void*            uniform,
    u32                    uniform_size
) {
    nya_assert(window != nullptr);
    nya_assert(textures != nullptr || texture_count == 0);
    nya_assert(uniform != nullptr || uniform_size == 0);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*      batch  = &render->draw_batch;

    // SDL_GPU binds at most eight per stage, and a post pass reads two or three.
    SDL_GPUTextureSamplerBinding bindings[8];
    nya_assert(texture_count <= nya_carray_length(bindings), "%u textures, past the eight a stage can bind", texture_count);

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    if (render->render_pass == nullptr || render->render_commands == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    _nya_render2d_pass_normals_set(window, false);

    SDL_GPUGraphicsPipeline* pipeline = nya_asset_graphics_pipeline(nya_asset_get((NYA_CString)pipeline_handle), batch->target_sample_count, false, true);

    if (pipeline == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    SDL_GPUSampler* sampler = _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR);

    for (u32 i = 0; i < texture_count; i++) {
        nya_assert(textures[i] != nullptr, "input %u of '%s' is null", i, pipeline_handle);

        bindings[i] = (SDL_GPUTextureSamplerBinding){ .texture = textures[i], .sampler = sampler };
    }

    SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline);

    if (texture_count > 0) SDL_BindGPUFragmentSamplers(render->render_pass, 0, bindings, texture_count);
    if (uniform_size > 0) SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform, uniform_size);

    SDL_DrawGPUPrimitives(render->render_pass, 3, 1, 0, 0);

    batch->frame_flushes++;
    nya_trace_draws(1);
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    // this bound a pipeline and samplers behind the batch's back.
    batch->pipeline = nullptr;
    batch->texture  = nullptr;
    batch->sampler  = nullptr;
}

void nya_render2d_scissor_begin(NYA_Window* window, f32 x, f32 y, f32 width, f32 height) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // Closes the range instead of flushing, so queued geometry keeps its unclipped state and stays reorderable by layer.
    _nya_render2d_range_close(window);

    // Clamped to the target: SDL_GPU rejects a scissor outside it, and panels scrolled half off screen are ordinary.
    f32 left   = nya_max(0.0F, x);
    f32 top    = nya_max(0.0F, y);
    f32 right  = nya_min((f32)batch->target_width, x + width);
    f32 bottom = nya_min((f32)batch->target_height, y + height);

    batch->scissor_x      = (s32)left;
    batch->scissor_y      = (s32)top;
    batch->scissor_width  = (s32)nya_max(0.0F, right - left);
    batch->scissor_height = (s32)nya_max(0.0F, bottom - top);
    batch->scissor_active = true;

    _nya_render2d_apply_scissor(window);
}

void nya_render2d_scissor_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // closes the range, as nya_render2d_scissor_begin does.
    _nya_render2d_range_close(window);

    batch->scissor_active = false;

    _nya_render2d_apply_scissor(window);
}

/*
 * ─────────────────────────────────────────────────────────
 * RENDER TEXTURES
 * ─────────────────────────────────────────────────────────
 */

NYA_RenderTexture nya_render_texture_create(NYA_Window* window, u32 width, u32 height) {
    return nya_render_texture_create_with(window, width, height, (NYA_RenderTextureOptions){ 0 });
}

NYA_RenderTexture nya_render_texture_create_with(NYA_Window* window, u32 width, u32 height, NYA_RenderTextureOptions options) {
    nya_assert(window != nullptr);
    nya_assert(width > 0 && height > 0, "a render texture needs a non-zero size");
    nya_assert(options.depth < NYA_RENDER_TEXTURE_DEPTH_COUNT, "NYA_RenderTextureOptions.depth is not one of the enum's values");
    nya_assert(!options.normals || options.depth == NYA_RENDER_TEXTURE_DEPTH_ATTACHED, "a normal buffer records a 3D scene, which needs depth");

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // the window's format unless asked otherwise, so its pipelines can draw here.
    SDL_GPUTextureFormat format = options.format != SDL_GPU_TEXTUREFORMAT_INVALID ? options.format : window->render_system.color_format;

    SDL_GPUTexture* texture = nya_gpu_texture_create(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = format,
            // drawn into as a colour target and drawn with as a sampler.
            .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );
    nya_assert(texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture: %s", SDL_GetError());

    // A multisampled companion at the renderer's sample count, unless asked without; drawing resolves onto the sampled texture when the pass ends.
    SDL_GPUSampleCount sample_count = options.single_sampled ? SDL_GPU_SAMPLECOUNT_1 : nya_app_get()->render_system.sample_count;
    SDL_GPUTexture*    msaa_texture = nullptr;

    if (sample_count != SDL_GPU_SAMPLECOUNT_1) {
        msaa_texture = nya_gpu_texture_create(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = format,
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
                .sample_count         = sample_count,
            }
        );
        nya_assert(msaa_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's MSAA buffer: %s", SDL_GetError());
    }

    // the window's depth format and the colour target's sample count, which are baked into the pipelines.
    SDL_GPUTexture* depth_texture = nullptr;

    if (options.depth == NYA_RENDER_TEXTURE_DEPTH_ATTACHED) {
        depth_texture = nya_gpu_texture_create(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = nya_app_get()->render_system.depth_format,
                .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
                .sample_count         = sample_count,
            }
        );
        nya_assert(depth_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's depth buffer: %s", SDL_GetError());
    }

    SDL_GPUTexture* normal_texture      = nullptr;
    SDL_GPUTexture* normal_msaa_texture = nullptr;

    if (options.normals) {
        normal_texture = nya_gpu_texture_create(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = NYA_RENDER3D_NORMAL_FORMAT,
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
            }
        );
        nya_assert(normal_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's normal buffer: %s", SDL_GetError());

        if (sample_count != SDL_GPU_SAMPLECOUNT_1) {
            normal_msaa_texture = nya_gpu_texture_create(
                gpu_device,
                &(SDL_GPUTextureCreateInfo){
                    .type                 = SDL_GPU_TEXTURETYPE_2D,
                    .format               = NYA_RENDER3D_NORMAL_FORMAT,
                    .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                    .width                = width,
                    .height               = height,
                    .layer_count_or_depth = 1,
                    .num_levels           = 1,
                    .sample_count         = sample_count,
                }
            );
            nya_assert(normal_msaa_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's MSAA normal buffer: %s",
                       SDL_GetError());
        }
    }

    return (NYA_RenderTexture){
        .texture             = texture,
        .msaa_texture        = msaa_texture,
        .depth_texture       = depth_texture,
        .normal_texture      = normal_texture,
        .normal_msaa_texture = normal_msaa_texture,
        .width               = width,
        .height              = height,
        .sample_count        = sample_count,
        .options             = options,
    };
}

void nya_render_texture_destroy(NYA_RenderTexture* render_texture) {
    if (render_texture == nullptr) return;
    if (render_texture->texture == nullptr) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // SDL_ReleaseGPUTexture already waits until the texture is unused; waiting for the GPU here would stall.
    nya_gpu_texture_release(gpu_device, render_texture->texture);
    if (render_texture->msaa_texture != nullptr) nya_gpu_texture_release(gpu_device, render_texture->msaa_texture);
    if (render_texture->depth_texture != nullptr) nya_gpu_texture_release(gpu_device, render_texture->depth_texture);
    if (render_texture->normal_texture != nullptr) nya_gpu_texture_release(gpu_device, render_texture->normal_texture);
    if (render_texture->normal_msaa_texture != nullptr) nya_gpu_texture_release(gpu_device, render_texture->normal_msaa_texture);

    *render_texture = (NYA_RenderTexture){ 0 };
}

b8 nya_render_texture_is_current(const NYA_RenderTexture* render_texture, u32 width, u32 height) {
    nya_assert(render_texture != nullptr);

    if (render_texture->texture == nullptr) return false;
    if (render_texture->width != width || render_texture->height != height) return false;

    return render_texture->options.single_sampled || render_texture->sample_count == nya_app_get()->render_system.sample_count;
}

void nya_render_texture_begin(NYA_Window* window, NYA_RenderTexture* render_texture, NYA_Color clear) {
    nya_assert(window != nullptr);
    nya_assert(render_texture != nullptr);
    nya_assert(render_texture->texture != nullptr, "the render texture was destroyed, or never created");

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    nya_assert(!batch->target_is_texture, "nya_render_texture_begin does not nest; end the current target first");

    if (render->render_pass == nullptr) return;

    // queued geometry belongs to the previous target and its projection.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;

    _nya_render_trace_mark(window);

    render->render_pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture         = render_texture->msaa_texture != nullptr ? render_texture->msaa_texture : render_texture->texture,
            .resolve_texture = render_texture->msaa_texture != nullptr ? render_texture->texture : nullptr,
            .clear_color     = (SDL_FColor){ .r = clear.r, .g = clear.g, .b = clear.b, .a = clear.a },
            // CLEAR: this starts drawing into the target, which still holds last frame.
            .load_op         = SDL_GPU_LOADOP_CLEAR,
            .store_op        = render_texture->msaa_texture != nullptr ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
        },
        1,
        // Depth cleared too, or last frame's depth occludes; null (not a struct) with no depth buffer, since SDL reads the pointer to decide.
        render_texture->depth_texture == nullptr ? nullptr
                                                 : &(SDL_GPUDepthStencilTargetInfo){
                                                       .texture          = render_texture->depth_texture,
                                                       .clear_depth      = 1.0F,
                                                       .load_op          = SDL_GPU_LOADOP_CLEAR,
                                                       .store_op         = SDL_GPU_STOREOP_STORE,
                                                       .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
                                                       .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
                                                   }
    );
    nya_assert(render->render_pass != nullptr, "SDL_BeginGPURenderPass() failed for a render texture: %s", SDL_GetError());

    render->frame_stats.passes++;

    batch->target_texture    = render_texture->texture;
    batch->target_msaa         = render_texture->msaa_texture;
    batch->target_sample_count = render_texture->sample_count;
    batch->target_depth        = render_texture->depth_texture;
    batch->target_width      = render_texture->width;
    batch->target_height     = render_texture->height;
    batch->target_is_texture = true;

    // cleared by the first pass that attaches it rather than here, where a 2D pass into it would pay for it.
    batch->target_normal         = render_texture->normal_texture;
    batch->target_normal_msaa    = render_texture->normal_msaa_texture;
    batch->target_normal_written = false;
    render->render_pass_normals  = false;
}

void nya_render_texture_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (!batch->target_is_texture) return;
    if (render->render_pass == nullptr) return;

    // drawn while the texture is still the target.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;

    _nya_render2d_normals_resolve(window);

    batch->target_normal        = nullptr;
    batch->target_normal_msaa   = nullptr;
    render->render_pass_normals = false;

    batch->target_texture    = render->swapchain_texture;
    batch->target_msaa         = render->msaa_texture;
    batch->target_sample_count = render->msaa_texture != nullptr ? render->msaa_sample_count : SDL_GPU_SAMPLECOUNT_1;
    batch->target_depth        = render->depth_texture;
    batch->target_width      = window->screen_width;
    batch->target_height     = window->screen_height;
    batch->target_is_texture = false;

    // LOAD: what was drawn to the window before the render texture is still wanted.
    _nya_render2d_pass_resume(window);
}

void nya_render2d_render_texture(NYA_Window* window, const NYA_RenderTexture* render_texture, f32 x, f32 y, f32 width, f32 height, NYA_Color tint) {
    nya_assert(window != nullptr);
    nya_assert(render_texture != nullptr);

    if (render_texture->texture == nullptr) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    nya_assert(render_texture->texture != batch->target_texture, "a render texture cannot be drawn while it is the target being drawn into");

    // zero means natural size.
    f32 destination_width  = width > 0.0F ? width : (f32)render_texture->width;
    f32 destination_height = height > 0.0F ? height : (f32)render_texture->height;

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, render_texture->texture, _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR), 4, 6)) return;

    _nya_render2d_quad(batch, x, y, destination_width, destination_height, 0.0F, 0.0F, 1.0F, 1.0F, tint);
}

void nya_render2d_target_size(NYA_Window* window, OUT u32* out_width, OUT u32* out_height) {
    nya_assert(window != nullptr);
    nya_assert(out_width != nullptr && out_height != nullptr);

    const NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    *out_width  = batch->target_width;
    *out_height = batch->target_height;
}

u32 nya_render2d_pending_vertex_count(NYA_Window* window) {
    nya_assert(window != nullptr);
    return window->render_system.draw_batch.vertex_count;
}

NYA_Render2DFrameStats nya_render2d_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    NYA_Render2DFrameStats stats = {
        .draw_calls    = batch->frame_flushes,
        .vertices      = batch->frame_vertices,
        .indices       = batch->frame_indices,
        .dropped_draws = batch->frame_dropped_draws,
    };

    for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) stats.draw_calls_by_reason[i] = batch->frame_flush_reasons[i];

    return stats;
}

NYA_ConstCString nya_render2d_flush_reason_name(NYA_Render2DFlushReason reason) {
    switch (reason) {
        case NYA_RENDER2D_FLUSH_PIPELINE:  return "pipeline";
        case NYA_RENDER2D_FLUSH_TEXTURE:   return "texture";
        case NYA_RENDER2D_FLUSH_SAMPLER:   return "sampler";
        case NYA_RENDER2D_FLUSH_STATE:     return "state";
        case NYA_RENDER2D_FLUSH_FULL:      return "batch full";
        case NYA_RENDER2D_FLUSH_FRAME_END: return "frame end";

        case NYA_RENDER2D_FLUSH_REASON_COUNT:
        default:                       return "unknown";
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32x2 _nya_render2d_text_box_layout(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params) {
    if (text == nullptr || text[0] == '\0') return f32x2_zero;

    NYA_ConstCString font_path  = params.font_path != nullptr ? params.font_path : _nya_render2d_current_font;
    f32              point_size = params.point_size > 0.0F ? params.point_size : _nya_render2d_current_font_size;

    TTF_Font* font = nya_text_font_for(font_path, point_size);
    if (font == nullptr) return f32x2_zero;

    /* The shaper does the wrapping. */
    s32 wrap_width = params.width > 0.0F ? (s32)params.width : 0;

    if (!nya_text_shape_with_font(font_path, point_size, text, wrap_width, &_nya_render2d_run)) return f32x2_zero;

    const NYA_TextRun* run = &_nya_render2d_run;

    // lines are laid out at the face's own spacing, so the caller's scale becomes a per-line offset.
    f32 spacing     = params.line_spacing > 0.0F ? params.line_spacing : 1.0F;
    f32 line_height = nya_text_line_height(font) * spacing;

    u32 lines = run->line_count;
    if (params.max_lines > 0 && lines > params.max_lines) lines = params.max_lines;

    b8 truncated = lines < run->line_count;

    NYA_FontAtlas* atlas = window != nullptr ? _nya_render2d_font_atlas(window, font_path, point_size) : nullptr;

    if (atlas != nullptr) {
        _nya_render2d_run_bake(window, atlas, run);

        // The ellipsis is baked before the upload too, only when truncated, so it never draws blank for a frame.
        if (truncated && params.ellipsis) {
            static NYA_TextRun ellipsis_run;

            if (nya_text_shape_with_font(font_path, point_size, "...", 0, &ellipsis_run)) _nya_render2d_run_bake(window, atlas, &ellipsis_run);
        }
    }

    f32 widest = 0.0F;

    for (u32 line_index = 0; line_index < lines; line_index++) {
        const NYA_TextLine* line = &run->lines[line_index];

        widest = nya_max(widest, (f32)line->width);

        // alignment offsets against the box width. with no width, centre and right align on `x` itself.
        f32 align_x = 0.0F;

        switch (params.align) {
            case NYA_TEXT_ALIGN_CENTER: align_x = (params.width - (f32)line->width) * 0.5F; break;
            case NYA_TEXT_ALIGN_RIGHT: align_x = params.width - (f32)line->width; break;

            case NYA_TEXT_ALIGN_LEFT:
            case NYA_TEXT_ALIGN_COUNT:
            default: break;
        }

        if (atlas == nullptr) continue;

        // glyphs are relative to the run, so the line's own y is replaced by where this layout puts the line.
        f32 origin_x = params.x + align_x;
        f32 origin_y = params.y + ((f32)line_index * line_height) - (f32)line->y;

        for (u32 i = 0; i < line->glyph_count; i++) {
            const NYA_TextGlyph* shaped = &run->glyphs[line->first_glyph + i];

            if (!_nya_render2d_glyph_emit(window, atlas, shaped, origin_x, origin_y, params.color)) {
                return (f32x2){ widest, (f32)lines * line_height };
            }
        }

        // Appended after the last line instead of displacing characters; it can overhang by three dots, which is simpler and rarely visible.
        if (!truncated || !params.ellipsis || line_index + 1 != lines) continue;

        static NYA_TextRun ellipsis_run;
        if (!nya_text_shape_with_font(font_path, point_size, "...", 0, &ellipsis_run)) continue;

        for (u32 i = 0; i < ellipsis_run.glyph_count; i++) {
            if (!_nya_render2d_glyph_emit(window, atlas, &ellipsis_run.glyphs[i], origin_x + (f32)line->width, origin_y, params.color)) {
                break;
            }
        }
    }

    return (f32x2){ widest, (f32)lines * line_height };
}

void _nya_render2d_vertex(NYA_Render2DBatch* batch, f32 x, f32 y, f32 u, f32 v, NYA_Color color) {
    nya_assert(batch->vertex_count < NYA_RENDER2D_MAX_VERTICES, "_nya_render2d_prepare was not called, or lied");

    NYA_Vertex2D* vertex = &batch->vertices[batch->vertex_count];

    // no z: nothing in 2D is depth tested.
    vertex->x = x;
    vertex->y = y;
    vertex->u = u;
    vertex->v = v;
    _nya_render2d_pack_color(color, vertex->color);

    batch->vertex_count++;
}

void _nya_render2d_quad(NYA_Render2DBatch* batch, f32 x, f32 y, f32 width, f32 height, f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color) {
    f32 right  = x + width;
    f32 bottom = y + height;

    // four corners and six indices; the triangles share the diagonal.
    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, x, y, u0, v0, color);
    _nya_render2d_vertex(batch, right, y, u1, v0, color);
    _nya_render2d_vertex(batch, right, bottom, u1, v1, color);
    _nya_render2d_vertex(batch, x, bottom, u0, v1, color);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
    _nya_render2d_triangle_indices(batch, base, 0, 2, 3);
}

void _nya_render2d_quad_corners(NYA_Render2DBatch* batch, const f32x2 corners[4], f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color) {
    // same triangles as the axis-aligned case, so zero rotation rasterizes identically.
    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, corners[0][0], corners[0][1], u0, v0, color);
    _nya_render2d_vertex(batch, corners[1][0], corners[1][1], u1, v0, color);
    _nya_render2d_vertex(batch, corners[2][0], corners[2][1], u1, v1, color);
    _nya_render2d_vertex(batch, corners[3][0], corners[3][1], u0, v1, color);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
    _nya_render2d_triangle_indices(batch, base, 0, 2, 3);
}

void _nya_render2d_rect_rotated_corners(f32x2 center, f32x2 size, f32 rotation, OUT f32x2 out_corners[4]) {
    f32 half_width  = size.x * 0.5F;
    f32 half_height = size.y * 0.5F;

    f32 sine   = sinf(rotation);
    f32 cosine = cosf(rotation);

    // y points down, so a positive angle is clockwise on screen, matching NYA_Render2DTexture.rotation and 2D bodies.
    f32x2 across = { cosine * half_width, sine * half_width };
    f32x2 down   = { -sine * half_height, cosine * half_height };

    out_corners[0] = center - across - down;
    out_corners[1] = center + across - down;
    out_corners[2] = center + across + down;
    out_corners[3] = center - across + down;
}



void _nya_render2d_triangle_indices(NYA_Render2DBatch* batch, u32 base, u32 a, u32 b, u32 c) {
    nya_assert(batch->index_count + 3 <= NYA_RENDER2D_MAX_INDICES, "_nya_render2d_prepare was not called, or lied");

    batch->indices[batch->index_count++] = base + a;
    batch->indices[batch->index_count++] = base + b;
    batch->indices[batch->index_count++] = base + c;
}

void _nya_render2d_pack_color(NYA_Color color, OUT u8 out_rgba[4]) {
    // clamped before scaling: an out-of-range component would wrap when cast to a byte.
    out_rgba[0] = (u8)(nya_clamp(color.r, 0.0F, 1.0F) * 255.0F + 0.5F);
    out_rgba[1] = (u8)(nya_clamp(color.g, 0.0F, 1.0F) * 255.0F + 0.5F);
    out_rgba[2] = (u8)(nya_clamp(color.b, 0.0F, 1.0F) * 255.0F + 0.5F);
    out_rgba[3] = (u8)(nya_clamp(color.a, 0.0F, 1.0F) * 255.0F + 0.5F);
}

b8 _nya_render2d_prepare(NYA_Window* window, NYA_ConstCString pipeline, SDL_GPUTexture* texture, SDL_GPUSampler* sampler, u32 vertex_count, u32 index_count) {
    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // nothing to draw into. counted, because a silent no-op looks like a success.
    if (window->render_system.render_pass == nullptr) {
        batch->frame_dropped_draws++;
        return false;
    }

    if (batch->vertices == nullptr) {
        batch->frame_dropped_draws++;
        return false;
    }

    // One shape bigger than the whole buffer, refused loudly since the fix is raising NYA_RENDER2D_MAX_VERTICES.
    if (vertex_count > NYA_RENDER2D_MAX_VERTICES || index_count > NYA_RENDER2D_MAX_INDICES) {
        nya_log_warn(
            "a single shape needs %u vertices and %u indices, past NYA_RENDER2D_MAX_VERTICES (%d) or NYA_RENDER2D_MAX_INDICES (%d)",
            vertex_count,
            index_count,
            NYA_RENDER2D_MAX_VERTICES,
            NYA_RENDER2D_MAX_INDICES
        );

        batch->frame_dropped_draws++;
        return false;
    }

    // A draw call has one pipeline and one texture; pipeline handles are the NYA_RENDER2D_PIPELINE_* literals compared by pointer, and a custom pipeline flushes via shader_override.
    if (batch->index_count > batch->range_first_index) {
        // Pipeline first: a pipeline change usually brings a texture change, and blaming the texture would suggest a useless atlas.
        if (batch->pipeline != pipeline) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_PIPELINE;
        else if (batch->texture != texture) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_TEXTURE;
        else if (batch->sampler != sampler) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_SAMPLER;

        // a state change closes a range; it is issued later in layer order. see NYA_Render2DDrawRange.
        if (batch->pipeline != pipeline || batch->texture != texture || batch->sampler != sampler) {
            _nya_render2d_range_close(window);
        }
    }

    // Out of room or ranges, so this draws now; geometry on either side loses cross-layer ordering, which is why the bounds are generous.
    if (batch->vertex_count + vertex_count > NYA_RENDER2D_MAX_VERTICES ||
        batch->index_count + index_count > NYA_RENDER2D_MAX_INDICES ||
        batch->range_count + 1 >= NYA_RENDER2D_MAX_RANGES) {
        batch->pending_flush_reason = NYA_RENDER2D_FLUSH_FULL;
        nya_render2d_flush(window);
    }

    batch->pipeline = (NYA_CString)pipeline;
    batch->texture  = texture;
    batch->sampler  = sampler;

    // a flush without a loaded pipeline clears the batch, and there is room again.
    return true;
}

void _nya_render2d_pass_suspend(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;

    if (render->render_pass == nullptr) return;

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;

    _nya_render_trace_mark(window);
}

void _nya_render2d_apply_scissor(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (render->render_pass == nullptr) return;

    if (!batch->scissor_active) {
        // the whole target. SDL has no "disable", so no clipping means clipping to everything.
        SDL_SetGPUScissor(render->render_pass, &(SDL_Rect){ .x = 0, .y = 0, .w = (s32)batch->target_width, .h = (s32)batch->target_height });
        return;
    }

    SDL_SetGPUScissor(
        render->render_pass,
        &(SDL_Rect){ .x = batch->scissor_x, .y = batch->scissor_y, .w = batch->scissor_width, .h = batch->scissor_height }
    );
}

void _nya_render2d_pass_resume(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (render->render_commands == nullptr) return;

    if (batch->target_texture == nullptr) return;

    _nya_render_trace_mark(window);

    // Multisampling resolves once, on the frame's last pass; resolving on every reopen would resolve per draw call, so intermediate passes store without a resolve target.
    b8 resolving = batch->target_msaa != nullptr && (batch->target_is_texture || batch->resolve_pending);

    SDL_GPUColorTargetInfo color_targets[2] = {
        {
            .texture         = batch->target_msaa != nullptr ? batch->target_msaa : batch->target_texture,
            .resolve_texture = resolving ? batch->target_texture : nullptr,
            // LOAD, since this reopens mid-target. RESOLVE_AND_STORE keeps the multisample contents the LOAD needs.
            .load_op  = SDL_GPU_LOADOP_LOAD,
            .store_op = resolving ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
        },
        {
            // stored without resolving: nothing reads it before nya_render_texture_end resolves it once.
            .texture  = batch->target_normal_msaa != nullptr ? batch->target_normal_msaa : batch->target_normal,
            .load_op  = batch->target_normal_written ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
            .store_op = SDL_GPU_STOREOP_STORE,
        },
    };

    b8 normals = render->render_pass_normals && batch->target_normal != nullptr;

    if (normals) batch->target_normal_written = true;

    render->render_pass = SDL_BeginGPURenderPass(
        render->render_commands,
        color_targets,
        normals ? 2 : 1,
        // LOAD for the same reason; null with no depth buffer (matching nya_render_texture_begin), or the pipelines would not match the pass.
        batch->target_depth == nullptr ? nullptr
                                       : &(SDL_GPUDepthStencilTargetInfo){
                                             .texture          = batch->target_depth,
                                             .load_op          = SDL_GPU_LOADOP_LOAD,
                                             .store_op         = SDL_GPU_STOREOP_STORE,
                                             .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
                                             .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
                                         }
    );
    nya_assert(render->render_pass != nullptr, "SDL_BeginGPURenderPass() failed while resuming: %s", SDL_GetError());

    render->frame_stats.passes++;

    // a new pass clips to nothing, so the batch's clip goes back on.
    _nya_render2d_apply_scissor(window);
}

void _nya_render2d_pass_normals_set(NYA_Window* window, b8 normals) {
    NYA_RenderSystemWindow* render = &window->render_system;

    normals = normals && render->draw_batch.target_normal != nullptr;

    if (render->render_pass_normals == normals) return;

    _nya_render2d_pass_suspend(window);

    render->render_pass_normals = normals;

    _nya_render2d_pass_resume(window);
}

void _nya_render2d_normals_resolve(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*      batch  = &render->draw_batch;

    if (!batch->target_normal_written || batch->target_normal_msaa == nullptr || render->render_commands == nullptr) return;

    // an empty pass whose only work is its store op, so the resolve happens once per capture instead of per pass.
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture         = batch->target_normal_msaa,
            .resolve_texture = batch->target_normal,
            .load_op         = SDL_GPU_LOADOP_LOAD,
            .store_op        = SDL_GPU_STOREOP_RESOLVE,
        },
        1,
        nullptr
    );
    nya_assert(pass != nullptr, "SDL_BeginGPURenderPass() failed resolving the normal buffer: %s", SDL_GetError());

    render->frame_stats.passes++;

    SDL_EndGPURenderPass(pass);
}

NYA_FontAtlas* _nya_render2d_font_atlas(NYA_Window* window, NYA_ConstCString font_path, f32 point_size) {
    nya_trace_scope(NYA_TRACE_TEXT);

    if (font_path == nullptr) return nullptr;
    if (point_size <= 0.0F) point_size = NYA_RENDER2D_FONT_DEFAULT_SIZE;

    // The handle is derived from path and size, queued if new; a face bakes in one size, so one .ttf at two sizes is two assets.
    char derived[NYA_TEXT_FONT_HANDLE_MAX];
    nya_text_font_handle(font_path, point_size, derived, sizeof(derived));

    /* Resolved on every call, not answered from the cache, so a reloaded font is noticed. */
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)derived);

    if (asset == nullptr) {
        // queued: every caller copes with no atlas yet by drawing nothing.
        NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
          .type    = NYA_ASSET_TYPE_FONT,
          .handle  = (NYA_AssetHandle)derived,
          .source  = font_path,
          .as_font = { .point_size = point_size },
      }), "while queueing a font");

        return nullptr;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;
    if (asset->as_font.font == nullptr) return nullptr;

    if (_nya_render2d_font_cache == nullptr) {
        _nya_render2d_font_cache = nya_cache_create(
            nya_app_get()->render_system.allocator,
            NYA_FontAtlas,
            .name         = "glyph_atlases",
            .capacity     = NYA_RENDER2D_FONT_CACHE_MAX,
            .key_size_max = NYA_TEXT_FONT_HANDLE_MAX,
            // Least recent, not refuse: refusing left a window walking through many sizes drawing blank text forever; the batch flush below guarantees no queued vertex names the evicted texture.
            .eviction     = NYA_CACHE_EVICTION_LEAST_RECENT,
            .destructor   = _nya_render2d_atlas_destroy,
        );
    }

    u64 derived_length = strlen(derived);

    void*           cached = nullptr;
    NYA_CacheLookup lookup = nya_cache_lookup(_nya_render2d_font_cache, derived, derived_length, asset->generation, &cached);

    if (lookup == NYA_CACHE_LOOKUP_HIT) return cached;

    if (lookup == NYA_CACHE_LOOKUP_STALE) {
        // Reloaded: rebuilt below and the insert destroys the stale atlas; flush first since queued vertices name it, and with no window return the stale atlas (a measurement one frame behind is fine).
        if (window == nullptr) return cached;

        nya_log_debug("font '%s' reloaded; rebuilding its glyph atlas", derived);

        _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);
    }

    // A new key in a full cache evicts an atlas whose texture queued vertices may still name, so flush first; with no window there is no batch to flush, so refuse the measurement and let the next windowed call build it.
    if (lookup == NYA_CACHE_LOOKUP_MISS && nya_cache_count(_nya_render2d_font_cache) == nya_cache_capacity(_nya_render2d_font_cache)) {
        if (window == nullptr) {
            if (_nya_render2d_atlas_warn_once(derived)) {
                nya_log_warn("no free glyph atlas slot for '%s' and no window to flush before evicting one", derived);
            }

            return nullptr;
        }

        _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);
    }

    TTF_Font*      font       = asset->as_font.font;
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    s32 line_skip = TTF_GetFontLineSkip(font);
    s32 ascent    = TTF_GetFontAscent(font);
    // SDL reports the descent as negative; flipped so ascent + descent is the ink height.
    s32 descent   = -TTF_GetFontDescent(font);

    NYA_GlyphGrid grid = _nya_render2d_glyph_grid(font);

    // zeroed, so the space between glyphs blends away.
    u8* coverage = SDL_calloc(1, (size_t)grid.atlas_width * (size_t)grid.atlas_height);
    if (coverage == nullptr) {
        if (_nya_render2d_atlas_warn_once(derived)) nya_log_warn("could not allocate a glyph atlas for '%s': out of memory", derived);
        return nullptr;
    }

    void*     claimed = nullptr;
    NYA_Error claim   = nya_cache_add(_nya_render2d_font_cache, derived, derived_length, asset->generation, &claimed);
    if (!claim.ok) {
        // the capacity was handled above, so this is a handle longer than the cache's key.
        if (_nya_render2d_atlas_warn_once(derived)) nya_log_warn("could not claim a glyph atlas slot for '%s': %s", derived, (NYA_ConstCString)claim.message);
        SDL_free(coverage);
        return nullptr;
    }

    NYA_FontAtlas* slot = claimed;

    *slot = (NYA_FontAtlas){
        .path         = font_path,
        .point_size   = point_size,
        .line_height  = (f32)line_skip,
        .ascent       = (f32)ascent,
        .descent      = (f32)descent,
        .coverage     = coverage,
        .grid         = grid,

        // Empty: glyph indices are only known by shaping (SDL_ttf has no codepoint-to-index map), so every glyph is baked on first use.
        .glyph_count = 0,
    };

    // copied, because the asset system keeps the pointer and `derived` is a local.
    (void)snprintf(slot->handle, sizeof(slot->handle), "%s", derived);

    SDL_GPUTexture* texture = nya_gpu_texture_create(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            // one channel; both text shaders read .r.
            .format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = (u32)grid.atlas_width,
            .height               = (u32)grid.atlas_height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );
    nya_assert(texture != nullptr, "SDL_CreateGPUTexture() failed for a glyph atlas: %s", SDL_GetError());

    slot->transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (u32)(grid.cell_width * grid.cell_height) }
    );
    nya_assert(slot->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for a glyph atlas: %s", SDL_GetError());

    // not uploaded empty: a glyph quad samples inside its own cell, and a cell reaches the GPU whole with its glyph.
    slot->texture = texture;

    // latched now, with the face that fills it.
    slot->sdf = TTF_GetFontSDF(font);

    // Logged because the mode decides the pipeline and is latched here, so a late distance-field request shows up in this line.
    nya_log_info("Built a glyph atlas for '%s' (%dx%d, %d slots, %s, filled on demand).", derived, grid.atlas_width, grid.atlas_height,
             NYA_RENDER2D_GLYPH_CAPACITY, slot->sdf ? "distance field" : "coverage");

    return slot;
}

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_lights_apply(NYA_Window* window, const NYA_Light2D* lights, const f32x2* positions, u32 count, NYA_Color ambient) {
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*      batch  = &render->draw_batch;

    SDL_GPUGraphicsPipeline* pipeline = nya_asset_graphics_pipeline(nya_asset_get(NYA_RENDER2D_PIPELINE_LIGHT), batch->target_sample_count, false, true);
    if (pipeline == nullptr) {
        // still loading. an unlit frame beats an all-dark one.
        batch->frame_dropped_draws++;
        return;
    }

    // the scene has to be in the target before it is multiplied.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    if (render->render_pass == nullptr || render->render_commands == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    struct NYA_ShaderLight2DUniform uniform = {
        .ambient_r     = ambient.r,
        .ambient_g     = ambient.g,
        .ambient_b     = ambient.b,
        .target_width  = (f32)batch->target_width,
        .target_height = (f32)batch->target_height,
    };

    u32 kept = nya_min(count, (u32)NYA_SHADER_LIGHT2D_MAX);

    for (u32 i = 0; i < kept; i++) {
        // World to target pixels through the scene's camera: lights live in world space and the fullscreen shader works in pixels, so the camera stays out of the uniforms.
        f32x2 screen = nya_render2d_world_to_screen(window, positions[i]);

        // the radius crosses the same boundary; zoom scales it.
        f32x2 edge   = nya_render2d_world_to_screen(window, positions[i] + (f32x2){ lights[i].radius, 0.0F });
        f32   radius = nya_vector_length(edge - screen);

        uniform.lights[i][0] = screen.x;
        uniform.lights[i][1] = screen.y;
        uniform.lights[i][2] = radius;
        uniform.lights[i][3] = lights[i].intensity;

        uniform.colors[i][0] = lights[i].color.r;
        uniform.colors[i][1] = lights[i].color.g;
        uniform.colors[i][2] = lights[i].color.b;
        uniform.colors[i][3] = 0.0F;
    }

    uniform.count = (f32)kept;

    // a light map multiplies a 2D scene, and its pipeline has no normals variant.
    _nya_render2d_pass_normals_set(window, false);

    SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline);

    // fragment only: the fullscreen vertex shader declares no uniforms.
    SDL_PushGPUFragmentUniformData(render->render_commands, 0, &uniform, sizeof(uniform));

    // three vertices, one oversized triangle. see procedural.vert.hlsl.
    SDL_DrawGPUPrimitives(render->render_pass, 3, 1, 0, 0);

    batch->frame_flushes++;
    nya_trace_draws(1);
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    // cleared: this bound a pipeline behind the batch's back.
    batch->pipeline = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * GLYPHS
 * ─────────────────────────────────────────────────────────
 */

void _nya_render2d_atlas_destroy(void* value, void* user_data) {
    nya_unused(user_data);

    NYA_FontAtlas* atlas      = value;
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    if (atlas->texture != nullptr) nya_gpu_texture_release(gpu_device, atlas->texture);
    if (atlas->transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, atlas->transfer_buffer);

    // kept for the whole run so glyphs can be baked in later; this is the one place it is freed.
    SDL_free(atlas->coverage);
}

b8 _nya_render2d_atlas_warn_once(NYA_ConstCString handle) {
    nya_assert(handle != nullptr);

    // never zero, which is a free slot.
    u64 hash = nya_hash_fnv1a(handle) | 1U;

    for (u32 i = 0; i < NYA_RENDER2D_FONT_WARNED_MAX; i++) {
        if (_nya_render2d_font_warned[i] == hash) return false;

        if (_nya_render2d_font_warned[i] == 0) {
            _nya_render2d_font_warned[i] = hash;
            return true;
        }
    }

    nya_assert(_nya_render2d_font_warned[0] != 0, "a full warned table has no free slot");

    return false;
}

TTF_Font* _nya_render2d_atlas_font(const NYA_FontAtlas* atlas) {
    if (atlas == nullptr) return nullptr;

    // cast because nya_asset_get only reads the handle.
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)atlas->handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;

    return asset->as_font.font;
}

const NYA_Glyph* _nya_render2d_glyph(NYA_FontAtlas* atlas, u32 glyph_index) {
    u32 bucket = _nya_render2d_glyph_bucket(glyph_index);

    // plus one, so a zeroed table is empty and slot 0 is addressable.
    u16 stored = atlas->lookup[bucket];
    if (stored != 0 && atlas->glyph_indices[stored - 1] == glyph_index) return &atlas->glyphs[stored - 1];

    /* The bucket is only a shortcut, so the scan decides. A collision costs a walk, never a wrong answer. */
    for (u32 i = 0; i < atlas->glyph_count; i++) {
        if (atlas->glyph_indices[i] != glyph_index) continue;

        // the most recent lookup claims the bucket.
        atlas->lookup[bucket] = (u16)(i + 1);

        return &atlas->glyphs[i];
    }

    if (atlas->glyph_count >= NYA_RENDER2D_GLYPH_CAPACITY) {
        // full, not evicting, for the font cache's reason. the glyph draws blank and its advance is kept.
        if (!atlas->full_warned) {
            nya_log_warn("glyph atlas for '%s' is full at %d glyphs; raise NYA_RENDER2D_GLYPH_CAPACITY", atlas->handle, NYA_RENDER2D_GLYPH_CAPACITY);
            atlas->full_warned = true;
        }

        return nullptr;
    }

    TTF_Font* font = _nya_render2d_atlas_font(atlas);
    if (font == nullptr) return nullptr;

    u32 slot = atlas->glyph_count++;

    atlas->glyph_indices[slot] = glyph_index;
    atlas->lookup[bucket]      = (u16)(slot + 1);

    if (atlas->glyph_count > _nya_render2d_glyph_count_worst) _nya_render2d_glyph_count_worst = atlas->glyph_count;

    // registered once. it tracks the worst atlas, because the capacity is per atlas.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("glyphs_per_atlas", NYA_RENDER2D_GLYPH_CAPACITY, &_nya_render2d_glyph_count_worst);
        ceiling_registered = true;
    }

    atlas->glyphs[slot] = _nya_render2d_glyph_rasterize(atlas->coverage, atlas->grid, font, glyph_index, slot);

    return &atlas->glyphs[slot];
}

void _nya_render2d_atlas_upload(NYA_Window* window, NYA_FontAtlas* atlas) {
    nya_trace_scope(NYA_TRACE_TEXT);

    nya_assert(atlas != nullptr);
    nya_assert(atlas->uploaded_count <= atlas->glyph_count);

    if (atlas->uploaded_count == atlas->glyph_count) return;
    if (atlas->texture == nullptr || atlas->coverage == nullptr) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // This runs inside a render pass, but a copy pass cannot open inside one, so the pass is suspended as a vertex flush does.
    b8 borrowed_pass = window != nullptr && window->render_system.render_pass != nullptr;
    if (borrowed_pass) _nya_render2d_pass_suspend(window);

    SDL_GPUCommandBuffer* command_buffer = borrowed_pass ? window->render_system.render_commands : SDL_AcquireGPUCommandBuffer(gpu_device);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    for (u32 slot = atlas->uploaded_count; slot < atlas->glyph_count; slot++) {
        s32 cell_x = 0;
        s32 cell_y = 0;
        _nya_render2d_glyph_cell(atlas->grid, slot, &cell_x, &cell_y);

        // cycled: the previous glyph's copy is only recorded, so it still reads the buffer this would overwrite.
        u8* mapped = SDL_MapGPUTransferBuffer(gpu_device, atlas->transfer_buffer, true);
        nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed for a glyph cell: %s", SDL_GetError());

        _nya_render2d_glyph_cell_read(atlas->coverage, atlas->grid, slot, mapped);

        SDL_UnmapGPUTransferBuffer(gpu_device, atlas->transfer_buffer);

        SDL_UploadToGPUTexture(
            copy_pass,
            &(SDL_GPUTextureTransferInfo){ .transfer_buffer = atlas->transfer_buffer, .offset = 0 },
            &(SDL_GPUTextureRegion){
                .texture = atlas->texture,
                .x       = (u32)cell_x,
                .y       = (u32)cell_y,
                .w       = (u32)atlas->grid.cell_width,
                .h       = (u32)atlas->grid.cell_height,
                .d       = 1,
            },
            false
        );
    }

    SDL_EndGPUCopyPass(copy_pass);

    // a coverage byte per texel, counted on the window whose frame it was.
    if (window != nullptr) {
        u32 cells = atlas->glyph_count - atlas->uploaded_count;

        window->render_system.frame_stats.uploads      += cells;
        window->render_system.frame_stats.upload_bytes += (u64)cells * (u32)atlas->grid.cell_width * (u32)atlas->grid.cell_height;
    }

    if (borrowed_pass) {
        _nya_render2d_pass_resume(window);
    } else {
        SDL_SubmitGPUCommandBuffer(command_buffer);
    }

    atlas->uploaded_count = atlas->glyph_count;
}
