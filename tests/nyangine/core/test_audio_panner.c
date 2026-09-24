/**
 * The stereo panner: the panning law, the interaural delay and the head shadow, and the DSP that lays them
 * onto a buffer. All pure, so none of it needs an audio device.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

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

    // a plausible interaural delay: a head's width over the speed of sound is well under a millisecond.
    nya_assert(left.right_delay_s > 0.0003F && left.right_delay_s < 0.0009F, "the interaural delay is out of the human range, got %f s", (f64)left.right_delay_s);

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

    // the near ear's impulse stays at frame 0; the far ear's has moved eight frames on. Open shadow and unit gain make this exact.
    nya_assert(pcm[0] == 1.0F, "the near (left) ear must be undelayed, got %f", (f64)pcm[0]);
    nya_assert(pcm[1] == 0.0F, "the far (right) ear must be silent before its delayed impulse, got %f", (f64)pcm[1]);
    nya_assert(pcm[(8 * 2) + 1] == 1.0F, "the far (right) ear's impulse must land eight frames late, got %f", (f64)pcm[(8 * 2) + 1]);
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
