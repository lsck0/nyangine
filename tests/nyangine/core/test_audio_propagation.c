/**
 * Sound propagation against a fake scene of boxes: partial occlusion, thickness, diffraction around a wall's edge, the
 * room estimate in a closed box against an open field, the echoes it places, the ray budget, and easing that never
 * jumps.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define SCENE_BOXES 8

/** A few boxes and a count of what was asked of them. */
typedef struct {
  f32x3 min[SCENE_BOXES];
  f32x3 max[SCENE_BOXES];
  u32   count;

  u32 calls;
  u32 rays;
  u32 largest_batch;
} Scene;

static void scene_add(Scene* scene, f32x3 min, f32x3 max) {
  nya_assert(scene->count < SCENE_BOXES);
  scene->min[scene->count] = min;
  scene->max[scene->count] = max;
  scene->count++;
}

/** Slab test. A ray starting inside a box does not hit it, as a physics raycast would not. */
static void scene_trace(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data) {
  Scene* scene = user_data;

  scene->calls++;
  scene->rays          += count;
  scene->largest_batch  = nya_max(scene->largest_batch, count);

  for (u32 r = 0; r < count; r++) {
    f32 nearest = 1.0F;

    for (u32 b = 0; b < scene->count; b++) {
      f32 enter = -1e30F;
      f32 leave = 1e30F;
      b8  miss  = false;

      for (u32 axis = 0; axis < 3; axis++) {
        f32 origin    = rays[r].origin[axis];
        f32 direction = rays[r].direction[axis];

        if (fabsf(direction) < 1e-9F) {
          if (origin < scene->min[b][axis] || origin > scene->max[b][axis]) miss = true;
          continue;
        }

        f32 near_t = (scene->min[b][axis] - origin) / direction;
        f32 far_t  = (scene->max[b][axis] - origin) / direction;

        enter = nya_max(enter, nya_min(near_t, far_t));
        leave = nya_min(leave, nya_max(near_t, far_t));
      }

      if (miss || enter > leave || enter < 0.0F || enter > 1.0F) continue;

      nearest = nya_min(nearest, enter);
    }

    out_fractions[r] = nearest;
  }
}

static const NYA_AudioEmitter* one_emitter(f32x3 position, f32 radius) {
  static NYA_AudioEmitter emitters[NYA_AUDIO_VOICES];

  for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) emitters[i] = (NYA_AudioEmitter){ 0 };
  emitters[0] = (NYA_AudioEmitter){ .position = position, .radius = radius, .active = true };

  return emitters;
}

#define EAR   ((f32x3){ 0.0F, 0.0F, 0.0F })
#define RIGHT ((f32x3){ 1.0F, 0.0F, 0.0F })
#define STEP  (1.0F / 60.0F)

static NYA_AudioTracer tracer;

s32 main(void) {
  // TEST: validation fills zeroes and keeps the budget within its ceiling
  {
    NYA_AudioPropagation zero = _nya_audio_propagation_validate((NYA_AudioPropagation){ 0 });

    nya_assert(zero.ray_budget == NYA_AUDIO_PROPAGATION_RAY_BUDGET && zero.voice_rays == NYA_AUDIO_PROPAGATION_VOICE_RAYS);
    nya_assert(zero.reflections == 0.0F, "no reflections without the environment");

    NYA_AudioPropagation greedy = _nya_audio_propagation_validate((NYA_AudioPropagation){ .ray_budget = 100000, .voice_rays = 99 });
    nya_assert(greedy.ray_budget == NYA_AUDIO_PROPAGATION_RAYS_MAX && greedy.voice_rays == NYA_AUDIO_PROPAGATION_VOICE_RAYS_MAX);

    // a budget too small for one hidden voice is raised, or that voice could never be traced again.
    NYA_AudioPropagation stingy = _nya_audio_propagation_validate((NYA_AudioPropagation){ .ray_budget = 1, .diffraction = true, .environment = true });
    nya_assert(stingy.ray_budget == NYA_AUDIO_PROPAGATION_ENVIRONMENT_SLICE + NYA_AUDIO_PROPAGATION_VOICE_RAYS + 1 + (2 * NYA_AUDIO_PROPAGATION_PROBES), "got %u", stingy.ray_budget);
  }

  // TEST: partial occlusion is the share of rays blocked, and thickness quietens what gets through
  {
    NYA_AudioPropagation propagation = _nya_audio_propagation_validate((NYA_AudioPropagation){ .enabled = true, .voice_rays = 5 });
    f32x3                source      = { 0.0F, 0.0F, -10.0F };

    // a wall covering only the right edge of a source a metre wide: one ray of five.
    Scene edge = { 0 };
    scene_add(&edge, (f32x3){ 0.3F, -20.0F, -6.0F }, (f32x3){ 20.0F, 20.0F, -5.0F });

    _nya_audio_tracer_reset(&tracer);
    _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &edge, EAR, RIGHT, one_emitter(source, 1.0F), STEP);

    NYA_AudioPath* path = &tracer.paths[0];
    nya_assert(path->traced, "a new voice is traced on its first update");
    nya_assert(fabsf(path->occlusion - 0.2F) < 1e-4F, "one ray of five blocked is 0.2, got %f", (f64)path->occlusion);
    nya_assert(path->target_gain < 1.0F && path->target_gain > 0.8F, "a sliver hidden is a little quieter, got %f", (f64)path->target_gain);
    nya_assert(path->gain == path->target_gain, "a voice's first result is taken as is");

    // a metre of wall over all of it. the centre ray meets it 5 m out, the reverse ray 3 m back: a metre thick.
    Scene wall = { 0 };
    scene_add(&wall, (f32x3){ -20.0F, -20.0F, -6.0F }, (f32x3){ 20.0F, 20.0F, -5.0F });

    _nya_audio_tracer_reset(&tracer);
    _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &wall, EAR, RIGHT, one_emitter(source, 1.0F), STEP);

    f32 expected = propagation.transmission * expf(-1.0F / propagation.thickness);
    nya_assert(path->occlusion == 1.0F, "a wall over everything blocks every ray, got %f", (f64)path->occlusion);
    nya_assert(fabsf(path->target_gain - expected) < 1e-3F, "a metre of wall transmits %f, got %f", (f64)expected, (f64)path->target_gain);
    nya_assert(path->target_muffle > 1.0F, "thicker than a thin surface is duller than one, got %f", (f64)path->target_muffle);

    // four metres of the same lets less through.
    Scene hill = { 0 };
    scene_add(&hill, (f32x3){ -20.0F, -20.0F, -8.0F }, (f32x3){ 20.0F, 20.0F, -4.0F });

    _nya_audio_tracer_reset(&tracer);
    _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &hill, EAR, RIGHT, one_emitter(source, 1.0F), STEP);

    nya_assert(path->target_gain < expected * 0.5F, "four metres must transmit well under one does, got %f", (f64)path->target_gain);
  }

  // TEST: diffraction finds the way around a wall's edge and moves the sound toward it
  {
    // a solid wall: little gets through, so the way round is what is heard.
    NYA_AudioPropagation without = _nya_audio_propagation_validate((NYA_AudioPropagation){ .enabled = true, .transmission = 0.1F });
    NYA_AudioPropagation with    = _nya_audio_propagation_validate((NYA_AudioPropagation){ .enabled = true, .transmission = 0.1F, .diffraction = true, .diffraction_reach = 2.0F });
    f32x3                source  = { 0.0F, 0.0F, -10.0F };

    // everything to the left of x = 1.5 is wall, floor to ceiling; the way round is to the right.
    Scene right_open = { 0 };
    scene_add(&right_open, (f32x3){ -30.0F, -30.0F, -6.0F }, (f32x3){ 1.5F, 30.0F, -5.0F });

    Scene left_open = { 0 };
    scene_add(&left_open, (f32x3){ -1.5F, -30.0F, -6.0F }, (f32x3){ 30.0F, 30.0F, -5.0F });

    _nya_audio_tracer_reset(&tracer);
    for (u32 i = 0; i < 4; i++) _nya_audio_tracer_step(&tracer, &without, scene_trace, &right_open, EAR, RIGHT, one_emitter(source, 0.25F), STEP);
    f32 through        = tracer.paths[0].target_gain;
    f32 through_muffle = tracer.paths[0].target_muffle;

    _nya_audio_tracer_reset(&tracer);
    for (u32 i = 0; i < 4; i++) _nya_audio_tracer_step(&tracer, &with, scene_trace, &right_open, EAR, RIGHT, one_emitter(source, 0.25F), STEP);
    NYA_AudioPath around = tracer.paths[0];

    nya_assert(around.occlusion == 1.0F, "the direct path is still blocked");
    nya_assert(around.target_gain > through * 1.5F, "a way around must be louder than through, %f against %f", (f64)around.target_gain, (f64)through);
    nya_assert(around.target_offset.x > 1.0F, "the sound must move toward the opening on the right, offset %f", (f64)around.target_offset.x);
    nya_assert(around.target_muffle < through_muffle, "round the edge is brighter than through, muffle %f against %f", (f64)around.target_muffle, (f64)through_muffle);

    // the detour is 0.78 m: 2.27 half wavelengths at 500 Hz, a 16.7 dB loss.
    nya_assert(fabsf(around.target_gain - 0.146F) < 0.01F, "a 0.78 m detour loses 16.7 dB, 0.146, got %f", (f64)around.target_gain);

    _nya_audio_tracer_reset(&tracer);
    for (u32 i = 0; i < 4; i++) _nya_audio_tracer_step(&tracer, &with, scene_trace, &left_open, EAR, RIGHT, one_emitter(source, 0.25F), STEP);

    nya_assert(tracer.paths[0].target_offset.x < -1.0F, "mirrored, it must move left, offset %f", (f64)tracer.paths[0].target_offset.x);

    // a clear path casts no probes: the only rays are the voice's own and its reverse.
    Scene empty = { 0 };
    _nya_audio_tracer_reset(&tracer);
    _nya_audio_tracer_step(&tracer, &with, scene_trace, &empty, EAR, RIGHT, one_emitter(source, 0.25F), STEP);
    empty.rays = 0;
    _nya_audio_tracer_step(&tracer, &with, scene_trace, &empty, EAR, RIGHT, one_emitter(source, 0.25F), STEP);
    nya_assert(empty.rays == with.voice_rays + 1, "an open voice costs its own rays and the reverse, cast %u", empty.rays);
  }

  // TEST: a closed box reads as a room, an open field barely, and the reverb follows
  {
    NYA_AudioPropagation propagation = _nya_audio_propagation_validate((NYA_AudioPropagation){ .enabled = true, .environment = true, .reflections = 0.5F });
    NYA_AudioEmitter     none[NYA_AUDIO_VOICES] = { 0 };

    Scene box = { 0 };
    scene_add(&box, (f32x3){ -4.0F, -3.2F, -4.0F }, (f32x3){ 4.0F, -3.0F, 4.0F });
    scene_add(&box, (f32x3){ -4.0F, 3.0F, -4.0F }, (f32x3){ 4.0F, 3.2F, 4.0F });
    scene_add(&box, (f32x3){ -4.0F, -4.0F, -4.0F }, (f32x3){ -3.0F, 4.0F, 4.0F });
    scene_add(&box, (f32x3){ 3.0F, -4.0F, -4.0F }, (f32x3){ 4.0F, 4.0F, 4.0F });
    scene_add(&box, (f32x3){ -4.0F, -4.0F, -4.0F }, (f32x3){ 4.0F, 4.0F, -3.0F });
    scene_add(&box, (f32x3){ -4.0F, -4.0F, 3.0F }, (f32x3){ 4.0F, 4.0F, 4.0F });

    Scene field = { 0 };
    scene_add(&field, (f32x3){ -100.0F, -1.2F, -100.0F }, (f32x3){ 100.0F, -1.0F, 100.0F });

    // ten seconds: many sweeps of the probes and far past the glide.
    _nya_audio_tracer_reset(&tracer);
    for (u32 i = 0; i < 600; i++) _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &box, EAR, RIGHT, none, STEP);
    NYA_AudioEnvironment room    = tracer.environment;
    NYA_AudioReflections echoes  = tracer.reflections;

    _nya_audio_tracer_reset(&tracer);
    for (u32 i = 0; i < 600; i++) _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &field, EAR, RIGHT, none, STEP);
    NYA_AudioEnvironment open    = tracer.environment;
    NYA_AudioReflections echoes2 = tracer.reflections;

    nya_assert(room.enclosure > 0.99F, "every probe hits a closed box, enclosure %f", (f64)room.enclosure);
    nya_assert(fabsf(open.enclosure - (5.0F / 14.0F)) < 0.01F, "a floor catches the down axis and four corners, enclosure %f", (f64)open.enclosure);
    nya_assert(room.reverb.room_size > open.reverb.room_size * 2.0F, "a box rings longer, room %f against %f", (f64)room.reverb.room_size, (f64)open.reverb.room_size);
    nya_assert(room.reverb.wet > open.reverb.wet * 4.0F, "a box is much wetter, wet %f against %f", (f64)room.reverb.wet, (f64)open.reverb.wet);

    // the nearest walls are three metres away on the axes: back in 17.5 ms. a floor a metre down: 5.8 ms.
    nya_assert(fabsf(echoes.taps[0].delay_s - (6.0F / 343.0F)) < 2e-4F, "a wall 3 m away echoes after 17.5 ms, got %f", (f64)echoes.taps[0].delay_s);
    nya_assert(fabsf(echoes2.taps[0].delay_s - (2.0F / 343.0F)) < 2e-4F, "a floor 1 m down echoes after 5.8 ms, got %f", (f64)echoes2.taps[0].delay_s);
    nya_assert(echoes.taps[NYA_AUDIO_REFLECTION_TAPS - 1].gain > 0.0F, "a box fills every tap");
    nya_assert(echoes2.taps[NYA_AUDIO_REFLECTION_TAPS - 1].gain == 0.0F, "a field has only five surfaces to echo off");

    // the wall on the right pans right.
    b8 panned = false;
    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) panned = panned || echoes.taps[tap].pan > 0.99F;
    nya_assert(panned, "one of a box's echoes comes from the wall on the right");
  }

  // TEST: the budget holds with every voice hidden, and the round robin reaches them all
  {
    NYA_AudioPropagation propagation = _nya_audio_propagation_validate((NYA_AudioPropagation){ .enabled = true, .diffraction = true, .environment = true, .ray_budget = 48 });

    Scene wall = { 0 };
    scene_add(&wall, (f32x3){ -50.0F, -50.0F, -6.0F }, (f32x3){ 50.0F, 50.0F, -5.0F });

    NYA_AudioEmitter crowd[NYA_AUDIO_VOICES];
    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) {
      crowd[i] = (NYA_AudioEmitter){ .position = { (f32)i - 8.0F, 0.0F, -10.0F }, .active = true };
    }

    _nya_audio_tracer_reset(&tracer);

    u32 updates = 0;
    b8  all     = false;

    while (!all && updates < 64) {
      u32 before = wall.calls;
      _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &wall, EAR, RIGHT, crowd, STEP);
      updates++;

      nya_assert(wall.calls - before <= 1, "one batch per update");
      nya_assert(tracer.rays_cast <= propagation.ray_budget, "cast %u against a budget of %u", tracer.rays_cast, propagation.ray_budget);

      all = true;
      for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) all = all && tracer.paths[i].traced;
    }

    // a hidden voice costs 4 + 1 + 8 = 13, and 44 of 48 are left after the room: three voices an update.
    nya_assert(all && updates == 6, "sixteen hidden voices at three an update take six updates, took %u", updates);
    nya_assert(wall.largest_batch <= propagation.ray_budget);
  }

  // TEST: easing is bounded per update and settles
  {
    NYA_AudioPropagation propagation = _nya_audio_propagation_validate((NYA_AudioPropagation){ .enabled = true, .smoothing_ms = 100.0F });
    f32x3                source      = { 0.0F, 0.0F, -10.0F };

    Scene empty = { 0 };
    Scene wall  = { 0 };
    scene_add(&wall, (f32x3){ -20.0F, -20.0F, -6.0F }, (f32x3){ 20.0F, 20.0F, -5.0F });

    _nya_audio_tracer_reset(&tracer);
    for (u32 i = 0; i < 10; i++) _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &empty, EAR, RIGHT, one_emitter(source, 0.5F), STEP);
    nya_assert(tracer.paths[0].gain == 1.0F, "open is unity");

    // the wall appears. no update may move the gain more than the easing allows over one step.
    f32 bound = 1.0F - expf(-(STEP * 1000.0F) / propagation.smoothing_ms);
    f32 last  = tracer.paths[0].gain;

    for (u32 i = 0; i < 120; i++) {
      _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &wall, EAR, RIGHT, one_emitter(source, 0.5F), STEP);

      f32 gain = tracer.paths[0].gain;
      nya_assert(fabsf(gain - last) <= bound + 1e-5F, "update %u moved the gain %f, over the bound %f", i, (f64)fabsf(gain - last), (f64)bound);
      last = gain;
    }

    nya_assert(fabsf(last - tracer.paths[0].target_gain) < 1e-3F, "two seconds settles a 100 ms ease, %f against %f", (f64)last, (f64)tracer.paths[0].target_gain);

    // a voice that stops is forgotten, so the next sound in its slot starts fresh.
    NYA_AudioEmitter none[NYA_AUDIO_VOICES] = { 0 };
    _nya_audio_tracer_step(&tracer, &propagation, scene_trace, &wall, EAR, RIGHT, none, STEP);
    nya_assert(!tracer.paths[0].traced && tracer.paths[0].gain == 1.0F, "a stopped voice's path resets");
  }

  // TEST: through the audio system, off traces nothing
  {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();
    NYA_EXPECT(nya_system_audio_init());

    defer nya_system_audio_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    Scene scene = { 0 };
    nya_audio_rays_set(NYA_AUDIO_SPACE_3D, scene_trace, &scene);

    for (u32 i = 0; i < 10; i++) nya_system_audio_update(STEP);
    nya_assert(scene.calls == 0, "propagation that was never enabled cast %u batches", scene.calls);

    nya_audio_propagation_set((NYA_AudioPropagation){ .enabled = true, .environment = true });
    NYA_AudioPropagation got = nya_audio_propagation_get();
    nya_assert(got.ray_budget == NYA_AUDIO_PROPAGATION_RAY_BUDGET, "the setter validates");

    for (u32 i = 0; i < 10; i++) nya_system_audio_update(STEP);

    if (_nya_audio_system.ready) {
      nya_assert(scene.calls == 10, "on, the room is probed every update, got %u", scene.calls);
    } else {
      nya_log_info("no audio device, so the system update was not exercised (this is expected in CI)");
    }

    nya_audio_propagation_set((NYA_AudioPropagation){ 0 });
    u32 calls = scene.calls;

    for (u32 i = 0; i < 10; i++) nya_system_audio_update(STEP);
    nya_assert(scene.calls == calls, "switched off, it must stop tracing");
    nya_assert(nya_audio_environment_get().enclosure == 0.0F, "off has no room");
  }

  printf("PASSED: test_audio_propagation\n");
  return 0;
}
