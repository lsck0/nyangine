/**
 * @file render_font.h
 *
 * Fonts as values: a handle, a name registry, and metrics — over the glyph atlas render2d already has.
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
 *
 * **What was missing was the noun.** render2d already caches glyph atlases per face and size, bakes
 * lazily, kerns, and measures — but every entry point takes a path and a point size as two loose
 * arguments, so there was no way to *hold* a font, pass one, or store one in a style struct. This is
 * that type, plus the registry that stops "the UI font" being spelled out at forty call sites.
 *
 * ✅ **Distance fields are a real option, per font.** Glyphs are bitmaps baked at a fixed point size by
 * default, which is right for pixel art and wrong for text magnified past the size it was baked at —
 * scaling one up blurs. `nya_font_sdf_set` switches a face to FreeType's distance-field rasteriser and
 * the atlas is then drawn through a shader that thresholds the field instead of showing it. See its own
 * note for why this is per face rather than global.
 *
 * ✅ **Shaping is real, through HarfBuzz.** The text path used to walk codepoints and add a kerning
 * correction per adjacent pair, which is shaping done by hand and only for Latin. It now goes through
 * `TTF_CreateText`, so kerning, ligatures, mark placement and reordering all come from the shaper —
 * and the glyph atlas is keyed by glyph index rather than codepoint, because that is what shaping
 * outputs. See render_text.h.
 *
 * ✅ **Metrics and measurement work in a headless build**, and answer exactly what the real renderer
 * would draw. Laying text out needs no GPU; only rasterising does.
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
 *
 * A value rather than an opaque handle into a table: it is two fields, it needs no lifetime, and a
 * struct that can be stored in a style, compared and copied is more useful here than an index would be.
 * The atlas behind it is still cached by render2d, keyed on exactly this pair.
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

/** A font value. No allocation and no validation — the atlas is built on first use. */
NYA_API NYA_Font nya_font(NYA_ConstCString path, f32 point_size) __attr_no_discard;

/** Whether it names something: a non-null path and a positive size. */
NYA_API b8 nya_font_valid(NYA_Font font) __attr_no_discard;

/** Whether two fonts are the same face at the same size. */
NYA_API b8 nya_font_equals(NYA_Font a, NYA_Font b) __attr_no_discard;

/**
 * The font to use when a call is handed NYA_FONT_NONE.
 *
 * What lets a UI pass a font through everything without every widget checking whether one was set, and
 * what makes a themed default a one-line change rather than a search.
 * */
NYA_API void     nya_font_default_set(NYA_Font font);
NYA_API NYA_Font nya_font_default(void) __attr_no_discard;

/** `font` if it is valid, otherwise the default. What every function here calls first. */
NYA_API NYA_Font nya_font_resolve(NYA_Font font) __attr_no_discard;

/**
 * Registers a font under a short name, replacing any existing one.
 *
 * Names rather than paths at call sites: "ui" and "title" survive an art change that moves the file or
 * settles on a different size, and a rebinding of both is one line here instead of forty elsewhere.
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
 *
 * Zero until the face has finished loading, which is normal for the first frames after it is first
 * asked for. Real in a headless build — reading metrics needs no GPU.
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
 * Rasterises this face as a signed distance field rather than as coverage.
 *
 * FreeType's own SDF renderer, reached through `TTF_SetFontSDF`. It is compiled into the vendored
 * FreeType and costs no new dependency — which is the whole reason this is a per-font switch rather
 * than a plan: it makes the quality question answerable by trying it.
 *
 * **Per font, not global, and that is the design.** At 1:1 integer scale a distance field is a
 * *regression*: it reintroduces the soft edge that render2d's nearest sampling and pixel snapping
 * exist to remove, so a pixel-art HUD wants `false` and a title that scales wants `true`. A global
 * switch would force one answer on both.
 *
 * **Both halves are wired up.** The atlas stores whatever the face rasterises, so turning this on
 * stores a distance field — and an atlas that holds one is drawn through
 * NYA_RENDER2D_PIPELINE_TEXT_SDF (`assets/shader/source/text_sdf.frag.hlsl`), which thresholds it with
 * a screen-space derivative, and sampled with linear filtering rather than the nearest a coverage
 * atlas wants. The choice is made from what the atlas actually holds, latched when it was built, not
 * from what the face says now — which is the other reason to set this once.
 *
 * ✅ **Callable at registration, which is the only place it makes sense to call it.** The face does not
 * exist yet at that point — the asset system resolves one over the following frames — so this records
 * what was asked for and applies it the moment there is something to apply it to, from whichever of
 * `nya_font_draw`, `nya_font_measure`, `nya_font_metrics` or `nya_font_sdf` gets there first. It used
 * to answer false and forget, which meant the call every game would naturally write did nothing and
 * the mode was reachable only by polling for the face. A reload re-applies it too: hot reload replaces
 * the TTF_Font, and the request is remembered against the face it was pushed onto rather than as a
 * flag.
 *
 * ⚠ **It changes the face's metrics**, which is the thing to watch: the field extends past the
 * outline, so glyph images come back larger. Shaping is asked after the mode is set, so positions
 * follow — but a face switched *while* text is on screen re-lays it out mid-frame. Worse, render2d
 * bakes a glyph atlas — sized from those metrics, and flagged with the mode it was baked in — the first
 * time a glyph is drawn from the face, and nothing rebuilds that atlas when the mode changes under it.
 * So: set it once, at registration, which now works.
 *
 * False only when `font` names nothing, or when there is no free request slot. Whether the renderer
 * ultimately accepts the mode is reported by `nya_font_sdf`, not here, because that answer does not
 * exist yet at the point this is called.
 * */
NYA_API b8 nya_font_sdf_set(NYA_Font font, b8 enabled);

/**
 * Whether this face is rasterising as a distance field.
 *
 * The loaded face when there is one, since that is what an atlas gets baked from. Before it loads,
 * what `nya_font_sdf_set` last asked for — a caller that has just asked for a distance field being
 * told it has none is indistinguishable from the request having been dropped, which is what used to
 * happen.
 * */
NYA_API b8 nya_font_sdf(NYA_Font font) __attr_no_discard;
