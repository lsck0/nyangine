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
