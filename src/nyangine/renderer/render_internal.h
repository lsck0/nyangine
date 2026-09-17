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
