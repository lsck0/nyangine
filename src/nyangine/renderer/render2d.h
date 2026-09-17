/**
 * @file render2d.h
 *
 * ```c
 * void layer_on_render(NYA_Window* window) {
 *     nya_render2d_rect(window, 16, 16, 200, 48, (NYA_Color){ 0.1F, 0.1F, 0.12F, 0.9F });
 *     nya_render2d_font_set(NYA_ASSET_FONTS_ALDRICH_TTF, 24.0F);
 *     nya_render2d_text(window, "score: 42", 24, 28, NYA_COLOR_WHITE);
 * }
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_camera.h"
#include "nyangine/renderer/render_color.h"
#include "nyangine/renderer/renderer.h"

// the C side of the shaders' constant buffers lives beside the shaders. see assets/shader/uniforms.h.
#include "../../../assets/shader/uniforms.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Vertices held before a flush is forced.
 * */
/**
 * Longest string nya_render2d_textf will produce, including the terminator.
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
 * */


/**
 * Releases everything the drawing module holds that is not tied to a window.
 * */
NYA_API void nya_render2d_shutdown(void);

/**
 * Uploads and draws everything accumulated so far, then empties the batch.
 * */
NYA_API void nya_render2d_flush(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────
 * SHAPES
 * ─────────────────────────────────────────────────────────
 */

/** A filled axis aligned rectangle, `x`/`y` being its top left corner. */
NYA_API void nya_render2d_rect(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, NYA_Color color);

/** A rectangle outline of `thickness`, drawn inside the given bounds. */
NYA_API void nya_render2d_rect_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 thickness, NYA_Color color);

/** A filled rectangle with corners of `radius`, clamped to half the shorter side. One fan, so any alpha blends once. */
NYA_API void nya_render2d_rect_rounded(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 radius, NYA_Color color);

/** The outline of that rectangle, `thickness` wide and inside the bounds, as one ring. */
NYA_API void nya_render2d_rect_rounded_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 radius, f32 thickness, NYA_Color color);

/**
 * A filled rectangle turned about its own centre.
 *
 * ```c
 * NYA_Entity* entity = nya_entity_get(crate);
 * nya_render2d_rect_rotated(window, (f32x2){ entity->position.x, entity->position.y }, entity->physics2d.size,
 *                       nya_physics2d_rotation(entity), NYA_COLOR_ORANGE);
 * ```
 * */
NYA_API void nya_render2d_rect_rotated(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, NYA_Color color);

/**
 * The outline of that rectangle, as four lines of `thickness` along its edges.
 * */
NYA_API void nya_render2d_rect_rotated_outline(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, f32 thickness, NYA_Color color);

/** A line of `thickness`, as a quad. Both ends are square; there is no cap or join. */
NYA_API void nya_render2d_line(NYA_Window* window, f32x2 from, f32x2 to, f32 thickness, NYA_Color color);

/**
 * Connects `count` points with lines of `thickness`, in order and without closing the loop.
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
 * Looks at the world from `camera` for everything drawn afterwards. The projection is per draw call, so queued
 * shapes are flushed first; set the camera once, draw the world, reset, draw the UI.
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
 * The same through an isometric projection. Draw coordinates become tile space.
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
 * */
NYA_API NYA_Camera2D nya_render2d_camera_get(NYA_Window* window) __attr_no_discard;

/**
 * The current camera as a top-down one, or the identity when it is not one.
 * */
NYA_API NYA_Camera2DTopDown nya_render2d_camera_top_down_get(NYA_Window* window) __attr_no_discard;

/**
 * Where a point on screen is in the world, under the current camera.
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
 * */
NYA_API void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint);

/**
 * Draws part of a texture into an arbitrary rectangle: the general case behind nya_render2d_texture.
 * */
/**
 * Everything a textured draw can vary, for the cases the two simple calls above cannot express.
 * */
typedef struct NYA_Render2DTexture NYA_Render2DTexture;

struct NYA_Render2DTexture {
    /** The part of the texture to read, in its pixels. A zero width or height means all of it. */
    f32 source_x, source_y, source_width, source_height;

    /** Where `origin` lands, in the current coordinate space. */
    f32 x, y;

    /** Size on screen. Zero means the source size, so a sprite draws at its natural scale. */
    f32 width, height;

    /** Clockwise, in radians, about `origin`. */
    f32 rotation;

    /** The point the sprite rotates about and is positioned by, in destination pixels from its top left. */
    f32x2 origin;

    /* Mirroring swaps the texture coordinates. */
    b8 flip_x;
    b8 flip_y;

    /** Multiplies the texture. All-zero means white, i.e. untouched. */
    NYA_Color tint;
};

/**
 * The general textured draw: rotation, a pivot, mirroring and scaling.
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
 * ```c
 * nya_render2d_nine_slice(window, NYA_ASSET_UI_PANEL_PNG, (NYA_NineSlice){
 *     .left = 12, .right = 12, .top = 12, .bottom = 12,
 *     .x = 40, .y = 40, .width = 320, .height = 96,
 * });
 * ```
 * */
typedef struct NYA_NineSlice NYA_NineSlice;

struct NYA_NineSlice {
    /**
     * Border insets in source pixels: how much of each side is a corner or edge rather than centre.
     * */
    f32 left, right, top, bottom;

    /** Destination rectangle, in the current coordinate space. */
    f32 x, y, width, height;

    /**
     * Leave the middle patch undrawn.
     * */
    b8 hollow;

    /** Multiplies the texture. All-zero means white, i.e. untouched. */
    NYA_Color tint;
};

/**
 * Draws `texture_handle` as a nine-slice. See NYA_NineSlice.
 * */
NYA_API void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params);

/*
 * ─────────────────────────────────────────────────────────
 * TEXT
 * ─────────────────────────────────────────────────────────
 */

/**
 * Sets the font every later nya_render2d_text and measurement uses.
 * */
NYA_API void nya_render2d_font_set(NYA_ConstCString font_path, f32 point_size);

/** The current font, or null when none has been set. */
NYA_API NYA_ConstCString nya_render2d_font_get(void) __attr_no_discard;

/** Point size of the current font. Pairs with nya_render2d_font_get; the two are one setting. */
NYA_API f32 nya_render2d_font_size_get(void) __attr_no_discard;

/**
 * Draws `text` in the current font, `x`/`y` being the top left of the line box.
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
 * */
NYA_API void nya_render2d_textf(NYA_Window* window, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) __attr_fmt_printf(5, 6);

/** nya_render2d_textf against a named font, the way nya_render2d_text_with_font is to nya_render2d_text. */
NYA_API void nya_render2d_textf_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color,
                                      NYA_ConstCString format, ...) __attr_fmt_printf(7, 8);

/*
 * Measurement, for layout before anything is drawn. Same metrics as drawing, and the atlas is built on demand,
 * so measuring first still gives real numbers.
 */

/** Width and height of `text` in the current font, in pixels. Height counts every line. */
/* Wrapped and aligned text. */

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
 * */
struct NYA_Render2DTextBox {
    /** The top left of the box, in the current coordinate space. */
    f32 x, y;

    /**
     * How wide lines may be before they wrap. Zero disables wrapping entirely.
     * */
    f32 width;

    NYA_TextAlign align;

    /**
     * Multiplier on the font's own line height. Zero becomes one.
     * */
    f32 line_spacing;

    /** Stops after this many lines. Zero is unlimited. */
    u32 max_lines;

    /**
     * Replace the end of the last line with an ellipsis when `max_lines` cut the text short.
     * */
    b8 ellipsis;

    NYA_Color color;

    /** Zero uses whatever nya_render2d_font_set last named, like the rest of the text calls. */
    NYA_ConstCString font_path;
    f32              point_size;
};

/**
 * Draws wrapped, aligned text and returns the size it occupied.
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
 * */
NYA_API f32 nya_render2d_font_line_height(void) __attr_no_discard;

/**
 * Top of the line box to the baseline.
 * */
NYA_API f32 nya_render2d_font_ascent(void) __attr_no_discard;

/**
 * Baseline to the bottom of the deepest descender, as a positive number.
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
 * */
NYA_API void nya_render2d_shader_begin(NYA_Window* window, NYA_ConstCString pipeline_handle);

/**
 * Hands `size` bytes to the current custom shader's fragment stage, at uniform slot 0.
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
 * */
NYA_API void nya_render2d_shader_set_uniform(NYA_Window* window, const void* data, u32 size);

/**
 * Binds a second image at t1 for the current custom shader, beside the texture being drawn: a texture or a lookup
 * table asset, sampled linearly and clamped. Returns false, binding nothing, while it is not loaded.
 *
 * ```c
 * nya_render2d_shader_begin(window, "grade_pipeline");
 * if (nya_render2d_shader_set_texture(window, NYA_ASSET_TEXTURES_GRADE_CUBE)) nya_render2d_render_texture(window, &scene, ...);
 * nya_render2d_shader_end(window);
 * ```
 * */
NYA_API b8 nya_render2d_shader_set_texture(NYA_Window* window, NYA_ConstCString texture_handle);

/** Returns to the built in pipelines, flushing whatever the custom one still has queued. */
NYA_API void nya_render2d_shader_end(NYA_Window* window);

/** Draws `vertex_count` vertices from a pipeline that generates its own geometry, with no vertex buffer. */
NYA_API void nya_render2d_procedural(NYA_Window* window, NYA_ConstCString pipeline_handle, u32 vertex_count, const void* uniform_data, u32 uniform_size);

/**
 * One fullscreen triangle through `pipeline_handle`, with `textures` bound from t0 in order and `uniform` at
 * fragment slot 0. For a pass reading several images, which the batch's one texture per draw cannot carry. The
 * pipeline pairs NYA_ASSET_SHADER_PROCEDURAL_VERT, whose uv has v growing up. Sampled linearly; a shader that
 * must not blend texels reads them with Load.
 *
 * ```c
 * SDL_GPUTexture* inputs[] = { scene.texture, scene.normal_texture };
 * nya_render2d_fullscreen(window, "ink_pipeline", inputs, 2, &ink, sizeof(ink));
 * ```
 * */
NYA_API void nya_render2d_fullscreen(
    NYA_Window*            window,
    NYA_ConstCString       pipeline_handle,
    SDL_GPUTexture* const* textures,
    u32                    texture_count,
    const void*            uniform,
    u32                    uniform_size
);

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

typedef struct NYA_Light2D NYA_Light2D;

/**
 * What an entity emits, if anything. Zeroed means it emits nothing, which is almost every entity.
 * */
struct NYA_Light2D {
    /**
     * How far the light reaches, in world units. Zero means the entity emits nothing.
     * */
    f32 radius;

    /**
     * How bright it is at the centre. One is "fully lights what it covers".
     * */
    f32 intensity;

    /** Zeroed is read as white, so a light that only says how big it is still works. */
    NYA_Color color;

    /**
     * Where the light sits relative to the entity, in world units.
     * */
    f32x2 offset;
};

/** The handle the built in light pipeline is registered under. Shared by every window. */
#define NYA_RENDER2D_PIPELINE_LIGHT "nya_light2d_pipeline"

/**
 * Draws a light map over everything already in the target, darkening what no light reaches.
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
 * */
NYA_API void nya_render2d_lights_apply(NYA_Window* window, const NYA_Light2D* lights, const f32x2* positions, u32 count, NYA_Color ambient);

/*
 * ─────────────────────────────────────────────────────────
 * SCISSOR
 * ─────────────────────────────────────────────────────────
 */

/**
 * Clips everything drawn afterwards to a rectangle, in the current target's pixels.
 * */
/**
 * Which layer subsequent draws land in. Painted low to high, whatever order they were declared in.
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
 * Creates an offscreen target that can be drawn into and then drawn with. It has a depth buffer; use
 * nya_render_texture_create_with to drop it for a 2D-only target.
 * */
NYA_API NYA_RenderTexture nya_render_texture_create(NYA_Window* window, u32 width, u32 height) __attr_no_discard;

/**
 * As nya_render_texture_create, with `options`.
 *
 * ```c
 * // A target for fullscreen 2D passes, with no depth buffer behind it.
 * NYA_RenderTexture scratch = nya_render_texture_create_with(
 *     window, width, height, (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_NONE }
 * );
 * ```
 * */
NYA_API NYA_RenderTexture nya_render_texture_create_with(NYA_Window* window, u32 width, u32 height,
                                                         NYA_RenderTextureOptions options) __attr_no_discard;

NYA_API void nya_render_texture_destroy(NYA_RenderTexture* render_texture);

/**
 * Whether `render_texture` can be drawn into as it is: created, `width` by `height`, and at the renderer's sample
 * count unless made single sampled. The check before recreating a target.
 *
 * ```c
 * if (!nya_render_texture_is_current(&view->target, width, height)) {
 *     nya_render_texture_destroy(&view->target);
 *     view->target = nya_render_texture_create(window, width, height);
 * }
 * ```
 * */
NYA_API b8 nya_render_texture_is_current(const NYA_RenderTexture* render_texture, u32 width, u32 height) __attr_no_discard;

/**
 * Points subsequent drawing at `render_texture`, clearing it to `clear`.
 * */
NYA_API void nya_render_texture_begin(NYA_Window* window, NYA_RenderTexture* render_texture, NYA_Color clear);

/** Returns drawing to the window, keeping everything already drawn there. */
NYA_API void nya_render_texture_end(NYA_Window* window);

/**
 * Draws a render texture, top left at `x`/`y`, scaled into `width` by `height`.
 * */
NYA_API void nya_render2d_render_texture(NYA_Window* window, const NYA_RenderTexture* render_texture, f32 x, f32 y, f32 width, f32 height, NYA_Color tint);


/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Vertices queued since the last flush.
 * */
NYA_API u32 nya_render2d_pending_vertex_count(NYA_Window* window) __attr_no_discard;

/**
 * Size of whatever is currently being drawn into: the window, or the render texture that replaced it.
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
     * */
    u32 dropped_draws;
};

/** A human readable name for a flush reason, for an overlay or a log line. */
NYA_API NYA_ConstCString nya_render2d_flush_reason_name(NYA_Render2DFlushReason reason) __attr_no_discard;

NYA_API NYA_Render2DFrameStats nya_render2d_frame_stats(NYA_Window* window) __attr_no_discard;
