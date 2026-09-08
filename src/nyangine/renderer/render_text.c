/**
 * @file render_text.c
 *
 * Shaping, through SDL3_ttf and therefore through HarfBuzz. See render_text.h for why.
 *
 * The whole file is one translation of `TTF_Text` into NYA_TextRun. Nothing here rasterises, opens a
 * device or knows what a window is, which is deliberate — it is what lets the headless build measure
 * text the same way the real one lays it out.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The largest glyph_count/line_count any single run built this process has needed.
 *
 * NYA_TEXT_RUN_GLYPHS_MAX/_LINES_MAX cap one NYA_TextRun, and a run is a stack value built fresh
 * per call rather than a persistent object — there is no one run to point the ceiling registry at,
 * so this tracks the closest any run has come instead. Same trade as _nya_render2d_glyph_count_worst.
 * */
NYA_INTERNAL u32 _nya_text_run_glyph_count_worst = 0;
NYA_INTERNAL u32 _nya_text_run_line_count_worst  = 0;

/**
 * Fills in the run's per-line boxes from the laid-out text.
 *
 * The lines come from SDL_ttf rather than from counting newlines here, because with a wrap width it
 * is the shaper that decided where the breaks went — and even without one, an embedded newline is
 * only one of the things that can start a line.
 * */
NYA_INTERNAL void _nya_text_collect_lines(TTF_Text* text, OUT NYA_TextRun* run) {
    s32 line_count = text->num_lines > 0 ? text->num_lines : 1;

    if ((u32)line_count > NYA_TEXT_RUN_LINES_MAX) {
        line_count     = NYA_TEXT_RUN_LINES_MAX;
        run->overflowed = true;
    }

    for (s32 line = 0; line < line_count; line++) {
        TTF_SubString substring = { 0 };

        // Zeroed on failure rather than skipped, so line indices stay dense and a glyph's `line` is
        // always a valid subscript into this array.
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
 *
 * Geometric rather than looked up through the cluster table: a copy operation carries a byte offset
 * into the text, but so does every other glyph in its cluster, and mapping offsets back to lines
 * would mean a search per glyph. Its `y` already came from the line it was laid out on.
 * */
NYA_INTERNAL u32 _nya_text_line_of(const NYA_TextRun* run, s32 y) {
    for (u32 line = 0; line < run->line_count; line++) {
        const NYA_TextLine* candidate = &run->lines[line];
        if (y >= candidate->y && y < candidate->y + candidate->height) return line;
    }

    // Past the last line box, which a glyph whose ink overshoots its line can be. Clamped rather than
    // dropped: the glyph is real and has to be drawn somewhere.
    return run->line_count > 0 ? run->line_count - 1 : 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_text_shape(TTF_Font* font, NYA_ConstCString text, u64 length, s32 wrap_width, OUT NYA_TextRun* out_run) {
    nya_assert(out_run != nullptr);

    // Registered once, on the very first call: this runs every time any text is laid out, in both
    // builds, so it is as early as the two counts above ever become meaningful.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("text_run_glyphs", NYA_TEXT_RUN_GLYPHS_MAX, &_nya_text_run_glyph_count_worst);
        nya_ceiling_register("text_run_lines", NYA_TEXT_RUN_LINES_MAX, &_nya_text_run_line_count_worst);
        ceiling_registered = true;
    }

    *out_run = (NYA_TextRun){ 0 };

    if (font == nullptr || text == nullptr) return false;

    // An empty string is not a failure. It occupies one line of no width, which is what a caller
    // stacking rows needs it to say.
    if (text[0] == '\0') {
        out_run->line_count = 1;
        out_run->height     = (s32)nya_text_line_height(font);
        out_run->lines[0]   = (NYA_TextLine){ .height = out_run->height };

        return true;
    }

    /*
     * A null engine, which is the whole trick. An engine is what *draws* a laid-out text; the layout
     * itself — shaping, kerning, line breaking, positioning — runs regardless, and reading it back out
     * of `internal->ops` is all this needs. So no device, no renderer, and identical results headless.
     */
    TTF_Text* shaped = TTF_CreateText(nullptr, font, text, (size_t)length);
    if (shaped == nullptr) {
        nya_log_warn("TTF_CreateText() failed while shaping: %s", SDL_GetError());
        return false;
    }

    defer TTF_DestroyText(shaped);

    // Before the layout is read: setting it afterwards would mark the layout stale and the ops already
    // copied out would describe the unwrapped text.
    if (wrap_width > 0 && !TTF_SetTextWrapWidth(shaped, wrap_width)) {
        nya_log_warn("TTF_SetTextWrapWidth() failed: %s", SDL_GetError());
    }

    // Forces the layout now rather than at the first read. `ops` is null until this has run.
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

        // FILL operations are underline and strikethrough rules, which are geometry rather than
        // glyphs. Neither style is exposed by the font API, so they should not appear at all —
        // skipped rather than asserted, since a face could carry one and this is not the place to
        // refuse it.
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
     *
     * Counted rather than tracked while appending because the operations are not guaranteed to be
     * emitted in line order — they are in practice, and a range built on that assumption would be
     * silently wrong the day they are not. Two passes over at most a thousand glyphs costs nothing.
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

f32x2 nya_text_measure_font(TTF_Font* font, NYA_ConstCString text, s32 wrap_width) {
    if (font == nullptr || text == nullptr) return f32x2_zero;

    if (text[0] == '\0') return (f32x2){ 0.0F, nya_text_line_height(font) };

    /*
     * Measured through the same layout the draw uses, but without keeping the glyphs — TTF_GetTextSize
     * reports the box the ops were positioned in. Sharing the layout is the point: a measure that took
     * its own path is a measure that eventually disagrees with what gets drawn, which is how a menu
     * ends up with its highlight one pixel off every entry.
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
    // Flipped, so ascent + descent is the ink height — which is what a caller doing arithmetic with
    // the two expects, and not what SDL returns.
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

/*
 * BODGE: nya_asset_get memoizes its last lookup by the *pointer* it was handed, not its content (see
 * core_asset.c) — correct for the string literals every other caller passes, but wrong for a handle
 * built into a caller-owned buffer. nya_text_font_for used to build one on the stack every call; called
 * back to back for two different sizes (a menu's title, then its items), the compiler reused the same
 * stack slot for both, so the memo's pointer check matched on address alone and handed back the
 * *previous* size's font — which is what made menu items measure at the title's point size and throw
 * their vertical centring off. Interning each derived handle into its own stable slot keeps every
 * (path, size) pair at an address of its own, restoring the pointer-identity assumption the memo relies
 * on. The real fix belongs in the asset system's memo; this just keeps this call site from tripping it.
 */
#define _NYA_TEXT_FONT_HANDLE_INTERN_MAX 32

NYA_INTERNAL char _nya_text_font_handle_intern[_NYA_TEXT_FONT_HANDLE_INTERN_MAX][NYA_TEXT_FONT_HANDLE_MAX] = { 0 };
NYA_INTERNAL u32  _nya_text_font_handle_intern_count                                                      = 0;

NYA_INTERNAL NYA_ConstCString _nya_text_font_handle_stable(NYA_ConstCString path, f32 point_size) {
    char handle[NYA_TEXT_FONT_HANDLE_MAX];
    nya_text_font_handle(path, point_size, handle, sizeof(handle));

    for (u32 i = 0; i < _nya_text_font_handle_intern_count; i++) {
        if (nya_string_equals(_nya_text_font_handle_intern[i], handle)) return _nya_text_font_handle_intern[i];
    }

    if (_nya_text_font_handle_intern_count >= _NYA_TEXT_FONT_HANDLE_INTERN_MAX) {
        // Full: a game legitimately drawing more than 32 distinct font/size pairs is rare enough that
        // raising this ceiling is the right response, not silently degrading back into the bug above.
        nya_log_warn("no free font handle intern slot for '%s'; raise _NYA_TEXT_FONT_HANDLE_INTERN_MAX", handle);
        return nullptr;
    }

    char* slot = _nya_text_font_handle_intern[_nya_text_font_handle_intern_count++];
    (void)snprintf(slot, NYA_TEXT_FONT_HANDLE_MAX, "%s", handle);

    return slot;
}

TTF_Font* nya_text_font_for(NYA_ConstCString path, f32 point_size) {
    if (path == nullptr || path[0] == '\0' || point_size <= 0.0F) return nullptr;

    NYA_ConstCString handle = _nya_text_font_handle_stable(path, point_size);

    // Overflow fallback: the pre-bodge behaviour. Capacity permitting (the common case), unreachable.
    char local_handle[NYA_TEXT_FONT_HANDLE_MAX];
    if (handle == nullptr) {
        nya_text_font_handle(path, point_size, local_handle, sizeof(local_handle));
        handle = local_handle;
    }

    /*
     * Cast rather than a const-correct asset API, matching nya_render2d_procedural and every other call
     * site that has a `const char*` in hand.
     *
     * NYA_AssetHandle is `char*` because the asset system stores the pointer it is given and hands it
     * back; nothing writes through it. Without the cast this warns twice on every build, which is two
     * more warnings than a build should have for something that is not a bug.
     */
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)handle);

    if (asset == nullptr) {
        // Queued, not loaded: the asset system resolves it over the next frames, and every caller here
        // already copes with there being no face yet.
        NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
          .type    = NYA_ASSET_TYPE_FONT,
          .handle  = (NYA_AssetHandle)handle,
          .source  = path,
          .as_font = { .point_size = point_size },
      }), "while queueing a font");

        return nullptr;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;

    return asset->as_font.font;
}
