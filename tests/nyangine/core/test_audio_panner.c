/**
 * The stereo panner: the panning law, the interaural delay and the head shadow, and the DSP that lays them
 * onto a buffer. All pure, so none of it needs an audio device.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define PAN_RATE 48000.0F

/** Root mean square of the back half of one channel, past the filter's start-up transient. */
static f64 channel_tail_rms(const f32* pcm, s32 frames, s32 channels, s32 channel) {
  s32 start = frames / 2;
  s32 count = frames - start;

  f64 sum = 0.0;
  for (s32 i = start; i < frames; i++) {
    f64 s = (f64)pcm[(i * channels) + channel];
    sum += s * s;
  }

  return sqrt(sum / (f64)count);
}

/** Fills an interleaved stereo buffer with the same sine in both channels. */
static void fill_stereo_sine(f32* pcm, s32 frames, f32 hz) {
  for (s32 i = 0; i < frames; i++) {
    f32 s              = (f32)sin(2.0 * M_PI * (f64)hz * (f64)i / (f64)PAN_RATE);
    pcm[(i * 2) + 0] = s;
    pcm[(i * 2) + 1] = s;
  }
}

s32 main(void) {
  // TEST: the azimuth of a listener-relative direction
  {
    // +x right, -z ahead. Pure arithmetic, so exact enough to compare against known angles.
    nya_assert(fabsf(nya_audio_pan_azimuth((f32x3){ 0.0F, 0.0F, -1.0F })) < 1e-5F, "a source dead ahead is azimuth zero");
    nya_assert(fabsf(nya_audio_pan_azimuth((f32x3){ 1.0F, 0.0F, 0.0F }) - (0.5F * (f32)M_PI)) < 1e-5F, "a source to the right is +pi/2");
    nya_assert(fabsf(nya_audio_pan_azimuth((f32x3){ -1.0F, 0.0F, 0.0F }) + (0.5F * (f32)M_PI)) < 1e-5F, "a source to the left is -pi/2");
    nya_assert(fabsf(fabsf(nya_audio_pan_azimuth((f32x3){ 0.0F, 0.0F, 1.0F })) - (f32)M_PI) < 1e-5F, "a source behind is +/-pi");

    // Height plays no part, and a source on the listener has no direction and is called ahead.
    nya_assert(fabsf(nya_audio_pan_azimuth((f32x3){ 0.0F, 9.0F, -1.0F })) < 1e-5F, "height must not tilt the azimuth");
    nya_assert(nya_audio_pan_azimuth((f32x3){ 0.0F, 0.0F, 0.0F }) == 0.0F, "a source on the listener is ahead, not a NaN");
  }

  // TEST: the panning law is centred and equal power
  {
    NYA_StereoPan centre = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = 0.0F });

    nya_assert(fabsf(centre.left_gain - centre.right_gain) < 1e-6F, "a centred source must be equal in both ears, got %f / %f", (f64)centre.left_gain, (f64)centre.right_gain);
    nya_assert(centre.left_delay_s == 0.0F && centre.right_delay_s == 0.0F, "a centred source has no interaural delay");
    nya_assert(centre.left_lowpass_hz == 0.0F && centre.right_lowpass_hz == 0.0F, "a centred source has no shadow, both ears open");

    // Equal power across the front: the two gains square-sum to one, so a pan does not change loudness.
    for (s32 i = -4; i <= 4; i++) {
      f32           az  = (f32)i / 4.0F * (0.5F * (f32)M_PI);
      NYA_StereoPan pan = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = az });
      f32           power = (pan.left_gain * pan.left_gain) + (pan.right_gain * pan.right_gain);

      nya_assert(fabsf(power - 1.0F) < 1e-5F, "the pan law must hold constant power, az %f gave %f", (f64)az, (f64)power);
    }
  }

  // TEST: hard left louder in the left ear, delayed and shadowed on the far (right) ear
  {
    NYA_StereoPan left = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = -0.5F * (f32)M_PI });

    // the near ear is louder.
    nya_assert(left.left_gain > left.right_gain, "hard left must be louder in the left ear, got %f / %f", (f64)left.left_gain, (f64)left.right_gain);

    // the far ear is the delayed one: the sound reaches the right ear after the left. Sign matters, so the near ear must be exactly zero and the far ear positive.
    nya_assert(left.left_delay_s == 0.0F, "the near (left) ear must lead, delay zero, got %f", (f64)left.left_delay_s);
    nya_assert(left.right_delay_s > 0.0F, "the far (right) ear must lag, delay positive, got %f", (f64)left.right_delay_s);

    // hard to the side is Woodworth's peak, r/c·(π/2 + 1), which for an adult head is about 0.66 ms.
    nya_assert(fabsf(left.right_delay_s - NYA_AUDIO_PAN_MAX_ITD_S) < 1e-7F, "hard left must be the full interaural delay, got %f s", (f64)left.right_delay_s);
    nya_assert(NYA_AUDIO_PAN_MAX_ITD_S > 0.00064F && NYA_AUDIO_PAN_MAX_ITD_S < 0.00067F, "the largest interaural delay is out of the human range, got %f s", (f64)NYA_AUDIO_PAN_MAX_ITD_S);

    // the head shadow rolls off the far ear alone.
    nya_assert(left.left_lowpass_hz == 0.0F, "the near (left) ear must stay open");
    nya_assert(left.right_lowpass_hz > 0.0F, "the far (right) ear must be shadowed, got %f Hz", (f64)left.right_lowpass_hz);
  }

  // TEST: hard right is the mirror, and the delay sign flips with it
  {
    NYA_StereoPan right = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = 0.5F * (f32)M_PI });

    nya_assert(right.right_gain > right.left_gain, "hard right must be louder in the right ear");

    // now the left ear is the far one, so the delay is on the left. This is the sign check.
    nya_assert(right.right_delay_s == 0.0F, "the near (right) ear must lead");
    nya_assert(right.left_delay_s > 0.0F, "the far (left) ear must lag when the source is on the right");

    nya_assert(right.right_lowpass_hz == 0.0F, "the near (right) ear must stay open");
    nya_assert(right.left_lowpass_hz > 0.0F, "the far (left) ear must be shadowed");
  }

  // TEST: the delay and the shadow are bounded all the way round the head
  {
    f32 previous_delay = 0.0F;
    for (s32 i = 0; i <= 360; i++) {
      f32           az  = ((f32)i - 180.0F) * (f32)M_PI / 180.0F;
      NYA_StereoPan pan = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = az });

      // only ever one ear lags, and never past the largest delay a head can make.
      nya_assert(pan.left_delay_s == 0.0F || pan.right_delay_s == 0.0F, "at most one ear may lag, az %f", (f64)az);
      nya_assert(pan.left_delay_s <= NYA_AUDIO_PAN_MAX_ITD_S && pan.right_delay_s <= NYA_AUDIO_PAN_MAX_ITD_S, "the delay must stay within the max ITD, az %f", (f64)az);

      // across the front from hard left to centre the far ear's delay only shrinks, and its cutoff only rises.
      if (az >= -0.5F * (f32)M_PI && az <= 0.0F) {
        if (i > 90) nya_assert(pan.right_delay_s <= previous_delay + 1e-9F, "the delay must fall toward the centre, az %f", (f64)az);
        previous_delay = pan.right_delay_s;
      }
    }

    // the delay line holds the longest delay at 96 kHz, one frame spare for the fractional read.
    nya_assert(NYA_AUDIO_PAN_MAX_ITD_S * 96000.0F < (f32)(NYA_AUDIO_PAN_MAX_DELAY_FRAMES - 1), "the delay line is too short for 96 kHz");
  }

  // TEST: behind is centred but duller than in front
  {
    NYA_StereoPan front  = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = 0.0F });
    NYA_StereoPan behind = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = (f32)M_PI });

    // directly behind is on the median plane, so it is still level and undelayed between the ears.
    nya_assert(fabsf(behind.left_gain - behind.right_gain) < 1e-5F, "a source directly behind is centred between the ears");
    nya_assert(fabsf(behind.left_delay_s - behind.right_delay_s) < 1e-6F, "a source directly behind has no interaural delay");

    // but the rear shadow rolls both ears off, where the front source was open. This is the front/back cue.
    nya_assert(front.left_lowpass_hz == 0.0F, "a source in front is open");
    nya_assert(behind.left_lowpass_hz > 0.0F && behind.right_lowpass_hz > 0.0F, "a source behind must be rolled off in both ears, got %f / %f", (f64)behind.left_lowpass_hz, (f64)behind.right_lowpass_hz);
  }

  // TEST: the render lays the gains onto a buffer
  {
    const s32   frames = 512;
    static f32  pcm[512 * 2];

    // a synthetic pan: louder left, no delay, no shadow, so only the gain is exercised.
    NYA_StereoPan pan = { .left_gain = 1.0F, .right_gain = 0.25F };

    NYA_AudioPanRender render;
    nya_audio_pan_render_reset(&render);

    fill_stereo_sine(pcm, frames, 1000.0F);
    nya_audio_pan_render(&render, PAN_RATE, 2, pan, pcm, frames * 2);

    f64 left  = channel_tail_rms(pcm, frames, 2, 0);
    f64 right = channel_tail_rms(pcm, frames, 2, 1);

    nya_assert(left > right, "the louder ear must come back louder, got %f / %f", left, right);

    // the ratio must be the gain ratio, since the first buffer snaps rather than eases.
    nya_assert(fabs((right / left) - 0.25) < 0.01, "the ear balance must be the gain ratio, got %f", right / left);
  }

  // TEST: the render delays the far ear by the interaural time difference
  {
    const s32  frames = 64;
    static f32 pcm[64 * 2];

    // an impulse in both channels, and a pan that delays the right ear by eight frames and nothing else.
    for (s32 i = 0; i < frames * 2; i++) pcm[i] = 0.0F;
    pcm[0] = 1.0F;   // left, frame 0
    pcm[1] = 1.0F;   // right, frame 0

    NYA_StereoPan pan = { .left_gain = 1.0F, .right_gain = 1.0F, .right_delay_s = 8.0F / PAN_RATE };

    NYA_AudioPanRender render;
    nya_audio_pan_render_reset(&render);

    nya_audio_pan_render(&render, PAN_RATE, 2, pan, pcm, frames * 2);

    // the near ear's impulse stays at frame 0; the far ear's has moved eight frames on. Open shadow and unit gain make this all but exact.
    nya_assert(pcm[0] == 1.0F, "the near (left) ear must be undelayed, got %f", (f64)pcm[0]);
    nya_assert(pcm[1] == 0.0F, "the far (right) ear must be silent before its delayed impulse, got %f", (f64)pcm[1]);
    nya_assert(fabsf(pcm[(8 * 2) + 1] - 1.0F) < 1e-4F, "the far (right) ear's impulse must land eight frames late, got %f", (f64)pcm[(8 * 2) + 1]);
  }

  // TEST: the delay is fractional, splitting an impulse between the frames either side
  {
    const s32  frames = 16;
    static f32 pcm[16 * 2];

    for (s32 i = 0; i < frames * 2; i++) pcm[i] = 0.0F;
    pcm[0] = 1.0F;
    pcm[1] = 1.0F;

    NYA_StereoPan pan = { .left_gain = 1.0F, .right_gain = 1.0F, .right_delay_s = 3.25F / PAN_RATE };

    NYA_AudioPanRender render;
    nya_audio_pan_render_reset(&render);
    nya_audio_pan_render(&render, PAN_RATE, 2, pan, pcm, frames * 2);

    // three and a quarter frames: three quarters of the impulse at frame 3, a quarter at frame 4, nothing else.
    nya_assert(fabsf(pcm[(3 * 2) + 1] - 0.75F) < 1e-3F, "frame 3 must carry three quarters, got %f", (f64)pcm[(3 * 2) + 1]);
    nya_assert(fabsf(pcm[(4 * 2) + 1] - 0.25F) < 1e-3F, "frame 4 must carry a quarter, got %f", (f64)pcm[(4 * 2) + 1]);
    nya_assert(pcm[(2 * 2) + 1] == 0.0F && pcm[(5 * 2) + 1] == 0.0F, "the impulse must not spread past the two frames");
  }

  // TEST: hard left through the whole path, and centred is symmetric
  {
    const s32  frames = 4800;
    static f32 pcm[4800 * 2];

    // hard left, a bass and a treble tone: the right ear is the far one, so it is quieter across the band by
    // the side level difference, and quieter again in the treble, where the head shadows it.
    f64 ratio[2];
    f32 tone_hz[2] = { 200.0F, 6000.0F };
    for (s32 t = 0; t < 2; t++) {
      NYA_AudioPanRender render;
      nya_audio_pan_render_reset(&render);

      fill_stereo_sine(pcm, frames, tone_hz[t]);
      nya_audio_pan_render(&render, PAN_RATE, 2, nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = -0.5F * (f32)M_PI }), pcm, frames * 2);

      ratio[t] = channel_tail_rms(pcm, frames, 2, 1) / channel_tail_rms(pcm, frames, 2, 0);
    }

    f64 side_ratio = pow(10.0, -(f64)NYA_AUDIO_PAN_SIDE_LEVEL_DB / 20.0);
    nya_assert(fabs(ratio[0] - side_ratio) < 0.02, "bass hard left must be the side level difference down in the far ear, got %f", ratio[0]);
    nya_assert(ratio[1] < 0.5 * ratio[0], "treble hard left must be shadowed in the far ear, got %f against bass %f", ratio[1], ratio[0]);
    nya_assert(ratio[1] > 0.0, "the far ear must still hear the source, or its delay says nothing");

    // centred: the same signal in both ears, sample for sample.
    {
      NYA_AudioPanRender render;
      nya_audio_pan_render_reset(&render);

      fill_stereo_sine(pcm, frames, 6000.0F);
      nya_audio_pan_render(&render, PAN_RATE, 2, nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = 0.0F }), pcm, frames * 2);

      for (s32 i = 0; i < frames; i++) nya_assert(pcm[i * 2] == pcm[(i * 2) + 1], "a centred source must be the same in both ears, frame %d", i);
    }
  }

  // TEST: a stereo clip is folded to one point source
  {
    const s32  frames = 64;
    static f32 pcm[64 * 2];

    // left channel only: a centred point source hears half of it in each ear, at the centre's equal power gain.
    for (s32 i = 0; i < frames; i++) {
      pcm[i * 2]       = 1.0F;
      pcm[(i * 2) + 1] = 0.0F;
    }

    NYA_AudioPanRender render;
    nya_audio_pan_render_reset(&render);
    nya_audio_pan_render(&render, PAN_RATE, 2, nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = 0.0F }), pcm, frames * 2);

    f32 expected = 0.5F * cosf(0.25F * (f32)M_PI);
    nya_assert(fabsf(pcm[0] - expected) < 1e-6F && fabsf(pcm[1] - expected) < 1e-6F, "a one-sided clip must fold to the centre, got %f / %f", (f64)pcm[0], (f64)pcm[1]);
  }

  // TEST: the render shadows the far ear's treble and leaves the near ear alone
  {
    const s32  frames = 4800;
    static f32 pcm[4800 * 2];

    // equal gain, no delay, a low pass on the right ear only.
    NYA_StereoPan pan = { .left_gain = 1.0F, .right_gain = 1.0F, .right_lowpass_hz = 1500.0F };

    // a treble tone: the shadowed ear must lose most of it.
    {
      NYA_AudioPanRender render;
      nya_audio_pan_render_reset(&render);

      fill_stereo_sine(pcm, frames, 8000.0F);
      nya_audio_pan_render(&render, PAN_RATE, 2, pan, pcm, frames * 2);

      f64 open    = channel_tail_rms(pcm, frames, 2, 0);
      f64 shadowed = channel_tail_rms(pcm, frames, 2, 1);

      nya_assert(shadowed < 0.5 * open, "the shadowed ear must lose most of an 8kHz tone, got %f against %f", shadowed, open);
    }

    // a bass tone: it passes both ears, so the shadow is a low pass and not a level drop.
    {
      NYA_AudioPanRender render;
      nya_audio_pan_render_reset(&render);

      fill_stereo_sine(pcm, frames, 200.0F);
      nya_audio_pan_render(&render, PAN_RATE, 2, pan, pcm, frames * 2);

      f64 open    = channel_tail_rms(pcm, frames, 2, 0);
      f64 shadowed = channel_tail_rms(pcm, frames, 2, 1);

      nya_assert(shadowed > 0.9 * open, "bass must pass the shadowed ear nearly untouched, got %f against %f", shadowed, open);
    }
  }

  // TEST: the render leaves a non-stereo buffer untouched
  {
    const s32  frames = 128;
    static f32 pcm[128];
    static f32 original[128];

    for (s32 i = 0; i < frames; i++) pcm[i] = original[i] = (f32)sin(0.1 * (f64)i);

    NYA_StereoPan pan = { .left_gain = 0.5F, .right_gain = 0.5F };

    NYA_AudioPanRender render;
    nya_audio_pan_render_reset(&render);

    // mono: the two-ear model has nothing to say, so the buffer must pass through untouched.
    nya_audio_pan_render(&render, PAN_RATE, 1, pan, pcm, frames);

    for (s32 i = 0; i < frames; i++) nya_assert(pcm[i] == original[i], "a mono buffer must pass through the panner untouched, sample %d changed", i);
  }

  printf("PASSED: test_audio_panner\n");
  return 0;
}
