/**
 * The water surface's pure math: the flow-map phases and blend, and the summed-sine wave height —
 * determinism, the bounds the shader was tuned for, and the periodicity that makes the flow wrap invisible.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** A pure function must give bit-identical results; the math carries no hidden state to drift. */
static b8 same_flow(NYA_WaterFlow a, NYA_WaterFlow b) {
  return a.phase_a == b.phase_a && a.phase_b == b.phase_b && a.blend == b.blend;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the flow-map phases, their bounds, and the half-cycle offset
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32 cycle = 6.0F;

    // at the start of a cycle the first layer is at zero and fully weighted out to the second, which sits
    // half a cycle along.
    NYA_WaterFlow start = nya_water_flow(0.0F, cycle);
    nya_assert(start.phase_a == 0.0F, "the first layer starts at phase zero, got %f", (double)start.phase_a);
    nya_assert(start.phase_b == 0.5F, "the second layer starts half a cycle along, got %f", (double)start.phase_b);
    nya_assert(start.blend == 1.0F, "the blend is one where the first layer wraps, got %f", (double)start.blend);

    // mid-cycle the first layer is fully weighted in and the blend is zero.
    NYA_WaterFlow middle = nya_water_flow(cycle * 0.5F, cycle);
    nya_assert(middle.phase_a == 0.5F, "the first layer is mid-cycle, got %f", (double)middle.phase_a);
    nya_assert(middle.blend <= 1e-6F, "the blend is zero mid-cycle, got %f", (double)middle.blend);

    // over a spread of times the phases stay in [0, 1) and the blend in [0, 1], and the two layers stay
    // exactly half a cycle apart.
    for (u32 i = 0; i < 4096; i++) {
      f32           t    = (f32)i * 0.031F;
      NYA_WaterFlow flow = nya_water_flow(t, cycle);

      nya_assert(flow.phase_a >= 0.0F && flow.phase_a < 1.0F, "phase_a in [0,1), got %f at i=" FMTu32, (double)flow.phase_a, i);
      nya_assert(flow.phase_b >= 0.0F && flow.phase_b < 1.0F, "phase_b in [0,1), got %f at i=" FMTu32, (double)flow.phase_b, i);
      nya_assert(flow.blend >= 0.0F && flow.blend <= 1.0F, "blend in [0,1], got %f at i=" FMTu32, (double)flow.blend, i);

      // the second layer is the first shifted half a cycle, wrapped: their fractional distance is 0.5.
      f32 gap = flow.phase_b - flow.phase_a;
      gap     = gap - floorf(gap);
      nya_assert(fabsf(gap - 0.5F) < 1e-4F, "the layers stay half a cycle apart, got %f at i=" FMTu32, (double)gap, i);
    }

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the flow is deterministic and periodic in the cycle, so the wrap is invisible
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32 cycle = 6.0F;
    f32 t     = 2.75F;

    NYA_WaterFlow first  = nya_water_flow(t, cycle);
    NYA_WaterFlow second = nya_water_flow(t, cycle);
    nya_assert(same_flow(first, second), "the same time and cycle give the same flow");

    // periodic: a whole cycle later the phases and blend are identical, which is what lets the two scrolled
    // layers hand off without a visible jump.
    NYA_WaterFlow later = nya_water_flow(t + cycle, cycle);
    nya_assert(fabsf(later.phase_a - first.phase_a) < 1e-4F, "phase_a repeats every cycle, got %f vs %f", (double)later.phase_a, (double)first.phase_a);
    nya_assert(fabsf(later.blend - first.blend) < 1e-4F, "the blend repeats every cycle");

    // a non-positive cycle is read as one second rather than dividing by zero.
    NYA_WaterFlow guarded = nya_water_flow(0.25F, 0.0F);
    nya_assert(guarded.phase_a == 0.25F, "a zero cycle is read as one second, got %f", (double)guarded.phase_a);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the wave height is deterministic, bounded, and moves with time
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32   amplitude = 0.2F;
    f32   frequency = 0.6F;
    f32   speed     = 1.0F;
    f32x2 flow      = { 1.0F, 0.3F };

    f32x2 at = { 3.5F, -1.25F };

    f32 first  = nya_water_wave_height(at, 2.75F, flow, amplitude, frequency, speed);
    f32 second = nya_water_wave_height(at, 2.75F, flow, amplitude, frequency, speed);
    nya_assert(first == second, "the same point and time give the same height");

    // a different time gives a different height, or the waves are not travelling.
    f32 later = nya_water_wave_height(at, 9.0F, flow, amplitude, frequency, speed);
    nya_assert(first != later, "the waves move over time");

    // the summed octave and chop weights are NYA_WATER_WAVE_PEAK, so the height never leaves that band —
    // which is what the vertex shader's crest normalise and the renderer's cull padding rest on.
    f32 bound = amplitude * NYA_WATER_WAVE_PEAK + 1e-4F;

    for (u32 i = 0; i < 4096; i++) {
      f32   t = (f32)i * 0.043F;
      f32x2 p = { (f32)i * 1.7F - 80.0F, (f32)i * -0.9F + 40.0F };

      f32 height = nya_water_wave_height(p, t, flow, amplitude, frequency, speed);
      nya_assert(fabsf(height) <= bound, "the height stays within amplitude * peak, got %f at i=" FMTu32, (double)height, i);
    }

    // a zero flow direction is read as +x rather than normalizing a zero vector, so it still produces waves.
    f32 zero_flow = nya_water_wave_height(at, 1.0F, f32x2_zero, amplitude, frequency, speed);
    f32 x_flow    = nya_water_wave_height(at, 1.0F, (f32x2){ 1.0F, 0.0F }, amplitude, frequency, speed);
    nya_assert(zero_flow == x_flow, "a zero flow is read as +x");

    // zero amplitude is a flat surface, whatever the point and time.
    nya_assert(nya_water_wave_height(at, 5.0F, flow, 0.0F, frequency, speed) == 0.0F, "zero amplitude is flat");

    printf("  PASSED\n");
  }

  printf("test_water: all passed\n");
  return 0;
}
