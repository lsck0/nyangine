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
     * */
    NYA_Color tint;
};

/**
 * The pair of targets a chain ping-pongs between. Zero-initialise it and hand it to nya_post_begin.
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
 * */
NYA_API b8 nya_post_begin(NYA_Window* window, NYA_PostChain* chain) __attr_no_discard;

/**
 * Ends the capture and runs `passes` over it, the last one landing on the window.
 * */
NYA_API void nya_post_end(NYA_Window* window, NYA_PostChain* chain, const NYA_PostPass* passes, u32 pass_count);

/** Releases the chain's targets. Safe on a zeroed or already destroyed chain. */
NYA_API void nya_post_chain_destroy(NYA_PostChain* chain);
