/**
 * @file core_audio_panner.h
 *
 * A stereo panner: it places one sound in the stereo field from where it sits relative to the listener,
 * the way two ears actually tell direction apart. Three cues, from an azimuth alone:
 *
 *   - a level difference, the near ear louder, on an equal power law;
 *   - an interaural time delay, the sound reaching the far ear a fraction of a millisecond late;
 *   - a head shadow, the far ear losing treble because the head is in the way, and both ears losing some
 *     behind the listener, where the outer ear no longer faces the source.
 *
 * The math is pure. nya_audio_pan_compute turns an azimuth into per-ear gains, delays and cutoffs with no
 * state and no globals, so it is the same answer every run; nya_audio_pan_render is the DSP that lays those
 * onto a stereo buffer, holding only a small bounded delay line and a one pole per ear. Neither touches the
 * mixer. core_audio runs them on every positioned voice when the device is a stereo pair, in place of
 * SDL_mixer's mono 3D path, which has a level difference and nothing else.
 *
 * ```c
 * NYA_StereoPan pan = nya_audio_pan_compute((NYA_StereoPanParams){ .azimuth_radians = nya_audio_pan_azimuth(dir) });
 * nya_audio_pan_render(&state, 48000.0F, 2, pan, pcm, samples);
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Radius of the average human head, metres. Sets both the interaural delay and where the shadow starts. */
#ifndef NYA_AUDIO_PAN_HEAD_RADIUS_M
#define NYA_AUDIO_PAN_HEAD_RADIUS_M 0.0875F
#endif

/** Speed of sound in air, metres per second. */
#ifndef NYA_AUDIO_PAN_SPEED_OF_SOUND_MPS
#define NYA_AUDIO_PAN_SPEED_OF_SOUND_MPS 343.0F
#endif

/** Where the far ear is rolled off when a source is hard to one side, hertz. */
#ifndef NYA_AUDIO_PAN_SHADOW_HZ
#define NYA_AUDIO_PAN_SHADOW_HZ 1800.0F
#endif

/**
 * How much quieter the far ear is across the band when a source is hard to one side, decibels. Measured
 * interaural level differences at 90° run from about 6 dB at 500 Hz to 20 dB at 6 kHz (Feddersen 1957); the
 * shadow's one pole at NYA_AUDIO_PAN_SHADOW_HZ supplies the treble part, so the flat gain carries the low
 * end. Anything finite keeps the far ear audible, which the interaural delay needs to be heard at all.
 * */
#ifndef NYA_AUDIO_PAN_SIDE_LEVEL_DB
#define NYA_AUDIO_PAN_SIDE_LEVEL_DB 6.0F
#endif

/** Where both ears are rolled off when a source is directly behind, hertz. Milder than the side shadow. */
#ifndef NYA_AUDIO_PAN_REAR_SHADOW_HZ
#define NYA_AUDIO_PAN_REAR_SHADOW_HZ 6500.0F
#endif

/**
 * The longest interaural delay, seconds: Woodworth's r/c·(θ + sin θ) at θ = π/2, a source level with one
 * ear. 0.0875 / 343 · (π/2 + 1) = 0.656 ms, against about 0.65 ms measured on adult heads.
 * */
#define NYA_AUDIO_PAN_MAX_ITD_S ((NYA_AUDIO_PAN_HEAD_RADIUS_M / NYA_AUDIO_PAN_SPEED_OF_SOUND_MPS) * ((0.5F * (f32)M_PI) + 1.0F))

/**
 * Frames the interaural delay line holds. The longest delay is 31.5 frames at 48 kHz and 63 at 96 kHz; a
 * power of two past that wraps with a mask, and anything longer, a bigger head or a faster device, clamps.
 * */
#define NYA_AUDIO_PAN_MAX_DELAY_FRAMES 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_StereoPanParams NYA_StereoPanParams;
typedef struct NYA_StereoPan       NYA_StereoPan;
typedef struct NYA_AudioPanRender  NYA_AudioPanRender;

/**
 * Where a sound is, as one angle in the listener's own frame. Zero fields take the constants above.
 * */
struct NYA_StereoPanParams {
    /** 0 dead ahead, +π/2 hard right, -π/2 hard left, ±π directly behind. Wrapped into that range. */
    f32 azimuth_radians;

    /** Head radius, metres. Zero takes NYA_AUDIO_PAN_HEAD_RADIUS_M. */
    f32 head_radius_m;

    /** Speed of sound, m/s. Zero takes NYA_AUDIO_PAN_SPEED_OF_SOUND_MPS. */
    f32 speed_of_sound_mps;

    /** Far-ear cutoff at hard side, hertz. Zero takes NYA_AUDIO_PAN_SHADOW_HZ. */
    f32 shadow_hz;
};

/**
 * The per-ear placement of one sound. Symmetric, so the plumbing needs no notion of which ear is which:
 * the near ear simply comes back louder, undelayed and open, and the far ear quieter, delayed and rolled off.
 * */
struct NYA_StereoPan {
    /* Linear, equal power: at the centre both are 1/√2, so a centred source keeps a mono one's power. */
    f32 left_gain;
    f32 right_gain;

    /* Seconds each ear lags the nearer one. The leading ear is zero; only the far ear is delayed. */
    f32 left_delay_s;
    f32 right_delay_s;

    /* Where each ear is rolled off, hertz. Zero is open, no filtering. */
    f32 left_lowpass_hz;
    f32 right_lowpass_hz;
};

/**
 * The DSP's memory for one voice: a bounded delay line and a one pole per ear, plus the smoothed values it
 * eases toward so a moving source does not zipper. All of it the mixer thread's alone once playing; reset it
 * on the calling thread before the sound starts.
 * */
struct NYA_AudioPanRender {
    /** The source folded to mono, since a point source has one signal; each ear reads it at its own delay. */
    f32 ring[NYA_AUDIO_PAN_MAX_DELAY_FRAMES];
    u32 write_index;

    /** The one pole's previous output per ear, all it remembers. */
    f32 shadow_state[2];

    /* Eased across a buffer toward the target, so a turn of the head is a glide rather than a step. */
    f32 gain[2];
    f32 coefficient[2];
    f32 delay_frames[2];

    /** False until the first buffer, which snaps to the target rather than easing up from silence. */
    b8 primed;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The panning law, delay and shadow for one azimuth. Pure: same params, same answer, no state.
 * */
NYA_API NYA_StereoPan nya_audio_pan_compute(NYA_StereoPanParams params) __attr_no_discard;

/**
 * The azimuth of a direction in the mixer's listener frame: +x right, +y up, -z ahead. Height is dropped;
 * a panner has only the horizontal angle to work with. A direction on the listener comes back zero, ahead.
 * */
NYA_API f32 nya_audio_pan_azimuth(f32x3 listener_relative) __attr_no_discard;

/** Clears the delay line and filter history and drops the smoothing, so the next buffer snaps to its target. */
NYA_API void nya_audio_pan_render_reset(NYA_AudioPanRender* render);

/**
 * Lays `target` onto an interleaved stereo buffer in place: folds it to mono, delays the far ear by a
 * fractional number of frames, rolls it off, and balances the two, easing from wherever the last buffer left
 * off. A buffer that is not two channels is left untouched, since the model is a two-ear one. No
 * allocation, no locks, bounded by the buffer: safe on the mixer thread.
 * */
NYA_API void nya_audio_pan_render(NYA_AudioPanRender* render, f32 sample_rate_hz, s32 channels, NYA_StereoPan target, f32* pcm, s32 samples);
