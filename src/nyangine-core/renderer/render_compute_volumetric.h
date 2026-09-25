/**
 * @file render_compute_volumetric.h
 *
 * A raymarched volumetric: fog, cloud and smoke drawn by a compute shader that marches a procedural 3D density
 * field. It is the compute sibling of the particle field — same pipeline skin (render_compute.h), same
 * desktop-only gate — but where that one gathers a buffer into an image, this one integrates light and
 * extinction along a view ray through a box of drifting noise and writes the lit volume, its coverage in alpha,
 * for the 2D path to composite over the scene.
 *
 * The effect splits the way the post passes do (see the SSR reflections gate): a small parameter block lives on
 * the window, set and read like any render option, so a game or nya_settings_graphics_apply tunes and gates it
 * without touching the GPU object; the object itself owns the pipeline and the image and does the marching.
 *
 * ```c
 * nya_volumetric_params_set(window, (NYA_VolumetricParams){
 *     .enabled = true, .density = 1.4F, .steps = 64, .absorption = 1.2F, .light_direction = { -0.5F, -0.7F, -0.4F },
 * });
 *
 * NYA_GPUVolumetric* volume = nya_gpu_volumetric_create(window, 256);
 *
 * // in on_render, bracketing the scene:
 * nya_gpu_volumetric_begin(window, volume, delta_time_s);      // march the field into the image (own command buffer)
 * // ... draw the 3D scene ...
 * nya_gpu_volumetric_end(window, volume, 0.0F, 0.0F, w, h);    // composite the volume over it, in the 2D pass
 *
 * nya_gpu_volumetric_destroy(window, volume);
 * ```
 *
 * DESKTOP ONLY. The march is a compute shader, which WebGL2/GLES3 cannot run, so the GPU object is compiled out
 * on the web build exactly as the particle field is; see render_compute.h and the renderer-web wall. The
 * parameter block is plain data and stays on both builds, so nya_settings_graphics_apply gates it unconditionally.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

typedef struct NYA_Window NYA_Window;

typedef struct NYA_VolumetricParams NYA_VolumetricParams;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Primary march steps along the view ray when NYA_VolumetricParams.steps is zero. */
#define NYA_VOLUMETRIC_STEPS 48

/** The most steps the march takes, matching VOLUMETRIC_STEPS_MAX in volumetric.comp.hlsl. */
#define NYA_VOLUMETRIC_STEPS_MAX 128

/** The field's opacity when NYA_VolumetricParams.density is zero. */
#define NYA_VOLUMETRIC_DENSITY 1.4F

/** How fast light is swallowed along the light march when NYA_VolumetricParams.absorption is zero. */
#define NYA_VOLUMETRIC_ABSORPTION 1.1F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The volumetric's tunables, stored on the window and read by the march each frame. A zeroed block reads as the
 * defaults above and disabled, so a game names only what it changes and nya_settings_graphics_apply can gate it.
 * */
struct NYA_VolumetricParams {
    /** Whether the pass runs at all. Off by default, since the march is not cheap. */
    b8 enabled;

    /** Overall opacity of the field, scaling the sampled extinction. See NYA_VOLUMETRIC_DENSITY. */
    f32 density;

    /** The Beer-Lambert absorption along the light march, how fast the volume self-shadows. See NYA_VOLUMETRIC_ABSORPTION. */
    f32 absorption;

    /** Primary march steps per ray, clamped to NYA_VOLUMETRIC_STEPS_MAX. See NYA_VOLUMETRIC_STEPS. */
    u32 steps;

    /** Which way the sun's light travels, world space; normalised by the setter. Zero reads as a default down-forward sun. */
    f32 light_direction[3];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PARAMETERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Replaces the window's volumetric parameters, clamping the step count and normalising the light direction. Plain
 * data, on both builds, so nya_settings_graphics_apply gates the effect through it the way it gates SSR.
 * */
NYA_API void nya_volumetric_params_set(NYA_Window* window, NYA_VolumetricParams params);

/** The window's volumetric parameters, as set. */
NYA_API NYA_VolumetricParams nya_volumetric_params(NYA_Window* window) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PASS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if !OS_WASM

/** The pass's GPU state: its pipeline and the image it marches into. Opaque; created and destroyed below. */
typedef struct NYA_GPUVolumetric NYA_GPUVolumetric;

/**
 * Builds the volumetric pass drawing into a `resolution` by `resolution` image, and returns it. `resolution` is
 * clamped to a few hundred, since the cost is per texel times the march. Returns null and logs when the device
 * has no compute or the pipeline will not build, so the caller can simply carry on without the effect.
 * */
NYA_API NYA_GPUVolumetric* nya_gpu_volumetric_create(NYA_Window* window, u32 resolution) __attr_no_discard;

/**
 * Marches the density field into the image on a command buffer of its own, so it opens no render pass and can be
 * called anywhere in a frame before the composite. Reads the window's NYA_VolumetricParams, and does nothing when
 * NYA_RENDER_FEATURE_VOLUMETRICS or the parameters' own switch turns it off. A null volume is ignored, so a caller
 * whose create returned null need not branch.
 * */
NYA_API void nya_gpu_volumetric_begin(NYA_Window* window, NYA_GPUVolumetric* volume, f32 delta_time_s);

/**
 * Composites the marched image over the frame at a screen rectangle, through the 2D textured path, its coverage
 * in alpha. Call from on_render inside the window's 2D pass, after the scene and its own begin. Draws nothing when
 * the pass is switched off, or for a null volume.
 * */
NYA_API void nya_gpu_volumetric_end(NYA_Window* window, NYA_GPUVolumetric* volume, f32 x, f32 y, f32 width, f32 height);

/** Releases the pass's pipeline and image. A null volume is ignored. */
NYA_API void nya_gpu_volumetric_destroy(NYA_Window* window, NYA_GPUVolumetric* volume);

#endif // !OS_WASM
