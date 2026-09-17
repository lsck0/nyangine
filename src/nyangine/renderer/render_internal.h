/**
 * @file render_internal.h
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/renderer/renderer.h"

/**
 * The render system's cached sampler for a filter. Never null; an unknown filter reads as linear.
 * */
NYA_INTERNAL SDL_GPUSampler* _nya_render_sampler_for(NYA_TextureFilter filter);

typedef struct NYA_Asset NYA_Asset;

/**
 * The build of a pipeline asset that fits the open pass: its sample count, and whether it attaches the scene normal
 * buffer. Null while the asset is not loaded. See nya_asset_graphics_pipeline.
 * */
NYA_INTERNAL SDL_GPUGraphicsPipeline* _nya_render_pipeline(NYA_Window* window, NYA_Asset* asset) __attr_no_discard;

/** The present mode the app's vsync option asks for, falling back to vsync where the window lacks it. */
NYA_INTERNAL __attr_allow_unused SDL_GPUPresentMode _nya_render_present_mode(NYA_Window* window);

/*
 * Defined in render_output.c. Allow-unused because only the build with a device calls them.
 */

/** Switches the swapchain to what NYA_RenderOutput asks for, if that changed. Called before a frame acquires its image. */
NYA_INTERNAL __attr_allow_unused void _nya_render_output_apply(NYA_Window* window);

/** What a frame draws into instead of `swapchain` while presenting in HDR, which is `swapchain` itself in SDR. */
NYA_INTERNAL __attr_allow_unused SDL_GPUTexture* _nya_render_output_target(NYA_Window* window, SDL_GPUTexture* swapchain, u32 width, u32 height);

/** Encodes the frame onto the swapchain while presenting in HDR. Called with no pass open. */
NYA_INTERNAL __attr_allow_unused void _nya_render_output_present(NYA_Window* window);

/*
 * Defined in render3d_decal.c, which the unity build includes after render3d.c. Allow-unused because only the build
 * with a device calls them.
 */

struct NYA_ShaderMesh3DUniform;

/** Releases everything decals hold, keeping the probe. */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_decals_release(NYA_Window* window);

/** Uploads the staged decals in the scene's copy pass, creating the buffers and the index pattern the first time. */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_decals_upload(NYA_Window* window, SDL_GPUCopyPass* copy_pass);

/** Draws a segment's decals in the camera pass. */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_decals_draw(NYA_Window* window, const NYA_Render3DSegment* segment,
                                                                const struct NYA_ShaderMesh3DUniform* uniform);
