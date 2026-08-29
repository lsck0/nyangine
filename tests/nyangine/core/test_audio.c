/**
 * The audio system: voices, generational handles, effects and gain.
 *
 * There is no sound asset in the repository, so this synthesizes a WAV, writes it next to the test
 * and loads it as an external asset. That is deliberately the whole path — decode, voice
 * acquisition, playback, effects — rather than a mock, because the parts worth testing are the ones
 * that talk to SDL_mixer.
 *
 * **Runs with or without an audio device.** A machine with no sound server, which is every CI
 * container, gets no mixer, and the audio system then reports itself unready and every call becomes
 * a no-op. The assertions below are written so both outcomes pass: anything device dependent is
 * guarded on whether the first play produced a voice, and the handle arithmetic that does not need a
 * device is asserted unconditionally.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Drains the asset queues, the way the end of a real frame does. */
static void end_frame(void) {
  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

/**
 * Checks one of the variation helpers: in range, actually varying, and unbiased.
 *
 * Both have the same shape — a uniform offset in ±range, applied as an exponent — so they get the
 * same three assertions rather than two copies of them. `units_per_doubling` is what turns a
 * returned ratio back into the units the range is expressed in, which is the only thing that differs
 * between a detune and a level change.
 * */
static void check_variation(NYA_ConstCString name, f32 (*vary)(f32, f32), f32 range, f32 units_per_doubling, NYA_ConstCString unit) {
  // Nothing asked for, nothing changed, and bit exact rather than nearly — a sound the game did not
  // ask to vary must come out as authored, and "almost 1.0" is still a variation.
  nya_assert(vary(1.0F, 0.0F) == 1.0F, "%s: no variation must leave the value untouched", name);
  nya_assert(vary(2.0F, 0.0F) == 2.0F, "%s: no variation must not disturb an explicit value either", name);

  // A negative range cannot be inverted, so it means none rather than something mirrored.
  nya_assert(vary(1.0F, -3.0F) == 1.0F, "%s: a negative range must be read as no variation", name);

  const u32 draws = 4096;

  /*
   * The bounds the range promises, as a ratio, with a hair of slack.
   *
   * The slack is not laziness: this reconstructs the bound through exp2f, while the gain helper
   * reaches it through powf(10, x/20). The two agree to a few ULP rather than exactly, so an extreme
   * draw could sit one bit outside a bound computed the other way. A relative 1e-5 is far below
   * anything this assertion is meant to catch — a range off by a factor, or by the 20-versus-10
   * decibel mistake — and far above float noise.
   */
  f32 slack = 1.0F + 1e-5F;
  f32 low   = exp2f(-range / units_per_doubling) / slack;
  f32 high  = exp2f(range / units_per_doubling) * slack;

  f64 sum   = 0.0;
  f32 first = vary(1.0F, range);
  b8  moved = false;

  for (u32 i = 0; i < draws; i++) {
    f32 ratio = vary(1.0F, range);

    nya_assert(ratio >= low && ratio <= high, "%s: draw %u left the range: %f not in [%f, %f]", name, i, (f64)ratio, (f64)low, (f64)high);

    // Summed in the exponent, which is where the distribution is uniform. Averaging the ratios
    // instead would find a mean above 1.0 by construction and prove nothing.
    sum += (f64)log2f(ratio) * (f64)units_per_doubling;

    if (ratio != first) moved = true;
  }

  // The whole point of the feature: a constant would satisfy every bound above.
  nya_assert(moved, "%s: the variation never varied — %u draws all came back as %f", name, draws, (f64)first);

  /*
   * Centred, which is what a symmetric range means.
   *
   * Uniform over ±range has standard deviation range/√3, so the mean of `draws` of them has standard
   * error range/(√3·√draws) — about range/111 here. Ten of those is the tolerance, which is loose
   * enough that the assertion effectively never trips by chance. What it catches is a one sided or
   * sign-flipped draw, where the mean lands half a range out and misses by a factor of five.
   *
   * It does *not* catch drawing linearly in the ratio instead of in the exponent: that biases by
   * only about 2% of the range, well inside this. The bounds above are what catch that one — a
   * linear ±v range reaches below 2^(-range/12) on the quiet side and trips the range assertion on
   * the first extreme draw. The same goes for confusing amplitude decibels with power decibels,
   * which widens the ratio range by a factor rather than shifting its centre.
   */
  f64 mean      = sum / (f64)draws;
  f64 tolerance = (f64)range / 11.0;

  nya_assert(fabs(mean) < tolerance, "%s: biased %s by %f %s over %u draws", name, mean < 0.0 ? "down" : "up", mean, unit, draws);
}

/** Frames of filter test signal, 100ms at the rate below. Long enough that the transient is noise. */
#define FILTER_FRAMES 4800
#define FILTER_RATE   48000

/** Fills `pcm` with a unit sine at `hz`, one channel. */
static void fill_sine(f32* pcm, s32 frames, f32 hz) {
  for (s32 i = 0; i < frames; i++) pcm[i] = (f32)sin(2.0 * M_PI * (f64)hz * (f64)i / (f64)FILTER_RATE);
}

/** Root mean square of the back half, which is past the filter's start-up transient. */
static f64 tail_rms(const f32* pcm, s32 frames) {
  s32 start = frames / 2;
  s32 count = frames - start;

  f64 sum = 0.0;
  for (s32 i = start; i < frames; i++) sum += (f64)pcm[i] * (f64)pcm[i];

  return sqrt(sum / (f64)count);
}

/**
 * What fraction of a sine at `hz` survives the filter.
 *
 * Measured rather than derived, so it checks the code rather than restating it: the buffer goes in
 * as a known tone and the ratio of what comes out is compared against the one pole's textbook
 * response. A unit sine has an RMS of 1/√2, which is what the ratio is taken against.
 * */
static f64 filter_response(f32 cutoff_hz, f32 hz) {
  static f32 pcm[FILTER_FRAMES];

  SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = 1, .freq = FILTER_RATE };

  NYA_AudioFilterState filter;
  _nya_audio_filter_reset(&filter);

  // Straight to the state, not through nya_audio_bus_filter_set: the mixer's thread is running under
  // the dummy driver and owns the buses, so a test that reached into one would be racing it.
  atomic_store_explicit(&filter.target_hz, cutoff_hz, memory_order_relaxed);
  atomic_store_explicit(&filter.glide_ms, 0.0F, memory_order_relaxed);

  fill_sine(pcm, FILTER_FRAMES, hz);
  _nya_audio_filter_apply(&filter, &spec, pcm, FILTER_FRAMES);

  return tail_rms(pcm, FILTER_FRAMES) / (1.0 / sqrt(2.0));
}

/** Where the synthesized clip is written. Removed at the end. */
#define TEST_WAV_PATH "./tests/nyangine/core/test_audio_tone.wav"

/** Writes a quarter second 8 bit mono sine at 8kHz. Small, valid, and decodable by SDL_mixer. */
static void write_test_wav(void) {
  const u32 sample_rate = 8000;
  const u32 samples     = sample_rate / 4;

  u8 pcm[8000 / 4];
  for (u32 i = 0; i < samples; i++) {
    // 440Hz, centred on 128 because 8 bit PCM in a WAV is unsigned.
    f64 t  = (f64)i / (f64)sample_rate;
    pcm[i] = (u8)(128.0 + (100.0 * sin(2.0 * M_PI * 440.0 * t)));
  }

  u32 data_size = samples;
  u32 riff_size = 36 + data_size;

  u8  header[44];
  u8* h = header;

#define PUT4(str)        nya_memcpy(h, (str), 4), h += 4
#define PUT32(v)         { u32 _v = (v); nya_memcpy(h, &_v, 4); h += 4; }
#define PUT16(v)         { u16 _v = (u16)(v); nya_memcpy(h, &_v, 2); h += 2; }

  PUT4("RIFF");
  PUT32(riff_size);
  PUT4("WAVE");
  PUT4("fmt ");
  PUT32(16);            // fmt chunk size
  PUT16(1);             // PCM
  PUT16(1);             // mono
  PUT32(sample_rate);
  PUT32(sample_rate);   // byte rate: rate * channels * bytes per sample
  PUT16(1);             // block align
  PUT16(8);             // bits per sample
  PUT4("data");
  PUT32(data_size);

#undef PUT4
#undef PUT32
#undef PUT16

  // Straight to stdio, the way test_asset.c writes its fixtures: the engine's file API is string
  // oriented and this is binary.
  FILE* file = fopen(TEST_WAV_PATH, "wb");
  nya_assert(file != nullptr, "could not create the fixture at %s", TEST_WAV_PATH);
  (void)fwrite(header, 1, sizeof(header), file);
  (void)fwrite(pcm, 1, data_size, file);
  (void)fclose(file);
}

s32 main(void) {
  /*
   * The dummy audio driver, for the reason test_asset.c gives: opening a real device on a machine
   * without one leaks ALSA's configuration tree and fails the leak sanitizer over memory no engine
   * code touched.
   *
   * Unlike that test, this one wants the mixer to actually come up — the dummy driver provides a
   * playback device that decodes and mixes normally and simply discards the output, so every path
   * below is the real one.
   */
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // The systems the audio system needs, rather than nya_app_init — a full init opens a window and
  // brings up the renderer, neither of which a headless test can do.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();
  NYA_EXPECT(nya_system_audio_init());

  defer nya_system_audio_deinit();
  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  write_test_wav();
  defer (void)nya_filesystem_delete(TEST_WAV_PATH);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a null handle is inert, whether or not there is a device
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The property that matters: NYA_SOUND_VOICE_NONE must not resolve to slot zero, which is a real
    // voice. Getting this wrong would make every failed play silently steer the first sound playing.
    nya_assert(!nya_audio_voice_valid(NYA_SOUND_VOICE_NONE), "the null voice must never be valid");

    // All of these take the null handle and must do nothing rather than crash, because that is what a
    // caller gets from a play that found no free voice and will pass along without checking.
    nya_audio_voice_set_gain(NYA_SOUND_VOICE_NONE, 0.5F);
    nya_audio_voice_set_pitch(NYA_SOUND_VOICE_NONE, 2.0F);
    nya_audio_voice_set_pan(NYA_SOUND_VOICE_NONE, -1.0F);
    nya_audio_voice_set_position(NYA_SOUND_VOICE_NONE, (f32x3){ 1.0F, 0.0F, 0.0F });
    nya_audio_voice_stop(NYA_SOUND_VOICE_NONE, 0);

    // An out of range index is refused the same way, since a handle is a plain struct a caller can
    // build by hand or leave uninitialised.
    nya_assert(!nya_audio_voice_valid((NYA_SoundVoice){ .index = 9999, .generation = 1 }));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: gains are clamped and readable back
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Independent of any device: these are plain state, and an options menu reads them back to
    // populate its sliders.
    nya_audio_set_master_gain(0.5F);
    nya_audio_set_sound_gain(0.25F);
    nya_audio_set_music_gain(0.75F);

    nya_assert(fabsf(nya_audio_master_gain() - 0.5F) < 0.0001F, "got %f", (f64)nya_audio_master_gain());
    nya_assert(fabsf(nya_audio_sound_gain() - 0.25F) < 0.0001F);
    nya_assert(fabsf(nya_audio_music_gain() - 0.75F) < 0.0001F);

    // Negative gain inverts a waveform rather than silencing it, so it is clamped rather than passed
    // through to the mixer.
    nya_audio_set_master_gain(-1.0F);
    nya_assert(fabsf(nya_audio_master_gain()) < 0.0001F, "a negative gain must clamp to zero");

    nya_audio_set_master_gain(1.0F);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: pitch and gain variation stay in range, actually vary, and are unbiased
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The arithmetic rather than the audible result, because neither a detune nor a level change can
    // be heard without capturing the mixer's output. Device independent: this never touches a track.
    // Twelve semitones to a doubling of the rate, and 20·log10(2) decibels to a doubling of the
    // amplitude — the two exponents these are drawn in.
    check_variation("pitch", _nya_audio_vary_pitch, 2.0F, 12.0F, "semitones");
    check_variation("gain", _nya_audio_vary_gain, 2.0F, 20.0F * log10f(2.0F), "dB");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the listener, and how a world point lands in the mixer's space
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Pure arithmetic, so device independent. Every value below is exact in binary, which is why
    // these compare with == rather than an epsilon.

    // Unspecified means one, not zero — a reference distance of zero is a division by it.
    nya_audio_listener_set((NYA_AudioListener){ .position = { 1.0F, 2.0F } });
    nya_assert(nya_audio_listener_get().reference_distance == 1.0F, "an unspecified reference distance must become 1.0");

    nya_audio_listener_set((NYA_AudioListener){ .position = { 10.0F, 20.0F }, .reference_distance = 4.0F, .plane = NYA_AUDIO_PLANE_SIDE });
    nya_assert(nya_audio_listener_get().reference_distance == 4.0F, "the listener must read back as it was set");

    // Wherever the listener stands is the origin, because the mixer's listener cannot be moved off
    // it. Getting this wrong puts every sound at a constant offset that no test of a single sound
    // would notice.
    f32x3 here = _nya_audio_world_to_audio((f32x2){ 10.0F, 20.0F });
    nya_assert(here[0] == 0.0F && here[1] == 0.0F && here[2] == 0.0F, "a sound on the listener must land at the origin");

    /*
     * Side on: the screen is a wall, so world y becomes height and is negated on the way in — the
     * renderer's y counts downward while the mixer's counts up. Nothing reaches z.
     *
     * World (14, 28) is 4 right and 8 below a listener at (10, 20); over a reference distance of 4
     * that is 1 right and 2 down, so 2 *below* in the mixer's terms.
     */
    f32x3 side = _nya_audio_world_to_audio((f32x2){ 14.0F, 28.0F });
    nya_assert(side[0] == 1.0F, "side on: x is unchanged, got %f", (f64)side[0]);
    nya_assert(side[1] == -2.0F, "side on: world y must be negated into height, got %f", (f64)side[1]);
    nya_assert(side[2] == 0.0F, "side on: nothing may reach z, got %f", (f64)side[2]);

    // Top down: the same point, but now the screen is the ground, so that 2 is depth behind the
    // listener rather than height below them, and nothing is ever overhead.
    nya_audio_listener_set((NYA_AudioListener){ .position = { 10.0F, 20.0F }, .reference_distance = 4.0F, .plane = NYA_AUDIO_PLANE_TOP_DOWN });

    f32x3 top = _nya_audio_world_to_audio((f32x2){ 14.0F, 28.0F });
    nya_assert(top[0] == 1.0F, "top down: x is unchanged, got %f", (f64)top[0]);
    nya_assert(top[1] == 0.0F, "top down: nothing may reach the mixer's y, got %f", (f64)top[1]);
    nya_assert(top[2] == 2.0F, "top down: world y must become depth, got %f", (f64)top[2]);

    /*
     * The reference distance is a divisor, which is the whole reason it sets the scale of the world.
     *
     * Doubling it halves every distance, so a sound that was at the edge of the falloff is now well
     * inside it. A version that added or multiplied instead would pass every assertion above.
     */
    nya_audio_listener_set((NYA_AudioListener){ .position = { 10.0F, 20.0F }, .reference_distance = 8.0F, .plane = NYA_AUDIO_PLANE_SIDE });

    f32x3 farther = _nya_audio_world_to_audio((f32x2){ 14.0F, 28.0F });
    nya_assert(farther[0] == 0.5F && farther[1] == -1.0F, "doubling the reference distance must halve the offset, got (%f, %f)", (f64)farther[0], (f64)farther[1]);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: bus filters — off is exact, on removes treble, and it glides there
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The DSP directly, on a buffer of known signal. Device independent, and the only way to assert
    // on what the filter does to audio — the mixer has no way to hand a test its output.
    SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = 1, .freq = FILTER_RATE };

    static f32 pcm[FILTER_FRAMES];
    static f32 original[FILTER_FRAMES];

    NYA_AudioFilterState filter;

    // ── No filter must be bit exact, not merely close ──
    {
      _nya_audio_filter_reset(&filter);

      fill_sine(pcm, FILTER_FRAMES, 5000.0F);
      for (s32 i = 0; i < FILTER_FRAMES; i++) original[i] = pcm[i];

      _nya_audio_filter_apply(&filter, &spec, pcm, FILTER_FRAMES);

      // A wide open one pole would come back nearly the same, which is not the same thing: an
      // unfiltered bus has to be untouched, or every game pays for a filter it never asked for.
      for (s32 i = 0; i < FILTER_FRAMES; i++) {
        nya_assert(pcm[i] == original[i], "an unfiltered bus must pass samples through untouched: sample %d became %f from %f", i, (f64)pcm[i], (f64)original[i]);
      }
    }

    // ── A low cutoff must crush treble and let bass through ──
    {
      /*
       * A one pole at 500Hz passes about 98% of a 100Hz tone and about 6.5% of an 8kHz one. The
       * thresholds are loose around those so the test is about the shape of the response rather
       * than its third decimal, but they are nowhere near each other — a filter that did nothing
       * would fail the treble check by a factor of ten.
       */
      f64 bass   = filter_response(500.0F, 100.0F);
      f64 treble = filter_response(500.0F, 8000.0F);

      nya_assert(bass > 0.7, "a 500Hz cutoff must pass a 100Hz tone, got %f of it", bass);
      nya_assert(treble < 0.15, "a 500Hz cutoff must crush an 8kHz tone, got %f of it", treble);
      nya_assert(treble < bass, "the filter must be a low pass, not a high pass: %f treble against %f bass", treble, bass);
    }

    // ── Glide must rate limit the coefficient rather than snapping ──
    {
      _nya_audio_filter_reset(&filter);
      atomic_store_explicit(&filter.target_hz, 500.0F, memory_order_relaxed);

      // No glide: one buffer arrives at the target exactly.
      atomic_store_explicit(&filter.glide_ms, 0.0F, memory_order_relaxed);
      fill_sine(pcm, FILTER_FRAMES, 1000.0F);
      _nya_audio_filter_apply(&filter, &spec, pcm, FILTER_FRAMES);

      f32 snapped = filter.coefficient;
      nya_assert(snapped < 0.1F, "an unglided filter must reach its target in one buffer, coefficient %f", (f64)snapped);

      /*
       * A one second glide against a 100ms buffer: the coefficient may cross at most a tenth of its
       * range, so it starts at 1.0 and lands near 0.9 rather than at the target. This is what stops
       * the response jumping between buffers, which is audible as a click.
       */
      _nya_audio_filter_reset(&filter);
      atomic_store_explicit(&filter.target_hz, 500.0F, memory_order_relaxed);
      atomic_store_explicit(&filter.glide_ms, 1000.0F, memory_order_relaxed);

      fill_sine(pcm, FILTER_FRAMES, 1000.0F);
      _nya_audio_filter_apply(&filter, &spec, pcm, FILTER_FRAMES);

      nya_assert(
          filter.coefficient > 0.85F && filter.coefficient < 0.95F,
          "a 1s glide must move a tenth of the way over a 100ms buffer, coefficient %f",
          (f64)filter.coefficient
      );
      nya_assert(filter.coefficient > snapped, "a glided filter must lag an unglided one");
    }

    // ── More channels than there is state for must pass through, not filter half of them ──
    {
      _nya_audio_filter_reset(&filter);
      atomic_store_explicit(&filter.target_hz, 500.0F, memory_order_relaxed);

      SDL_AudioSpec wide = { .format = SDL_AUDIO_F32, .channels = NYA_AUDIO_FILTER_MAX_CHANNELS + 1, .freq = FILTER_RATE };

      fill_sine(pcm, FILTER_FRAMES, 5000.0F);
      for (s32 i = 0; i < FILTER_FRAMES; i++) original[i] = pcm[i];

      _nya_audio_filter_apply(&filter, &wide, pcm, FILTER_FRAMES);

      for (s32 i = 0; i < FILTER_FRAMES; i++) {
        nya_assert(pcm[i] == original[i], "a device with too many channels must go unfiltered: sample %d changed", i);
      }
    }

    // ── The public setter, which is the only part of this a game touches ──
    {
      nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .lowpass_hz = 700.0F, .glide_ms = 120.0F });

      NYA_AudioFilter got = nya_audio_bus_filter_get(NYA_AUDIO_BUS_SOUND);
      nya_assert(got.lowpass_hz == 700.0F && got.glide_ms == 120.0F, "a bus filter must read back as it was set, got %f / %f", (f64)got.lowpass_hz, (f64)got.glide_ms);

      // Each bus is its own, or "muffle the world" would take the music with it.
      NYA_AudioFilter music = nya_audio_bus_filter_get(NYA_AUDIO_BUS_MUSIC);
      nya_assert(music.lowpass_hz == 0.0F, "filtering one bus must leave the others alone, music got %f", (f64)music.lowpass_hz);

      // Negative is clamped rather than refused, since it means the same thing as off.
      nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .lowpass_hz = -5.0F, .glide_ms = -1.0F });

      got = nya_audio_bus_filter_get(NYA_AUDIO_BUS_SOUND);
      nya_assert(got.lowpass_hz == 0.0F && got.glide_ms == 0.0F, "a negative filter must clamp to off, got %f / %f", (f64)got.lowpass_hz, (f64)got.glide_ms);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: playing a real clip, and steering it while it runs
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_SOUND,
      .handle   = TEST_WAV_PATH,
      // External: the file was written just now and is not in the build's asset index, so there is
      // nothing in the blob to resolve it against.
      .external = true,
      .as_sound = { .predecode = true },
    }), "while queueing the test tone");

    // Assets resolve on an event, so the queue has to be pumped before the sound exists.
    end_frame();

    NYA_Asset* asset = nya_asset_get(TEST_WAV_PATH);
    nya_assert(asset != nullptr, "the test tone was never queued");

    /*
     * Everything below is guarded on there being a device.
     *
     * Without a mixer the asset fails to decode and every play returns the null voice, which is the
     * documented behaviour rather than a failure — so the test asserts that shape instead of
     * skipping, and only checks the playing behaviour where there is something to play it.
     */
    NYA_SoundVoice voice = nya_audio_play_sound(TEST_WAV_PATH, 1.0F);

    if (voice.generation == 0) {
      nya_log_info("no audio device, so playback was not exercised (this is expected in CI)");
      nya_assert(!nya_audio_voice_valid(voice), "a null voice from a failed play must not be valid");
    } else {
      nya_assert(nya_audio_voice_valid(voice), "a voice that just started must be valid");

      // Effects on a live voice. None of these can be observed without capturing the output, so what
      // is asserted is that they neither crash nor invalidate the voice.
      nya_audio_voice_set_gain(voice, 0.5F);
      nya_audio_voice_set_pitch(voice, 1.5F);
      nya_audio_voice_set_pan(voice, -0.5F);
      nya_assert(nya_audio_voice_valid(voice), "an effect must not stop the sound");

      // Position overrides pan, and is the other half of the same decision.
      nya_audio_voice_set_position(voice, (f32x3){ 2.0F, 0.0F, -1.0F });
      nya_assert(nya_audio_voice_valid(voice));

      // A pitch of zero would park the playhead rather than silence it, so it is refused outright.
      nya_audio_voice_set_pitch(voice, 0.0F);
      nya_assert(nya_audio_voice_valid(voice), "a refused pitch must leave the voice alone");

      // ── The generational property, which is the point of the handle ──
      nya_audio_voice_stop(voice, 0);

      NYA_SoundVoice second = nya_audio_play_sound(TEST_WAV_PATH, 1.0F);
      if (second.generation != 0) {
        // Two live voices must never collide, whether they landed in the same slot or not.
        nya_assert(
            second.index != voice.index || second.generation != voice.generation,
            "a reused slot must not hand back the handle it had before"
        );

        // The decisive case: if the new sound took the old slot, the old handle must have stopped
        // resolving. That is what stops a stale handle retuning somebody else's sound.
        if (second.index == voice.index) nya_assert(!nya_audio_voice_valid(voice), "a stale handle must not resolve after its slot was reused");

        nya_audio_voice_stop(second, 0);
      }

      // The varied form is the plain one with a detune, so it has to acquire a voice the same way.
      // What the detune actually was is not observable here; that is what the block above covers.
      NYA_SoundVoice varied = nya_audio_play_sound_varied(TEST_WAV_PATH, 1.0F);
      if (varied.generation != 0) {
        nya_assert(nya_audio_voice_valid(varied), "a varied sound must yield a live voice like any other");
        nya_audio_voice_stop(varied, 0);
      }

      // ── Placement, which unlike pan can be read back off the track ──
      nya_audio_listener_set((NYA_AudioListener){ .position = { 0.0F, 0.0F }, .reference_distance = 1.0F, .plane = NYA_AUDIO_PLANE_SIDE });

      NYA_SoundVoice placed = nya_audio_play_sound_at(TEST_WAV_PATH, (f32x2){ 3.0F, -4.0F }, (NYA_SoundParams){ .gain = 1.0F });
      if (placed.generation != 0) {
        MIX_Point3D point = { 0 };
        nya_assert(MIX_GetTrack3DPosition(_nya_audio_system.slots[placed.index].track, &point), "MIX_GetTrack3DPosition failed: %s", SDL_GetError());

        /*
         * The decisive assertion, and the one that catches placing the sound *after* MIX_PlayTrack
         * rather than before: a voice's generation is only bumped once it is running, so a handle
         * built inside the play path names the previous sound and every setter given it silently
         * does nothing. Four units up, because the renderer's y counts down.
         */
        nya_assert(
            point.x == 3.0F && point.y == 4.0F && point.z == 0.0F,
            "a sound played at (3, -4) must be placed at (3, 4, 0), got (%f, %f, %f)",
            (f64)point.x,
            (f64)point.y,
            (f64)point.z
        );

        // And a live voice can be moved, which is what an emitter that travels needs.
        nya_audio_voice_set_world_position(placed, (f32x2){ -1.0F, 0.0F });
        nya_assert(MIX_GetTrack3DPosition(_nya_audio_system.slots[placed.index].track, &point), "MIX_GetTrack3DPosition failed: %s", SDL_GetError());
        nya_assert(point.x == -1.0F && point.y == 0.0F, "a moved voice must follow, got (%f, %f)", (f64)point.x, (f64)point.y);

        nya_audio_voice_stop(placed, 0);
      }
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: music is a voice, so the same effects reach it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Nothing playing yet, so the music voice is not valid — but asking is safe.
    nya_audio_stop_music(0);
    nya_assert(!nya_audio_voice_valid(nya_audio_music_voice()), "silent music is not a live voice");

    nya_audio_play_music(TEST_WAV_PATH, true, 0);

    NYA_SoundVoice music = nya_audio_music_voice();
    if (music.generation != 0 && nya_audio_voice_valid(music)) {
      nya_assert(nya_audio_music_playing(), "music that started must report as playing");

      // The whole reason music is a voice rather than a special case: one set of effect functions.
      nya_audio_voice_set_pitch(music, 0.8F);
      nya_audio_voice_set_pan(music, 0.25F);
      nya_assert(nya_audio_voice_valid(music));

      nya_audio_pause_music();
      nya_assert(!nya_audio_music_playing(), "paused music is not playing");

      nya_audio_resume_music();
      nya_audio_stop_music(0);
    }
  }

  printf("PASSED: test_audio\n");
  return 0;
}
