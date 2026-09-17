/**
 * @file render_text.c
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The largest glyph_count and line_count any run has needed in this process. */
NYA_INTERNAL u32 _nya_text_run_glyph_count_worst = 0;
NYA_INTERNAL u32 _nya_text_run_line_count_worst  = 0;

/**
 * Laid out texts, one `TTF_Text*` per (wrap width, face handle, text), tagged with the font asset's generation.
 * Created on the first cached shape.
 * */
NYA_INTERNAL NYA_Cache* _nya_text_run_cache = nullptr;

/** Empties a run without touching its glyph and line arrays, which are 30 KB and only read up to the counts. */
NYA_INTERNAL void _nya_text_run_reset(OUT NYA_TextRun* run);

/** Reads a laid out text into `out_run`. */
NYA_INTERNAL b8 _nya_text_run_fill(TTF_Text* shaped, OUT NYA_TextRun* out_run);

/** The loaded font asset for a face at a size, queueing the load on the first ask. Null until it has loaded. */
NYA_INTERNAL NYA_Asset* _nya_text_font_asset(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity);

/**
 * The laid out text for a face, a string and a wrap width, from the cache or laid out now. Null while the face
 * loads. `out_owned` is set for a key too long to cache, and the caller destroys that text.
 * */
NYA_INTERNAL TTF_Text* _nya_text_run_resolve(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width, OUT b8* out_owned);

/** The cache's destructor. */
NYA_INTERNAL void _nya_text_run_destroy(void* value, void* user_data);

/**
 * Fills in the run's per-line boxes from the laid-out text.
 * */
NYA_INTERNAL void _nya_text_collect_lines(TTF_Text* text, OUT NYA_TextRun* run) {
    s32 line_count = text->num_lines > 0 ? text->num_lines : 1;

    if ((u32)line_count > NYA_TEXT_RUN_LINES_MAX) {
        line_count     = NYA_TEXT_RUN_LINES_MAX;
        run->overflowed = true;
    }

    for (s32 line = 0; line < line_count; line++) {
        TTF_SubString substring = { 0 };

        // zeroed on failure, so a glyph's `line` is always a valid index.
        if (!TTF_GetTextSubStringForLine(text, line, &substring)) substring = (TTF_SubString){ 0 };

        run->lines[line] = (NYA_TextLine){
            .x      = substring.rect.x,
            .y      = substring.rect.y,
            .width  = substring.rect.w,
            .height = substring.rect.h,
            .offset = (u32)(substring.offset < 0 ? 0 : substring.offset),
            .length = (u32)(substring.length < 0 ? 0 : substring.length),
        };
    }

    run->line_count = (u32)line_count;

    if (run->line_count > _nya_text_run_line_count_worst) _nya_text_run_line_count_worst = run->line_count;
}

/**
 * Which line a glyph belongs to, by the line box its top edge falls in.
 * */
NYA_INTERNAL u32 _nya_text_line_of(const NYA_TextRun* run, s32 y) {
    for (u32 line = 0; line < run->line_count; line++) {
        const NYA_TextLine* candidate = &run->lines[line];
        if (y >= candidate->y && y < candidate->y + candidate->height) return line;
    }

    // past the last line box, which an overshooting glyph can be. Clamped, since it still has to draw.
    return run->line_count > 0 ? run->line_count - 1 : 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_text_shape(TTF_Font* font, NYA_ConstCString text, u64 length, s32 wrap_width, OUT NYA_TextRun* out_run) {
    nya_assert(out_run != nullptr);

    _nya_text_run_reset(out_run);

    if (font == nullptr || text == nullptr) return false;

    // an empty string is one line of no width, which is what a caller stacking rows needs.
    if (text[0] == '\0') {
        out_run->line_count = 1;
        out_run->height     = (s32)nya_text_line_height(font);
        out_run->lines[0]   = (NYA_TextLine){ .height = out_run->height };

        return true;
    }

    /*
     * A null engine. The engine only draws; shaping, kerning and line breaking run anyway and
     * `internal->ops` has the result, so this needs no device and matches headless.
     */
    TTF_Text* shaped = TTF_CreateText(nullptr, font, text, (size_t)length);
    if (shaped == nullptr) {
        nya_log_warn("TTF_CreateText() failed while shaping: %s", SDL_GetError());
        return false;
    }

    defer TTF_DestroyText(shaped);

    // before the layout is read, since setting it later marks the layout stale.
    if (wrap_width > 0 && !TTF_SetTextWrapWidth(shaped, wrap_width)) {
        nya_log_warn("TTF_SetTextWrapWidth() failed: %s", SDL_GetError());
    }

    return _nya_text_run_fill(shaped, out_run);
}

f32x2 nya_text_measure_font(TTF_Font* font, NYA_ConstCString text, s32 wrap_width) {
    if (font == nullptr || text == nullptr) return f32x2_zero;

    if (text[0] == '\0') return (f32x2){ 0.0F, nya_text_line_height(font) };

    /*
     * Measured through the same layout the draw uses, so measure and draw cannot disagree.
     * TTF_GetTextSize reports the box the ops were positioned in.
     */
    TTF_Text* shaped = TTF_CreateText(nullptr, font, text, 0);
    if (shaped == nullptr) return f32x2_zero;

    defer TTF_DestroyText(shaped);

    if (wrap_width > 0) (void)TTF_SetTextWrapWidth(shaped, wrap_width);

    s32 width = 0, height = 0;
    if (!TTF_GetTextSize(shaped, &width, &height)) return f32x2_zero;

    return (f32x2){ (f32)width, (f32)height };
}

f32 nya_text_line_height(TTF_Font* font) {
    return font != nullptr ? (f32)TTF_GetFontLineSkip(font) : 0.0F;
}

f32 nya_text_ascent(TTF_Font* font) {
    return font != nullptr ? (f32)TTF_GetFontAscent(font) : 0.0F;
}

f32 nya_text_descent(TTF_Font* font) {
    // flipped, so ascent + descent is the ink height. SDL returns descent negative.
    return font != nullptr ? (f32)(-TTF_GetFontDescent(font)) : 0.0F;
}

void nya_text_font_handle(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity) {
    nya_assert(out_handle != nullptr && capacity > 0);

    if (path == nullptr) {
        out_handle[0] = '\0';
        return;
    }

    (void)snprintf(out_handle, (size_t)capacity, "%s@%.0f", path, (f64)point_size);
}

TTF_Font* nya_text_font_for(NYA_ConstCString path, f32 point_size) {
    char       handle[NYA_TEXT_FONT_HANDLE_MAX];
    NYA_Asset* asset = _nya_text_font_asset(path, point_size, handle, sizeof(handle));

    return asset != nullptr ? asset->as_font.font : nullptr;
}

b8 nya_text_shape_with_font(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width, OUT NYA_TextRun* out_run) {
    nya_assert(out_run != nullptr);

    if (text == nullptr || text[0] == '\0') return nya_text_shape(nya_text_font_for(path, point_size), text, 0, wrap_width, out_run);

    b8        owned  = false;
    TTF_Text* shaped = _nya_text_run_resolve(path, point_size, text, wrap_width, &owned);

    if (shaped == nullptr) {
        _nya_text_run_reset(out_run);
        return false;
    }

    b8 filled = _nya_text_run_fill(shaped, out_run);
    if (owned) TTF_DestroyText(shaped);

    return filled;
}

f32x2 nya_text_measure_with_font(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width) {
    if (text == nullptr || text[0] == '\0') return nya_text_measure_font(nya_text_font_for(path, point_size), text, wrap_width);

    b8        owned  = false;
    TTF_Text* shaped = _nya_text_run_resolve(path, point_size, text, wrap_width, &owned);
    if (shaped == nullptr) return f32x2_zero;

    s32 width = 0, height = 0;
    b8  sized = TTF_GetTextSize(shaped, &width, &height);

    if (owned) TTF_DestroyText(shaped);

    return sized ? (f32x2){ (f32)width, (f32)height } : f32x2_zero;
}

void nya_text_run_cache_destroy(void) {
    if (_nya_text_run_cache == nullptr) return;

    nya_cache_destroy(_nya_text_run_cache);
    _nya_text_run_cache = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_text_run_reset(OUT NYA_TextRun* run) {
    run->glyph_count = 0;
    run->line_count  = 0;
    run->width       = 0;
    run->height      = 0;
    run->overflowed  = false;
}

b8 _nya_text_run_fill(TTF_Text* shaped, OUT NYA_TextRun* out_run) {
    nya_assert(shaped != nullptr && out_run != nullptr);

    // registered on the first call, which is when the counts become meaningful.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("text_run_glyphs", NYA_TEXT_RUN_GLYPHS_MAX, &_nya_text_run_glyph_count_worst);
        nya_ceiling_register("text_run_lines", NYA_TEXT_RUN_LINES_MAX, &_nya_text_run_line_count_worst);
        ceiling_registered = true;
    }

    _nya_text_run_reset(out_run);

    // forces layout now; for a cached text nothing has changed since, it is a flag test. `ops` is null until it has run.
    if (!TTF_UpdateText(shaped)) {
        nya_log_warn("TTF_UpdateText() failed while shaping: %s", SDL_GetError());
        return false;
    }

    if (!TTF_GetTextSize(shaped, &out_run->width, &out_run->height)) {
        out_run->width  = 0;
        out_run->height = 0;
    }

    _nya_text_collect_lines(shaped, out_run);

    const TTF_TextData* data = shaped->internal;
    if (data == nullptr) return true;

    for (s32 i = 0; i < data->num_ops; i++) {
        const TTF_DrawOperation* op = &data->ops[i];

        // FILL ops are underline and strikethrough rules. The font API exposes neither, so skip them.
        if (op->cmd != TTF_DRAW_COMMAND_COPY) continue;

        if (out_run->glyph_count >= NYA_TEXT_RUN_GLYPHS_MAX) {
            out_run->overflowed = true;
            break;
        }

        out_run->glyphs[out_run->glyph_count++] = (NYA_TextGlyph){
            .glyph_index = op->copy.glyph_index,
            .x           = op->copy.dst.x,
            .y           = op->copy.dst.y,
            .width       = op->copy.dst.w,
            .height      = op->copy.dst.h,
            .source_x    = op->copy.src.x,
            .source_y    = op->copy.src.y,
            .line        = _nya_text_line_of(out_run, op->copy.dst.y),
        };

        if (out_run->glyph_count > _nya_text_run_glyph_count_worst) _nya_text_run_glyph_count_worst = out_run->glyph_count;
    }

    /*
     * The per-line glyph ranges, in a second pass.
     */
    for (u32 line = 0; line < out_run->line_count; line++) {
        out_run->lines[line].first_glyph = out_run->glyph_count;
        out_run->lines[line].glyph_count = 0;
    }

    for (u32 i = 0; i < out_run->glyph_count; i++) {
        NYA_TextLine* line = &out_run->lines[out_run->glyphs[i].line];

        if (line->glyph_count == 0) line->first_glyph = i;
        line->glyph_count++;
    }

    return true;
}

NYA_Asset* _nya_text_font_asset(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity) {
    if (path == nullptr || path[0] == '\0' || point_size <= 0.0F) return nullptr;

    nya_text_font_handle(path, point_size, out_handle, capacity);

    /* Cast, matching nya_render2d_procedural and the other call sites holding a `const char*`. */
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)out_handle);

    if (asset == nullptr) {
        // queued, not loaded. Callers cope with no face for the next few frames.
        NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
          .type    = NYA_ASSET_TYPE_FONT,
          .handle  = (NYA_AssetHandle)out_handle,
          .source  = path,
          .as_font = { .point_size = point_size },
      }), "while queueing a font");

        return nullptr;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED || asset->as_font.font == nullptr) return nullptr;

    return asset;
}

TTF_Text* _nya_text_run_resolve(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width, OUT b8* out_owned) {
    nya_assert(text != nullptr && out_owned != nullptr);

    *out_owned = false;

    char       handle[NYA_TEXT_FONT_HANDLE_MAX];
    NYA_Asset* asset = _nya_text_font_asset(path, point_size, handle, sizeof(handle));
    if (asset == nullptr) return nullptr;

    if (_nya_text_run_cache == nullptr) {
        _nya_text_run_cache = nya_cache_create(
            nya_app_get()->asset_system.allocator,
            TTF_Text*,
            .name         = "text_runs",
            .capacity     = NYA_TEXT_RUN_CACHE_CAPACITY,
            .key_size_max = NYA_TEXT_RUN_CACHE_KEY_MAX,
            .eviction     = NYA_CACHE_EVICTION_LEAST_RECENT,
            .destructor   = _nya_text_run_destroy,
        );
    }

    // the wrap width, the handle with its terminator, then the text. the terminator keeps "a@1" + "2x" from
    // matching "a@12" + "x".
    u64 handle_size = strlen(handle) + 1;
    u64 text_size   = strlen(text);
    u64 key_size    = sizeof(wrap_width) + handle_size + text_size;

    u8 key[NYA_TEXT_RUN_CACHE_KEY_MAX];
    b8 cacheable = key_size <= sizeof(key);

    if (cacheable) {
        nya_memcpy(key, &wrap_width, sizeof(wrap_width));
        nya_memcpy(key + sizeof(wrap_width), handle, handle_size);
        nya_memcpy(key + sizeof(wrap_width) + handle_size, text, text_size);

        TTF_Text** cached = nya_cache_get(_nya_text_run_cache, key, key_size, asset->generation);
        if (cached != nullptr) return *cached;
    }

    TTF_Text* shaped = TTF_CreateText(nullptr, asset->as_font.font, text, 0);
    if (shaped == nullptr) {
        nya_log_warn("TTF_CreateText() failed while shaping: %s", SDL_GetError());
        return nullptr;
    }

    if (wrap_width > 0 && !TTF_SetTextWrapWidth(shaped, wrap_width)) nya_log_warn("TTF_SetTextWrapWidth() failed: %s", SDL_GetError());

    if (!cacheable) {
        *out_owned = true;
        return shaped;
    }

    // replaces a stale entry for the key, destroying the text laid out with the old face.
    void*     slot     = nullptr;
    NYA_Error inserted = nya_cache_insert(_nya_text_run_cache, key, key_size, asset->generation, &slot);
    nya_assert(inserted.ok, "a least recently used cache evicts instead of refusing a key that fits");

    *(TTF_Text**)slot = shaped;

    return shaped;
}

void _nya_text_run_destroy(void* value, void* user_data) {
    nya_unused(user_data);

    TTF_DestroyText(*(TTF_Text**)value);
}
