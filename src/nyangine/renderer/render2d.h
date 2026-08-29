/**
 * @file render2d.h
 *
 * Immediate mode 2D drawing: shapes, textures and text in pixels, batched into as few draw calls as
 * possible, with offscreen render targets and swappable shaders.
 *
 * Coordinates are **pixels, y down, origin at the top left** of whatever is being drawn into — the
 * window or a render texture. The same convention SDL reports mouse positions in, so a hit test
 * against something drawn here is a comparison rather than a conversion.
 *
 * ```c
 * void layer_on_render(NYA_Window* window) {
 *     nya_render2d_rect(window, 16, 16, 200, 48, (NYA_Color){ 0.1F, 0.1F, 0.12F, 0.9F });
 *     nya_render2d_font_set(NYA_ASSET_FONTS_ALDRICH_TTF, 24.0F);
 *     nya_render2d_text(window, "score: 42", 24, 28, NYA_COLOR_WHITE);
 * }
 * ```
 *
 * Nothing has to be flushed by hand: vertices accumulate until something forces a draw call, and
 * nya_render_end flushes the rest.
 *
 * **One flush is one draw call, and a draw call has one pipeline and one texture** — so shapes batch
 * with shapes, glyphs of one font batch together, and alternating between a shape and a sprite costs
 * a draw call each time. Later draws land on top; there is no depth test and no sorting, so across
 * layers, layer order is draw order.
 *
 * Positions are f32 rather than NYA_Rect's s32: snapping to whole pixels would put a floor under
 * every animation and make text at fractional advances jitter as it moves.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_camera.h"
#include "nyangine/renderer/render_color.h"
#include "nyangine/renderer/renderer.h"

// The C side of the built in shaders' constant buffers. Beside the shaders rather than here, so a
// change to a cbuffer has one obvious place to be mirrored. See assets/shader/uniforms.h.
#include "../../../assets/shader/uniforms.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Vertices held before a flush is forced.
 *
 * Not a limit on how much can be drawn in a frame — going over simply flushes and carries on, which
 * costs one extra draw call rather than dropping anything. It is a memory/draw-call trade.
 *
 * The default holds roughly 5400 rectangles in one call. Override with -DNYA_DRAW_MAX_VERTICES=<n>.
 * */
/**
 * Longest string nya_render2d_textf will produce, including the terminator.
 *
 * A stack buffer, so this is the whole cost of the call. Generous for a HUD line and far short of
 * anything that would matter on a stack.
 * */
/** Point size used when a call passes zero or a negative size. */
#ifndef NYA_RENDER2D_FONT_DEFAULT_SIZE
#define NYA_RENDER2D_FONT_DEFAULT_SIZE 24.0F
#endif

#ifndef NYA_RENDER2D_TEXT_MAX
#define NYA_RENDER2D_TEXT_MAX 512
#endif

#ifndef NYA_RENDER2D_MAX_VERTICES
#define NYA_RENDER2D_MAX_VERTICES 32768
#endif

/**
 * Indices held before a flush is forced.
 *
 * Separate from the vertex ceiling because the ratio is not one to one: a quad is four vertices and
 * six indices, a circle fan one vertex and three indices per segment. Index-heavy work therefore
 * hits this first, which is why it is larger.
 * */
#ifndef NYA_RENDER2D_MAX_INDICES
#define NYA_RENDER2D_MAX_INDICES (NYA_RENDER2D_MAX_VERTICES * 2)
#endif

/** Segments used to approximate a full circle. Scaled down for small radii; see nya_render2d_circle. */
#ifndef NYA_RENDER2D_CIRCLE_SEGMENTS
#define NYA_RENDER2D_CIRCLE_SEGMENTS 64
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFECYCLE
 * ─────────────────────────────────────────────────────────
 */

/**
 * Loads the pipelines the batch draws with and allocates its buffers.
 *
 * Called by nya_system_renderer_for_window_init; a game does not call this. It needs a window
 * because a pipeline is compiled against that window's swapchain format.
 * */


/**
 * Releases everything the drawing module holds that is not tied to a window.
 *
 * Today that is the glyph atlases, which are keyed by font handle rather than by window and so
 * outlive any one of them. Called by nya_system_renderer_deinit; a game does not call this.
 * */
NYA_API void nya_render2d_shutdown(void);

/**
 * Uploads and draws everything accumulated so far, then empties the batch.
 *
 * Called automatically whenever the pipeline, the texture or the render target changes, when the
 * batch fills, and by nya_render_end — so this is only needed to force ordering against something
 * drawn outside this module, such as a raw SDL_GPU pass of your own.
 *
 * A flush ends the current render pass and opens another that loads rather than clears, because
 * SDL_GPU forbids a copy pass while a render pass is open and the vertices have to be copied.
 * */
NYA_API void nya_render2d_flush(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────
 * SHAPES
 * ─────────────────────────────────────────────────────────
 */

/** A filled axis aligned rectangle, `x`/`y` being its top left corner. */
NYA_API void nya_render2d_rect(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, NYA_Color color);

/**
 * A rectangle outline of `thickness`, drawn **inside** the given bounds.
 *
 * Inset rather than centred on the edge, so an outline and a fill given the same coordinates line
 * up — which is what a bordered panel wants.
 * */
NYA_API void nya_render2d_rect_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 thickness, NYA_Color color);

/**
 * A filled rectangle turned about its own centre.
 *
 * Positioned by the **centre**, unlike nya_render2d_rect, because a rotation needs a pivot and the
 * centre is the only one that does not also move the shape — which is also what a rigid body hands
 * you, since its transform *is* a centre and an angle.
 *
 * ```c
 * NYA_Entity* entity = nya_entity_get(crate);
 * nya_render2d_rect_rotated(window, (f32x2){ entity->position.x, entity->position.y }, entity->physics2d.size,
 *                       nya_physics2d_rotation(entity), NYA_COLOR_ORANGE);
 * ```
 *
 * Clockwise, in radians, the same sense as NYA_Render2DTexture.rotation. Batches with every other shape.
 * */
NYA_API void nya_render2d_rect_rotated(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, NYA_Color color);

/**
 * The outline of that rectangle, as four lines of `thickness` along its edges.
 *
 * Centred on the edge rather than inset, unlike nya_render2d_rect_outline — an inset outline has to know
 * which side of a rotated edge is inside, and the answer stops being obvious the moment the shape
 * turns. Half of the stroke therefore sits outside the bounds a fill would cover.
 * */
NYA_API void nya_render2d_rect_rotated_outline(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, f32 thickness, NYA_Color color);

/** A line of `thickness`, as a quad. Both ends are square; there is no cap or join. */
NYA_API void nya_render2d_line(NYA_Window* window, f32x2 from, f32x2 to, f32 thickness, NYA_Color color);

/**
 * Connects `count` points with lines of `thickness`, in order and without closing the loop.
 *
 * What a polyline of terrain, a graph line or a traced path is. Each segment is an independent quad
 * with square ends, so a sharp corner shows a notch on its outside; that is the same trade
 * nya_render2d_line makes, and at terrain thicknesses it is invisible.
 *
 * Fewer than two points draws nothing.
 * */
NYA_API void nya_render2d_polyline(NYA_Window* window, const f32x2* points, u32 count, f32 thickness, NYA_Color color);

/** A filled triangle. Either winding order; culling is off. */
NYA_API void nya_render2d_triangle(NYA_Window* window, f32x2 a, f32x2 b, f32x2 c, NYA_Color color);

/** A filled circle centred on `center`, as a fan of triangles. */
NYA_API void nya_render2d_circle(NYA_Window* window, f32x2 center, f32 radius, NYA_Color color);

/*
 * ─────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────
 */

/**
 * Looks at the world from `camera` for everything drawn afterwards.
 *
 * Draw coordinates become **world** coordinates. `position` is the world point that appears at the
 * centre of the target, so scrolling is moving the camera rather than subtracting an offset at every
 * call site.
 *
 * ⚠ Anything queued is flushed first, because the projection is uniform across a draw call — changing
 * the camera per shape costs a draw call per shape. Set it once, draw the world, reset, draw the UI.
 *
 * ```c
 * nya_render2d_camera_set(window, (NYA_Camera2DTopDown){ .position = player_position, .zoom = 2.0F });
 * // ... tiles and entities, in world coordinates ...
 * nya_render2d_camera_reset(window);
 * // ... HUD, in screen pixels ...
 * ```
 * */
NYA_API void nya_render2d_camera_set(NYA_Window* window, NYA_Camera2DTopDown camera);

/**
 * The same, through an isometric projection. Draw coordinates become **tile space**.
 *
 * A separate entry point rather than a mode, because the two cameras take different numbers: one has a
 * rotation and the other a tile size, and neither means anything to the other.
 *
 * ```c
 * nya_render2d_camera_isometric_set(window, (NYA_Camera2DIsometric){
 *     .position = { player_tile_x, player_tile_y }, .zoom = 1.0F, .tile_width = 64, .tile_height = 32,
 * });
 * ```
 * */
NYA_API void nya_render2d_camera_isometric_set(NYA_Window* window, NYA_Camera2DIsometric camera);

/** Back to screen pixels, origin at the target's top left. What drawing starts every frame as. */
NYA_API void nya_render2d_camera_reset(NYA_Window* window);

/**
 * The camera currently in effect, tagged with which kind it is.
 *
 * Kind NYA_CAMERA2D_KIND_NONE when none is set, which is the UI case — including the UI drawn over a
 * 3D scene, where render2d is deliberately left at screen pixels while render3d owns the world.
 * */
NYA_API NYA_Camera2D nya_render2d_camera_get(NYA_Window* window) __attr_no_discard;

/**
 * The current camera as a top-down one, or the identity when it is not one.
 *
 * The convenience for the overwhelmingly common case, where a caller knows perfectly well which kind
 * it set and wants the position and zoom without unwrapping a union. An isometric camera answers the
 * identity rather than something approximating it: there is no top-down camera that means the same
 * thing, and inventing one would put a caller's world coordinates in the wrong place silently.
 * */
NYA_API NYA_Camera2DTopDown nya_render2d_camera_top_down_get(NYA_Window* window) __attr_no_discard;

/**
 * Where a point on screen is in the world, under the current camera.
 *
 * The call a mouse needs: input reports screen pixels and the world is somewhere else entirely once
 * the camera has moved, zoomed or rotated. Exactly inverts nya_render2d_world_to_screen.
 * */
NYA_API f32x2 nya_render2d_screen_to_world(NYA_Window* window, f32x2 screen) __attr_no_discard;

/** Where a world point lands on screen. For pinning a label to something in the world. */
NYA_API f32x2 nya_render2d_world_to_screen(NYA_Window* window, f32x2 world) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * TEXTURES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Draws a whole texture asset at its natural size, top left at `x`/`y`.
 *
 * `tint` multiplies the texture. White leaves it untouched, which is the usual case; anything else
 * recolours it, and a tint alpha below one fades it. That is why a white sprite can be drawn in any
 * colour without a second copy of the image.
 *
 * A handle that is missing or still loading draws nothing rather than failing — assets load
 * asynchronously, so the first frames after a load are expected to have nothing to show.
 * */
NYA_API void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint);

/**
 * Draws part of a texture into an arbitrary rectangle: the general case behind nya_render2d_texture.
 *
 * `source_*` are in pixels within the texture, which is what a sprite sheet needs. Passing a
 * `destination` larger or smaller than the source scales, with the linear filtering the shared
 * sampler applies.
 * */
/**
 * Everything a textured draw can vary, for the cases the two simple calls above cannot express.
 *
 * Zero initialising is meaningful: an all-zero source rect means the whole texture, an all-zero
 * destination size means the source size, and an all-zero `tint` means white rather than invisible.
 * So `(NYA_Render2DTexture){ .x = 100, .y = 40, .rotation = angle }` is a complete call, and the fields
 * that are not mentioned behave the way nya_render2d_texture would.
 * */
typedef struct NYA_Render2DTexture NYA_Render2DTexture;

struct NYA_Render2DTexture {
    /** The part of the texture to read, in its pixels. A zero width or height means all of it. */
    f32 source_x, source_y, source_width, source_height;

    /**
     * Where `origin` lands, in the current coordinate space.
     *
     * Not necessarily the top left corner of what is drawn — that is what `origin` decides.
     * */
    f32 x, y;

    /** Size on screen. Zero means the source size, so a sprite draws at its natural scale. */
    f32 width, height;

    /** Clockwise, in radians, about `origin`. */
    f32 rotation;

    /**
     * The point the sprite rotates about and is positioned by, in destination pixels from its top
     * left corner.
     *
     * Zero is the top left, which matches every other draw call here. Half the width and height is
     * the centre, which is what a rotating entity almost always wants — a sprite rotated about its
     * corner orbits rather than spins.
     * */
    f32x2 origin;

    /*
     * Mirroring, done by swapping the texture coordinates rather than by negating the size.
     *
     * A negative width would also flip the winding, and with culling off that happens to work today —
     * but it would break the moment culling is turned on, and it makes the rotation pivot mirror too.
     */
    b8 flip_x;
    b8 flip_y;

    /** Multiplies the texture. All-zero means white, i.e. untouched. */
    NYA_Color tint;
};

/**
 * The general textured draw: rotation, a pivot, mirroring and scaling.
 *
 * What an entity renderer needs — a sprite at a position, turned to face somewhere, possibly
 * mirrored. The two calls above are this one with everything left at its default.
 * */
NYA_API void nya_render2d_texture_ex(NYA_Window* window, NYA_ConstCString texture_handle, NYA_Render2DTexture params);

NYA_API void nya_render2d_texture_rect(
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
);

/**
 * A nine-slice: a bordered image stretched to any size without stretching its corners.
 *
 * Scaling a rounded panel to twice its width scales its corner radius with it. A nine-slice cuts the
 * image into a three by three grid, draws the corners at source size, stretches the edges along one
 * axis only and the centre in both — so the border stays the width it was authored at.
 *
 * Borders are in *source* pixels, inward from each side: a 48x48 image with a 16-pixel round corner is
 * `.left = 16, .right = 16, .top = 16, .bottom = 16`.
 *
 * ```c
 * nya_render2d_nine_slice(window, NYA_ASSET_UI_PANEL_PNG, (NYA_NineSlice){
 *     .left = 12, .right = 12, .top = 12, .bottom = 12,
 *     .x = 40, .y = 40, .width = 320, .height = 96,
 * });
 * ```
 *
 * Nine quads in the ordinary batch, so a screen of panels is still one draw call if they share an atlas.
 * */
typedef struct NYA_NineSlice NYA_NineSlice;

struct NYA_NineSlice {
    /**
     * Border insets in source pixels: how much of each side is a corner or edge rather than centre.
     *
     * A zero on one axis collapses that axis's slices, which is a legitimate three-slice: `.left` and
     * `.right` with `.top` and `.bottom` at zero gives a horizontally stretchable bar.
     * */
    f32 left, right, top, bottom;

    /** Destination rectangle, in the current coordinate space. */
    f32 x, y, width, height;

    /**
     * Leave the middle patch undrawn.
     *
     * For a frame or an outline, where the centre of the source image is empty and drawing it would put
     * a stretched transparent quad — six vertices and a blend — over whatever the panel sits on.
     * */
    b8 hollow;

    /** Multiplies the texture. All-zero means white, i.e. untouched. */
    NYA_Color tint;
};

/**
 * Draws `texture_handle` as a nine-slice. See NYA_NineSlice.
 *
 * A destination smaller than the borders it was given would make the corners overlap and the edges draw
 * inside out. The borders are scaled down proportionally instead, so a panel collapsed to nothing shrinks
 * cleanly rather than turning into an artefact — which is what an animating panel does on its first frame.
 * */
NYA_API void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params);

/*
 * ─────────────────────────────────────────────────────────
 * TEXT
 * ─────────────────────────────────────────────────────────
 */

/**
 * Sets the font every later nya_render2d_text and measurement uses.
 *
 * Immediate mode state, like the rest of this module: set it once and draw, rather than naming the
 * font on every call. It persists across frames — there is no per frame reset — so a game that uses
 * one face sets it at startup and never mentions it again.
 *
 * The font is rasterized into a glyph atlas the first time it is used, which is why this can be
 * called before the asset has finished loading: nothing happens until something is drawn or
 * measured.
 * */
NYA_API void nya_render2d_font_set(NYA_ConstCString font_path, f32 point_size);

/** The current font, or null when none has been set. */
NYA_API NYA_ConstCString nya_render2d_font_get(void) __attr_no_discard;

/** Point size of the current font. Pairs with nya_render2d_font_get; the two are one setting. */
NYA_API f32 nya_render2d_font_size_get(void) __attr_no_discard;

/**
 * Draws `text` in the current font, `x`/`y` being the top left of the line box.
 *
 * The pen is the *top* of the line, not the baseline — consistent with every other coordinate here, and
 * what makes stacking lines a matter of adding the line height. A whole line is one draw call, since
 * every glyph comes out of the same atlas.
 *
 * ASCII 32 to 126 is baked eagerly; anything beyond is baked on first use, up to
 * NYA_RENDER2D_GLYPH_CAPACITY cells per atlas. Past that a glyph advances by a space and draws nothing,
 * which is visible as a gap rather than a crash. The cap is a fixed grid, so CJK is out of scope — that
 * wants a real packer and eviction rather than a bigger number.
 *
 * Newlines are honoured, using the font's own line advance. There is no wrapping: where lines break is
 * a layout decision, and this draws what it is given.
 * */
NYA_API void nya_render2d_text(NYA_Window* window, NYA_ConstCString text, f32 x, f32 y, NYA_Color color);

/** Draws with a named font, leaving the current one alone. For the occasional odd label. */
NYA_API void nya_render2d_text_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text, f32 x, f32 y, NYA_Color color);

/**
 * printf into nya_render2d_text. The common case, since almost no drawn text is a literal.
 *
 * ```c
 * nya_render2d_textf(window, 16, 16, NYA_COLOR_WHITE, "score %d  x %.1f", score, position.x);
 * ```
 *
 * Formats into a NYA_RENDER2D_TEXT_MAX byte stack buffer and draws it. Nothing is allocated, and a
 * result past that length is truncated rather than growing — a line of HUD text that runs past a
 * few hundred characters is a bug, and silently allocating for it hides the bug on the hot path.
 *
 * The format string is checked at compile time, so a mismatched argument is an error here rather
 * than garbage on screen.
 * */
NYA_API void nya_render2d_textf(NYA_Window* window, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) __attr_fmt_printf(5, 6);

/** nya_render2d_textf against a named font, the way nya_render2d_text_with_font is to nya_render2d_text. */
NYA_API void nya_render2d_textf_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color,
                                      NYA_ConstCString format, ...) __attr_fmt_printf(7, 8);

/*
 * ── Measurement ──
 *
 * Everything a layout pass needs before anything is on screen: centring, right alignment, hit
 * testing and line stacking. These use the same metrics the drawing path does, so the two agree, and
 * they build the atlas on demand — measuring before the first draw gives real numbers rather than
 * zero.
 */

/** Width and height of `text` in the current font, in pixels. Height counts every line. */
/*
 * ── Wrapped and aligned text ──
 */

typedef enum NYA_TextAlign        NYA_TextAlign;
typedef struct NYA_Render2DTextBox NYA_Render2DTextBox;

/** Where a line sits within the box's width. Left is the zero value, so it is the default. */
enum NYA_TextAlign {
    NYA_TEXT_ALIGN_LEFT = 0,
    NYA_TEXT_ALIGN_CENTER,
    NYA_TEXT_ALIGN_RIGHT,

    NYA_TEXT_ALIGN_COUNT,
};

/**
 * A block of text laid out in a box: wrapped to a width, aligned, and optionally truncated.
 *
 * nya_render2d_text draws one run at a position and breaks only where the string already has a newline,
 * which is everything a HUD line needs and nothing a paragraph does. Everything here is about the case
 * that has a *width*: dialogue, a tooltip, item description, anything whose length is not known when the
 * layout is written.
 *
 * Zero means unspecified throughout, so `(NYA_Render2DTextBox){ .x = 20, .y = 20, .width = 300 }` is a
 * complete call.
 * */
struct NYA_Render2DTextBox {
    /** The top left of the box, in the current coordinate space. */
    f32 x, y;

    /**
     * How wide lines may be before they wrap. Zero disables wrapping entirely.
     *
     * With wrapping off the box still aligns and still respects newlines — which is how a centred title
     * is written, since centring needs a width to centre *within* only when lines can be shorter than it.
     * A zero width with centre alignment therefore centres on `x` itself.
     * */
    f32 width;

    NYA_TextAlign align;

    /**
     * Multiplier on the font's own line height. Zero becomes one.
     *
     * A multiplier rather than a pixel gap, because the right leading depends on the point size and a
     * fixed gap goes wrong the moment the font does.
     * */
    f32 line_spacing;

    /** Stops after this many lines. Zero is unlimited. */
    u32 max_lines;

    /**
     * Replace the end of the last line with an ellipsis when `max_lines` cut the text short.
     *
     * Only does anything when something was actually dropped, so a string that fits is never marked as
     * though it did not.
     * */
    b8 ellipsis;

    NYA_Color color;

    /** Zero uses whatever nya_render2d_font_set last named, like the rest of the text calls. */
    NYA_ConstCString font_path;
    f32              point_size;
};

/**
 * Draws wrapped, aligned text and returns the size it occupied.
 *
 * The return value is the laid-out block's width and height — the widest line and the total of the line
 * heights — which is what a panel drawn behind the text needs and would otherwise have to be computed by
 * calling the measure below with the identical parameters.
 *
 * Breaks at spaces where it can and inside a word where it cannot: a single word longer than the box is
 * split rather than allowed to overflow, because overflowing text in a UI is worse than an ugly break.
 * */
NYA_API f32x2 nya_render2d_text_box(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params);

/** The size nya_render2d_text_box would occupy, without drawing. Same layout, same parameters. */
NYA_API f32x2 nya_render2d_text_box_measure(NYA_ConstCString text, NYA_Render2DTextBox params) __attr_no_discard;

NYA_API f32x2 nya_render2d_text_measure(NYA_ConstCString text) __attr_no_discard;
NYA_API f32x2 nya_render2d_text_measure_with_font(NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text) __attr_no_discard;

/** Width alone, for the common case of aligning on one axis. */
NYA_API f32 nya_render2d_text_width(NYA_ConstCString text) __attr_no_discard;

/** Height alone: one line height per line in `text`, so a single line is exactly the line height. */
NYA_API f32 nya_render2d_text_height(NYA_ConstCString text) __attr_no_discard;

/**
 * Baseline to baseline distance for the current font.
 *
 * What to add to y for the next line. Larger than the font's height, because it includes the gap the
 * designer intended between lines.
 * */
NYA_API f32 nya_render2d_font_line_height(void) __attr_no_discard;

/**
 * Top of the line box to the baseline.
 *
 * The offset to add to a y to sit something on the same baseline as the text — an underline, an
 * icon beside a label, a caret.
 * */
NYA_API f32 nya_render2d_font_ascent(void) __attr_no_discard;

/**
 * Baseline to the bottom of the deepest descender, as a positive number.
 *
 * SDL reports this negative; it is flipped here so that ascent plus descent is the ink height, which
 * is what a caller doing arithmetic expects.
 * */
NYA_API f32 nya_render2d_font_descent(void) __attr_no_discard;

/** Ascent plus descent: the tallest a single line of this font can be, ignoring line spacing. */
NYA_API f32 nya_render2d_font_height(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * SHADERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Draws everything after this with `pipeline_handle` instead of the built in pipeline.
 *
 * The counterpart to raylib's BeginShaderMode. The pipeline is fed out of this batch, so it has two
 * hard requirements:
 *
 * - **`.vertex_layout = NYA_VERTEX_LAYOUT_2D`** on the pipeline asset. The default is the wider
 *   NYA_Vertex3D layout, and a mismatch is not an error anywhere — the shader reads the batch's twenty
 *   byte vertices at a sixty-four byte stride and draws geometry in nonsense positions.
 * - A vertex shader taking POSITION, COLOR0 and TEXCOORD0, with a `float4x4` projection in uniform
 *   slot 0, because that is what the batch pushes.
 *
 * Copy `assets/shader/source/shape.vert.hlsl` and change only the fragment stage; that is the
 * intended path, and `shape_grayscale.frag.hlsl` is a worked example.
 *
 * Anything queued is flushed first, so shapes drawn before this are not retroactively shaded.
 *
 * A custom pipeline that samples a texture only sees one if the draws inside the mode are textured;
 * shapes bind nothing at t0.
 * */
NYA_API void nya_render2d_shader_begin(NYA_Window* window, NYA_ConstCString pipeline_handle);

/**
 * Hands `size` bytes to the current custom shader's fragment stage, at uniform slot 0.
 *
 * The parameters a post-process needs and the vertex layout cannot carry: a texel size, a blur
 * direction, a threshold, a time. Vertex slot 0 is the projection matrix and is not available.
 *
 * ```c
 * // A separable blur: same pipeline twice, once across and once down.
 * typedef struct { f32 texel_x, texel_y, direction_x, direction_y; } BlurUniform;
 *
 * nya_render2d_shader_begin(window, "blur_pipeline");
 * nya_render2d_shader_set_uniform(window, &(BlurUniform){ 1.0F / w, 1.0F / h, 1.0F, 0.0F }, sizeof(BlurUniform));
 * nya_render2d_render_texture(window, &scene, x, y, w, h, NYA_COLOR_WHITE);
 * nya_render2d_shader_end(window);
 * ```
 *
 * Anything queued is flushed first, because a uniform is per draw call — so changing it per sprite
 * costs a draw call per sprite. Set it once per pass.
 *
 * Cleared by nya_render2d_shader_end, so a custom shader never inherits the previous one's parameters.
 * */
NYA_API void nya_render2d_shader_set_uniform(NYA_Window* window, const void* data, u32 size);

/** Returns to the built in pipelines, flushing whatever the custom one still has queued. */
NYA_API void nya_render2d_shader_end(NYA_Window* window);

/**
 * Draws `vertex_count` vertices from a pipeline that generates its own geometry, with no vertex
 * buffer bound.
 *
 * For the shader that builds its shape from SV_VertexID — a fullscreen triangle, a procedural
 * effect, a debug primitive. There is nothing for the 2D batch to hold, so this is not batched: it
 * flushes what is queued, issues its own draw, and leaves the batch to rebind on the next shape.
 *
 * `uniform_data` is pushed to slot 0 of *both* the vertex and fragment stages, since a generator
 * shader normally wants the same value in each. Null skips the push.
 *
 * This exists so a layer never has to reach past the renderer. A layer's on_render calling
 * SDL_BindGPUGraphicsPipeline and SDL_DrawGPUPrimitives itself works, but it writes into a render
 * pass the batch believes it owns — the batch's idea of the bound pipeline goes stale, and the next
 * batched shape draws with whatever the layer left bound. That is the bug behind
 * '!"Graphics pipeline not bound!"'.
 * */
NYA_API void nya_render2d_procedural(NYA_Window* window, NYA_ConstCString pipeline_handle, u32 vertex_count, const void* uniform_data, u32 uniform_size);

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

typedef struct NYA_Light2D NYA_Light2D;

/**
 * What an entity emits, if anything. Zeroed means it emits nothing, which is almost every entity.
 *
 * Declared here rather than on the entity because the renderer is what consumes it, and core_entity.h
 * already includes this file — the other direction would be a cycle. NYA_Entity.light is one of
 * these, and so is anything a game assembles by hand to light a scene with no entities in it.
 *
 * "Emits light" is a property of the thing rather than of the drawing, which is why it is on the
 * entity and not on NYA_EntityVisual: a torch carried into a room lights it whether or not the torch
 * itself is on screen, and something invisible can still glow.
 *
 * Collected by nya_system_entity_lights and drawn as one multiply pass over the finished scene — see
 * nya_render2d_lights_apply. There is no per-light shadow and no occlusion: a 2D scene has no
 * geometry to cast one, and faking it is a different feature (a visibility polygon) rather than a
 * bigger version of this.
 * */
struct NYA_Light2D {
    /**
     * How far the light reaches, in world units. Zero means the entity emits nothing.
     *
     * The falloff is windowed to land on exactly zero here rather than trailing off forever, so this
     * is a real edge and not a suggestion — which is what makes culling lights by distance correct.
     * */
    f32 radius;

    /**
     * How bright it is at the centre. One is "fully lights what it covers".
     *
     * Above one over-brightens, which is how a light reads as a *source* rather than as illumination
     * — a lamp should blow out its own bulb.
     * */
    f32 intensity;

    /** Zeroed is read as white, so a light that only says how big it is still works. */
    NYA_Color color;

    /**
     * Where the light sits relative to the entity, in world units.
     *
     * Because the thing that glows is rarely the thing's origin: a torch is held at the top of a
     * sprite whose origin is at its feet, and a muzzle flash is at the end of a barrel.
     * */
    f32x2 offset;
};

/** The handle the built in light pipeline is registered under. Shared by every window. */
#define NYA_RENDER2D_PIPELINE_LIGHT "nya_light2d_pipeline"

/**
 * Draws a light map over everything already in the target, darkening what no light reaches.
 *
 * Called from a layer's on_render **after** the world is drawn and **before** the HUD, with the
 * camera still set — the lights are given in world coordinates and this converts them:
 *
 * ```c
 * nya_render2d_camera_set(window, camera);
 * gny_world_draw(window);
 *
 * NYA_Light2D lights[NYA_SHADER_LIGHT2D_MAX];
 * f32x2           positions[NYA_SHADER_LIGHT2D_MAX];
 * u32             count = nya_system_entity_lights(min, max, lights, positions, nya_carray_length(lights));
 *
 * nya_render2d_lights_apply(window, lights, positions, count, (NYA_Color){ 0.15F, 0.15F, 0.22F, 1.0F });
 * nya_render2d_camera_reset(window);
 * ```
 *
 * ## How it works, and what that costs
 *
 * One fullscreen triangle, blended NYA_BLEND_MULTIPLY. The shader computes how much light reaches
 * each pixel and the multiply applies it to the scene already there — so a fully lit pixel is left
 * exactly as it was and an unlit one falls to `ambient`.
 *
 * Multiply rather than a deferred pass, because a 2D scene has no normals and no depth to shade
 * with. Each light is a radial falloff around a point, which is what a torch looks like from
 * directly above and is the whole of what a light can mean without geometry to bounce off. There is
 * no shadow and no occlusion: faking those is a visibility polygon per light, which is a different
 * feature rather than a bigger version of this.
 *
 * One draw call, whatever the light count, because the lights are a uniform rather than geometry.
 * At most NYA_SHADER_LIGHT2D_MAX of them; nya_system_entity_lights already sorts so that the ones
 * dropped past the cap are the dimmest.
 *
 * `ambient` is the floor. Zero is pitch black, which almost nothing wants — a night scene still has
 * a moon, and a scene with no lights at all and a zero ambient renders as a black rectangle that
 * looks exactly like a broken renderer.
 * */
NYA_API void nya_render2d_lights_apply(NYA_Window* window, const NYA_Light2D* lights, const f32x2* positions, u32 count, NYA_Color ambient);

/*
 * ─────────────────────────────────────────────────────────
 * SCISSOR
 * ─────────────────────────────────────────────────────────
 */

/**
 * Clips everything drawn afterwards to a rectangle, in the current target's pixels.
 *
 * What a scroll view is: draw the clip, draw the contents shifted by the scroll offset, and the part
 * outside simply does not appear. Also how a label is truncated rather than overflowing its panel.
 *
 * **Screen pixels, not world coordinates**, even while a camera is set — a clip is a region of the
 * target rather than a region of the world. Use nya_render2d_world_to_screen to clip to something that
 * moves with the camera.
 *
 * Anything queued is flushed first, because clipping is render pass state rather than anything
 * carried in a vertex. So this costs a draw call, the same way a camera change does.
 *
 * Does not nest, and does not intersect with an outer rectangle: a second call replaces the first.
 * Nesting is what a UI framework built on top of this should do, by intersecting before it calls.
 * */
/**
 * Which layer subsequent draws land in. Painted low to high, whatever order they were declared in.
 *
 * The problem this solves: a dropdown is declared *inside* the panel that owns it, and has to paint
 * over the panels declared after it. Without layers the only fix is to restructure the call order,
 * which an immediate mode UI cannot do — the panel does not know the dropdown exists until it is
 * asked to draw one.
 *
 * Within a layer, declaration order still decides, so a frame that never touches this renders exactly
 * as it always did. Zero is the default and negative layers are fine, for a backdrop behind everything.
 *
 * Ordering holds until the next nya_render2d_flush, which a render target change or the end of the
 * frame forces — so a layer cannot reorder across a target switch, and should not: the two targets
 * are different images.
 * */
NYA_API void nya_render2d_layer_set(NYA_Window* window, s32 layer);

/** The layer draws currently land in. */
NYA_API s32 nya_render2d_layer(NYA_Window* window) __attr_no_discard;

NYA_API void nya_render2d_scissor_begin(NYA_Window* window, f32 x, f32 y, f32 width, f32 height);

/** Stops clipping. Everything afterwards can draw anywhere in the target again. */
NYA_API void nya_render2d_scissor_end(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────
 * RENDER TEXTURES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Creates an offscreen target that can be drawn into and then drawn with.
 *
 * The engine's RenderTexture2D. Created at the window's swapchain format, so every pipeline that
 * draws to the window also draws to this — see NYA_RenderTexture.
 *
 * Not an asset: it has no file behind it, nothing to hot reload, and its lifetime belongs to whoever
 * made it. Destroy it with nya_render_texture_destroy.
 * */
NYA_API NYA_RenderTexture nya_render_texture_create(NYA_Window* window, u32 width, u32 height) __attr_no_discard;
NYA_API void              nya_render_texture_destroy(NYA_RenderTexture* render_texture);

/**
 * Points subsequent drawing at `render_texture`, clearing it to `clear`.
 *
 * The counterpart to raylib's BeginTextureMode. Coordinates inside are relative to the texture, with
 * `(0, 0)` its top left corner, and the projection uses the texture's size rather than the window's.
 *
 * Does not nest: one target at a time, ended with nya_render_texture_end before another begins.
 * */
NYA_API void nya_render_texture_begin(NYA_Window* window, NYA_RenderTexture* render_texture, NYA_Color clear);

/** Returns drawing to the window, keeping everything already drawn there. */
NYA_API void nya_render_texture_end(NYA_Window* window);

/**
 * Draws a render texture, top left at `x`/`y`, scaled into `width` by `height`.
 *
 * Separate from nya_render2d_texture because a render texture is not an asset and has no handle. Pass
 * zero for `width`/`height` to draw at its natural size.
 * */
NYA_API void nya_render2d_render_texture(NYA_Window* window, const NYA_RenderTexture* render_texture, f32 x, f32 y, f32 width, f32 height, NYA_Color tint);

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Vertices queued since the last flush.
 *
 * For a debug overlay reporting how much the frame is batching, and for tests, which otherwise have
 * no way to observe that a draw call did anything without a GPU.
 * */
NYA_API u32 nya_render2d_pending_vertex_count(NYA_Window* window) __attr_no_discard;

/**
 * Size of whatever is currently being drawn into: the window, or the render texture that replaced it.
 *
 * The pair nya_render2d_screen_to_world maps against, which is why it is exposed at all — a caller
 * working out which part of the world is on screen needs the target's corners, and the window's own
 * size is the wrong answer the moment a render texture is bound.
 * */
NYA_API void nya_render2d_target_size(NYA_Window* window, OUT u32* out_width, OUT u32* out_height);

/** What the 2D batch cost this frame. Reset when the frame opens; read it after drawing. */
typedef struct NYA_Render2DFrameStats NYA_Render2DFrameStats;

struct NYA_Render2DFrameStats {
    /** Draw calls issued. The number that matters: each one is a state change that could not batch. */
    u32 draw_calls;

    u32 vertices;
    u32 indices;

    /** draw_calls split by what forced each one. Indexed by NYA_Render2DFlushReason. */
    u32 draw_calls_by_reason[NYA_RENDER2D_FLUSH_REASON_COUNT];

    /**
     * Draws asked for that produced nothing.
     *
     * Non-zero is not necessarily wrong — assets load asynchronously, so the first frames after a
     * load legitimately drop draws — but a number that stays non-zero means something is being asked
     * for and never appearing, which is otherwise invisible.
     * */
    u32 dropped_draws;
};

/** A human readable name for a flush reason, for an overlay or a log line. */
NYA_API NYA_ConstCString nya_render2d_flush_reason_name(NYA_Render2DFlushReason reason) __attr_no_discard;

NYA_API NYA_Render2DFrameStats nya_render2d_frame_stats(NYA_Window* window) __attr_no_discard;
