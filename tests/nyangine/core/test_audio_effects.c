/**
 * The bus effect chain, on known signals: filter and equaliser responses, the limiter's ceiling, echo and reflection
 * timing, the compressor's ratio, a bypassed chain being exact, and a moving cutoff not clicking.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define RATE   48000
#define FRAMES 24000

/** SDL_mixer hands callbacks a few hundred frames at a time, so the chain is fed that way too. */
#define BUFFER 480

static const SDL_AudioSpec MONO = { .format = SDL_AUDIO_F32, .channels = 1, .freq = RATE };

static f32 signal_in[FRAMES];
static f32 signal_out[FRAMES];

static void run(NYA_AudioChain* chain, const SDL_AudioSpec* spec, f32* pcm, s32 frames) {
  for (s32 start = 0; start < frames; start += BUFFER) {
    s32 count = nya_min(BUFFER, frames - start);
    _nya_audio_chain_process(chain, spec, &pcm[start * spec->channels], count * spec->channels);
  }
}

static NYA_AudioChain* chain_with(NYA_Arena* arena, NYA_AudioEffects effects) {
  NYA_AudioChain*        chain    = _nya_audio_chain_create(arena);
  NYA_AudioChainSettings settings = { .effects = _nya_audio_effects_validate(effects) };

  _nya_audio_chain_publish(chain, &settings, arena);
  return chain;
}

/** Amplitude of the back half, once glides and transients are over, relative to a unit sine. RMS, since a few samples a cycle miss the peak. */
static f32 sine_response(NYA_Arena* arena, NYA_AudioEffects effects, f32 hz) {
  NYA_AudioChain* chain = chain_with(arena, effects);

  for (s32 i = 0; i < FRAMES; i++) signal_out[i] = sinf(2.0F * (f32)M_PI * hz * (f32)i / (f32)RATE);

  run(chain, &MONO, signal_out, FRAMES);

  f64 sum = 0.0;
  for (s32 i = FRAMES / 2; i < FRAMES; i++) sum += (f64)signal_out[i] * (f64)signal_out[i];

  return (f32)sqrt(2.0 * sum / (f64)(FRAMES / 2));
}

/** Where the loudest sample after the first is, for an impulse at frame zero. */
static s32 impulse_peak(NYA_AudioChain* chain) {
  for (s32 i = 0; i < FRAMES; i++) signal_out[i] = i == 0 ? 1.0F : 0.0F;

  run(chain, &MONO, signal_out, FRAMES);

  s32 at = 1;
  for (s32 i = 1; i < FRAMES; i++) {
    if (fabsf(signal_out[i]) > fabsf(signal_out[at])) at = i;
  }

  return at;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_audio_effects");
  defer      nya_arena_destroy(arena);

  // TEST: a chain with every unit off is exact, and zeroed settings never make one
  {
    NYA_AudioChainSettings off = { .effects = _nya_audio_effects_validate((NYA_AudioEffects){ 0 }) };
    nya_assert(!_nya_audio_chain_settings_active(&off), "validated zeroes must still read as nothing to run");

    NYA_AudioChain* chain = chain_with(arena, (NYA_AudioEffects){ 0 });

    for (s32 i = 0; i < FRAMES; i++) signal_in[i] = signal_out[i] = sinf((f32)i * 0.37F) * 0.8F;

    run(chain, &MONO, signal_out, FRAMES);

    for (s32 i = 0; i < FRAMES; i++) nya_assert(signal_out[i] == signal_in[i], "a bypassed chain changed sample %d", i);
  }

  // TEST: validation defaults and clamps
  {
    NYA_AudioEffects effects = _nya_audio_effects_validate((NYA_AudioEffects){
      .pass    = { .lowpass_hz = -5.0F },
      .echo    = { .enabled = true, .feedback = 3.0F },
      .limiter = { .enabled = true, .ceiling_db = 6.0F },
      .reverb  = { .room_size = 2.0F },
    });

    nya_assert(effects.pass.lowpass_hz == 0.0F, "a negative cutoff is off, got %f", (f64)effects.pass.lowpass_hz);
    nya_assert(effects.echo.feedback < 1.0F, "an echo feeding back at or past one never dies, got %f", (f64)effects.echo.feedback);
    nya_assert(effects.limiter.ceiling_db < 0.0F, "a ceiling above full scale limits nothing, got %f", (f64)effects.limiter.ceiling_db);
    nya_assert(effects.reverb.room_size < 1.0F, "a room of one or more grows forever, got %f", (f64)effects.reverb.room_size);
    nya_assert(effects.compressor.ratio >= 1.0F && effects.pass.resonance > 0.0F, "zeroes take their defaults");
  }

  // TEST: the passes are twelve decibels an octave, flat far from the corner
  {
    // three octaves past 1 kHz is about -36 dB, 0.016. a one pole would still pass 0.12.
    f32 low_bass   = sine_response(arena, (NYA_AudioEffects){ .pass = { .lowpass_hz = 1000.0F } }, 100.0F);
    f32 low_treble = sine_response(arena, (NYA_AudioEffects){ .pass = { .lowpass_hz = 1000.0F } }, 8000.0F);
    f32 low_corner = sine_response(arena, (NYA_AudioEffects){ .pass = { .lowpass_hz = 1000.0F } }, 1000.0F);

    nya_assert(low_bass > 0.97F, "a 1 kHz low pass must pass 100 Hz, got %f", (f64)low_bass);
    nya_assert(low_treble < 0.03F, "a 1 kHz low pass must take 8 kHz down past 30 dB, got %f", (f64)low_treble);
    nya_assert(fabsf(low_corner - 0.707F) < 0.05F, "a Butterworth corner is -3 dB, got %f", (f64)low_corner);

    f32 high_bass   = sine_response(arena, (NYA_AudioEffects){ .pass = { .highpass_hz = 1000.0F } }, 125.0F);
    f32 high_treble = sine_response(arena, (NYA_AudioEffects){ .pass = { .highpass_hz = 1000.0F } }, 8000.0F);

    nya_assert(high_bass < 0.03F, "a 1 kHz high pass must take 125 Hz down past 30 dB, got %f", (f64)high_bass);
    nya_assert(high_treble > 0.97F, "a 1 kHz high pass must pass 8 kHz, got %f", (f64)high_treble);
  }

  // TEST: the equaliser's bell and shelves land their gain where they sit
  {
    f32 bell_centre = sine_response(arena, (NYA_AudioEffects){ .equalizer = { .mid_db = 12.0F } }, 1000.0F);
    f32 bell_far    = sine_response(arena, (NYA_AudioEffects){ .equalizer = { .mid_db = 12.0F } }, 40.0F);

    nya_assert(fabsf(bell_centre - 3.98F) < 0.1F, "+12 dB at the bell's centre is 3.98, got %f", (f64)bell_centre);
    nya_assert(fabsf(bell_far - 1.0F) < 0.05F, "far from the bell is flat, got %f", (f64)bell_far);

    f32 low_shelf  = sine_response(arena, (NYA_AudioEffects){ .equalizer = { .low_db = -12.0F } }, 30.0F);
    f32 high_shelf = sine_response(arena, (NYA_AudioEffects){ .equalizer = { .high_db = 6.0F } }, 16000.0F);

    nya_assert(fabsf(low_shelf - 0.251F) < 0.03F, "-12 dB below the low shelf is 0.251, got %f", (f64)low_shelf);
    nya_assert(fabsf(high_shelf - 1.995F) < 0.1F, "+6 dB above the high shelf is 1.995, got %f", (f64)high_shelf);
  }

  // TEST: the limiter never lets a sample past its ceiling
  {
    NYA_AudioChain* chain   = chain_with(arena, (NYA_AudioEffects){ .limiter = { .enabled = true, .ceiling_db = -6.0F } });
    f32             ceiling = powf(10.0F, -6.0F / 20.0F);

    static f32    stereo[FRAMES * 2];
    SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = 2, .freq = RATE };

    // bursts up to four times full scale, with quiet gaps the release recovers in, then bursts again.
    for (s32 i = 0; i < FRAMES * 2; i++) {
      f32 noise    = nya_ihash2(i, i / 2, 0x51CE);
      f32 envelope = ((i / 2) / 4000) % 2 == 0 ? 4.0F : 0.05F;
      stereo[i]    = noise * envelope;
    }

    run(chain, &spec, stereo, FRAMES);

    f32 loudest = 0.0F;
    for (s32 i = 0; i < FRAMES * 2; i++) loudest = nya_max(loudest, fabsf(stereo[i]));

    nya_assert(loudest <= ceiling, "the limiter let %f through a ceiling of %f", (f64)loudest, (f64)ceiling);
    nya_assert(loudest > ceiling * 0.9F, "the limiter must reach its ceiling, not squash everything, got %f", (f64)loudest);
  }

  // TEST: an echo repeats after its delay, and a reflection tap after its own
  {
    // wet eases in over the smoothing time, so the impulse is heard through the chain a moment after it is enabled.
    NYA_AudioChain* echo = chain_with(arena, (NYA_AudioEffects){ .echo = { .enabled = true, .delay_ms = 100.0F, .feedback = 0.1F, .wet = 1.0F, .lowpass_hz = 20000.0F } });
    run(echo, &MONO, signal_in, 4800);

    s32 at = impulse_peak(echo);
    nya_assert(at >= 4800 && at <= 4802, "a 100 ms echo at 48 kHz repeats at frame 4800, got %d", at);

    NYA_AudioChain*        tapped   = _nya_audio_chain_create(arena);
    NYA_AudioChainSettings settings = { .effects = _nya_audio_effects_validate((NYA_AudioEffects){ 0 }) };
    settings.reflections.taps[0]    = (NYA_AudioReflectionTap){ .delay_s = 0.05F, .gain = 1.0F, .pan = 0.0F, .lowpass_hz = 20000.0F };

    nya_assert(_nya_audio_chain_settings_active(&settings), "one audible tap is enough to need a chain");
    _nya_audio_chain_publish(tapped, &settings, arena);

    for (s32 i = 0; i < 4800; i++) signal_in[i] = 0.0F;
    run(tapped, &MONO, signal_in, 4800);

    at = impulse_peak(tapped);
    nya_assert(at >= 2400 && at <= 2402, "a 50 ms reflection at 48 kHz returns at frame 2400, got %d", at);
  }

  // TEST: the compressor cuts by its ratio above the threshold
  {
    // a full scale tone, 20 dB over a -20 dB threshold at 4:1, comes out 15 dB down: 0.178.
    f32 level = sine_response(arena, (NYA_AudioEffects){ .compressor = { .enabled = true, .threshold_db = -20.0F, .ratio = 4.0F } }, 1000.0F);

    nya_assert(fabsf(level - 0.178F) < 0.03F, "4:1 over 20 dB leaves 0.178, got %f", (f64)level);
  }

  // TEST: a cutoff that jumps is eased, so the output never steps
  {
    NYA_AudioChain* chain = chain_with(arena, (NYA_AudioEffects){ 0 });

    for (s32 i = 0; i < FRAMES; i++) signal_out[i] = sinf(2.0F * (f32)M_PI * 1000.0F * (f32)i / (f32)RATE);

    NYA_AudioChainSettings dull = { .effects = _nya_audio_effects_validate((NYA_AudioEffects){ .pass = { .lowpass_hz = 150.0F } }) };

    run(chain, &MONO, signal_out, FRAMES / 2);
    _nya_audio_chain_publish(chain, &dull, arena);
    run(chain, &MONO, &signal_out[FRAMES / 2], FRAMES / 2);

    // a 1 kHz unit sine moves at most 0.131 a sample. a click is a step far past that.
    f32 steepest = 0.0F;
    for (s32 i = 1; i < FRAMES; i++) steepest = nya_max(steepest, fabsf(signal_out[i] - signal_out[i - 1]));

    nya_assert(steepest < 0.15F, "switching a low pass on stepped the output by %f", (f64)steepest);
  }

  printf("PASSED: test_audio_effects\n");
  return 0;
}
