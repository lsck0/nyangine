/**
 * @file render_font.h
 *
 * ```c
 * nya_font_register("ui",    NYA_ASSET_FONTS_INTER_TTF, 16.0F);
 * nya_font_register("title", NYA_ASSET_FONTS_INTER_TTF, 48.0F);
 * nya_font_default_set(nya_font_named("ui"));
 *
 * nya_font_draw(window, nya_font_named("title"), "Nyangine", 32.0F, 32.0F, NYA_COLOR_WHITE);
 *
 * // A zeroed font resolves to the default, so this is the UI font without naming it.
 * f32 width = nya_font_width(NYA_FONT_NONE, "Continue");
 * ```
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/renderer/render_color.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many fonts may be registered by name. */
#ifndef NYA_FONT_REGISTRY_MAX
#define NYA_FONT_REGISTRY_MAX 32
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Font        NYA_Font;
typedef struct NYA_FontMetrics NYA_FontMetrics;

/**
 * A face at a size. Two of these naming the same pair are the same font.
 * */
struct NYA_Font {
    NYA_ConstCString path;
    f32              point_size;
};

/** A zeroed font. Resolves to the default wherever one is accepted. */
#define NYA_FONT_NONE ((NYA_Font){ .path = nullptr, .point_size = 0.0F })

/** Vertical metrics, in pixels at the font's point size. */
struct NYA_FontMetrics {
    /** Baseline to baseline. What to advance by for the next line. */
    f32 line_height;

    /** Baseline to the top of the tallest glyph, positive upward. */
    f32 ascent;

    /** Baseline to the bottom of the lowest glyph, negative. */
    f32 descent;

    /** ascent − descent: how tall a line's ink can be, which is not the same as line_height. */
    f32 height;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A font value. No allocation and no validation; the atlas is built on first use. */
NYA_API NYA_Font nya_font(NYA_ConstCString path, f32 point_size) __attr_no_discard;

/** Whether it names something: a non-null path and a positive size. */
NYA_API b8 nya_font_valid(NYA_Font font) __attr_no_discard;

/** Whether two fonts are the same face at the same size. */
NYA_API b8 nya_font_equals(NYA_Font a, NYA_Font b) __attr_no_discard;

/**
 * The font to use when a call is handed NYA_FONT_NONE.
 * */
NYA_API void     nya_font_default_set(NYA_Font font);
NYA_API NYA_Font nya_font_default(void) __attr_no_discard;

/** `font` if it is valid, otherwise the default. What every function here calls first. */
NYA_API NYA_Font nya_font_resolve(NYA_Font font) __attr_no_discard;

/**
 * Registers a font under a short name, replacing any existing one.
 * */
NYA_API b8 nya_font_register(NYA_ConstCString name, NYA_ConstCString path, f32 point_size);

/** The font registered as `name`, or NYA_FONT_NONE. */
NYA_API NYA_Font nya_font_named(NYA_ConstCString name) __attr_no_discard;

NYA_API b8   nya_font_registered(NYA_ConstCString name) __attr_no_discard;
NYA_API void nya_font_unregister(NYA_ConstCString name);
NYA_API void nya_font_clear(void);
NYA_API u32  nya_font_count(void) __attr_no_discard;

/**
 * The font's vertical metrics.
 * */
NYA_API NYA_FontMetrics nya_font_metrics(NYA_Font font) __attr_no_discard;

/**
 * The size `text` would occupy, shaped exactly as it would be drawn.
 *
 * Zero until the face has loaded. Real in a headless build, for the same reason as the metrics.
 * */
NYA_API f32x2 nya_font_measure(NYA_Font font, NYA_ConstCString text) __attr_no_discard;
NYA_API f32   nya_font_width(NYA_Font font, NYA_ConstCString text) __attr_no_discard;
NYA_API f32   nya_font_height(NYA_Font font, NYA_ConstCString text) __attr_no_discard;

/** Draws `text` with `font`, at a baseline-agnostic top-left like the rest of render2d's text. */
NYA_API void nya_font_draw(NYA_Window* window, NYA_Font font, NYA_ConstCString text, f32 x, f32 y, NYA_Color color);

/*
 * ─────────────────────────────────────────────────────────
 * DISTANCE FIELDS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Rasterises this face as a signed distance field instead of coverage.
 *
 * Changes the face's metrics: the field extends past the outline, so glyph images grow. render2d
 * bakes the glyph atlas from those metrics on first draw and does not rebuild it when the mode
 * changes, so set it once at registration.
 * */
NYA_API b8 nya_font_sdf_set(NYA_Font font, b8 enabled);

/**
 * Whether this face is rasterising as a distance field.
 * */
NYA_API b8 nya_font_sdf(NYA_Font font) __attr_no_discard;
