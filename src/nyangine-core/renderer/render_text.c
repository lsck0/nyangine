/**
 * @file render_text.c
 * */
#include "nyangine-core/nyangine.h"

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

/**
 * The laid out text's size. SDL_ttf measures a distance field line out to its last ink plus the spread wherever that
 * passes the advance, which is up to the spread wider than the same face as coverage and puts centred text a few
 * pixels left. Such a text is measured again to where each line's last glyph advances, within a pixel, since the
 * advance reads back whole.
 * */
NYA_INTERNAL b8 _nya_text_size(TTF_Text* shaped, OUT s32* out_width, OUT s32* out_height);

/** The loaded font asset for a face at a size, queueing the load on the first ask. Null until it has loaded. */
NYA_INTERNAL NYA_Asset* _nya_text_font_asset(NYA_ConstCString path, f32 point_size, OUT char* out_handle, u64 capacity);

/**
 * The laid out text for a face, a string and a wrap width, from the cache or laid out now. Null while the face
 * loads. `out_owned` is set for a key too long to cache, and the caller destroys that text.
 * */
NYA_INTERNAL TTF_Text* _nya_text_run_resolve(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width, OUT b8* out_owned);

/** The cache's destructor. */
NYA_INTERNAL void _nya_text_run_destroy(void* value, void* user_data);

/** Whether a line could hold a right-to-left script: any lead byte at or past U+0590's, a cheap gate for the ASCII path. */
NYA_INTERNAL b8 _nya_text_maybe_rtl(NYA_ConstCString text, u64 length) __attr_no_discard;

/** The visual runs of a line, and whether any is right-to-left. False leaves the fast left-to-right path to run. */
NYA_INTERNAL b8 _nya_text_bidi_needed(NYA_ConstCString text, u64 length, OUT NYA_BidiRun* runs, OUT u32* out_count) __attr_no_discard;

/** Shapes each visual run in its own direction and lays them out left-to-right into one line of `out_run`, which is reset. */
NYA_INTERNAL b8 _nya_text_shape_runs(TTF_Font* font, NYA_ConstCString text, u64 length, const NYA_BidiRun* runs, u32 count, OUT NYA_TextRun* out_run);

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
    nya_trace_scope(NYA_TRACE_TEXT);

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

    // A right-to-left script needs the bidi pass; wrapping stays on the shaper's own path, which breaks the lines.
    NYA_BidiRun runs[NYA_BIDI_RUNS_MAX];
    u32         count = 0;

    if (wrap_width <= 0 && _nya_text_bidi_needed(text, length, runs, &count)) {
        return _nya_text_shape_runs(font, text, length, runs, count, out_run);
    }

    // A null engine: shaping, kerning and line breaking run without a device (in `internal->ops`), matching headless.
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

    // Measured through the same layout the draw uses, so measure and draw cannot disagree.
    TTF_Text* shaped = TTF_CreateText(nullptr, font, text, 0);
    if (shaped == nullptr) return f32x2_zero;

    defer TTF_DestroyText(shaped);

    if (wrap_width > 0) (void)TTF_SetTextWrapWidth(shaped, wrap_width);

    s32 width = 0, height = 0;
    if (!_nya_text_size(shaped, &width, &height)) return f32x2_zero;

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

    // By hand for the common case: %.0f alone cost more than a cached shape; nearbyint rounds half to even like %.0f.
    f64 rounded     = nearbyint((f64)point_size);
    u64 path_length = strlen(path);

    char digits[10];
    u32  digit_count = 0;

    if (point_size > 0.0F && rounded < 1e9) {
        for (u32 size = (u32)rounded; digit_count == 0 || size > 0; size /= 10) digits[digit_count++] = (char)('0' + (size % 10));
    }

    if (digit_count == 0 || path_length + 1 + digit_count >= capacity) {
        (void)snprintf(out_handle, (size_t)capacity, "%s@%.0f", path, (f64)point_size);
        return;
    }

    nya_memcpy(out_handle, path, path_length);
    out_handle[path_length] = '@';

    for (u32 i = 0; i < digit_count; i++) out_handle[path_length + 1 + i] = digits[digit_count - 1 - i];
    out_handle[path_length + 1 + digit_count] = '\0';
}

TTF_Font* nya_text_font_for(NYA_ConstCString path, f32 point_size) {
    char       handle[NYA_TEXT_FONT_HANDLE_MAX];
    NYA_Asset* asset = _nya_text_font_asset(path, point_size, handle, sizeof(handle));

    return asset != nullptr ? asset->as_font.font : nullptr;
}

b8 nya_text_shape_with_font(NYA_ConstCString path, f32 point_size, NYA_ConstCString text, s32 wrap_width, OUT NYA_TextRun* out_run) {
    nya_assert(out_run != nullptr);

    if (text == nullptr || text[0] == '\0') return nya_text_shape(nya_text_font_for(path, point_size), text, 0, wrap_width, out_run);

    // A right-to-left line is shaped fresh through the bidi pass rather than the single-text cache below, which
    // holds one TTF_Text per string and so cannot store the several a bidi line shapes into.
    NYA_BidiRun runs[NYA_BIDI_RUNS_MAX];
    u32         count = 0;

    if (wrap_width <= 0 && _nya_text_bidi_needed(text, 0, runs, &count)) {
        TTF_Font* font = nya_text_font_for(path, point_size);

        _nya_text_run_reset(out_run);
        if (font == nullptr) return false;

        return _nya_text_shape_runs(font, text, 0, runs, count, out_run);
    }

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
    b8  sized = _nya_text_size(shaped, &width, &height);

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

b8 _nya_text_size(TTF_Text* shaped, OUT s32* out_width, OUT s32* out_height) {
    nya_assert(shaped != nullptr && out_width != nullptr && out_height != nullptr);

    if (!TTF_GetTextSize(shaped, out_width, out_height)) return false;

    TTF_Font*           font = TTF_GetTextFont(shaped);
    const TTF_TextData* data = shaped->internal;

    if (font == nullptr || data == nullptr || !TTF_GetFontSDF(font)) return true;

    // a glyph's image reaches the spread past its ink, which is exactly where SDL_ttf measures it to.
    s32 image_right = 0;

    for (s32 i = 0; i < data->num_ops; i++) {
        const TTF_DrawOperation* op = &data->ops[i];

        if (op->cmd == TTF_DRAW_COMMAND_COPY) image_right = nya_max(image_right, op->copy.dst.x + op->copy.dst.w);
    }

    // past every image, the width is where the advance ends and already right.
    if (*out_width > image_right) return true;

    s32 width = 0;

    for (s32 i = 0; i < data->num_ops; i++) {
        const TTF_DrawOperation* op = &data->ops[i];

        if (op->cmd != TTF_DRAW_COMMAND_COPY) continue;

        width = nya_max(width, op->copy.dst.x + op->copy.dst.w - (2 * NYA_TEXT_SDF_SPREAD));

        // only the last glyph of a line advances to where the line ends: the next one starts a new line further left.
        const TTF_DrawOperation* next = i + 1 < data->num_ops ? &data->ops[i + 1] : nullptr;

        if (next != nullptr && next->cmd == TTF_DRAW_COMMAND_COPY && next->copy.dst.x > op->copy.dst.x) continue;

        NYA_ConstCString cursor    = &shaped->text[op->copy.text_offset];
        size_t           remaining = strlen(cursor);
        u32              codepoint = SDL_StepUTF8(&cursor, &remaining);

        s32 min_x = 0, advance = 0;

        // SDL_ttf's own width stands when the glyph cannot be read back.
        if (!TTF_GetGlyphMetrics(font, codepoint, &min_x, nullptr, nullptr, nullptr, &advance)) return true;

        // the image starts the spread before the ink, which starts min_x after the pen.
        width = nya_max(width, op->copy.dst.x + NYA_TEXT_SDF_SPREAD - min_x + advance);
    }

    *out_width = width;

    return true;
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

    if (!_nya_text_size(shaped, &out_run->width, &out_run->height)) {
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

    // The per-line glyph ranges, in a second pass.
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

    // The wrap width, the handle with its terminator, then the text; the terminator keeps "a@1"+"2x" from matching "a@12"+"x".
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
    NYA_Error inserted = nya_cache_add(_nya_text_run_cache, key, key_size, asset->generation, &slot);
    nya_assert(inserted.ok, "a least recently used cache evicts instead of refusing a key that fits");

    *(TTF_Text**)slot = shaped;

    return shaped;
}

void _nya_text_run_destroy(void* value, void* user_data) {
    nya_unused(user_data);

    TTF_DestroyText(*(TTF_Text**)value);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BIDI (UAX #9, reduced)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A character's bidi class, folded to the four the reordering below needs. */
typedef enum _NYA_BidiClass {
    _NYA_BIDI_L,       /**< strong left-to-right */
    _NYA_BIDI_R,       /**< strong right-to-left; Arabic letters fold in here too */
    _NYA_BIDI_EN,      /**< a digit: runs left-to-right, but resolves a neutral like an R */
    _NYA_BIDI_NEUTRAL, /**< whitespace, punctuation, and anything with no strong direction */
} _NYA_BidiClass;

/** `codepoint`'s bidi class from a compact range table, not the Unicode database. */
NYA_INTERNAL _NYA_BidiClass _nya_bidi_class(u32 codepoint) {
    if (codepoint < 0x80U) {
        if (codepoint >= '0' && codepoint <= '9') return _NYA_BIDI_EN;
        if ((codepoint | 0x20U) >= 'a' && (codepoint | 0x20U) <= 'z') return _NYA_BIDI_L;
        return _NYA_BIDI_NEUTRAL;
    }

    // Hebrew, and the Hebrew presentation forms.
    if ((codepoint >= 0x0590U && codepoint <= 0x05FFU) || (codepoint >= 0xFB1DU && codepoint <= 0xFB4FU)) return _NYA_BIDI_R;

    // Arabic across its blocks and both presentation-form ranges; its own digits are numbers, not letters.
    if ((codepoint >= 0x0600U && codepoint <= 0x06FFU) || (codepoint >= 0x0750U && codepoint <= 0x077FU) ||
        (codepoint >= 0x08A0U && codepoint <= 0x08FFU) || (codepoint >= 0xFB50U && codepoint <= 0xFDFFU) ||
        (codepoint >= 0xFE70U && codepoint <= 0xFEFFU)) {
        if ((codepoint >= 0x0660U && codepoint <= 0x0669U) || (codepoint >= 0x06F0U && codepoint <= 0x06F9U)) return _NYA_BIDI_EN;
        return _NYA_BIDI_R;
    }

    // Everything else (Latin, CJK, and the rest) reads left-to-right.
    return _NYA_BIDI_L;
}

/** Appends [offset, offset + length) at `level`, extending the last run when the level and bytes run straight on. */
NYA_INTERNAL void _nya_bidi_emit(NYA_BidiRun* runs, u32* count, u32 capacity, u32 offset, u32 length, u8 level) {
    if (length == 0) return;

    if (*count > 0 && runs[*count - 1].level == level && runs[*count - 1].offset + runs[*count - 1].length == offset) {
        runs[*count - 1].length += length;
        return;
    }

    // Bounded: a line with more runs than the cap keeps the ones it has, the way a run keeps its first glyphs.
    if (*count >= capacity) return;

    runs[(*count)++] = (NYA_BidiRun){ .offset = offset, .length = length, .level = level };
}

u32 nya_text_bidi_runs(NYA_ConstCString text, u64 length, OUT NYA_BidiRun* out_runs, u32 capacity) {
    nya_assert(out_runs != nullptr);

    if (text == nullptr || capacity == 0) return 0;

    u64 len = length > 0 ? length : (u64)strlen(text);
    if (len == 0) return 0;

    // P2/P3: the base direction is the first strong character's, left-to-right when there is none.
    b8 base_rtl = false;
    for (u64 i = 0; i < len;) {
        u32 codepoint = 0;
        i += nya_utf8_next(text + i, &codepoint);

        _NYA_BidiClass first = _nya_bidi_class(codepoint);
        if (first == _NYA_BIDI_L) break;
        if (first == _NYA_BIDI_R) {
            base_rtl = true;
            break;
        }
    }

    // A left-to-right run sits one level above a right-to-left paragraph, so digits and Latin nest inside it.
    u8 even_level = base_rtl ? 2U : 0U;

    u32 count       = 0;
    u8  prev_strong = base_rtl ? 1U : 0U; // the side (0 left, 1 right) neutrals lean toward; a number counts as right.
    u64 pending     = 0;                  // the byte a still-open neutral run began at.
    b8  has_pending = false;

    for (u64 i = 0; i < len;) {
        u64 start     = i;
        u32 codepoint = 0;
        i += nya_utf8_next(text + i, &codepoint);

        _NYA_BidiClass character = _nya_bidi_class(codepoint);

        if (character == _NYA_BIDI_NEUTRAL) {
            if (!has_pending) {
                pending     = start;
                has_pending = true;
            }
            continue;
        }

        u8 side  = character == _NYA_BIDI_L ? 0U : 1U;                     // L leans left; R and a digit lean right.
        u8 level = character == _NYA_BIDI_R ? 1U : even_level;            // a digit runs left-to-right within the line.

        if (has_pending) {
            // N1/N2: a neutral run between two equal sides takes that side, otherwise the paragraph's.
            u8 neutral_side  = prev_strong == side ? side : (base_rtl ? 1U : 0U);
            u8 neutral_level = neutral_side == 1U ? 1U : even_level;

            _nya_bidi_emit(out_runs, &count, capacity, (u32)pending, (u32)(start - pending), neutral_level);
            has_pending = false;
        }

        _nya_bidi_emit(out_runs, &count, capacity, (u32)start, (u32)(i - start), level);
        prev_strong = side;
    }

    // A trailing neutral run takes the paragraph side: its far neighbour is the paragraph, so the two never agree away from it.
    if (has_pending) _nya_bidi_emit(out_runs, &count, capacity, (u32)pending, (u32)(len - pending), base_rtl ? 1U : even_level);

    // L1/L2: from the highest level down to the lowest odd one, reverse each maximal stretch of runs at that level or above.
    u8 max_level = 0;
    u8 min_odd   = 0xFFU;

    for (u32 r = 0; r < count; r++) {
        if (out_runs[r].level > max_level) max_level = out_runs[r].level;
        if ((out_runs[r].level & 1U) != 0 && out_runs[r].level < min_odd) min_odd = out_runs[r].level;
    }

    for (u8 level = max_level; min_odd != 0xFFU && level >= min_odd; level--) {
        for (u32 r = 0; r < count;) {
            if (out_runs[r].level < level) {
                r++;
                continue;
            }

            u32 end = r;
            while (end < count && out_runs[end].level >= level) end++;

            for (u32 a = r, b = end - 1; a < b; a++, b--) {
                NYA_BidiRun swap = out_runs[a];
                out_runs[a]      = out_runs[b];
                out_runs[b]      = swap;
            }

            r = end;
        }
    }

    return count;
}

b8 _nya_text_maybe_rtl(NYA_ConstCString text, u64 length) {
    if (text == nullptr) return false;

    u64 len = length > 0 ? length : (u64)strlen(text);

    // The right-to-left blocks begin at U+0590, whose first UTF-8 byte is 0xD6; below that no byte can open one.
    for (u64 i = 0; i < len; i++) {
        if ((u8)text[i] >= 0xD6U) return true;
    }

    return false;
}

b8 _nya_text_bidi_needed(NYA_ConstCString text, u64 length, OUT NYA_BidiRun* runs, OUT u32* out_count) {
    nya_assert(runs != nullptr && out_count != nullptr);

    *out_count = 0;

    // The byte gate keeps Latin and ASCII off the codepoint walk entirely; only a possible right-to-left line pays it.
    if (!_nya_text_maybe_rtl(text, length)) return false;

    u32 count = nya_text_bidi_runs(text, length, runs, NYA_BIDI_RUNS_MAX);

    // A false alarm (a non-Latin left-to-right script like CJK) shapes as one left-to-right run, so keep the fast path.
    for (u32 r = 0; r < count; r++) {
        if ((runs[r].level & 1U) != 0) {
            *out_count = count;
            return true;
        }
    }

    return false;
}

b8 _nya_text_shape_runs(TTF_Font* font, NYA_ConstCString text, u64 length, const NYA_BidiRun* runs, u32 count, OUT NYA_TextRun* out_run) {
    nya_assert(font != nullptr && text != nullptr && runs != nullptr && out_run != nullptr);

    _nya_text_run_reset(out_run);

    if (count == 0) return false;

    s32 pen_x  = 0;
    s32 height = (s32)nya_text_line_height(font);

    for (u32 r = 0; r < count; r++) {
        b8 rtl = (runs[r].level & 1U) != 0;

        // Each run shapes on its own, in its own direction: harfbuzz then orders an RTL run's glyphs right-to-left for us.
        TTF_Text* shaped = TTF_CreateText(nullptr, font, text + runs[r].offset, (size_t)runs[r].length);
        if (shaped == nullptr) continue;

        (void)TTF_SetTextDirection(shaped, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);

        if (!TTF_UpdateText(shaped)) {
            TTF_DestroyText(shaped);
            continue;
        }

        s32 width = 0, run_height = 0;
        (void)TTF_GetTextSize(shaped, &width, &run_height);
        if (run_height > height) height = run_height;

        const TTF_TextData* data = shaped->internal;

        for (s32 i = 0; data != nullptr && i < data->num_ops; i++) {
            const TTF_DrawOperation* op = &data->ops[i];

            if (op->cmd != TTF_DRAW_COMMAND_COPY) continue;

            if (out_run->glyph_count >= NYA_TEXT_RUN_GLYPHS_MAX) {
                out_run->overflowed = true;
                break;
            }

            // The run's glyphs are already visually ordered; only the run's left edge shifts, by every run before it.
            out_run->glyphs[out_run->glyph_count++] = (NYA_TextGlyph){
                .glyph_index = op->copy.glyph_index,
                .x           = op->copy.dst.x + pen_x,
                .y           = op->copy.dst.y,
                .width       = op->copy.dst.w,
                .height      = op->copy.dst.h,
                .source_x    = op->copy.src.x,
                .source_y    = op->copy.src.y,
                .line        = 0,
            };
        }

        pen_x += width;
        TTF_DestroyText(shaped);
    }

    out_run->width      = pen_x;
    out_run->height     = height;
    out_run->line_count = 1;
    out_run->lines[0]   = (NYA_TextLine){
        .first_glyph = 0,
        .glyph_count = out_run->glyph_count,
        .width       = pen_x,
        .height      = height,
        .offset      = 0,
        .length      = (u32)(length > 0 ? length : strlen(text)),
    };

    if (out_run->glyph_count > _nya_text_run_glyph_count_worst) _nya_text_run_glyph_count_worst = out_run->glyph_count;
    if (out_run->line_count > _nya_text_run_line_count_worst) _nya_text_run_line_count_worst = out_run->line_count;

    return true;
}
