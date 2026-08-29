/**
 * @file render_post.h
 *
 * Full-screen post-processing: draw a scene into a target, then run it through a chain of effects.
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
 * **nya_post_begin can fail, and the caller must draw anyway when it does.** A zero-sized window is
 * minimised or mid-resize and the GPU will not make a target of no size. Returning false rather than
 * asserting is what lets the fallback be one branch.
 *
 * Two targets, ping-ponged: pass *n* reads what pass *n-1* wrote, and the last pass draws to the
 * window rather than to a target. One pass therefore needs no intermediate at all, and any number of
 * passes needs exactly two — which is why this is a fixed pair rather than a list.
 *
 * The targets are recreated when the window size changes, not resized, because a GPU texture has no
 * resize. The old one is destroyed first: a window dragged to a new size produces one of these per
 * frame, and leaking them adds up to hundreds of megabytes by the time the mouse comes up.
 *
 * Effects are ordinary 2D pipelines — a fragment shader over `batch2d.vert.hlsl` — so anything
 * `nya_render2d_shader_begin` accepts works here. The engine ships bloom, blur, CRT, grayscale and
 * pixelate under assets/shader/source/effect_*.hlsl.
 * */
#pragma once

// Deliberately not renderer.h: this header is included from the end of it, once NYA_RenderTexture
// and NYA_Window exist. Including it back would be a cycle.
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_PostPass  NYA_PostPass;
typedef struct NYA_PostChain NYA_PostChain;

/** One full-screen effect: a pipeline, and the uniform it wants. */
struct NYA_PostPass {
    /** Asset handle of a 2D graphics pipeline. A pass whose pipeline is not loaded yet is skipped. */
    NYA_ConstCString pipeline;

    /** Pushed once for the single draw this pass makes. Null for a pipeline that takes no uniform. */
    const void* uniform;
    u32         uniform_size;

    /**
     * Multiplied into the result by the shader, so anything but white tints the whole screen.
     *
     * Zeroed alpha means opaque white rather than invisible, so a `{ .pipeline = ... }` pass with no
     * tint set does the obvious thing.
     * */
    NYA_Color tint;
};

/**
 * The pair of targets a chain ping-pongs between. Zero-initialise it and hand it to nya_post_begin.
 *
 * Owns GPU textures, so it is not arena memory and does not disappear with an arena — call
 * nya_post_chain_destroy when the window goes away.
 * */
struct NYA_PostChain {
    NYA_RenderTexture targets[2];

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
 * Starts capturing the scene into the chain's first target.
 *
 * Clears to transparent, not to a colour: whatever was drawn to the window before this — a
 * background layer, a sky — is underneath, and the chain composites over it. Clearing to black paints
 * a rectangle over that.
 *
 * Returns false when there is no usable target, in which case nothing has been begun and the caller
 * must draw straight to the window.
 * */
NYA_API b8 nya_post_begin(NYA_Window* window, NYA_PostChain* chain) __attr_no_discard;

/**
 * Ends the capture and runs `passes` over it, the last one landing on the window.
 *
 * Zero passes blits the captured scene back, which is what an effects toggle set to "off" wants
 * without special-casing the call.
 * */
NYA_API void nya_post_end(NYA_Window* window, NYA_PostChain* chain, const NYA_PostPass* passes, u32 pass_count);

/** Releases the chain's targets. Safe on a zeroed or already destroyed chain. */
NYA_API void nya_post_chain_destroy(NYA_PostChain* chain);
