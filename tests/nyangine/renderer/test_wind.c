/**
 * The wind field: determinism, the steady direction, and the bounds the gust cannot leave.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** A pure function must give bit-identical results; the field carries no hidden state to drift. */
static b8 same_vector(f32x3 a, f32x3 b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // TEST: the defaults, and the steady wind with no gust
  {
    NYA_WindField calm = nya_wind_field((NYA_WindOptions){ 0 });

    nya_assert(calm.strength == 1.0F, "an unset strength is one");
    nya_assert(calm.direction.x == 1.0F && calm.direction.y == 0.0F && calm.direction.z == 0.0F, "an unset direction is +x");

    // gustiness zero: the wind is exactly its steady push, at every point and time.
    NYA_WindField steady = nya_wind_field((NYA_WindOptions){ .direction = { 0, 0, 2 }, .strength = 3.0F, .gustiness = 0.0F });

    f32x3 base   = nya_wind_base(&steady);
    f32x3 sample = nya_wind_at(&steady, (f32x3){ 5, 1, -2 }, 4.0F);

    nya_assert(same_vector(base, sample), "with no gust the sample is the steady push");
    nya_assert(base.z == 3.0F && base.x == 0.0F && base.y == 0.0F, "the push is direction times strength, got {%f,%f,%f}",
               (double)base.x, (double)base.y, (double)base.z);

    printf("  PASSED\n");
  }

  // TEST: sampling is deterministic, and a copy samples identically
  {
    NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0.4F }, .strength = 2.0F, .gustiness = 0.7F, .seed = 12.0F });

    f32x3 position = { 3.5F, 0.0F, -1.25F };

    f32x3 first  = nya_wind_at(&wind, position, 2.75F);
    f32x3 second = nya_wind_at(&wind, position, 2.75F);
    nya_assert(same_vector(first, second), "the same field, point and time give the same vector");

    // a copy carries the same fields, so it is the same pure function.
    NYA_WindField copy   = wind;
    f32x3         copied = nya_wind_at(&copy, position, 2.75F);
    nya_assert(same_vector(first, copied), "a copy samples identically");

    // a different time gives a different wind, or the gust is not doing anything.
    f32x3 later = nya_wind_at(&wind, position, 9.0F);
    nya_assert(!same_vector(first, later), "the gust changes the wind over time");

    printf("  PASSED\n");
  }

  // TEST: advance accumulates time, and the field's clock feeds nya_wind_sample
  {
    NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .strength = 1.0F, .gustiness = 0.5F });

    nya_assert(wind.time == 0.0F, "a fresh field starts at time zero");

    nya_wind_advance(&wind, 0.25F);
    nya_wind_advance(&wind, 0.25F);
    nya_assert(wind.time == 0.5F, "advance accumulates, got %f", (double)wind.time);

    // a non-positive step is ignored, the same rule the particle update uses.
    nya_wind_advance(&wind, -1.0F);
    nya_wind_advance(&wind, 0.0F);
    nya_assert(wind.time == 0.5F, "a non-positive step does nothing");

    f32x3 position = { 1, 0, 1 };
    nya_assert(same_vector(nya_wind_sample(&wind, position), nya_wind_at(&wind, position, 0.5F)), "sample reads the field's own clock");

    printf("  PASSED\n");
  }

  // TEST: the gust cannot leave the bounds the amplitudes set
  {
    f32 strength  = 4.0F;
    f32 gustiness = 0.8F;

    NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0 }, .strength = strength, .gustiness = gustiness });

    // the along-wind swing is bounded by strength * gustiness (the octave amplitudes sum to one), and the swirl is perpendicular so it does not touch the along-wind component. a small slack for float error.
    f32 along_min = strength * (1.0F - gustiness) - 0.001F;
    f32 along_max = strength * (1.0F + gustiness) + 0.001F;

    // the whole vector is bounded by the push plus the swing plus the (halved) swirl.
    f32 magnitude_max = (strength * (1.0F + gustiness)) + (strength * gustiness * 0.5F) + 0.001F;

    for (u32 i = 0; i < 4096; i++) {
      // a spread of points and times, deterministic so a failure reproduces.
      f32   t        = (f32)i * 0.037F;
      f32x3 position = { (f32)i * 1.9F - 100.0F, (f32)(i % 7), (f32)i * -0.7F + 50.0F };

      f32x3 sample = nya_wind_at(&wind, position, t);

      f32 along = nya_vector_dot(sample, wind.direction);
      nya_assert(along >= along_min && along <= along_max, "the along-wind component stays in its band, got %f at i=" FMTu32, (double)along, i);

      f32 magnitude = nya_vector_length(sample);
      nya_assert(magnitude <= magnitude_max, "the wind's magnitude stays bounded, got %f at i=" FMTu32, (double)magnitude, i);
    }

    // and gustiness is clamped, so a caller cannot push the bound past what the shader was tuned for.
    NYA_WindField clamped = nya_wind_field((NYA_WindOptions){ .gustiness = 5.0F });
    nya_assert(clamped.gustiness == 1.0F, "gustiness clamps to one");

    printf("  PASSED\n");
  }

  // TEST: nya_wind_set repoints the wind and leaves the clock alone
  {
    NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0 }, .strength = 1.0F, .gustiness = 0.0F });

    nya_wind_advance(&wind, 3.0F);

    // repoint it: a new direction, strength and gust. the accumulated time is not touched, so a running scene can change the wind without the sway jumping.
    nya_wind_set(&wind, (f32x3){ 0, 0, -4 }, 5.0F, 0.9F);

    nya_assert(wind.time == 3.0F, "set leaves the clock alone");
    nya_assert(wind.direction.z == -1.0F && wind.direction.x == 0.0F, "the wind now points along -z");
    nya_assert(wind.strength == 5.0F, "and blows harder");
    nya_assert(wind.gustiness == 0.9F, "and gustier");

    // a zero direction falls back to +x rather than normalizing a zero vector.
    nya_wind_set(&wind, f32x3_zero, 2.0F, 0.0F);
    nya_assert(wind.direction.x == 1.0F, "a zero direction is read as +x");

    // with the gust off again the sample is the steady push along the new direction.
    f32x3 base = nya_wind_base(&wind);
    nya_assert(same_vector(base, nya_wind_at(&wind, (f32x3){ 7, 2, 3 }, wind.time)), "no gust, so the sample is the steady push");

    printf("  PASSED\n");
  }

  printf("test_wind: all passed\n");
  return 0;
}
