/**
 * @file render_glyph_atlas.c
 *
 * The CPU side of a glyph atlas: the cell grid, a glyph rasterised into its cell as one coverage byte per
 * texel, and a cell read back out for upload. No GPU state, so both builds include it and a headless test
 * reaches the bake render2d.c runs.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The ASCII range the atlas sizes its cells against, inclusive. */
#define NYA_RENDER2D_GLYPH_FIRST 32
#define NYA_RENDER2D_GLYPH_LAST  126

/**
 * Glyphs one atlas can hold, which sets the atlas height. The busiest atlas in gnyame fills 53 (HUD plus debug
 * overlay) and printable ASCII is 95, so 128 leaves room for accents and ligatures. A full atlas warns once and
 * draws new glyphs blank. Not grown, since baked uvs and queued vertices depend on the texture size. A power of
 * two, because the lookup is a masked hash.
 * */
#ifndef NYA_RENDER2D_GLYPH_CAPACITY
#define NYA_RENDER2D_GLYPH_CAPACITY 128
#endif

/** Buckets in an atlas's glyph-index lookup. A power of two, because the index is a masked hash. */
#define NYA_RENDER2D_GLYPH_LOOKUP (NYA_RENDER2D_GLYPH_CAPACITY * 4)

/** Cells across the atlas texture. Rows follow from the capacity. */
#define NYA_RENDER2D_GLYPH_COLUMNS 16

/** How far SDL_ttf extends a distance field past the ink on every side, in texels. Its DEFAULT_SDF_SPREAD, unexported. */
#define NYA_RENDER2D_GLYPH_SDF_SPREAD 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Glyph     NYA_Glyph;
typedef struct NYA_GlyphGrid NYA_GlyphGrid;

/**
 * One glyph's place in the atlas, in pixels except the uvs. Bearing and advance depend on the string
 * around a glyph, so they live on NYA_TextGlyph, which the shaper fills.
 * */
struct NYA_Glyph {
    f32 u0, v0, u1, v1;

    f32 width, height;
};

/** A fixed grid of equal cells, so a slot's place is a multiply instead of a rectangle packer. In texels. */
struct NYA_GlyphGrid {
    s32 cell_width;
    s32 cell_height;
    s32 atlas_width;
    s32 atlas_height;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The grid for a face: cells sized against its widest ASCII glyph and its line box, with a one texel gutter. A
 * distance field face gets room for the spread as well, so it has to be set before the grid is taken.
 * */
NYA_INTERNAL __attr_allow_unused NYA_GlyphGrid _nya_render2d_glyph_grid(TTF_Font* font) __attr_no_discard;

/** The top left texel of `slot`'s cell. */
NYA_INTERNAL __attr_allow_unused void _nya_render2d_glyph_cell(NYA_GlyphGrid grid, u32 slot, OUT s32* out_x, OUT s32* out_y);

/** Which lookup bucket a glyph index maps to. Mixed, so adjacent indices do not collide. */
NYA_INTERNAL __attr_allow_unused u32 _nya_render2d_glyph_bucket(u32 glyph_index) __attr_no_discard;

/**
 * Rasterises one glyph index of `font` into `slot`'s cell of `coverage`, which is `grid.atlas_width *
 * grid.atlas_height` bytes. A zero glyph when the face has no such glyph, and the cell is left alone.
 * */
NYA_INTERNAL __attr_allow_unused NYA_Glyph _nya_render2d_glyph_rasterize(u8* coverage, NYA_GlyphGrid grid, TTF_Font* font, u32 glyph_index, u32 slot);

/**
 * Clears `slot`'s cell and copies the alpha of `width` by `height` RGBA32 pixels into it, clipped to the
 * cell inside its gutter. A zero glyph, and the cell left alone, when the image is empty.
 * */
NYA_INTERNAL __attr_allow_unused NYA_Glyph _nya_render2d_glyph_cell_write(u8* coverage, NYA_GlyphGrid grid, u32 slot, const u8* pixels, s32 width,
                                                                          s32 height, s32 pitch);

/** Copies `slot`'s cell out of `coverage` into `out`, `grid.cell_width * grid.cell_height` bytes row by row. */
NYA_INTERNAL __attr_allow_unused void _nya_render2d_glyph_cell_read(const u8* coverage, NYA_GlyphGrid grid, u32 slot, OUT u8* out);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_GlyphGrid _nya_render2d_glyph_grid(TTF_Font* font) {
    nya_assert(font != nullptr);

    /* A cell holds a glyph's cropped ink image, so sizing it against the line box is conservative. */
    s32 widest  = 1;
    s32 tallest = nya_max(TTF_GetFontHeight(font), TTF_GetFontLineSkip(font));

    for (s32 character = NYA_RENDER2D_GLYPH_FIRST; character <= NYA_RENDER2D_GLYPH_LAST; character++) {
        s32 min_x = 0, max_x = 0, min_y = 0, max_y = 0, advance = 0;
        if (!TTF_GetGlyphMetrics(font, (u32)character, &min_x, &max_x, &min_y, &max_y, &advance)) continue;

        // the wider of advance and ink, so overhanging glyphs are not clipped.
        widest = nya_max(widest, nya_max(advance, max_x));
    }

    /*
     * Half again wider than ASCII needs: cells are sized once, and Latin Extended glyphs run about a third wider
     * than the widest ASCII one. Too small a cell clips those glyphs silently. The two is a one texel gutter on
     * each side, so linear filtering does not bleed in the neighbouring glyph.
     */
    s32 cell_width  = ((widest * 3) / 2) + 2;
    s32 cell_height = ((tallest * 3) / 2) + 2;

    // a fixed width on both sides of the ink, so the half again above does not cover it at small sizes.
    if (TTF_GetFontSDF(font)) {
        cell_width  += 2 * NYA_RENDER2D_GLYPH_SDF_SPREAD;
        cell_height += 2 * NYA_RENDER2D_GLYPH_SDF_SPREAD;
    }

    s32 rows = (NYA_RENDER2D_GLYPH_CAPACITY + NYA_RENDER2D_GLYPH_COLUMNS - 1) / NYA_RENDER2D_GLYPH_COLUMNS;

    return (NYA_GlyphGrid){
        .cell_width   = cell_width,
        .cell_height  = cell_height,
        .atlas_width  = cell_width * NYA_RENDER2D_GLYPH_COLUMNS,
        .atlas_height = cell_height * rows,
    };
}

void _nya_render2d_glyph_cell(NYA_GlyphGrid grid, u32 slot, OUT s32* out_x, OUT s32* out_y) {
    nya_assert(slot < NYA_RENDER2D_GLYPH_CAPACITY);

    *out_x = (s32)(slot % NYA_RENDER2D_GLYPH_COLUMNS) * grid.cell_width;
    *out_y = (s32)(slot / NYA_RENDER2D_GLYPH_COLUMNS) * grid.cell_height;

    nya_assert(*out_x + grid.cell_width <= grid.atlas_width && *out_y + grid.cell_height <= grid.atlas_height);
}

/* The attribute is required: this multiply overflows on purpose, and the sanitized build aborts without it. */
__attr_no_sanitize("unsigned-integer-overflow") u32 _nya_render2d_glyph_bucket(u32 glyph_index) {
    // mixed, because glyph indices in one script are consecutive and would collide along a word.
    u32 hash = glyph_index * 2654435761U;

    return (hash >> 16) & (NYA_RENDER2D_GLYPH_LOOKUP - 1);
}

NYA_Glyph _nya_render2d_glyph_rasterize(u8* coverage, NYA_GlyphGrid grid, TTF_Font* font, u32 glyph_index, u32 slot) {
    nya_assert(font != nullptr);

    /* By glyph index, as shaped: a ligature has no codepoint, and a mark cluster has several glyphs for one. */
    TTF_ImageType image_type    = TTF_IMAGE_INVALID;
    SDL_Surface*  glyph_surface = TTF_GetGlyphImageForIndex(font, glyph_index, &image_type);

    // no such glyph, or it failed to rasterise: the cell stays empty.
    if (glyph_surface == nullptr) return (NYA_Glyph){ 0 };

    defer SDL_DestroySurface(glyph_surface);

    /* Converted when the format differs, since only RGBA32 puts alpha in an indexable byte. */
    SDL_Surface* converted = glyph_surface->format == SDL_PIXELFORMAT_RGBA32 ? nullptr : SDL_ConvertSurface(glyph_surface, SDL_PIXELFORMAT_RGBA32);

    defer SDL_DestroySurface(converted);

    const SDL_Surface* source = converted != nullptr ? converted : glyph_surface;
    if (source->format != SDL_PIXELFORMAT_RGBA32) return (NYA_Glyph){ 0 };

    // a distance field comes through the same way, with the distance in alpha.
    return _nya_render2d_glyph_cell_write(coverage, grid, slot, source->pixels, source->w, source->h, source->pitch);
}

NYA_Glyph _nya_render2d_glyph_cell_write(u8* coverage, NYA_GlyphGrid grid, u32 slot, const u8* pixels, s32 width, s32 height, s32 pitch) {
    nya_assert(coverage != nullptr);

    s32 cell_x = 0;
    s32 cell_y = 0;
    _nya_render2d_glyph_cell(grid, slot, &cell_x, &cell_y);

    // clipped rather than spilling into the neighbouring cell.
    s32 clipped_width  = nya_min(width, grid.cell_width - 2);
    s32 clipped_height = nya_min(height, grid.cell_height - 2);

    if (clipped_width <= 0 || clipped_height <= 0) return (NYA_Glyph){ 0 };

    nya_assert(pixels != nullptr);
    nya_assert(pitch >= width * 4, "an RGBA32 row is four bytes a pixel, got a pitch of %d for %d wide", pitch, width);

    // cleared first: a re-bake after a reload would otherwise show old ink.
    for (s32 row = cell_y; row < cell_y + grid.cell_height; row++) {
        nya_memset(coverage + ((size_t)row * (size_t)grid.atlas_width) + (size_t)cell_x, 0, (size_t)grid.cell_width);
    }

    /*
     * The glyph's alpha becomes the coverage. Coverage is kept rather than thresholded: with pixel-snapped quads
     * and nearest sampling, one pixel maps to one texel, so the antialiasing survives unblurred.
     */
    for (s32 y = 0; y < clipped_height; y++) {
        const u8* source_row = pixels + ((size_t)y * (size_t)pitch);
        u8*       atlas_row  = coverage + ((size_t)(cell_y + 1 + y) * (size_t)grid.atlas_width) + (size_t)(cell_x + 1);

        // SDL_PIXELFORMAT_RGBA32 puts alpha in the last byte on either endianness.
        for (s32 x = 0; x < clipped_width; x++) atlas_row[x] = source_row[((size_t)x * 4) + 3];
    }

    return (NYA_Glyph){
        .u0     = (f32)(cell_x + 1) / (f32)grid.atlas_width,
        .v0     = (f32)(cell_y + 1) / (f32)grid.atlas_height,
        .u1     = (f32)(cell_x + 1 + clipped_width) / (f32)grid.atlas_width,
        .v1     = (f32)(cell_y + 1 + clipped_height) / (f32)grid.atlas_height,
        .width  = (f32)clipped_width,
        .height = (f32)clipped_height,
    };
}

void _nya_render2d_glyph_cell_read(const u8* coverage, NYA_GlyphGrid grid, u32 slot, OUT u8* out) {
    nya_assert(coverage != nullptr);
    nya_assert(out != nullptr);

    s32 cell_x = 0;
    s32 cell_y = 0;
    _nya_render2d_glyph_cell(grid, slot, &cell_x, &cell_y);

    for (s32 row = 0; row < grid.cell_height; row++) {
        const u8* source = coverage + ((size_t)(cell_y + row) * (size_t)grid.atlas_width) + (size_t)cell_x;
        nya_memcpy(out + ((size_t)row * (size_t)grid.cell_width), source, (size_t)grid.cell_width);
    }
}
