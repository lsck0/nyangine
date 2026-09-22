/**
 * The CPU side of a glyph atlas: where cells sit, what a bake writes into the coverage, and what an upload
 * reads back out. render2d.c runs all of it before a texture is touched, so none of it needs a device.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** A font that is actually in the repository. */
#define FACE "./assets/fonts/Aldrich.ttf"

#define ROWS (((NYA_RENDER2D_GLYPH_CAPACITY) + (NYA_RENDER2D_GLYPH_COLUMNS) - 1) / (NYA_RENDER2D_GLYPH_COLUMNS))

/** Small odd cells, so an off by one in either axis lands on a different texel. */
static const NYA_GlyphGrid GRID = {
    .cell_width   = 7,
    .cell_height  = 9,
    .atlas_width  = 7 * NYA_RENDER2D_GLYPH_COLUMNS,
    .atlas_height = 9 * ROWS,
};

/** Every printable ASCII character, shaped once per face. */
static const char PRINTABLE[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

/** Too large for the stack. */
static NYA_TextRun run;

/**
 * Zeroed coverage for `grid`. The arena poisons the padding after every allocation, and what lies before
 * one is either the previous allocation's padding or the region's own heap redzone, so the sanitizer
 * still sees a write past either end.
 */
static u8* coverage_create(NYA_Arena* arena, NYA_GlyphGrid grid) {
    u64 size     = (u64)grid.atlas_width * (u64)grid.atlas_height;
    u8* coverage = nya_arena_alloc(arena, size);
    nya_assert(coverage != nullptr);
    nya_memset(coverage, 0, size);

    return coverage;
}

/** Nonzero texels outside `slot`'s cell. */
static u32 ink_outside(const u8* coverage, NYA_GlyphGrid grid, u32 slot) {
    s32 cell_x = 0;
    s32 cell_y = 0;
    _nya_render2d_glyph_cell(grid, slot, &cell_x, &cell_y);

    u32 count = 0;
    for (s32 y = 0; y < grid.atlas_height; y++) {
        for (s32 x = 0; x < grid.atlas_width; x++) {
            b8 inside = x >= cell_x && x < cell_x + grid.cell_width && y >= cell_y && y < cell_y + grid.cell_height;
            if (!inside && coverage[((size_t)y * (size_t)grid.atlas_width) + (size_t)x] != 0) count++;
        }
    }

    return count;
}

/** The alpha the test images carry at (x, y). Never zero, so a texel that was not copied shows. */
static u8 alpha_at(s32 x, s32 y) {
    return (u8)(1 + (((x * 13) + (y * 7)) % 254));
}

/** An RGBA32 image of `alpha_at` with zero colour, so a copy of the wrong channel shows. Rows are `pitch` apart. */
static u8* image_create(NYA_Arena* arena, s32 width, s32 height, s32 pitch) {
    u64 size   = (u64)pitch * (u64)height;
    u8* pixels = nya_arena_alloc(arena, size);
    nya_assert(pixels != nullptr);
    // zeroed: the colour channels and the padding past `width` must read as nothing
    nya_memset(pixels, 0, size);

    for (s32 y = 0; y < height; y++) {
        for (s32 x = 0; x < width; x++) pixels[((size_t)y * (size_t)pitch) + ((size_t)x * 4) + 3] = alpha_at(x, y);
    }

    return pixels;
}

/** Whether `slot`'s cell holds the first `width` by `height` of the test image, inside an empty gutter. */
static b8 cell_holds_image(const u8* coverage, NYA_GlyphGrid grid, u32 slot, s32 width, s32 height) {
    s32 cell_x = 0;
    s32 cell_y = 0;
    _nya_render2d_glyph_cell(grid, slot, &cell_x, &cell_y);

    for (s32 y = 0; y < grid.cell_height; y++) {
        for (s32 x = 0; x < grid.cell_width; x++) {
            b8 ink  = x >= 1 && x <= width && y >= 1 && y <= height;
            u8 want = ink ? alpha_at(x - 1, y - 1) : 0;
            u8 got  = coverage[((size_t)(cell_y + y) * (size_t)grid.atlas_width) + (size_t)(cell_x + x)];

            if (got != want) return false;
        }
    }

    return true;
}

/** Rasterises every printable ASCII glyph of `font` into its own slot and checks none is clipped or spills. */
static void check_face(TTF_Font* font, NYA_ConstCString label, b8 distance_field) {
    NYA_GlyphGrid grid = _nya_render2d_glyph_grid(font);

    nya_check(grid.atlas_width == grid.cell_width * NYA_RENDER2D_GLYPH_COLUMNS && grid.atlas_height == grid.cell_height * ROWS,
              "%s: the atlas is exactly the grid, %dx%d for %dx%d cells", label, grid.atlas_width, grid.atlas_height, grid.cell_width,
              grid.cell_height);

    nya_check(nya_text_shape(font, PRINTABLE, 0, 0, &run), "%s: printable ASCII shapes", label);
    nya_check(run.glyph_count > 0 && run.glyph_count <= NYA_RENDER2D_GLYPH_CAPACITY, "%s: into a slot each, got %u", label, run.glyph_count);

    NYA_Arena* arena = nya_arena_create(.name = "check_face");
    defer      nya_arena_destroy(arena);

    u8* coverage = coverage_create(arena, grid);

    u8 darkest = 0;

    for (u32 slot = 0; slot < run.glyph_count; slot++) {
        u32 glyph_index = run.glyphs[slot].glyph_index;

        SDL_Surface* image = TTF_GetGlyphImageForIndex(font, glyph_index, nullptr);
        nya_check(image != nullptr, "%s: glyph %u has an image", label, glyph_index);
        if (image == nullptr) continue;

        s32 image_width  = image->w;
        s32 image_height = image->h;
        SDL_DestroySurface(image);

        NYA_Glyph glyph = _nya_render2d_glyph_rasterize(coverage, grid, font, glyph_index, slot);

        nya_check((s32)glyph.width == image_width && (s32)glyph.height == image_height, "%s: glyph %u is %dx%d, baked %.0fx%.0f in a %dx%d cell",
                  label, glyph_index, image_width, image_height, (f64)glyph.width, (f64)glyph.height, grid.cell_width, grid.cell_height);
    }

    for (u32 slot = 0; slot < run.glyph_count; slot++) {
        s32 cell_x = 0;
        s32 cell_y = 0;
        _nya_render2d_glyph_cell(grid, slot, &cell_x, &cell_y);

        // the gutter stays empty around every cell.
        for (s32 x = 0; x < grid.cell_width; x++) {
            nya_check(coverage[((size_t)cell_y * (size_t)grid.atlas_width) + (size_t)(cell_x + x)] == 0, "%s: slot %u top gutter", label, slot);
        }
        for (s32 y = 0; y < grid.cell_height; y++) {
            nya_check(coverage[((size_t)(cell_y + y) * (size_t)grid.atlas_width) + (size_t)cell_x] == 0, "%s: slot %u left gutter", label, slot);
        }
    }

    for (u64 i = 0; i < (u64)grid.atlas_width * (u64)grid.atlas_height; i++) darkest = nya_max(darkest, coverage[i]);

    // a distance field puts the outline at 128 and rises inside it; coverage saturates inside the ink.
    if (distance_field) {
        nya_check(darkest > 128, "%s: the inside of some glyph reads above the outline, got %u at most", label, darkest);
    } else {
        nya_check(darkest == 255, "%s: somewhere a glyph covers a texel fully, got %u at most", label, darkest);
    }
}

s32 main(void) {
    // ── Cells tile the atlas: a slot's place is a multiply, and the last cell ends on the atlas edge.
    {
        s32 x = -1;
        s32 y = -1;

        _nya_render2d_glyph_cell(GRID, 0, &x, &y);
        nya_check(x == 0 && y == 0, "slot 0 is the top left cell, got %d,%d", x, y);

        _nya_render2d_glyph_cell(GRID, NYA_RENDER2D_GLYPH_COLUMNS - 1, &x, &y);
        nya_check(x == GRID.atlas_width - GRID.cell_width && y == 0, "the last slot of a row ends on the right edge, got %d,%d", x, y);

        _nya_render2d_glyph_cell(GRID, NYA_RENDER2D_GLYPH_COLUMNS, &x, &y);
        nya_check(x == 0 && y == GRID.cell_height, "the next slot starts the second row, got %d,%d", x, y);

        _nya_render2d_glyph_cell(GRID, NYA_RENDER2D_GLYPH_CAPACITY - 1, &x, &y);
        nya_check(x + GRID.cell_width == GRID.atlas_width && y + GRID.cell_height == GRID.atlas_height, "the last slot ends on the corner, got %d,%d",
                  x, y);
    }

    // ── The lookup hash wraps on purpose. Sanitized builds abort on the first wrap without the attribute.
    {
        u32 out_of_range = 0;
        u32 adjacent     = 0;

        for (u32 index = 0; index < 65536; index++) {
            u32 bucket = _nya_render2d_glyph_bucket(index);
            if (bucket >= NYA_RENDER2D_GLYPH_LOOKUP) out_of_range++;
            if (bucket == _nya_render2d_glyph_bucket(index + 1)) adjacent++;
        }

        const u32 extremes[] = { 0x7FFFFFFFU, 0x80000000U, 2654435761U, U32_MAX - 1, U32_MAX };
        for (u32 i = 0; i < nya_carray_length(extremes); i++) {
            if (_nya_render2d_glyph_bucket(extremes[i]) >= NYA_RENDER2D_GLYPH_LOOKUP) out_of_range++;
        }

        nya_check(out_of_range == 0, "every bucket is inside the lookup, %u were not", out_of_range);
        nya_check(adjacent == 0, "consecutive glyph indices never share a bucket, %u did", adjacent);
    }

    // ── A write lands one texel in from the cell's corner, copies alpha only, and nothing else changes.
    {
        NYA_Arena* arena = nya_arena_create(.name = "glyph_cell");
        defer      nya_arena_destroy(arena);

        const u32 slot   = NYA_RENDER2D_GLYPH_COLUMNS + 1;
        u8*       atlas  = coverage_create(arena, GRID);
        u8*       pixels = image_create(arena, 4, 5, 4 * 4);

        NYA_Glyph glyph = _nya_render2d_glyph_cell_write(atlas, GRID, slot, pixels, 4, 5, 4 * 4);

        nya_check(cell_holds_image(atlas, GRID, slot, 4, 5), "the cell holds the image's alpha inside an empty gutter");
        nya_check(ink_outside(atlas, GRID, slot) == 0, "and nothing outside the cell was written");
        nya_check(glyph.width == 4.0F && glyph.height == 5.0F, "the glyph is the image's size, got %.0fx%.0f", (f64)glyph.width, (f64)glyph.height);

        // slot 17 is column 1, row 1.
        f32 u0 = (f32)(GRID.cell_width + 1) / (f32)GRID.atlas_width;
        f32 v0 = (f32)(GRID.cell_height + 1) / (f32)GRID.atlas_height;
        f32 u1 = (f32)(GRID.cell_width + 5) / (f32)GRID.atlas_width;
        f32 v1 = (f32)(GRID.cell_height + 6) / (f32)GRID.atlas_height;
        nya_check(glyph.u0 == u0 && glyph.v0 == v0 && glyph.u1 == u1 && glyph.v1 == v1, "the uvs frame exactly the copied texels");

        // what an upload sends is the cell, gutter included, row by row.
        u8 cell[7 * 9];
        _nya_render2d_glyph_cell_read(atlas, GRID, slot, cell);

        b8 matches = true;
        for (s32 y = 0; y < GRID.cell_height; y++) {
            for (s32 x = 0; x < GRID.cell_width; x++) {
                b8 ink  = x >= 1 && x <= 4 && y >= 1 && y <= 5;
                u8 want = ink ? alpha_at(x - 1, y - 1) : 0;
                if (cell[(y * GRID.cell_width) + x] != want) matches = false;
            }
        }
        nya_check(matches, "reading the cell back gives the same texels");

        _nya_render2d_glyph_cell_read(atlas, GRID, slot + 1, cell);

        b8 empty = true;
        for (u32 i = 0; i < sizeof(cell); i++) empty = empty && cell[i] == 0;
        nya_check(empty, "and reading its neighbour gives nothing");
    }

    // ── Rows are `pitch` apart, which SDL may pad past four bytes a pixel.
    {
        NYA_Arena* arena = nya_arena_create(.name = "glyph_cell");
        defer      nya_arena_destroy(arena);

        u8* atlas  = coverage_create(arena, GRID);
        u8* pixels = image_create(arena, 3, 3, (3 * 4) + 5);

        (void)_nya_render2d_glyph_cell_write(atlas, GRID, 2, pixels, 3, 3, (3 * 4) + 5);
        nya_check(cell_holds_image(atlas, GRID, 2, 3, 3), "a padded image copies as if it were packed");
    }

    // ── An image larger than the cell is clipped to it, even in the last cell, where a spill leaves the buffer.
    {
        NYA_Arena* arena = nya_arena_create(.name = "glyph_cell");
        defer      nya_arena_destroy(arena);

        const u32 slot  = NYA_RENDER2D_GLYPH_CAPACITY - 1;
        u8*       atlas = coverage_create(arena, GRID);

        s32 width  = GRID.cell_width + 10;
        s32 height = GRID.cell_height + 10;
        u8* pixels = image_create(arena, width, height, width * 4);

        NYA_Glyph glyph = _nya_render2d_glyph_cell_write(atlas, GRID, slot, pixels, width, height, width * 4);

        nya_check(glyph.width == (f32)(GRID.cell_width - 2) && glyph.height == (f32)(GRID.cell_height - 2), "clipped to the cell inside its gutter, got %.0fx%.0f",
                  (f64)glyph.width, (f64)glyph.height);
        nya_check(cell_holds_image(atlas, GRID, slot, GRID.cell_width - 2, GRID.cell_height - 2), "with the gutter still empty");
        nya_check(ink_outside(atlas, GRID, slot) == 0, "and nothing written outside the cell");
        nya_check(glyph.u1 == (f32)(GRID.atlas_width - 1) / (f32)GRID.atlas_width && glyph.v1 == (f32)(GRID.atlas_height - 1) / (f32)GRID.atlas_height,
                  "the uvs stop one texel short of the atlas edge");
    }

    // ── An empty image leaves the cell alone; a smaller glyph over a larger one leaves no old ink.
    {
        NYA_Arena* arena = nya_arena_create(.name = "glyph_cell");
        defer      nya_arena_destroy(arena);

        const u32 slot  = 3;
        u8*       atlas = coverage_create(arena, GRID);
        u8*       large = image_create(arena, 5, 7, 5 * 4);

        (void)_nya_render2d_glyph_cell_write(atlas, GRID, slot, large, 5, 7, 5 * 4);

        NYA_Glyph nothing = _nya_render2d_glyph_cell_write(atlas, GRID, slot, nullptr, 0, 0, 0);
        nya_check(nothing.width == 0.0F && nothing.u1 == 0.0F, "an empty image bakes a zero glyph");
        nya_check(cell_holds_image(atlas, GRID, slot, 5, 7), "and does not touch the cell");

        u8* small = image_create(arena, 2, 2, 2 * 4);

        (void)_nya_render2d_glyph_cell_write(atlas, GRID, slot, small, 2, 2, 2 * 4);
        nya_check(cell_holds_image(atlas, GRID, slot, 2, 2), "a smaller glyph written over it replaces it whole");
    }

    // ── A real face, as coverage and as a distance field, at the sizes gnyame bakes.
    {
        b8 sdl_ok = SDL_Init(0);
        nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());
        defer SDL_Quit();

        nya_assert(TTF_Init(), "TTF_Init failed: %s", SDL_GetError());
        defer TTF_Quit();

        const f32 sizes[] = { 17.0F, 22.0F, 28.0F, 44.0F };

        for (u32 i = 0; i < nya_carray_length(sizes); i++) {
            TTF_Font* font = TTF_OpenFont(FACE, sizes[i]);
            nya_assert(font != nullptr, "TTF_OpenFont failed: %s", SDL_GetError());
            defer TTF_CloseFont(font);

            char label[32];
            (void)snprintf(label, sizeof(label), "@%.0f coverage", (f64)sizes[i]);
            check_face(font, label, false);

            nya_assert(TTF_SetFontSDF(font, true));
            (void)snprintf(label, sizeof(label), "@%.0f distance field", (f64)sizes[i]);
            check_face(font, label, true);
        }
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
