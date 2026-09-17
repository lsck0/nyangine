/**
 * @file render_post.h
 *
 * ```c
 * // Once, wherever the game keeps its renderer state.
 * static NYA_PostChain post = { 0 };
 *
 * // Each frame.
 * if (nya_post_begin(window, &post)) {
 *     draw_the_world(window);
 *     nya_post_end(window, &post, (NYA_PostPass[]){
 *         { .pipeline = PIPELINE_BLOOM, .uniform = &bloom, .uniform_size = sizeof(bloom) },
 *         { .pipeline = PIPELINE_CRT,   .uniform = &crt,   .uniform_size = sizeof(crt)   },
 *     }, 2);
 * } else {
 *     draw_the_world(window);   // no target this frame; the scene still has to appear
 * }
 * ```
 *
 * The scene passes are options on the window rather than passes, since they read buffers only the chain
 * knows about. One call each turns them on, and a zeroed field takes its default:
 *
 * ```c
 * nya_post_ink_set(window, (NYA_PostInk){ .enabled = true });
 * nya_post_ambient_occlusion_set(window, (NYA_PostAmbientOcclusion){ .enabled = true, .strength = 0.4F });
 * nya_post_antialias_set(window, (NYA_PostAntialias){ .enabled = true });
 * nya_post_depth_of_field_set(window, (NYA_PostDepthOfField){ .focus = NYA_POST_FOCUS_TILT_SHIFT });
 * nya_post_speed_lines_set(window, (NYA_PostSpeedLines){ .amount = camera_speed / top_speed, .motion = camera_velocity });
 * nya_post_bloom_set(window, (NYA_PostBloom){ .enabled = true });
 * ```
 *
 * They run inside nya_post_end before the caller's passes, occlusion then ink then depth of field then
 * antialiasing, and the debug view after them. Depth of field follows the ink, so a line blurs with the surface it
 * is drawn on, and comes before antialiasing, which smooths the cut between sharp and blurred. A feature that is off
 * has no pass, no pipeline and no target. Ink, occlusion and distance focus read the scene normal buffer
 * (NYA_RENDER3D_NORMAL_FORMAT), which the chain's scene target carries only while one of them or a debug view is on,
 * and they skip a frame whose capture drew no 3D. Bloom and speed lines are drawn after the caller's passes, so a grade
 * shapes what glows and leaves the lines as drawn.
 * */
#pragma once

// Deliberately not renderer.h: this header is included from the end of it, once NYA_RenderTexture
// and NYA_Window exist. Including it back would be a cycle.
#include "nyangine/base/base_types.h"
#include "nyangine/debug/debug_trace.h"
#include "nyangine/renderer/render_color.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Ink width in pixels when NYA_PostInk.width is zero, at the distance lines start to fade. */
#define NYA_POST_INK_WIDTH 2.0F

/** The angle between two faces, in degrees, past which their edge is inked when NYA_PostInk.crease is zero. */
#define NYA_POST_INK_CREASE 40.0F

/** Where ink starts thinning and where it is gone, in world units, when the fields are zero. */
#define NYA_POST_INK_FADE_START 25.0F
#define NYA_POST_INK_FADE_END   90.0F

/** Occlusion reach in world units when NYA_PostAmbientOcclusion.radius is zero. */
#define NYA_POST_OCCLUSION_RADIUS 0.75F

/**
 * The smallest occlusion reach in pixels of the full image, when NYA_PostAmbientOcclusion.min_radius is zero. A
 * distant camera widens the world radius to this, so contact shading still reads from far away.
 * */
#define NYA_POST_OCCLUSION_MIN_RADIUS 32.0F

/** How dark the occlusion band goes when NYA_PostAmbientOcclusion.strength is zero. */
#define NYA_POST_OCCLUSION_STRENGTH 0.45F

/** How occluded a pixel has to be before it falls into the band, when NYA_PostAmbientOcclusion.band is zero. */
#define NYA_POST_OCCLUSION_BAND 0.25F

/**
 * Half the width of the band's edge in occlusion units, when NYA_PostAmbientOcclusion.softness is zero. Narrow
 * enough to read as a band, wide enough not to alias; toward one it becomes a plain gradient.
 * */
#define NYA_POST_OCCLUSION_SOFTNESS 0.08F

/** FXAA's sub-pixel smoothing when NYA_PostAntialias.subpixel is zero. Higher softens more. */
#define NYA_POST_ANTIALIAS_SUBPIXEL 0.75F

/** The smallest local contrast FXAA treats as an edge, when NYA_PostAntialias.threshold is zero. */
#define NYA_POST_ANTIALIAS_THRESHOLD 0.125F

/** Half the sharp band's height as a fraction of the screen, when NYA_PostDepthOfField.band is zero. */
#define NYA_POST_DEPTH_OF_FIELD_BAND 0.08F

/** Where distance focus is sharpest, in world units, when NYA_PostDepthOfField.focus_distance is zero. */
#define NYA_POST_DEPTH_OF_FIELD_DISTANCE 10.0F

/** World units either side of the focus kept sharp, when NYA_PostDepthOfField.focus_range is zero. */
#define NYA_POST_DEPTH_OF_FIELD_RANGE 2.0F

/** The widest blur in pixels of the full image, when NYA_PostDepthOfField.radius is zero. */
#define NYA_POST_DEPTH_OF_FIELD_RADIUS 10.0F

/** The widest blur allowed. The taps are fixed, so past this they separate into a visible pattern. */
#define NYA_POST_DEPTH_OF_FIELD_RADIUS_MAX 24.0F

/** Distinct amounts of blur, when NYA_PostDepthOfField.layers is zero. */
#define NYA_POST_DEPTH_OF_FIELD_LAYERS 3

/** Luma above which a pixel glows, when NYA_PostBloom.threshold is zero. */
#define NYA_POST_BLOOM_THRESHOLD 0.75F

/** How strongly the glow is added back, when NYA_PostBloom.intensity is zero. Past two it blows out. */
#define NYA_POST_BLOOM_INTENSITY 1.0F

/** Pixels of the full image between the glow's taps, when NYA_PostBloom.spread is zero. */
#define NYA_POST_BLOOM_SPREAD 3.0F

/** The widest spread allowed. The taps are fixed, so past this they separate into a grid. */
#define NYA_POST_BLOOM_SPREAD_MAX 8.0F

/** Lines around the full circle, when NYA_PostSpeedLines.density is zero. */
#define NYA_POST_SPEED_LINES_DENSITY 140.0F

/** The clear middle at full amount, a fraction of the screen's height, when NYA_PostSpeedLines.clear_radius is zero. */
#define NYA_POST_SPEED_LINES_CLEAR_RADIUS 0.28F

/** How many times a second the lines are redrawn. Low on purpose: every frame reads as noise, not drawing. */
#define NYA_POST_SPEED_LINES_RATE 12.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_PostPass             NYA_PostPass;
typedef struct NYA_PostChain            NYA_PostChain;
typedef struct NYA_PostInk              NYA_PostInk;
typedef struct NYA_PostAmbientOcclusion NYA_PostAmbientOcclusion;
typedef struct NYA_PostAntialias        NYA_PostAntialias;
typedef struct NYA_PostDepthOfField     NYA_PostDepthOfField;
typedef struct NYA_PostSpeedLines       NYA_PostSpeedLines;
typedef struct NYA_PostBloom            NYA_PostBloom;
typedef enum NYA_PostFocus              NYA_PostFocus;
typedef enum NYA_PostDebugView          NYA_PostDebugView;

/** One full-screen effect: a pipeline, and the uniform it wants. */
struct NYA_PostPass {
    /** Asset handle of a 2D graphics pipeline. A pass whose pipeline is not loaded yet is skipped. */
    NYA_ConstCString pipeline;

    /**
     * Asset handle of a texture or lookup table bound at t1 beside the image being processed. Null for none. A pass
     * whose texture is not loaded yet is skipped, like one whose pipeline is not.
     * */
    NYA_ConstCString texture;

    /** Pushed once for the single draw this pass makes. Null for a pipeline that takes no uniform. */
    const void* uniform;
    u32         uniform_size;

    /**
     * Multiplied into the result by the shader, so anything but white tints the whole screen.
     * */
    NYA_Color tint;

    /** The trace feature the pass is timed as. Zero is NYA_TRACE_POST. */
    NYA_TraceFeature trace;
};

/**
 * Screen-space ink: lines where depth jumps (silhouettes) and where the surface turns sharply (creases), which
 * catches the hard edges mesh3d_edge cannot see. Drawn on the nearer side of a silhouette, thinned and faded with
 * distance, and faded into fog.
 * */
// @reflect
struct NYA_PostInk {
    b8 enabled;

    /** Zero is near black. Alpha scales how strongly the line covers the scene. */
    NYA_Color color;

    /** Silhouette width in pixels near the camera; creases get half, drawn on both faces. See NYA_POST_INK_WIDTH. */
    f32 width;

    /** Degrees between two faces past which their shared edge is inked. See NYA_POST_INK_CREASE. */
    f32 crease;

    /** World distances where lines start to thin and where they are gone. See NYA_POST_INK_FADE_START. */
    f32 fade_start;
    f32 fade_end;
};

/**
 * Stylised ambient occlusion: contact areas and crevices darkened in one soft band rather than a gradient.
 * Eight samples at half resolution, blurred against depth when it is applied.
 * */
// @reflect
struct NYA_PostAmbientOcclusion {
    b8 enabled;

    /** How far occluders are looked for, in world units. See NYA_POST_OCCLUSION_RADIUS. */
    f32 radius;

    /** The least reach in pixels, however far the camera is. See NYA_POST_OCCLUSION_MIN_RADIUS. */
    f32 min_radius;

    /** How dark the band is, in [0, 1]. See NYA_POST_OCCLUSION_STRENGTH. */
    f32 strength;

    /** The occlusion, in [0, 1], at which the band starts. See NYA_POST_OCCLUSION_BAND. */
    f32 band;

    /** How soft the band's edge is, in [0, 1]. See NYA_POST_OCCLUSION_SOFTNESS. */
    f32 softness;
};

/**
 * FXAA over the finished image. The scene capture is multisampled when the device allows, but the ink and any
 * single-sampled target are not, and this smooths both.
 * */
// @reflect
struct NYA_PostAntialias {
    b8 enabled;

    /** See NYA_POST_ANTIALIAS_SUBPIXEL. */
    f32 subpixel;

    /** See NYA_POST_ANTIALIAS_THRESHOLD. */
    f32 threshold;
};

/** What decides how blurred a pixel is. */
// @reflect
enum NYA_PostFocus {
    /** No blur, no pass and no target. */
    NYA_POST_FOCUS_OFF = 0,

    /** Sharp in a horizontal band, blurring toward the top and bottom: the miniature look. Reads no depth. */
    NYA_POST_FOCUS_TILT_SHIFT,

    /** Sharp at a distance from the camera, read from the scene normal buffer, which it turns on. */
    NYA_POST_FOCUS_DISTANCE,

    NYA_POST_FOCUS_COUNT,
};

/**
 * A toy-like depth of field. Gathered at half resolution through a hexagon of flat weighted taps, so bright points
 * open into flat hexagons, with the amount stepped into a few layers so the scene reads as cut-out planes rather than
 * a lens. Where the blur applies is decided again at full resolution, so what is in focus keeps a clean edge.
 * */
// @reflect
struct NYA_PostDepthOfField {
    NYA_PostFocus focus;

    /** Tilt shift: the middle of the sharp band as an offset from the middle of the screen, down positive. */
    f32 band_offset;

    /** Tilt shift: half the sharp band's height as a fraction of the screen. See NYA_POST_DEPTH_OF_FIELD_BAND. */
    f32 band;

    /** Distance: how far from the camera is sharpest, in world units. See NYA_POST_DEPTH_OF_FIELD_DISTANCE. */
    f32 focus_distance;

    /** Distance: world units either side of `focus_distance` still sharp. See NYA_POST_DEPTH_OF_FIELD_RANGE. */
    f32 focus_range;

    /**
     * How far past the sharp zone the blur is full: a fraction of the screen for tilt shift, world units for distance.
     * Zero is three times the band, or twice the range.
     * */
    f32 falloff;

    /** The widest blur in pixels of the full image. See NYA_POST_DEPTH_OF_FIELD_RADIUS. */
    f32 radius;

    /** Distinct amounts of blur. See NYA_POST_DEPTH_OF_FIELD_LAYERS. */
    u32 layers;
};

/**
 * Speed lines, a stylised stand-in for motion blur: thin spikes converging on the point the camera heads for,
 * redrawn a few times a second like cel animation.
 * */
// @reflect
struct NYA_PostSpeedLines {
    /** How many lines show and how far in they reach, in [0, 1]. Zero is off. */
    f32 amount;

    /**
     * Where the lines converge without motion, as an offset from the middle of the screen in [-0.5, 0.5], down
     * positive.
     * */
    f32 center_x;
    f32 center_y;

    /**
     * The 3D camera's velocity in world units. The lines converge where it points on screen, easing back to the
     * centre above as it turns across the view, and staying there while the camera backs away. Zero for none.
     * */
    f32x3 motion;

    /** Lines around the full circle. See NYA_POST_SPEED_LINES_DENSITY. */
    f32 density;

    /** The clear middle at full amount, a fraction of the screen's height. See NYA_POST_SPEED_LINES_CLEAR_RADIUS. */
    f32 clear_radius;

    /** Zero is white. Alpha is how opaque a line is. */
    NYA_Color color;
};

/**
 * A glow around what is brighter than a threshold. The bright parts are gathered and blurred at half resolution, in
 * the target depth of field uses, and added back over the image.
 * */
// @reflect
struct NYA_PostBloom {
    b8 enabled;

    /** Luma above which a pixel glows. See NYA_POST_BLOOM_THRESHOLD. */
    f32 threshold;

    /** How strongly the glow is added back. See NYA_POST_BLOOM_INTENSITY. */
    f32 intensity;

    /** Pixels between the blur's taps; wider carries the glow further. See NYA_POST_BLOOM_SPREAD. */
    f32 spread;
};

/** A buffer shown in place of the image, for looking at what the scene passes read. */
// @reflect
enum NYA_PostDebugView {
    NYA_POST_DEBUG_VIEW_NONE = 0,

    /** World normals as colour. */
    NYA_POST_DEBUG_VIEW_NORMALS,

    /** Distance from the camera, near white and far black. */
    NYA_POST_DEBUG_VIEW_DEPTH,

    /** The raw occlusion before banding, white where open. */
    NYA_POST_DEBUG_VIEW_OCCLUSION,

    /** Where the ink lands, black on white. */
    NYA_POST_DEBUG_VIEW_INK,

    /** The scene tinted by the shadow cascade covering each pixel: red, green, blue from nearest. */
    NYA_POST_DEBUG_VIEW_CASCADES,

    NYA_POST_DEBUG_VIEW_COUNT,
};

/**
 * The targets a chain draws through. Zero-initialise it and hand it to nya_post_begin.
 * */
struct NYA_PostChain {
    /**
     * [0] holds the scene, at the renderer's sample count, with the normal buffer while a scene pass reads it. [1]
     * exists only while more than one pass runs, single sampled and without depth, so a one pass chain such as a
     * bloom costs one target.
     * */
    NYA_RenderTexture targets[2];

    /** The half resolution occlusion, single sampled. Exists only while occlusion or a debug view is on. */
    NYA_RenderTexture half;

    /**
     * The half resolution blur, single sampled: depth of field's, then bloom's after the caller's passes. Exists only
     * while one of them is on.
     * */
    NYA_RenderTexture blur;

    /**
     * How the scene target is made. Zeroed attaches depth for a 3D scene; a 2D world saves it with DEPTH_NONE. The
     * chain adds `normals` itself.
     * */
    NYA_RenderTextureOptions scene;

    /** What the targets were built for. A change means they are recreated. */
    u32 width, height;

    /** Which target the scene was captured into, so nya_post_end knows where to start reading. */
    u32 scene_index;

    /** Whether nya_post_begin succeeded, so nya_post_end can do nothing rather than draw a dead target. */
    b8 capturing;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts capturing the scene into the chain's first target, building or releasing targets the window's options
 * now need.
 * */
NYA_API b8 nya_post_begin(NYA_Window* window, NYA_PostChain* chain) __attr_no_discard;

/**
 * Ends the capture, runs the window's scene passes and then `passes` over it, the last one landing on the window.
 * */
NYA_API void nya_post_end(NYA_Window* window, NYA_PostChain* chain, const NYA_PostPass* passes, u32 pass_count);

/** Releases the chain's targets. Safe on a zeroed or already destroyed chain. */
NYA_API void nya_post_chain_destroy(NYA_PostChain* chain);

/** Sets this window's ink. Out of range fields are clamped, since they usually come from a config file. */
NYA_API void        nya_post_ink_set(NYA_Window* window, NYA_PostInk ink);
NYA_API NYA_PostInk nya_post_ink(NYA_Window* window) __attr_no_discard;

/** Sets this window's ambient occlusion, clamped like the ink. */
NYA_API void                     nya_post_ambient_occlusion_set(NYA_Window* window, NYA_PostAmbientOcclusion occlusion);
NYA_API NYA_PostAmbientOcclusion nya_post_ambient_occlusion(NYA_Window* window) __attr_no_discard;

/** Sets this window's antialiasing, clamped like the ink. */
NYA_API void              nya_post_antialias_set(NYA_Window* window, NYA_PostAntialias antialias);
NYA_API NYA_PostAntialias nya_post_antialias(NYA_Window* window) __attr_no_discard;

/** Sets this window's depth of field, clamped like the ink. An unknown focus reads as off. */
NYA_API void                 nya_post_depth_of_field_set(NYA_Window* window, NYA_PostDepthOfField depth_of_field);
NYA_API NYA_PostDepthOfField nya_post_depth_of_field(NYA_Window* window) __attr_no_discard;

/** Sets this window's speed lines, clamped like the ink. Cheap enough to drive every frame. */
NYA_API void               nya_post_speed_lines_set(NYA_Window* window, NYA_PostSpeedLines speed_lines);
NYA_API NYA_PostSpeedLines nya_post_speed_lines(NYA_Window* window) __attr_no_discard;

/** Sets this window's bloom, clamped like the ink. */
NYA_API void          nya_post_bloom_set(NYA_Window* window, NYA_PostBloom bloom);
NYA_API NYA_PostBloom nya_post_bloom(NYA_Window* window) __attr_no_discard;

/** Shows a buffer instead of the image. An unknown view reads as none. */
NYA_API void              nya_post_debug_view_set(NYA_Window* window, NYA_PostDebugView view);
NYA_API NYA_PostDebugView nya_post_debug_view(NYA_Window* window) __attr_no_discard;
