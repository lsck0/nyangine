/**
 * @file render2d_terminal.c
 *
 * The 2D renderer, drawing into character cells instead of into a swapchain. Selected in place of
 * render2d.c by -DNYA_TERMINAL; see render2d_terminal.h for what it does and does not support, and
 * terminal.h for the device underneath it.
 *
 * Shapes are rasterised per cell against the cell's own centre in pixels, which is why the cell size
 * is in pixels at all: the same arithmetic a window's renderer does, at a coarser grid.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Where the baseline sits in a cell, as a share of its height. Three quarters is what every
 * monospace font puts it at; nothing here can measure a real one, and a caller that positions text
 * against the ascent needs a number that is not zero.
 * */
#define _NYA_RENDER2D_TERMINAL_ASCENT_SHARE 0.75F

/** The character a filled shape paints with. A space shows only its background, which is the fill. */
#define _NYA_RENDER2D_TERMINAL_FILL_GLYPH ' '

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The backend's state: one window, one scissor, and the font the text calls fall back on. */
typedef struct {
    NYA_Window window;

    /** The clip, in pixels. Reset to the whole window every frame, exactly as a scissor is. */
    NYA_Rectf clip;

    /** Mirrored so the measurements that take no font have one to name, as the headless backend does. */
    NYA_ConstCString font_path;
    f32              font_point_size;
} _NYA_Render2DTerminal;

NYA_INTERNAL _NYA_Render2DTerminal _nya_render2d_terminal = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: CELLS AND COLOUR
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** `color` laid over `base`, by `color.a`. What a translucent fill does to the cell under it. */
NYA_INTERNAL u32 _nya_render2d_terminal_blend(u32 base, NYA_Color color) {
    f32 alpha = nya_clamp(color.a, 0.0F, 1.0F);

    f32 red   = (f32)((base >> 16U) & 0xFFU) / 255.0F;
    f32 green = (f32)((base >> 8U) & 0xFFU) / 255.0F;
    f32 blue  = (f32)(base & 0xFFU) / 255.0F;

    return nya_terminal_ink(red + ((color.r - red) * alpha), green + ((color.g - green) * alpha), blue + ((color.b - blue) * alpha));
}

/** `a` and `b` mixed by `t`, component by component. What a gradient's corners are blended with. */
NYA_INTERNAL NYA_Color _nya_render2d_terminal_mix(NYA_Color a, NYA_Color b, f32 t) {
    return (NYA_Color){
        nya_lerp(a.r, b.r, t),
        nya_lerp(a.g, b.g, t),
        nya_lerp(a.b, b.b, t),
        nya_lerp(a.a, b.a, t),
    };
}

/** Whether a pixel is inside the current scissor. One test, at the one place cells are written. */
NYA_INTERNAL b8 _nya_render2d_terminal_clipped(f32 x, f32 y) {
    const NYA_Rectf* clip = &_nya_render2d_terminal.clip;

    return x < clip->x || y < clip->y || x >= clip->x + clip->width || y >= clip->y + clip->height;
}

/**
 * Paints one cell.
 *
 * An opaque fill erases whatever character was there, because a panel drawn over a label has to hide
 * it. A translucent one keeps the character and dims both its ink and its paper, so a scrim over a
 * menu reads as a scrim rather than as an erase.
 * */
NYA_INTERNAL void _nya_render2d_terminal_paint(u16 column, u16 row, NYA_Color color) {
    NYA_TerminalCell cell = nya_terminal_cell_get(column, row);

    cell.background = _nya_render2d_terminal_blend(cell.background, color);

    if (color.a >= 1.0F) {
        cell.codepoint  = _NYA_RENDER2D_TERMINAL_FILL_GLYPH;
        cell.attributes = 0;
    } else {
        cell.foreground = _nya_render2d_terminal_blend(cell.foreground, color);
    }

    nya_terminal_cell_set(column, row, cell);
}

/** The cell range a pixel span covers, clamped to the grid. False when it covers none. */
NYA_INTERNAL b8 _nya_render2d_terminal_span(f32 from, f32 to, u32 cell_size, u16 limit, OUT u16* out_first, OUT u16* out_last) {
    nya_assert(out_first != nullptr && out_last != nullptr);
    nya_assert(cell_size > 0);

    if (to <= from || limit == 0) return false;

    f32 first = floorf(from / (f32)cell_size);
    f32 last  = ceilf(to / (f32)cell_size) - 1.0F;

    if (last < 0.0F || first > (f32)(limit - 1)) return false;

    *out_first = (u16)nya_max(first, 0.0F);
    *out_last  = (u16)nya_min(last, (f32)(limit - 1));

    return true;
}

/** An axis aligned filled rectangle in pixels. Every filled shape here ends up in this. */
NYA_INTERNAL void _nya_render2d_terminal_fill(f32 x, f32 y, f32 width, f32 height, NYA_Color color) {
    if (width <= 0.0F || height <= 0.0F || color.a <= 0.0F) return;

    u16 first_column = 0, last_column = 0, first_row = 0, last_row = 0;

    if (!_nya_render2d_terminal_span(x, x + width, NYA_TERMINAL_CELL_WIDTH_PX, nya_terminal_columns(), &first_column, &last_column)) return;
    if (!_nya_render2d_terminal_span(y, y + height, NYA_TERMINAL_CELL_HEIGHT_PX, nya_terminal_rows(), &first_row, &last_row)) return;

    for (u16 row = first_row; row <= last_row; row++) {
        for (u16 column = first_column; column <= last_column; column++) {
            f32 centre_x = ((f32)column + 0.5F) * (f32)NYA_TERMINAL_CELL_WIDTH_PX;
            f32 centre_y = ((f32)row + 0.5F) * (f32)NYA_TERMINAL_CELL_HEIGHT_PX;

            if (_nya_render2d_terminal_clipped(centre_x, centre_y)) continue;

            _nya_render2d_terminal_paint(column, row, color);
        }
    }
}

/**
 * Every cell whose centre is inside the bounds and passes `covers`. The one loop the shapes that are
 * not rectangles share, so a triangle, a circle and a rotated rectangle differ by a predicate rather
 * than by a copy of this.
 * */
NYA_INTERNAL void _nya_render2d_terminal_scan(NYA_Rectf bounds, NYA_Color color, b8 (*covers)(f32 x, f32 y, const void* shape), const void* shape) {
    nya_assert(covers != nullptr);

    if (color.a <= 0.0F) return;

    u16 first_column = 0, last_column = 0, first_row = 0, last_row = 0;

    if (!_nya_render2d_terminal_span(bounds.x, bounds.x + bounds.width, NYA_TERMINAL_CELL_WIDTH_PX, nya_terminal_columns(), &first_column, &last_column)) {
        return;
    }

    if (!_nya_render2d_terminal_span(bounds.y, bounds.y + bounds.height, NYA_TERMINAL_CELL_HEIGHT_PX, nya_terminal_rows(), &first_row, &last_row)) {
        return;
    }

    for (u16 row = first_row; row <= last_row; row++) {
        for (u16 column = first_column; column <= last_column; column++) {
            f32 centre_x = ((f32)column + 0.5F) * (f32)NYA_TERMINAL_CELL_WIDTH_PX;
            f32 centre_y = ((f32)row + 0.5F) * (f32)NYA_TERMINAL_CELL_HEIGHT_PX;

            if (_nya_render2d_terminal_clipped(centre_x, centre_y)) continue;
            if (!covers(centre_x, centre_y, shape)) continue;

            _nya_render2d_terminal_paint(column, row, color);
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The switches a terminal has no pass to run, so the overlay and a caller both see the truth. */
NYA_INTERNAL NYA_RenderFeatures _nya_render2d_terminal_features(void) {
    return (NYA_RenderFeatures){
        // there is no depth buffer, no sort, and nothing to cull against: everything is painted in
        // the order it was recorded, which is what a cell grid is.
        .frustum_culling   = NYA_RENDER_TOGGLE_OFF,
        .occlusion_culling = NYA_RENDER_TOGGLE_OFF,
        .backface_culling  = NYA_RENDER_TOGGLE_OFF,
        .depth_test        = NYA_RENDER_TOGGLE_OFF,
        .draw_sorting      = NYA_RENDER_TOGGLE_OFF,

        // a cell blends when it is painted, which is the one thing here that really is on.
        .transparency = NYA_RENDER_TOGGLE_ON,

        .shadows      = NYA_RENDER_TOGGLE_OFF,
        .lighting     = NYA_RENDER_TOGGLE_OFF,
        .point_lights = NYA_RENDER_TOGGLE_OFF,
        .reflections  = NYA_RENDER_TOGGLE_OFF,

        // no sampler, no texture. Pictures go out through the kitty protocol instead; see
        // nya_render2d_terminal_image.
        .textures = NYA_RENDER_TOGGLE_OFF,

        .fog       = NYA_RENDER_TOGGLE_OFF,
        .sky       = NYA_RENDER_TOGGLE_OFF,
        .decals    = NYA_RENDER_TOGGLE_OFF,
        .lod       = NYA_RENDER_TOGGLE_OFF,
        .particles = NYA_RENDER_TOGGLE_OFF,
        .haze      = NYA_RENDER_TOGGLE_OFF,

        // the post chain is a set of fullscreen passes over a render texture, and there is neither.
        .post              = NYA_RENDER_TOGGLE_OFF,
        .ink               = NYA_RENDER_TOGGLE_OFF,
        .ambient_occlusion = NYA_RENDER_TOGGLE_OFF,
        .antialias         = NYA_RENDER_TOGGLE_OFF,
        .depth_of_field    = NYA_RENDER_TOGGLE_OFF,
        .speed_lines       = NYA_RENDER_TOGGLE_OFF,
        .bloom             = NYA_RENDER_TOGGLE_OFF,
        .light_shafts      = NYA_RENDER_TOGGLE_OFF,
        .motion_blur       = NYA_RENDER_TOGGLE_OFF,
        .eye_adaptation    = NYA_RENDER_TOGGLE_OFF,
        .grade             = NYA_RENDER_TOGGLE_OFF,
    };
}

/** The window's size from the terminal's, in pixels. Called every frame, since a resize is free here. */
NYA_INTERNAL void _nya_render2d_terminal_resize(void) {
    NYA_Window* window = &_nya_render2d_terminal.window;

    window->screen_width  = (u32)nya_terminal_columns() * NYA_TERMINAL_CELL_WIDTH_PX;
    window->screen_height = (u32)nya_terminal_rows() * NYA_TERMINAL_CELL_HEIGHT_PX;
    window->width         = window->screen_width;
    window->height        = window->screen_height;
}

NYA_Error nya_render2d_terminal_open(NYA_TerminalOptions options) {
    NYA_TRY(nya_terminal_open(options));

    _nya_render2d_terminal.window = (NYA_Window){ .title = "nyangine" };

    _nya_render2d_terminal_resize();

    NYA_RenderFeatures features = _nya_render2d_terminal_features();

    /*
     * Every switch answered, and none left at DEFAULT. A feature added to render_features.h would
     * otherwise pass through this backend as "whatever its own options say", which for a pass that
     * cannot run here is the pretending the header asks backends not to do. The struct is an array
     * of switches in NYA_RenderFeature's order, which render_features.h states and asserts.
     */
    const NYA_RenderToggle* switches = (const NYA_RenderToggle*)&features;
    for (u32 i = 0; i < NYA_RENDER_FEATURE_COUNT; i++) {
        nya_assert(switches[i] != NYA_RENDER_TOGGLE_DEFAULT, "the terminal backend has no answer for '%s'; a feature was added and this was not",
                   nya_render_feature_name((NYA_RenderFeature)i));
    }

    nya_render_features_set(&_nya_render2d_terminal.window, features);

    char disabled[512] = { 0 };
    if (nya_render_features_disabled_text(&_nya_render2d_terminal.window, disabled, sizeof(disabled)) > 0) {
        nya_log_info("Terminal renderer: %s are off; a terminal has no pass to run them in.", disabled);
    }

    return NYA_OK;
}

void nya_render2d_terminal_close(void) {
    nya_terminal_close();
    _nya_render2d_terminal = (_NYA_Render2DTerminal){ 0 };
}

NYA_Window* nya_render2d_terminal_window(void) {
    return &_nya_render2d_terminal.window;
}

void nya_render2d_terminal_frame_begin(NYA_Window* window, NYA_Color clear) {
    nya_assert(window == &_nya_render2d_terminal.window, "the terminal backend draws into its own window; see nya_render2d_terminal_window");

    // before the clear, so a frame that started at a new size clears the whole new grid.
    _nya_render2d_terminal_resize();

    nya_terminal_clear(nya_terminal_ink(clear.r, clear.g, clear.b));

    _nya_render2d_terminal.clip = (NYA_Rectf){ 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height };
}

void nya_render2d_terminal_frame_end(NYA_Window* window) {
    nya_assert(window == &_nya_render2d_terminal.window);

    nya_terminal_present();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION (TERMINAL)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render2d_shutdown(void) {
    // the terminal is closed by nya_render2d_terminal_close, which is the partner of the open that
    // took it. Nothing here owns anything else.
}

void nya_render2d_flush(NYA_Window* window) {
    nya_assert(window != nullptr);

    // cells are written where they are drawn, so there is no batch to hand over. A caller that
    // flushes between two draws still sees them in order.
}

void nya_render2d_layer_set(NYA_Window* window, s32 layer) {
    nya_assert(window != nullptr);

    // painter's order, and nothing sorts, so the layer is recorded and read back but changes nothing.
    window->render_system.draw_batch.layer = layer;
}

s32 nya_render2d_layer(NYA_Window* window) {
    nya_assert(window != nullptr);
    return window->render_system.draw_batch.layer;
}

void nya_render2d_target_size(NYA_Window* window, OUT u32* out_width, OUT u32* out_height) {
    nya_assert(window != nullptr);
    nya_assert(out_width != nullptr && out_height != nullptr);

    *out_width  = window->screen_width;
    *out_height = window->screen_height;
}

/*
 * ─────────────────────────────────────────────────────────
 * SHAPES
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_rect(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, NYA_Color color) {
    nya_assert(window != nullptr);
    _nya_render2d_terminal_fill(x, y, width, height, color);
}

void nya_render2d_rect_gradient(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, const NYA_Color corners[4]) {
    nya_assert(window != nullptr && corners != nullptr);

    if (width <= 0.0F || height <= 0.0F) return;

    u16 first_column = 0, last_column = 0, first_row = 0, last_row = 0;

    if (!_nya_render2d_terminal_span(x, x + width, NYA_TERMINAL_CELL_WIDTH_PX, nya_terminal_columns(), &first_column, &last_column)) return;
    if (!_nya_render2d_terminal_span(y, y + height, NYA_TERMINAL_CELL_HEIGHT_PX, nya_terminal_rows(), &first_row, &last_row)) return;

    for (u16 row = first_row; row <= last_row; row++) {
        for (u16 column = first_column; column <= last_column; column++) {
            f32 centre_x = ((f32)column + 0.5F) * (f32)NYA_TERMINAL_CELL_WIDTH_PX;
            f32 centre_y = ((f32)row + 0.5F) * (f32)NYA_TERMINAL_CELL_HEIGHT_PX;

            if (_nya_render2d_terminal_clipped(centre_x, centre_y)) continue;

            f32 across = nya_clamp((centre_x - x) / width, 0.0F, 1.0F);
            f32 down   = nya_clamp((centre_y - y) / height, 0.0F, 1.0F);

            // the same bilinear blend the vertex colours would have given, evaluated at the cell centre.
            NYA_Color top    = _nya_render2d_terminal_mix(corners[0], corners[1], across);
            NYA_Color bottom = _nya_render2d_terminal_mix(corners[3], corners[2], across);

            _nya_render2d_terminal_paint(column, row, _nya_render2d_terminal_mix(top, bottom, down));
        }
    }
}

void nya_render2d_rect_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    // at least one cell, because a half cell border drawn as nothing is a box with no edges.
    f32 edge = nya_max(thickness, (f32)NYA_TERMINAL_CELL_WIDTH_PX);

    _nya_render2d_terminal_fill(x, y, width, edge, color);
    _nya_render2d_terminal_fill(x, y + height - edge, width, edge, color);
    _nya_render2d_terminal_fill(x, y, edge, height, color);
    _nya_render2d_terminal_fill(x + width - edge, y, edge, height, color);
}

void nya_render2d_rect_rounded(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    // square. A radius is a fraction of a cell at every size a TUI uses, and rounding it would only
    // knock the corner cells out of a box that is meant to read as solid.
    nya_unused(radius);
    _nya_render2d_terminal_fill(x, y, width, height, color);
}

void nya_render2d_rect_rounded_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 radius, f32 thickness, NYA_Color color) {
    nya_unused(radius);
    nya_render2d_rect_outline(window, x, y, width, height, thickness, color);
}

/** A rotated rectangle, as the four corners a cell centre is tested against. */
typedef struct {
    f32x2 corners[4];
} _NYA_Render2DTerminalQuad;

/** Whether a point is on the inner side of all four edges, which for a convex quad is inside it. */
NYA_INTERNAL b8 _nya_render2d_terminal_in_quad(f32 x, f32 y, const void* shape) {
    const _NYA_Render2DTerminalQuad* quad = shape;

    f32 sign = 0.0F;

    for (u32 i = 0; i < 4; i++) {
        f32x2 from = quad->corners[i];
        f32x2 to   = quad->corners[(i + 1) % 4];

        f32 cross = ((to.x - from.x) * (y - from.y)) - ((to.y - from.y) * (x - from.x));

        // exactly on an edge counts as inside, so two quads sharing one do not leave a gap.
        if (cross == 0.0F) continue;
        if (sign == 0.0F) sign = cross;
        if ((cross > 0.0F) != (sign > 0.0F)) return false;
    }

    return true;
}

void nya_render2d_rect_rotated(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    f32 cosine = cosf(rotation);
    f32 sine   = sinf(rotation);

    f32 half_width  = size.x * 0.5F;
    f32 half_height = size.y * 0.5F;

    f32x2 offsets[4] = {
        { -half_width, -half_height },
        {  half_width, -half_height },
        {  half_width,  half_height },
        { -half_width,  half_height },
    };

    _NYA_Render2DTerminalQuad quad = { 0 };

    f32 least_x = 0.0F, most_x = 0.0F, least_y = 0.0F, most_y = 0.0F;

    for (u32 i = 0; i < 4; i++) {
        quad.corners[i] = (f32x2){
            center.x + (offsets[i].x * cosine) - (offsets[i].y * sine),
            center.y + (offsets[i].x * sine) + (offsets[i].y * cosine),
        };

        least_x = i == 0 ? quad.corners[i].x : nya_min(least_x, quad.corners[i].x);
        most_x  = i == 0 ? quad.corners[i].x : nya_max(most_x, quad.corners[i].x);
        least_y = i == 0 ? quad.corners[i].y : nya_min(least_y, quad.corners[i].y);
        most_y  = i == 0 ? quad.corners[i].y : nya_max(most_y, quad.corners[i].y);
    }

    _nya_render2d_terminal_scan((NYA_Rectf){ least_x, least_y, most_x - least_x, most_y - least_y }, color, _nya_render2d_terminal_in_quad, &quad);
}

void nya_render2d_rect_rotated_outline(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    // the filled one minus a smaller filled one would need two passes over the same cells with no
    // way to undo the first, so the four sides are drawn as four thin rotated rectangles instead.
    f32 edge = nya_max(thickness, (f32)NYA_TERMINAL_CELL_WIDTH_PX);

    f32 cosine = cosf(rotation);
    f32 sine   = sinf(rotation);

    f32x2 across = { cosine, sine };
    f32x2 down   = { -sine, cosine };

    f32 half_width  = size.x * 0.5F;
    f32 half_height = size.y * 0.5F;

    f32x2 sides[4] = {
        { center.x - (down.x * half_height), center.y - (down.y * half_height) },
        { center.x + (down.x * half_height), center.y + (down.y * half_height) },
        { center.x - (across.x * half_width), center.y - (across.y * half_width) },
        { center.x + (across.x * half_width), center.y + (across.y * half_width) },
    };

    f32x2 extents[4] = {
        { size.x, edge },
        { size.x, edge },
        {   edge, size.y },
        {   edge, size.y },
    };

    for (u32 i = 0; i < 4; i++) nya_render2d_rect_rotated(window, sides[i], extents[i], rotation, color);
}

void nya_render2d_line(NYA_Window* window, f32x2 from, f32x2 to, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    f32 length = nya_vector_length((f32x2){ to.x - from.x, to.y - from.y });

    // a line of no length is a dot, and the midpoint form below would divide by zero finding its angle.
    if (length <= 0.0F) {
        _nya_render2d_terminal_fill(from.x, from.y, (f32)NYA_TERMINAL_CELL_WIDTH_PX, (f32)NYA_TERMINAL_CELL_HEIGHT_PX, color);
        return;
    }

    f32x2 centre = { (from.x + to.x) * 0.5F, (from.y + to.y) * 0.5F };
    f32   angle  = atan2f(to.y - from.y, to.x - from.x);

    nya_render2d_rect_rotated(window, centre, (f32x2){ length, nya_max(thickness, 1.0F) }, angle, color);
}

void nya_render2d_polyline(NYA_Window* window, const f32x2* points, u32 count, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);
    nya_assert(points != nullptr || count == 0);

    for (u32 i = 1; i < count; i++) nya_render2d_line(window, points[i - 1], points[i], thickness, color);
}

/** A triangle, as the three corners a cell centre is tested against. */
typedef struct {
    f32x2 a, b, c;
} _NYA_Render2DTerminalTriangle;

NYA_INTERNAL b8 _nya_render2d_terminal_in_triangle(f32 x, f32 y, const void* shape) {
    const _NYA_Render2DTerminalTriangle* triangle = shape;

    f32 cross_ab = ((triangle->b.x - triangle->a.x) * (y - triangle->a.y)) - ((triangle->b.y - triangle->a.y) * (x - triangle->a.x));
    f32 cross_bc = ((triangle->c.x - triangle->b.x) * (y - triangle->b.y)) - ((triangle->c.y - triangle->b.y) * (x - triangle->b.x));
    f32 cross_ca = ((triangle->a.x - triangle->c.x) * (y - triangle->c.y)) - ((triangle->a.y - triangle->c.y) * (x - triangle->c.x));

    b8 any_negative = cross_ab < 0.0F || cross_bc < 0.0F || cross_ca < 0.0F;
    b8 any_positive = cross_ab > 0.0F || cross_bc > 0.0F || cross_ca > 0.0F;

    // the winding is the caller's, so inside means "on the same side of all three" either way.
    return !(any_negative && any_positive);
}

void nya_render2d_triangle(NYA_Window* window, f32x2 a, f32x2 b, f32x2 c, NYA_Color color) {
    nya_assert(window != nullptr);

    _NYA_Render2DTerminalTriangle triangle = { a, b, c };

    f32 least_x = nya_min(a.x, nya_min(b.x, c.x));
    f32 most_x  = nya_max(a.x, nya_max(b.x, c.x));
    f32 least_y = nya_min(a.y, nya_min(b.y, c.y));
    f32 most_y  = nya_max(a.y, nya_max(b.y, c.y));

    _nya_render2d_terminal_scan((NYA_Rectf){ least_x, least_y, most_x - least_x, most_y - least_y }, color, _nya_render2d_terminal_in_triangle,
                                &triangle);
}

/** A circle, as the centre and radius a cell centre is measured against. */
typedef struct {
    f32x2 center;
    f32   radius;
} _NYA_Render2DTerminalCircle;

NYA_INTERNAL b8 _nya_render2d_terminal_in_circle(f32 x, f32 y, const void* shape) {
    const _NYA_Render2DTerminalCircle* circle = shape;

    f32 delta_x = x - circle->center.x;
    f32 delta_y = y - circle->center.y;

    return (delta_x * delta_x) + (delta_y * delta_y) <= circle->radius * circle->radius;
}

void nya_render2d_circle(NYA_Window* window, f32x2 center, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    if (radius <= 0.0F) return;

    _NYA_Render2DTerminalCircle circle = { center, radius };

    _nya_render2d_terminal_scan((NYA_Rectf){ center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F }, color,
                                _nya_render2d_terminal_in_circle, &circle);
}

/*
 * ─────────────────────────────────────────────────────────
 * SCISSOR
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_scissor_begin(NYA_Window* window, f32 x, f32 y, f32 width, f32 height) {
    nya_assert(window != nullptr);

    _nya_render2d_terminal.clip = (NYA_Rectf){ x, y, nya_max(width, 0.0F), nya_max(height, 0.0F) };
}

void nya_render2d_scissor_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    _nya_render2d_terminal.clip = (NYA_Rectf){ 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height };
}

/*
 * ─────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────
 */

/* Identical to the headless backend's, and for the same reason: game logic reads the camera back. */

void nya_render2d_camera_set(NYA_Window* window, NYA_Camera2DTopDown camera) {
    nya_assert(window != nullptr);

    window->render_system.draw_batch.camera = (NYA_Camera2D){
        .kind        = NYA_CAMERA2D_KIND_TOP_DOWN,
        .as_top_down = nya_camera2d_top_down_sanitized(camera),
    };
}

void nya_render2d_camera_isometric_set(NYA_Window* window, NYA_Camera2DIsometric camera) {
    nya_assert(window != nullptr);

    window->render_system.draw_batch.camera = (NYA_Camera2D){
        .kind         = NYA_CAMERA2D_KIND_ISOMETRIC,
        .as_isometric = nya_camera2d_isometric_sanitized(camera),
    };
}

void nya_render2d_camera_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    window->render_system.draw_batch.camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_NONE };
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

    u32 width = 0, height = 0;
    nya_render2d_target_size(window, &width, &height);

    return nya_camera2d_screen_to_world(&window->render_system.draw_batch.camera, screen, width, height);
}

f32x2 nya_render2d_world_to_screen(NYA_Window* window, f32x2 world) {
    nya_assert(window != nullptr);

    u32 width = 0, height = 0;
    nya_render2d_target_size(window, &width, &height);

    return nya_camera2d_world_to_screen(&window->render_system.draw_batch.camera, world, width, height);
}

/*
 * ─────────────────────────────────────────────────────────
 * TEXT
 * ─────────────────────────────────────────────────────────
 */

/**
 * Code points in a UTF-8 string, which in a monospace grid is its width in cells. Bounded, because
 * the string may come from anywhere and a measurement is not worth an unbounded walk.
 * */
NYA_INTERNAL u32 _nya_render2d_terminal_length(NYA_ConstCString text) {
    if (text == nullptr) return 0;

    const u8* bytes = (const u8*)text;
    u64       size  = strnlen(text, NYA_RENDER2D_TEXT_MAX);

    u32 cells = 0;
    u64 at    = 0;

    while (at < size) {
        u32 codepoint = 0;
        u32 used      = nya_terminal_utf8_decode(&bytes[at], size - at, &codepoint);

        if (used == 0) break;

        at    += used;
        cells += 1;
    }

    return cells;
}

/** Writes a string into the grid from one cell rightwards. The one place text becomes cells. */
NYA_INTERNAL u32 _nya_render2d_terminal_write(f32 x, f32 y, NYA_ConstCString text, NYA_Color color, u8 attributes) {
    if (text == nullptr || text[0] == '\0' || color.a <= 0.0F) return 0;

    const u8* bytes = (const u8*)text;
    u64       size  = strnlen(text, NYA_RENDER2D_TEXT_MAX);

    s32 column = (s32)floorf(x / (f32)NYA_TERMINAL_CELL_WIDTH_PX);
    s32 row    = (s32)floorf(y / (f32)NYA_TERMINAL_CELL_HEIGHT_PX);

    u32 ink   = nya_terminal_ink(color.r, color.g, color.b);
    u32 cells = 0;
    u64 at    = 0;

    while (at < size) {
        u32 codepoint = 0;
        u32 used      = nya_terminal_utf8_decode(&bytes[at], size - at, &codepoint);

        if (used == 0) break;
        at += used;

        s32 target = column + (s32)cells;
        cells     += 1;

        // negative columns are off the left edge, and nya_terminal_cell_set takes unsigned cells.
        if (target < 0 || row < 0) continue;

        f32 centre_x = ((f32)target + 0.5F) * (f32)NYA_TERMINAL_CELL_WIDTH_PX;
        f32 centre_y = ((f32)row + 0.5F) * (f32)NYA_TERMINAL_CELL_HEIGHT_PX;

        if (_nya_render2d_terminal_clipped(centre_x, centre_y)) continue;

        NYA_TerminalCell cell = nya_terminal_cell_get((u16)target, (u16)row);

        cell.codepoint  = codepoint;
        cell.foreground = ink;
        cell.attributes = attributes;

        nya_terminal_cell_set((u16)target, (u16)row, cell);
    }

    return cells;
}

void nya_render2d_font_set(NYA_ConstCString font_path, f32 point_size) {
    // recorded so nya_render2d_font_get answers, and otherwise ignored: a terminal has one font and
    // it belongs to whoever configured the terminal.
    _nya_render2d_terminal.font_path       = font_path;
    _nya_render2d_terminal.font_point_size = point_size;
}

NYA_ConstCString nya_render2d_font_get(void) {
    return _nya_render2d_terminal.font_path;
}

f32 nya_render2d_font_size_get(void) {
    return _nya_render2d_terminal.font_point_size;
}

void nya_render2d_text(NYA_Window* window, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_assert(window != nullptr);
    (void)_nya_render2d_terminal_write(x, y, text, color, 0);
}

void nya_render2d_text_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_unused(font_path, point_size);
    nya_render2d_text(window, text, x, y, color);
}

void nya_render2d_textf(NYA_Window* window, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    nya_assert(window != nullptr && format != nullptr);

    char    line[NYA_RENDER2D_TEXT_MAX];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    nya_render2d_text(window, line, x, y, color);
}

void nya_render2d_textf_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format,
                                  ...) {
    nya_assert(window != nullptr && format != nullptr);
    nya_unused(font_path, point_size);

    char    line[NYA_RENDER2D_TEXT_MAX];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    nya_render2d_text(window, line, x, y, color);
}

f32x2 nya_render2d_text_measure_with_font(NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text) {
    nya_unused(font_path, point_size);

    if (text == nullptr || text[0] == '\0') return f32x2_zero;

    return (f32x2){ (f32)_nya_render2d_terminal_length(text) * (f32)NYA_TERMINAL_CELL_WIDTH_PX, (f32)NYA_TERMINAL_CELL_HEIGHT_PX };
}

f32x2 nya_render2d_text_measure(NYA_ConstCString text) {
    return nya_render2d_text_measure_with_font(nullptr, 0.0F, text);
}

f32 nya_render2d_text_width(NYA_ConstCString text) {
    return nya_render2d_text_measure(text).x;
}

f32 nya_render2d_text_height(NYA_ConstCString text) {
    return nya_render2d_text_measure(text).y;
}

f32 nya_render2d_font_line_height(void) {
    return (f32)NYA_TERMINAL_CELL_HEIGHT_PX;
}

f32 nya_render2d_font_ascent(void) {
    return (f32)NYA_TERMINAL_CELL_HEIGHT_PX * _NYA_RENDER2D_TERMINAL_ASCENT_SHARE;
}

f32 nya_render2d_font_descent(void) {
    return (f32)NYA_TERMINAL_CELL_HEIGHT_PX * (1.0F - _NYA_RENDER2D_TERMINAL_ASCENT_SHARE);
}

f32 nya_render2d_font_height(void) {
    return (f32)NYA_TERMINAL_CELL_HEIGHT_PX;
}

/**
 * Lays a box out, drawing when `draw` says to, and returns the size it took. One function for both
 * the measure and the draw, since a measurement that does not walk the same wrap is a layout bug
 * waiting to happen.
 * */
NYA_INTERNAL f32x2 _nya_render2d_terminal_box(NYA_ConstCString text, NYA_Render2DTextBox params, b8 draw) {
    if (text == nullptr || text[0] == '\0') return f32x2_zero;

    f32 line_height = (f32)NYA_TERMINAL_CELL_HEIGHT_PX * (params.line_spacing > 0.0F ? params.line_spacing : 1.0F);

    u32 columns = params.width > 0.0F ? (u32)(params.width / (f32)NYA_TERMINAL_CELL_WIDTH_PX) : 0;
    u32 length  = _nya_render2d_terminal_length(text);

    // no width given means one line, which is what the GPU backend does with a zero wrap width.
    if (columns == 0) {
        if (draw) (void)_nya_render2d_terminal_write(params.x, params.y, text, params.color, 0);
        return (f32x2){ (f32)length * (f32)NYA_TERMINAL_CELL_WIDTH_PX, line_height };
    }

    /*
     * Wrapped on the cell, not on the word: this walks bytes and has no dictionary of where a word
     * ends. A caller that wants word wrapping splits the string itself, which is the same thing the
     * headless backend's measurement leaves to render_text.c.
     */
    u32 lines = (length + columns - 1) / columns;
    if (params.max_lines > 0 && lines > params.max_lines) lines = params.max_lines;

    if (draw) {
        const u8* bytes = (const u8*)text;
        u64       size  = strnlen(text, NYA_RENDER2D_TEXT_MAX);

        u64 at = 0;
        for (u32 line = 0; line < lines && at < size; line++) {
            char piece[NYA_RENDER2D_TEXT_MAX];
            u64  used = 0;

            for (u32 cell = 0; cell < columns && at < size; cell++) {
                u32 codepoint = 0;
                u32 taken     = nya_terminal_utf8_decode(&bytes[at], size - at, &codepoint);

                if (taken == 0 || used + taken >= sizeof(piece)) break;

                nya_memcpy(&piece[used], &bytes[at], taken);

                used += taken;
                at   += taken;
            }

            piece[used] = '\0';
            (void)_nya_render2d_terminal_write(params.x, params.y + ((f32)line * line_height), piece, params.color, 0);
        }
    }

    f32 widest = (f32)nya_min(length, columns) * (f32)NYA_TERMINAL_CELL_WIDTH_PX;

    return (f32x2){ widest, (f32)lines * line_height };
}

f32x2 nya_render2d_text_box_measure(NYA_ConstCString text, NYA_Render2DTextBox params) {
    return _nya_render2d_terminal_box(text, params, false);
}

f32x2 nya_render2d_text_box(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params) {
    nya_assert(window != nullptr);
    return _nya_render2d_terminal_box(text, params, true);
}

/*
 * ─────────────────────────────────────────────────────────
 * IMAGES AND GLYPHS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_render2d_terminal_image(NYA_Window* window, f32 x, f32 y, const u8* rgba, u32 width, u32 height) {
    nya_assert(window != nullptr && rgba != nullptr);

    if (x < 0.0F || y < 0.0F) return false;

    u16 column = (u16)(x / (f32)NYA_TERMINAL_CELL_WIDTH_PX);
    u16 row    = (u16)(y / (f32)NYA_TERMINAL_CELL_HEIGHT_PX);

    return nya_terminal_image_draw(column, row, rgba, width, height);
}

void nya_render2d_terminal_glyph(NYA_Window* window, f32 x, f32 y, u32 codepoint, NYA_Color color, u8 attributes) {
    nya_assert(window != nullptr);

    u8 utf8[5] = { 0 };

    // encoded rather than written straight into the cell, so this goes through the same write every
    // other text call does and a clipped glyph is clipped once, in one place.
    if (codepoint < 0x80U) {
        utf8[0] = (u8)codepoint;
    } else if (codepoint < 0x800U) {
        utf8[0] = (u8)(0xC0U | (codepoint >> 6U));
        utf8[1] = (u8)(0x80U | (codepoint & 0x3FU));
    } else if (codepoint < 0x10000U) {
        utf8[0] = (u8)(0xE0U | (codepoint >> 12U));
        utf8[1] = (u8)(0x80U | ((codepoint >> 6U) & 0x3FU));
        utf8[2] = (u8)(0x80U | (codepoint & 0x3FU));
    } else {
        utf8[0] = (u8)(0xF0U | (codepoint >> 18U));
        utf8[1] = (u8)(0x80U | ((codepoint >> 12U) & 0x3FU));
        utf8[2] = (u8)(0x80U | ((codepoint >> 6U) & 0x3FU));
        utf8[3] = (u8)(0x80U | (codepoint & 0x3FU));
    }

    (void)_nya_render2d_terminal_write(x, y, (NYA_ConstCString)utf8, color, attributes);
}

/*
 * ─────────────────────────────────────────────────────────
 * WHAT A TERMINAL HAS NO ANSWER FOR
 * ─────────────────────────────────────────────────────────
 */

/*
 * Stubbed as a block, the way render2d_headless.c stubs itself: there is no sampler, no render pass,
 * no pipeline and no render texture behind any of these. A call succeeds and draws nothing, so a
 * program written against a window still runs under -DNYA_TERMINAL rather than failing to link.
 * The features these belong to are reported off; see _nya_render2d_terminal_features.
 */

void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint) {
    nya_unused(window, texture_handle, x, y, tint);
}

void nya_render2d_texture_ex(NYA_Window* window, NYA_ConstCString texture_handle, NYA_Render2DTexture params) {
    nya_unused(window, texture_handle, params);
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
    nya_unused(window, texture_handle, source_x, source_y, source_width, source_height);
    nya_unused(destination_x, destination_y, destination_width, destination_height, tint);
}

void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params) {
    nya_unused(window, texture_handle, params);
}

void nya_render2d_lights_apply(NYA_Window* window, const NYA_Light2D* lights, const f32x2* positions, u32 count, NYA_Color ambient) {
    nya_unused(window, lights, positions, count, ambient);
}

void nya_render2d_shader_begin(NYA_Window* window, NYA_ConstCString pipeline_handle) {
    nya_unused(window, pipeline_handle);
}

void nya_render2d_shader_set_uniform(NYA_Window* window, const void* data, u32 size) {
    nya_unused(window, data, size);
}

b8 nya_render2d_shader_set_texture(NYA_Window* window, NYA_ConstCString texture_handle) {
    nya_unused(window);

    // loaded is all a caller can observe here, so that is what decides, as it does headless.
    return nya_asset_status((NYA_CString)texture_handle) == NYA_ASSET_STATUS_LOADED;
}

void nya_render2d_shader_end(NYA_Window* window) {
    nya_unused(window);
}

void nya_render2d_procedural(NYA_Window* window, NYA_ConstCString pipeline_handle, u32 vertex_count, const void* uniform_data, u32 uniform_size) {
    nya_unused(window, pipeline_handle, vertex_count, uniform_data, uniform_size);
}

void nya_render2d_fullscreen(
    NYA_Window*            window,
    NYA_ConstCString       pipeline_handle,
    SDL_GPUTexture* const* textures,
    u32                    texture_count,
    const void*            uniform,
    u32                    uniform_size
) {
    nya_unused(window, pipeline_handle, textures, texture_count, uniform, uniform_size);
}

NYA_RenderTexture nya_render_texture_create(NYA_Window* window, u32 width, u32 height) {
    nya_unused(window);
    return (NYA_RenderTexture){ .texture = nullptr, .width = width, .height = height };
}

NYA_RenderTexture nya_render_texture_create_with(NYA_Window* window, u32 width, u32 height, NYA_RenderTextureOptions options) {
    nya_unused(window);
    return (NYA_RenderTexture){ .texture = nullptr, .width = width, .height = height, .options = options };
}

b8 nya_render_texture_is_current(const NYA_RenderTexture* render_texture, u32 width, u32 height) {
    nya_assert(render_texture != nullptr);

    // nothing is ever created here, so only the size can be current.
    return render_texture->width == width && render_texture->height == height;
}

void nya_render_texture_destroy(NYA_RenderTexture* render_texture) {
    if (render_texture != nullptr) *render_texture = (NYA_RenderTexture){ 0 };
}

void nya_render_texture_begin(NYA_Window* window, NYA_RenderTexture* render_texture, NYA_Color clear) {
    nya_unused(window, render_texture, clear);
}

void nya_render_texture_end(NYA_Window* window) {
    nya_unused(window);
}

void nya_render2d_render_texture(NYA_Window* window, const NYA_RenderTexture* render_texture, f32 x, f32 y, f32 width, f32 height, NYA_Color tint) {
    nya_unused(window, render_texture, x, y, width, height, tint);
}

u32 nya_render2d_pending_vertex_count(NYA_Window* window) {
    nya_assert(window != nullptr);

    // cells are written where they are drawn, so nothing is ever pending.
    return 0;
}

NYA_Render2DFrameStats nya_render2d_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);
    return (NYA_Render2DFrameStats){ 0 };
}

NYA_ConstCString nya_render2d_flush_reason_name(NYA_Render2DFlushReason reason) {
    nya_unused(reason);

    // nothing here ever flushes for a reason, because nothing here batches.
    return "none";
}
