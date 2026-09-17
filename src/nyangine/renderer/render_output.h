/**
 * @file render_output.h
 *
 * ```c
 * // HDR where the display and driver offer it, and the same SDR image everywhere else.
 * nya_render_output_set(window, (NYA_RenderOutput){ .hdr = true, .peak = 3.0F });
 * ```
 *
 * The frame is still drawn in SDR, into a copy of the window at the format everything was built for, and
 * written to an HDR swapchain at the end of the frame by one fullscreen pass. That pass decodes sRGB to linear
 * light and lifts only the brightest colours, the ones the scene's tonemap pushed against white, up to `peak`.
 * Authored colour below NYA_RENDER_OUTPUT_HIGHLIGHT comes out exactly as in SDR.
 *
 * Only the scene is lifted, so a white HUD stays at paper white. nya_post_end marks where the scene ends; a game
 * drawing its scene without a post chain marks it itself:
 *
 * ```c
 * draw_the_world(window);
 * nya_render_output_scene_end(window);
 * draw_the_hud(window);
 * ```
 *
 * A frame that never marks it lifts everything.
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The encode pipelines, one per HDR swapchain format. Queued when HDR is first switched on. */
#define NYA_RENDER_PIPELINE_OUTPUT_LINEAR "nya_output_linear_pipeline"
#define NYA_RENDER_PIPELINE_OUTPUT_PQ     "nya_output_pq_pipeline"

/** Marks the scene in the frame's alpha. Queued with the encode pipelines. See nya_render_output_scene_end. */
#define NYA_RENDER_PIPELINE_OUTPUT_MASK "nya_output_mask_pipeline"

/** How bright the brightest highlight gets when NYA_RenderOutput.peak is zero, in multiples of SDR white. */
#ifndef NYA_RENDER_OUTPUT_PEAK
#define NYA_RENDER_OUTPUT_PEAK 2.5F
#endif

/** The brightest highlight allowed, in multiples of SDR white. */
#define NYA_RENDER_OUTPUT_PEAK_MAX 10.0F

/** SDR white on an HDR10 display, in nits. BT.2408's reference white. */
#ifndef NYA_RENDER_OUTPUT_PAPER_WHITE_NITS
#define NYA_RENDER_OUTPUT_PAPER_WHITE_NITS 203.0F
#endif

/** The encoded value past which a colour counts as a highlight. The scene tonemap keeps lit surfaces below it. */
#ifndef NYA_RENDER_OUTPUT_HIGHLIGHT
#define NYA_RENDER_OUTPUT_HIGHLIGHT 0.95F
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_RenderOutput    NYA_RenderOutput;
typedef struct NYA_RenderOutputGPU NYA_RenderOutputGPU;

/** Zero for any field but `hdr` picks its default. */
// @reflect
struct NYA_RenderOutput {
    /** Present in HDR when the window supports it. Off, or unsupported, the swapchain stays SDR and nothing changes. */
    b8 hdr;

    /** The brightest highlight in multiples of SDR white, at most NYA_RENDER_OUTPUT_PEAK_MAX. Zero becomes NYA_RENDER_OUTPUT_PEAK. */
    f32 peak;
};

/** What HDR output holds while it is on. */
struct NYA_RenderOutputGPU {
    /** What the swapchain presents, SDR unless HDR was asked for and supported. */
    SDL_GPUSwapchainComposition composition;

    /** The frame drawn in SDR, at the window's colour format and the swapchain's size. */
    SDL_GPUTexture* scene;
    u32             width, height;

    /** This frame's swapchain image, which the scene is encoded onto at the end of the frame. */
    SDL_GPUTexture* swapchain;

    /** NYA_RenderOutput.hdr as last applied, so the swapchain is only reconsidered when it changes. */
    b8 hdr_requested;

    /** Whether this frame marked where its scene ends, so the encode lifts by the frame's alpha. */
    b8 scene_marked;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Replaces how the window presents, from the next frame. Cheap unless `hdr` changes, which rebuilds the swapchain. */
NYA_API void nya_render_output_set(NYA_Window* window, NYA_RenderOutput output);

/** The output as given, before defaults. */
NYA_API NYA_RenderOutput nya_render_output(NYA_Window* window) __attr_no_discard;

/** Whether the window is presenting in HDR now, which can differ from what was asked for. */
NYA_API b8 nya_render_output_hdr_active(NYA_Window* window) __attr_no_discard;

/**
 * Marks everything drawn to the window so far as the scene, which HDR may lift; what is drawn after, such as a HUD,
 * stays at paper white. One fullscreen triangle while presenting in HDR, nothing otherwise. nya_post_end calls it.
 * */
NYA_API void nya_render_output_scene_end(NYA_Window* window);
