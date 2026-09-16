/**
 * @file render2d.c
 * */
#include "assets/shader/uniforms.h"

#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_internal.h"

#include "generated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */


/** The ASCII range the atlas sizes its cells against, inclusive. */
#define NYA_RENDER2D_GLYPH_FIRST 32
#define NYA_RENDER2D_GLYPH_LAST  126

/**
 * Glyphs one atlas can hold in total.
 * */
#ifndef NYA_RENDER2D_GLYPH_CAPACITY
#define NYA_RENDER2D_GLYPH_CAPACITY 512
#endif

/** Buckets in an atlas's glyph-index lookup. A power of two, because the index is a masked hash. */
#define NYA_RENDER2D_GLYPH_LOOKUP (NYA_RENDER2D_GLYPH_CAPACITY * 4)

/** Cells across the atlas texture. Rows follow from the capacity. */
#define NYA_RENDER2D_GLYPH_COLUMNS 16

/** Fonts whose atlases are held at once. A game uses a handful of faces. */
#define NYA_RENDER2D_FONT_CACHE_MAX 8

/** Longest derived font asset handle: a path, an '@', and a point size. */
#define NYA_RENDER2D_FONT_HANDLE_MAX 256

typedef struct NYA_Glyph      NYA_Glyph;
typedef struct NYA_FontAtlas  NYA_FontAtlas;

/**
 * One glyph's place in the atlas, in pixels except the uvs. Bearing and advance depend on the string
 * around a glyph, so they live on NYA_TextGlyph, which the shaper fills.
 * */
struct NYA_Glyph {
    f32 u0, v0, u1, v1;

    f32 width, height;
};

struct NYA_FontAtlas {
    /**
     * The path the face was loaded from. Null means the slot is free.
     * */
    NYA_ConstCString path;

    /** Point size this atlas was rasterised at. Part of the cache key, with the path. */
    f32 point_size;

    /**
     * The asset handle, the path plus the point size ("./assets/fonts/x.ttf@19"). Owned here because the
     * asset system keeps the pointer it is given.
     * */
    char handle[NYA_RENDER2D_FONT_HANDLE_MAX];

    /**
     * The TTF_Font the glyphs came from. A reload gives the asset a new TTF_Font, and comparing this pointer
     * is how the atlas notices its glyphs are stale.
     * */
    TTF_Font* source_font;

    SDL_GPUTexture* texture;

    /** Baseline to baseline. What to add to y for the next line. */
    f32 line_height;

    /** Top of the line box to the baseline, and baseline to the deepest descender (positive). */
    f32 ascent;
    f32 descent;

    /*
     * The lazily baked glyph table
     *
     * Keyed by glyph index, not codepoint, because shaping outputs indices (see render_text.h). SDL_ttf has
     * no codepoint to index mapping, so glyphs are baked on first use instead of an eager ASCII block.
     */

    NYA_Glyph glyphs[NYA_RENDER2D_GLYPH_CAPACITY];

    /** The glyph index each slot holds. */
    u32 glyph_indices[NYA_RENDER2D_GLYPH_CAPACITY];

    /** Slot number plus one for each glyph index, zero for "not baked". Masked, so the size is a power of two. */
    u16 lookup[NYA_RENDER2D_GLYPH_LOOKUP];

    /** Slots used. Grows as glyphs are baked. */
    u32 glyph_count;

    /**
     * One byte of coverage per texel, `atlas_width * atlas_height`. Kept after the first upload so later
     * glyphs can be baked in without rasterising the whole atlas again. The shaders read one channel.
     * */
    u8* coverage;

    /** Reused for every upload rather than created per glyph. Sized for the whole atlas. */
    SDL_GPUTransferBuffer* transfer_buffer;

    s32 atlas_width;
    s32 atlas_height;
    s32 cell_width;
    s32 cell_height;

    /** Set when a glyph is baked and cleared by the upload. See _nya_render2d_atlas_upload. */
    b8 upload_pending;

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

/*
 * Closes and reopens the render pass around work that needs a copy pass.
 */
NYA_INTERNAL void _nya_render2d_pass_suspend(NYA_Window* window);

/** Pushes the batch's scissor state onto the current render pass, or clears it. */
NYA_INTERNAL void _nya_render2d_apply_scissor(NYA_Window* window);
NYA_INTERNAL void _nya_render2d_range_close(NYA_Window* window);
NYA_INTERNAL s32  _nya_render2d_range_compare(const void* a, const void* b);
NYA_INTERNAL f32_4x4 _nya_render2d_range_projection(const NYA_Render2DDrawRange* range);
NYA_INTERNAL void _nya_render2d_range_apply_scissor(NYA_Window* window, const NYA_Render2DDrawRange* range);
NYA_INTERNAL void _nya_render2d_pass_resume(NYA_Window* window);

/**
 * Builds the glyph atlas for a font asset, or returns the one already built. Null on failure.
 * */
NYA_INTERNAL NYA_FontAtlas* _nya_render2d_font_atlas(NYA_Window* window, NYA_ConstCString font_path, f32 point_size);

/** The glyph for a glyph index, rasterising it into a free cell if needed. */
NYA_INTERNAL const NYA_Glyph* _nya_render2d_glyph(NYA_FontAtlas* atlas, u32 glyph_index);

/** Rasterises one glyph index into `slot`'s cell of the atlas surface and fills in its NYA_Glyph. */
NYA_INTERNAL void _nya_render2d_glyph_bake(NYA_FontAtlas* atlas, TTF_Font* font, u32 glyph_index, u32 slot);

/** Which lookup bucket a glyph index maps to. Mixed, so adjacent indices do not collide. */
NYA_INTERNAL u32 _nya_render2d_glyph_bucket(u32 glyph_index) __attr_no_discard;

/**
 * The face an atlas was built from, or null.
 * */
NYA_INTERNAL TTF_Font* _nya_render2d_atlas_font(const NYA_FontAtlas* atlas) __attr_no_discard;

/** Uploads the atlas if anything was baked since the last upload. */
NYA_INTERNAL void _nya_render2d_atlas_upload(NYA_Window* window, NYA_FontAtlas* atlas);


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Glyph atlases, keyed by font handle.
 * */
NYA_INTERNAL NYA_FontAtlas _nya_render2d_font_cache[NYA_RENDER2D_FONT_CACHE_MAX] = { 0 };

/** Slots claimed in _nya_render2d_font_cache, kept for the ceiling registry. */
NYA_INTERNAL u32 _nya_render2d_font_cache_count = 0;

/**
 * The fullest any atlas has been. Glyphs are never evicted, so this is the busiest atlas now.
 * NYA_RENDER2D_GLYPH_CAPACITY is per atlas, so the ceiling registry needs a single number to watch.
 * */
NYA_INTERNAL u32 _nya_render2d_glyph_count_worst = 0;


/** The font nya_render2d_text and the measurements use, set by nya_render2d_font_set. */
NYA_INTERNAL NYA_ConstCString _nya_render2d_current_font = nullptr;

/** Point size of the current font; the two are one setting. */
NYA_INTERNAL f32 _nya_render2d_current_font_size = 0.0F;

/** The atlas for _nya_render2d_current_font, resolved once rather than per call. */
NYA_INTERNAL NYA_FontAtlas* _nya_render2d_current_atlas = nullptr;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render2d_shutdown(void) {
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // keyed by font, not window, so nothing per-window frees them.
    for (u32 i = 0; i < NYA_RENDER2D_FONT_CACHE_MAX; i++) {
        NYA_FontAtlas* atlas = &_nya_render2d_font_cache[i];

        if (atlas->texture != nullptr) SDL_ReleaseGPUTexture(gpu_device, atlas->texture);
        if (atlas->transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, atlas->transfer_buffer);

        // kept for the whole run so glyphs can be baked in later; this is the one place it is freed.
        SDL_free(atlas->coverage);

        *atlas = (NYA_FontAtlas){ 0 };
    }

    // they point into the cache that was just emptied.
    _nya_render2d_current_font  = nullptr;
    _nya_render2d_current_atlas = nullptr;

    _nya_render2d_font_cache_count = 0;
    _nya_render2d_glyph_count_worst = 0;
}

/**
 * The projection a range draws through, from its own target and camera. Per range because both change
 * mid-frame: a render texture differs in size from the window, and a world camera is set around the HUD.
 * */
NYA_INTERNAL f32_4x4 _nya_render2d_range_projection(const NYA_Render2DDrawRange* range) {
    f32_4x4 projection = nya_matrix_orthographic(0.0F, (f32)range->target_width, 0.0F, (f32)range->target_height);

    // the camera is a view matrix ahead of the projection: world to pixels, then pixels to clip space. the
    // translation centres the camera on the target so zoom happens around what is being looked at.
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

        .texture = batch->texture,
        .sampler = batch->sampler,

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

    batch->range_count++;
    batch->range_sequence++;
    batch->range_first_index = batch->index_count;
}

/**
 * Layer first, declaration order second.
 * */
NYA_INTERNAL s32 _nya_render2d_range_compare(const void* a, const void* b) {
    const NYA_Render2DDrawRange* left  = a;
    const NYA_Render2DDrawRange* right = b;

    if (left->layer != right->layer) return left->layer < right->layer ? -1 : 1;
    if (left->sequence != right->sequence) return left->sequence < right->sequence ? -1 : 1;

    return 0;
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
    // timed per call: the run count is the draw call count, and the total is what it costs.
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    // the open range becomes the last one, so the loop below is the only thing that draws.
    _nya_render2d_range_close(window);

    if (batch->range_count == 0) {
        batch->vertex_count      = 0;
        batch->index_count       = 0;
        batch->range_first_index = 0;
        return;
    }

    // no pass: the window is occluded or minimised. dropped rather than drawn stale later.
    if (render->render_pass == nullptr) {
        batch->vertex_count      = 0;
        batch->index_count       = 0;
        batch->range_count       = 0;
        batch->range_first_index = 0;
        batch->range_sequence    = 0;
        return;
    }

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
    nya_memcpy(mapped_indices, batch->indices, index_upload_size);
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
    SDL_EndGPUCopyPass(copy_pass);

    _nya_render2d_pass_resume(window);

    /*
     * Rebuilt each flush: it depends on the target size, which changes on resize and when drawing moves
     * into a render texture. y grows down from the top left.
     */
    /* Sorted, then issued. The buffers are bound once, since every range indexes the same upload. */
    qsort(batch->ranges, batch->range_count, sizeof(NYA_Render2DDrawRange), _nya_render2d_range_compare);

    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer, .offset = 0 }, 1);
    SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer, .offset = 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    for (u32 i = 0; i < batch->range_count; i++) {
        const NYA_Render2DDrawRange* range = &batch->ranges[i];

        if (range->index_count == 0) continue;

        NYA_Asset* pipeline_asset = range->pipeline != nullptr ? nya_asset_get(range->pipeline) : nullptr;

        // still loading; skipped so one pipeline does not hold up the frame.
        if (pipeline_asset == nullptr || pipeline_asset->status != NYA_ASSET_STATUS_LOADED) continue;

        // per range, since target and camera belong to the range.
        f32_4x4 range_projection = _nya_render2d_range_projection(range);

        _nya_render2d_range_apply_scissor(window, range);

        SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline_asset->as_graphics_pipeline.pipeline);
        SDL_PushGPUVertexUniformData(render->render_commands, 0, &range_projection, sizeof(range_projection));

        // only for a custom shader: the built-in pipelines declare no fragment uniforms, and pushing one is a
        // validation error.
        if (range->uniform_size > 0) {
            SDL_PushGPUFragmentUniformData(render->render_commands, 0, range->uniform, range->uniform_size);
        }

        if (range->texture != nullptr) {
            SDL_BindGPUFragmentSamplers(
                render->render_pass,
                0,
                &(SDL_GPUTextureSamplerBinding){ .texture = range->texture, .sampler = range->sampler },
                1
            );
        }

        SDL_DrawGPUIndexedPrimitives(render->render_pass, range->index_count, 1, range->first_index, 0, 0);

        // a range is a draw call.
        batch->frame_flushes++;
    }

    batch->frame_vertices += batch->vertex_count;
    batch->frame_indices  += batch->index_count;

    // consumed here so an unattributed flush (the frame end) still lands somewhere.
    batch->frame_flush_reasons[batch->pending_flush_reason % NYA_RENDER2D_FLUSH_REASON_COUNT]++;
    batch->pending_flush_reason = NYA_RENDER2D_FLUSH_FRAME_END;

    batch->vertex_count      = 0;
    batch->index_count       = 0;
    batch->range_count       = 0;
    batch->range_first_index = 0;
    batch->range_sequence    = 0;
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

    /*
     * Segments scale with the radius, about one per two pixels of circumference, at least 8. The cap starts
     * at 64 and grows with the radius up to 512, so large circles stay round and one shape cannot fill the batch.
     */
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

    // closes the range instead of flushing: camera and clip are per range, so queued geometry keeps its state
    // and stays reorderable by layer.
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


void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint) {
    // cast because nya_asset_get takes a mutable handle it only reads.
    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return;

    f32 width  = (f32)asset->as_texture.width;
    f32 height = (f32)asset->as_texture.height;

    nya_render2d_texture_rect(window, texture_handle, 0.0F, 0.0F, width, height, x, y, width, height, tint);
}

void nya_render2d_texture_ex(NYA_Window* window, NYA_ConstCString texture_handle, NYA_Render2DTexture params) {
    nya_assert(window != nullptr);

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

    /*
     * Corners relative to the pivot, then rotated, then moved to the pivot's position, so `rotation` turns
     * about `origin`.
     */
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

    // dropped, not rebuilt here: the atlas may not be buildable yet, and every consumer builds on demand.
    _nya_render2d_current_atlas = nullptr;
}

NYA_ConstCString nya_render2d_font_get(void) {
    return _nya_render2d_current_font;
}

f32 nya_render2d_font_size_get(void) {
    return _nya_render2d_current_font_size;
}

void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params) {
    nya_assert(window != nullptr);

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);

    // missing or still loading, normal right after a load.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_TEXTURE) return;

    f32 texture_width  = (f32)asset->as_texture.width;
    f32 texture_height = (f32)asset->as_texture.height;

    if (texture_width <= 0.0F || texture_height <= 0.0F) return;
    if (params.width <= 0.0F || params.height <= 0.0F) return;

    // a zero tint means white. nya_render2d_texture_rect passes colours through unchanged, so the substitution
    // has to happen here or a default panel draws invisible.
    NYA_Color tint = params.tint;

    if (tint.r == 0.0F && tint.g == 0.0F && tint.b == 0.0F && tint.a == 0.0F) tint = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };

    f32 left   = nya_max(params.left, 0.0F);
    f32 right  = nya_max(params.right, 0.0F);
    f32 top    = nya_max(params.top, 0.0F);
    f32 bottom = nya_max(params.bottom, 0.0F);

    /*
     * Borders shrink with a destination smaller than them. Otherwise a panel animating open from zero draws
     * inside-out quads. Scaled proportionally, so uneven borders stay uneven.
     */
    f32 horizontal = left + right;
    f32 vertical   = top + bottom;

    if (horizontal > params.width && horizontal > 0.0F) {
        f32 shrink  = params.width / horizontal;
        left       *= shrink;
        right      *= shrink;
    }

    if (vertical > params.height && vertical > 0.0F) {
        f32 shrink  = params.height / vertical;
        top        *= shrink;
        bottom     *= shrink;
    }

    // source borders name the authored image; destination borders may be shrunk. corners keep their size and
    // the middle absorbs the rest.
    f32 source_x[4]   = { 0.0F, params.left, texture_width - params.right, texture_width };
    f32 source_y[4]   = { 0.0F, params.top, texture_height - params.bottom, texture_height };
    f32 destination_x[4] = { params.x, params.x + left, params.x + params.width - right, params.x + params.width };
    f32 destination_y[4] = { params.y, params.y + top, params.y + params.height - bottom, params.y + params.height };

    for (u32 row = 0; row < 3; row++) {
        for (u32 column = 0; column < 3; column++) {
            // the centre patch is skipped for a frame. see NYA_NineSlice.hollow.
            if (params.hollow && row == 1 && column == 1) continue;

            f32 source_width       = source_x[column + 1] - source_x[column];
            f32 source_height      = source_y[row + 1] - source_y[row];
            f32 destination_width  = destination_x[column + 1] - destination_x[column];
            f32 destination_height = destination_y[row + 1] - destination_y[row];

            // a zero border makes a valid three-slice. skipped so it costs no vertices.
            if (source_width <= 0.0F || source_height <= 0.0F) continue;
            if (destination_width <= 0.0F || destination_height <= 0.0F) continue;

            nya_render2d_texture_rect(
                window,
                texture_handle,
                source_x[column],
                source_y[row],
                source_width,
                source_height,
                destination_x[column],
                destination_y[row],
                destination_width,
                destination_height,
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

    // not bakeable (full atlas, or no such glyph in the face). the shaper already placed the next glyph, so the
    // line keeps its width.
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
    f32 texel_width  = 1.0F / (f32)atlas->atlas_width;
    f32 texel_height = 1.0F / (f32)atlas->atlas_height;

    f32 u0 = glyph->u0 + ((f32)shaped->source_x * texel_width);
    f32 v0 = glyph->v0 + ((f32)shaped->source_y * texel_height);
    f32 u1 = u0 + ((f32)shaped->width * texel_width);
    f32 v1 = v0 + ((f32)shaped->height * texel_height);

    // snapped to whole pixels: nearest sampling off the pixel grid drops or doubles columns and makes small
    // text shimmer. only the destination is rounded, not the layout.
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

    TTF_Font* font = _nya_render2d_atlas_font(atlas);
    if (font == nullptr) return;

    /* Shaped once; the rest is placement. */
    if (!nya_text_shape(font, text, 0, 0, &_nya_render2d_run)) return;

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
    TTF_Font* font = nya_text_font_for(font_path, point_size);
    if (font == nullptr) return f32x2_zero;

    return nya_text_measure_font(font, text, 0);
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

void nya_render2d_shader_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    window->render_system.draw_batch.shader_override    = nullptr;
    // cleared with the shader so the next custom pipeline does not inherit these.
    window->render_system.draw_batch.shader_uniform_size = 0;
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
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_graphics_pipeline.pipeline == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    // what is queued was queued for another pipeline.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    if (render->render_pass == nullptr || render->render_commands == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    SDL_BindGPUGraphicsPipeline(render->render_pass, asset->as_graphics_pipeline.pipeline);

    if (uniform_data != nullptr && uniform_size > 0) {
        SDL_PushGPUVertexUniformData(render->render_commands, 0, uniform_data, uniform_size);
        SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform_data, uniform_size);
    }

    SDL_DrawGPUPrimitives(render->render_pass, (Uint32)vertex_count, 1, 0, 0);

    batch->frame_flushes++;
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    /*
     * The cached pipeline is cleared: the batch skips rebinding what it thinks is bound, and this draw bound
     * another one behind its back.
     */
    batch->pipeline = nullptr;
    batch->texture  = nullptr;
    batch->sampler  = nullptr;
}

void nya_render2d_scissor_begin(NYA_Window* window, f32 x, f32 y, f32 width, f32 height) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // closes the range instead of flushing, so queued geometry keeps its unclipped state and stays
    // reorderable by layer.
    _nya_render2d_range_close(window);

    // clamped to the target: SDL_GPU rejects a scissor outside it, and panels scrolled half off screen are
    // ordinary.
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

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // the swapchain's format, so the window's pipelines can draw here.
    SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(gpu_device, window->sdl_window);

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(
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

    // a multisampled companion, since the pipelines are built for the renderer's sample count. drawing resolves
    // onto the sampled texture when the pass ends.
    SDL_GPUTexture* msaa_texture = nullptr;

    if (nya_app_get()->render_system.sample_count != SDL_GPU_SAMPLECOUNT_1) {
        msaa_texture = SDL_CreateGPUTexture(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = format,
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
                .sample_count         = nya_app_get()->render_system.sample_count,
            }
        );
        nya_assert(msaa_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's MSAA buffer: %s", SDL_GetError());
    }

    // the window's depth format and sample count, which are baked into the pipelines.
    SDL_GPUTexture* depth_texture = nullptr;

    if (options.depth == NYA_RENDER_TEXTURE_DEPTH_ATTACHED) {
        depth_texture = SDL_CreateGPUTexture(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = nya_app_get()->render_system.depth_format,
                .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
                .sample_count         = nya_app_get()->render_system.sample_count,
            }
        );
        nya_assert(depth_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's depth buffer: %s", SDL_GetError());
    }

    return (NYA_RenderTexture){
        .texture       = texture,
        .msaa_texture  = msaa_texture,
        .depth_texture = depth_texture,
        .width         = width,
        .height        = height,
    };
}

void nya_render_texture_destroy(NYA_RenderTexture* render_texture) {
    if (render_texture == nullptr) return;
    if (render_texture->texture == nullptr) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // SDL_ReleaseGPUTexture already waits until the texture is unused; waiting for the GPU here would stall.
    SDL_ReleaseGPUTexture(gpu_device, render_texture->texture);
    if (render_texture->msaa_texture != nullptr) SDL_ReleaseGPUTexture(gpu_device, render_texture->msaa_texture);
    if (render_texture->depth_texture != nullptr) SDL_ReleaseGPUTexture(gpu_device, render_texture->depth_texture);

    *render_texture = (NYA_RenderTexture){ 0 };
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
        // depth cleared too, or last frame's depth occludes this frame. null (not a struct naming null) when there
        // is no depth buffer: SDL decides from the pointer whether the pass has a depth target.
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

    batch->target_texture    = render_texture->texture;
    batch->target_msaa       = render_texture->msaa_texture;
    batch->target_depth      = render_texture->depth_texture;
    batch->target_width      = render_texture->width;
    batch->target_height     = render_texture->height;
    batch->target_is_texture = true;
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

    batch->target_texture    = render->swapchain_texture;
    batch->target_msaa       = render->msaa_texture;
    batch->target_depth      = render->depth_texture;
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

    if (!nya_text_shape(font, text, 0, wrap_width, &_nya_render2d_run)) return f32x2_zero;

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

        /*
         * The ellipsis is baked before the upload too, only when the text is truncated, so it never draws blank for
         * a frame.
         */
        if (truncated && params.ellipsis) {
            static NYA_TextRun ellipsis_run;

            if (nya_text_shape(font, "...", 0, 0, &ellipsis_run)) _nya_render2d_run_bake(window, atlas, &ellipsis_run);
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

        // appended after the last line instead of displacing characters. it can overhang by three dots, which is
        // simpler and rarely visible.
        if (!truncated || !params.ellipsis || line_index + 1 != lines) continue;

        static NYA_TextRun ellipsis_run;
        if (!nya_text_shape(font, "...", 0, 0, &ellipsis_run)) continue;

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

    // y points down, so a positive angle is clockwise on screen, matching NYA_Render2DTexture.rotation and 2D
    // bodies.
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

    // one shape bigger than the whole buffer. refused loudly, since the fix is raising
    // NYA_RENDER2D_MAX_VERTICES.
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

    /*
     * A draw call has one pipeline and one texture. Pipeline handles are the NYA_RENDER2D_PIPELINE_* literals and
     * compare by pointer. A custom pipeline goes through shader_override, which flushes on its own.
     */
    if (batch->index_count > batch->range_first_index) {
        // pipeline first: a pipeline change usually brings a texture change, and blaming the texture would suggest
        // an atlas that would not help.
        if (batch->pipeline != pipeline) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_PIPELINE;
        else if (batch->texture != texture) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_TEXTURE;
        else if (batch->sampler != sampler) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_SAMPLER;

        // a state change closes a range; it is issued later in layer order. see NYA_Render2DDrawRange.
        if (batch->pipeline != pipeline || batch->texture != texture || batch->sampler != sampler) {
            _nya_render2d_range_close(window);
        }
    }

    // out of room or ranges, so this one draws now. geometry on either side loses cross-layer ordering, which is
    // why the bounds are generous.
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

    /*
     * Resuming a shadow pass: back onto the shadow map, or everything after the first flush lands in the scene's
     * colour buffer.
     */
    if (render->mesh_batch.shadow_pass_active && render->mesh_batch.active) {
        render->render_pass = SDL_BeginGPURenderPass(
            render->render_commands,
            &(SDL_GPUColorTargetInfo){
                .texture = render->mesh_batch.shadow_color,
                .load_op = SDL_GPU_LOADOP_LOAD,
                .store_op = SDL_GPU_STOREOP_STORE,
            },
            1,
            &(SDL_GPUDepthStencilTargetInfo){
                .texture          = render->mesh_batch.shadow_depth,
                .load_op          = SDL_GPU_LOADOP_LOAD,
                .store_op         = SDL_GPU_STOREOP_STORE,
                .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
                .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
            }
        );

        nya_assert(render->render_pass != nullptr, "SDL_BeginGPURenderPass() failed while resuming a shadow pass: %s", SDL_GetError());

        // a viewport belongs to a pass, and this is a new pass.
        _nya_render3d_shadow_viewport_apply(window, render->mesh_batch.shadow_cascade);

        return;
    }

    if (batch->target_texture == nullptr) return;

    /*
     * Multisampling resolves once, on the frame's last pass. Resolving on every reopen would resolve once per draw
     * call. Intermediate passes store the multisample texture without a resolve target attached.
     */
    b8 resolving = batch->target_msaa != nullptr && (batch->target_is_texture || batch->resolve_pending);

    render->render_pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture         = batch->target_msaa != nullptr ? batch->target_msaa : batch->target_texture,
            .resolve_texture = resolving ? batch->target_texture : nullptr,
            // LOAD, since this reopens mid-target. RESOLVE_AND_STORE keeps the multisample contents the LOAD needs.
            .load_op  = SDL_GPU_LOADOP_LOAD,
            .store_op = resolving ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
        },
        1,
        // LOAD for the same reason. null when there is no depth buffer, matching nya_render_texture_begin, or the
        // bound pipelines do not match the pass.
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

    // a new pass clips to nothing, so the batch's clip goes back on.
    _nya_render2d_apply_scissor(window);
}

NYA_FontAtlas* _nya_render2d_font_atlas(NYA_Window* window, NYA_ConstCString font_path, f32 point_size) {
    if (font_path == nullptr) return nullptr;
    if (point_size <= 0.0F) point_size = NYA_RENDER2D_FONT_DEFAULT_SIZE;

    /*
     * The handle is derived from path and size, and the asset queued if new. A face has one size baked in, so
     * one .ttf at two sizes is two assets, and the caller only ever passes the path.
     */
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

    // the common case: the current font, same face as when it was built.
    if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size && _nya_render2d_current_atlas != nullptr &&
        _nya_render2d_current_atlas->source_font == asset->as_font.font) {
        return _nya_render2d_current_atlas;
    }

    for (u32 i = 0; i < NYA_RENDER2D_FONT_CACHE_MAX; i++) {
        if (_nya_render2d_font_cache[i].path == nullptr) continue;
        if (_nya_render2d_font_cache[i].point_size != point_size || strcmp(_nya_render2d_font_cache[i].path, font_path) != 0) continue;

        if (_nya_render2d_font_cache[i].source_font == asset->as_font.font) {
            if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size) _nya_render2d_current_atlas = &_nya_render2d_font_cache[i];
            return &_nya_render2d_font_cache[i];
        }

        /*
         * Reloaded: the slot is freed and rebuilt below. Flushed first, since queued vertices name the texture about
         * to be released. Without a window there is no batch or pass, so the stale atlas is returned; that is a
         * measurement, and one frame behind is fine.
         */
        if (window == nullptr) return &_nya_render2d_font_cache[i];

        nya_log_debug("font '%s' reloaded; rebuilding its glyph atlas", derived);

        _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

        // texture, CPU coverage and transfer buffer all go, or a hot reload leaks two of them per edit.
        SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;
        NYA_FontAtlas* stale  = &_nya_render2d_font_cache[i];

        SDL_ReleaseGPUTexture(device, stale->texture);
        if (stale->transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(device, stale->transfer_buffer);
        SDL_free(stale->coverage);

        if (_nya_render2d_current_atlas == stale) _nya_render2d_current_atlas = nullptr;

        *stale = (NYA_FontAtlas){ 0 };
        break;
    }

    NYA_FontAtlas* slot = nullptr;
    for (u32 i = 0; i < NYA_RENDER2D_FONT_CACHE_MAX; i++) {
        if (_nya_render2d_font_cache[i].path == nullptr) {
            slot = &_nya_render2d_font_cache[i];
            break;
        }
    }

    // full, not evicting: eviction would need to know no queued vertex still references the texture.
    if (slot == nullptr) {
        nya_log_warn("no free glyph atlas slot for '%s'; raise NYA_RENDER2D_FONT_CACHE_MAX (%d)", derived, NYA_RENDER2D_FONT_CACHE_MAX);
        return nullptr;
    }

    _nya_render2d_font_cache_count++;

    // registered once, when the first atlas exists.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("glyph_atlases", NYA_RENDER2D_FONT_CACHE_MAX, &_nya_render2d_font_cache_count);
        ceiling_registered = true;
    }

    TTF_Font*      font       = asset->as_font.font;
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // a fixed grid sized to the largest glyph, so layout is a multiply instead of a rectangle packer.
    s32 line_skip = TTF_GetFontLineSkip(font);
    s32 ascent    = TTF_GetFontAscent(font);
    // SDL reports the descent as negative; flipped so ascent + descent is the ink height.
    s32 descent   = -TTF_GetFontDescent(font);

    /* A cell holds a glyph's cropped ink image, so sizing it against the line box is conservative. */
    s32 cell_width  = 1;
    s32 cell_height = nya_max(TTF_GetFontHeight(font), line_skip);

    for (s32 character = NYA_RENDER2D_GLYPH_FIRST; character <= NYA_RENDER2D_GLYPH_LAST; character++) {
        s32 min_x = 0, max_x = 0, min_y = 0, max_y = 0, advance = 0;
        if (!TTF_GetGlyphMetrics(font, (u32)character, &min_x, &max_x, &min_y, &max_y, &advance)) continue;

        // the wider of advance and ink, so overhanging glyphs are not clipped.
        cell_width = nya_max(cell_width, nya_max(advance, max_x));
    }

    /*
     * Half again wider than ASCII needs: cells are sized once, and Latin Extended glyphs run about a third wider
     * than the widest ASCII one. Too small a cell clips those glyphs silently.
     */
    cell_width  = (cell_width * 3) / 2;
    cell_height = (cell_height * 3) / 2;

    // a one pixel gutter so linear filtering does not bleed in the neighbouring glyph.
    cell_width  += 2;
    cell_height += 2;

    const s32 columns      = NYA_RENDER2D_GLYPH_COLUMNS;
    s32       rows         = (NYA_RENDER2D_GLYPH_CAPACITY + columns - 1) / columns;
    s32       atlas_width  = cell_width * columns;
    s32       atlas_height = cell_height * rows;

    // zeroed, so the space between glyphs blends away.
    u8* coverage = SDL_calloc(1, (size_t)atlas_width * (size_t)atlas_height);
    if (coverage == nullptr) {
        nya_log_warn("could not allocate a glyph atlas for '%s': out of memory", derived);
        return nullptr;
    }

    *slot = (NYA_FontAtlas){
        .path         = font_path,
        .point_size   = point_size,
        .line_height  = (f32)line_skip,
        .ascent       = (f32)ascent,
        .descent      = (f32)descent,
        .coverage     = coverage,
        .atlas_width  = atlas_width,
        .atlas_height = atlas_height,
        .cell_width   = cell_width,
        .cell_height  = cell_height,

        /*
         * Empty: glyph indices are only known by shaping text, since SDL_ttf has no codepoint-to-index mapping, so
         * every glyph is baked on first use.
         */
        .glyph_count = 0,
    };

    // copied, because the asset system keeps the pointer and `derived` is a local.
    (void)snprintf(slot->handle, sizeof(slot->handle), "%s", derived);

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            // one channel; both text shaders read .r.
            .format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = (u32)atlas_width,
            .height               = (u32)atlas_height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );
    nya_assert(texture != nullptr, "SDL_CreateGPUTexture() failed for a glyph atlas: %s", SDL_GetError());

    slot->transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (u32)(atlas_width * atlas_height) }
    );
    nya_assert(slot->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for a glyph atlas: %s", SDL_GetError());

    slot->texture = texture;

    // the empty atlas is uploaded anyway, or text drawn before the first bake samples undefined memory.
    slot->upload_pending = true;
    _nya_render2d_atlas_upload(window, slot);

    // compared later to notice a reload; set with the texture so the two agree.
    slot->source_font = font;

    // latched now, with the face that fills it.
    slot->sdf = TTF_GetFontSDF(font);

    if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size) _nya_render2d_current_atlas = slot;

    // logged because the mode decides the pipeline and is latched here, so a late distance-field request shows up
    // in this line.
    nya_log_info("Built a glyph atlas for '%s' (%dx%d, %d slots, %s, filled on demand).", derived, atlas_width, atlas_height,
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

    NYA_Asset* asset = nya_asset_get(NYA_RENDER2D_PIPELINE_LIGHT);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_graphics_pipeline.pipeline == nullptr) {
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
        // world to target pixels through the scene's camera. lights live in world space with their entities, and the
        // fullscreen shader works in pixels, so the camera stays out of the uniforms.
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

    SDL_BindGPUGraphicsPipeline(render->render_pass, asset->as_graphics_pipeline.pipeline);

    // fragment only: the fullscreen vertex shader declares no uniforms.
    SDL_PushGPUFragmentUniformData(render->render_commands, 0, &uniform, sizeof(uniform));

    // three vertices, one oversized triangle. see procedural.vert.hlsl.
    SDL_DrawGPUPrimitives(render->render_pass, 3, 1, 0, 0);

    batch->frame_flushes++;
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    // cleared: this bound a pipeline behind the batch's back.
    batch->pipeline = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * GLYPHS
 * ─────────────────────────────────────────────────────────
 */

TTF_Font* _nya_render2d_atlas_font(const NYA_FontAtlas* atlas) {
    if (atlas == nullptr) return nullptr;

    // cast because nya_asset_get only reads the handle.
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)atlas->handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;

    return asset->as_font.font;
}

/* The attribute is required: this multiply overflows on purpose, and the sanitized build aborts without it. */
__attr_no_sanitize("unsigned-integer-overflow") u32 _nya_render2d_glyph_bucket(u32 glyph_index) {
    // mixed, because glyph indices in one script are consecutive and would collide along a word.
    u32 hash = glyph_index * 2654435761U;

    return (hash >> 16) & (NYA_RENDER2D_GLYPH_LOOKUP - 1);
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
        // full, not evicting, for the font cache's reason.
        nya_log_warn("glyph atlas for '%s' is full at %d glyphs; raise NYA_RENDER2D_GLYPH_CAPACITY", atlas->path, NYA_RENDER2D_GLYPH_CAPACITY);
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

    _nya_render2d_glyph_bake(atlas, font, glyph_index, slot);

    return &atlas->glyphs[slot];
}

void _nya_render2d_glyph_bake(NYA_FontAtlas* atlas, TTF_Font* font, u32 glyph_index, u32 slot) {
    NYA_Glyph* glyph = &atlas->glyphs[slot];

    *glyph = (NYA_Glyph){ 0 };

    s32 cell_x = (s32)(slot % NYA_RENDER2D_GLYPH_COLUMNS) * atlas->cell_width;
    s32 cell_y = (s32)(slot / NYA_RENDER2D_GLYPH_COLUMNS) * atlas->cell_height;

    /* By glyph index, as shaped: a ligature has no codepoint, and a mark cluster has several glyphs for one. */
    TTF_ImageType image_type = TTF_IMAGE_INVALID;
    SDL_Surface*  glyph_surface = TTF_GetGlyphImageForIndex(font, glyph_index, &image_type);

    // no such glyph, or it failed to rasterise: the cell stays empty.
    if (glyph_surface == nullptr) return;

    defer SDL_DestroySurface(glyph_surface);

    // clipped rather than spilling into the neighbouring cell.
    s32 width  = nya_min(glyph_surface->w, atlas->cell_width - 2);
    s32 height = nya_min(glyph_surface->h, atlas->cell_height - 2);

    if (width <= 0 || height <= 0) return;

    // cleared first: a re-bake after a reload would otherwise show old ink.
    for (s32 row = cell_y; row < cell_y + atlas->cell_height && row < atlas->atlas_height; row++) {
        nya_memset(atlas->coverage + ((size_t)row * (size_t)atlas->atlas_width) + (size_t)cell_x, 0, (size_t)atlas->cell_width);
    }

    /* Converted when the format differs, since only RGBA32 puts alpha in an indexable byte. */
    SDL_Surface* source    = glyph_surface;
    SDL_Surface* converted = nullptr;

    if (source->format != SDL_PIXELFORMAT_RGBA32) {
        converted = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
        if (converted == nullptr) return;

        source = converted;
    }

    /*
     * The glyph's alpha becomes the coverage. Coverage is kept rather than thresholded: with pixel-snapped quads
     * and nearest sampling, one pixel maps to one texel, so the antialiasing survives unblurred.
     */
    for (s32 y = 0; y < height; y++) {
        const u8* source_row = (const u8*)source->pixels + ((size_t)y * (size_t)source->pitch);
        u8*       atlas_row  = atlas->coverage + ((size_t)(cell_y + 1 + y) * (size_t)atlas->atlas_width) + (size_t)(cell_x + 1);

        // SDL_PIXELFORMAT_RGBA32 puts alpha in the last byte on either endianness.
        for (s32 x = 0; x < width; x++) atlas_row[x] = source_row[((size_t)x * 4) + 3];
    }

    if (converted != nullptr) SDL_DestroySurface(converted);

    *glyph = (NYA_Glyph){
        .u0     = (f32)(cell_x + 1) / (f32)atlas->atlas_width,
        .v0     = (f32)(cell_y + 1) / (f32)atlas->atlas_height,
        .u1     = (f32)(cell_x + 1 + width) / (f32)atlas->atlas_width,
        .v1     = (f32)(cell_y + 1 + height) / (f32)atlas->atlas_height,
        .width  = (f32)width,
        .height = (f32)height,
    };

    atlas->upload_pending = true;
}

void _nya_render2d_atlas_upload(NYA_Window* window, NYA_FontAtlas* atlas) {
    if (!atlas->upload_pending) return;
    if (atlas->texture == nullptr || atlas->coverage == nullptr) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    u32 upload_size = (u32)(atlas->atlas_width * atlas->atlas_height);

    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, atlas->transfer_buffer, false);
    nya_memcpy(mapped, atlas->coverage, upload_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, atlas->transfer_buffer);

    /*
     * This runs inside a render pass, and a copy pass cannot open inside one, so the pass is suspended as a vertex
     * flush does. It happens a handful of times per run.
     */
    b8 borrowed_pass = window != nullptr && window->render_system.render_pass != nullptr;
    if (borrowed_pass) _nya_render2d_pass_suspend(window);

    SDL_GPUCommandBuffer* command_buffer = borrowed_pass ? window->render_system.render_commands : SDL_AcquireGPUCommandBuffer(gpu_device);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_UploadToGPUTexture(
        copy_pass, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = atlas->transfer_buffer, .offset = 0 },
        &(SDL_GPUTextureRegion){ .texture = atlas->texture, .w = (u32)atlas->atlas_width, .h = (u32)atlas->atlas_height, .d = 1 }, false
    );
    SDL_EndGPUCopyPass(copy_pass);

    if (borrowed_pass) {
        _nya_render2d_pass_resume(window);
    } else {
        SDL_SubmitGPUCommandBuffer(command_buffer);
    }

    atlas->upload_pending = false;
}
