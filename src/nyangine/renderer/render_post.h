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
 * The cartoon passes are options on the window rather than passes, since they read buffers only the chain
 * knows about. One call each turns them on, and a zeroed field takes its default:
 *
 * ```c
 * nya_post_ink_set(window, (NYA_PostInk){ .enabled = true });
 * nya_post_ambient_occlusion_set(window, (NYA_PostAmbientOcclusion){ .enabled = true, .strength = 0.4F });
 * nya_post_antialias_set(window, (NYA_PostAntialias){ .enabled = true });
 * ```
 *
 * They run inside nya_post_end before the caller's passes, occlusion then ink then antialiasing, and the debug
 * view after them. A feature that is off has no pass, no pipeline and no target. Ink and occlusion read the scene
 * normal buffer (NYA_RENDER3D_NORMAL_FORMAT), which the chain's scene target carries only while one of them or a
 * debug view is on, and they skip a frame whose capture drew no 3D.
 * */
#pragma once

// Deliberately not renderer.h: this header is included from the end of it, once NYA_RenderTexture
// and NYA_Window exist. Including it back would be a cycle.
#include "nyangine/base/base_types.h"
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

/** How dark the occlusion band goes when NYA_PostAmbientOcclusion.strength is zero. */
#define NYA_POST_OCCLUSION_STRENGTH 0.45F

/** How occluded a pixel has to be before it falls into the band, when NYA_PostAmbientOcclusion.band is zero. */
#define NYA_POST_OCCLUSION_BAND 0.25F

/** FXAA's sub-pixel smoothing when NYA_PostAntialias.subpixel is zero. Higher softens more. */
#define NYA_POST_ANTIALIAS_SUBPIXEL 0.75F

/** The smallest local contrast FXAA treats as an edge, when NYA_PostAntialias.threshold is zero. */
#define NYA_POST_ANTIALIAS_THRESHOLD 0.125F

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

    /** How dark the band is, in [0, 1]. See NYA_POST_OCCLUSION_STRENGTH. */
    f32 strength;

    /** The occlusion, in [0, 1], at which the band starts. See NYA_POST_OCCLUSION_BAND. */
    f32 band;
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

/** A buffer shown in place of the image, for looking at what the cartoon passes read. */
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
     * [0] holds the scene, at the renderer's sample count, with the normal buffer while a cartoon pass reads it. [1]
     * exists only while more than one pass runs, single sampled and without depth, so a one pass chain such as a
     * bloom costs one target.
     * */
    NYA_RenderTexture targets[2];

    /** The half resolution occlusion, single sampled. Exists only while occlusion or a debug view is on. */
    NYA_RenderTexture half;

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
 * Ends the capture, runs the window's cartoon passes and then `passes` over it, the last one landing on the window.
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

/** Shows a buffer instead of the image. An unknown view reads as none. */
NYA_API void              nya_post_debug_view_set(NYA_Window* window, NYA_PostDebugView view);
NYA_API NYA_PostDebugView nya_post_debug_view(NYA_Window* window) __attr_no_discard;
