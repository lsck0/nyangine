/**
 * @file render_compute_particles.h
 *
 * A small GPU particle field, and the concrete proof that render_compute.h's pipeline abstraction works
 * end to end. Particle positions and velocities live in a storage buffer that never leaves the GPU: one
 * compute pass integrates them each frame, a second gathers them into a texture, and that texture is drawn
 * with the ordinary 2D path. No data is read back to the CPU.
 *
 * ```c
 * NYA_GPUParticleField* field = nya_gpu_particle_field_create(window, 512, 192);
 *
 * // in on_render, before drawing the texture:
 * nya_gpu_particle_field_step(window, field, delta_time_s);
 * nya_gpu_particle_field_draw(window, field, 24.0F, 24.0F, 256.0F, 256.0F);
 *
 * // at teardown:
 * nya_gpu_particle_field_destroy(window, field);
 * ```
 *
 * DESKTOP ONLY. It rests on compute shaders, which WebGL2/GLES3 cannot run, so the whole thing is compiled
 * out on the web build — see render_compute.h and the renderer-web wall. A create call on a device without
 * compute (some offscreen and software backends) returns null and logs; the caller runs without the field.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

#if !OS_WASM

typedef struct NYA_Window NYA_Window;

/** The field's state. Opaque; created and destroyed by the calls below. */
typedef struct NYA_GPUParticleField NYA_GPUParticleField;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds a field of `count` particles drawn into a `resolution` by `resolution` texture, seeds the
 * particles on the GPU, and returns it. `count` is clamped to a few thousand and `resolution` to a few
 * hundred, since the draw is a per-texel gather over every particle. Returns null and logs when the device
 * has no compute or a pipeline will not build, so the caller can simply carry on without it.
 * */
NYA_API NYA_GPUParticleField* nya_gpu_particle_field_create(NYA_Window* window, u32 count, u32 resolution) __attr_no_discard;

/**
 * One frame: integrate the particles, then redraw the field texture from them. Runs on a command buffer of
 * its own, so it never touches the frame's open render pass, and can be called anywhere in a frame before
 * the texture is drawn. A null field is ignored, so a caller whose create returned null need not branch.
 * */
NYA_API void nya_gpu_particle_field_step(NYA_Window* window, NYA_GPUParticleField* field, f32 delta_time_s);

/**
 * Draws the field texture at a screen rectangle through the 2D textured path. Call from on_render inside
 * the window's 2D pass, after the step. A null field is ignored.
 * */
NYA_API void nya_gpu_particle_field_draw(NYA_Window* window, NYA_GPUParticleField* field, f32 x, f32 y, f32 width, f32 height);

/** Releases the field's pipelines, buffer and texture. A null field is ignored. */
NYA_API void nya_gpu_particle_field_destroy(NYA_Window* window, NYA_GPUParticleField* field);

#endif // !OS_WASM
