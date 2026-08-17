/**
 * @file render_internal.h
 *
 * What the renderer's translation units share with each other and with nobody else.
 *
 * Deliberately not included by renderer.h. Everything here is NYA_INTERNAL, which is
 * `visibility("hidden") static`, and a static function *declared* in a public header reaches every
 * translation unit that includes nyangine.h without nyangine.c — the game DLL above all — where it is
 * declared, never defined and never called, and each one warns about it. net_bytes.h carries the same
 * note for the same reason.
 *
 * It also cannot live in renderer.h for a second reason: the helpers below read NYA_App, and
 * core_app.h includes renderer.h rather than the other way round. A header that reached upward for
 * nya_app_get would close that circle.
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/renderer/renderer.h"

/**
 * The render system's cached sampler for a filter. Never null; an unknown filter reads as linear.
 *
 * Shared rather than owned by either batch. It was `_nya_render2d_sampler_for` in render2d.c, which was
 * fine while render2d was the only thing that sampled a texture — and became a layering problem the day
 * the 3D mesh path needed one, because render3d then reached into render2d's private surface for a two
 * line lookup that belongs to neither of them.
 *
 * The samplers themselves are created once by the render system, so this is an array index and not a
 * cache miss waiting to happen.
 * */
NYA_INTERNAL SDL_GPUSampler* _nya_render_sampler_for(NYA_TextureFilter filter);
