/**
 * @file core_audio_effects.h
 *
 * ```c
 * // The world dulls over a tenth of a second; music and UI are on another bus and stay crisp.
 * nya_audio_bus_effects_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioEffects){ .pass = { .lowpass_hz = 700.0F, .glide_ms = 120.0F } });
 * ```
 *
 * ```c
 * // A canyon: a slapback on the effects, the mix held together, and nothing past -1 dBFS.
 * nya_audio_bus_effects_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioEffects){
 *     .echo       = { .enabled = true, .delay_ms = 420.0F, .feedback = 0.3F },
 *     .compressor = { .enabled = true },
 * });
 * nya_audio_bus_effects_set(NYA_AUDIO_BUS_MASTER, (NYA_AudioEffects){ .limiter = { .enabled = true } });
 * ```
 *
 * Each bus runs one fixed chain, in this order: high and low pass, three band equaliser, compressor, echo, the
 * listener's reflections (sound bus only, from nya_audio_propagation_set), reverb, limiter. A bus nobody set allocates
 * nothing and its callback returns after one load. A unit that is off is skipped; one being switched off eases out
 * first. Settings reach the mixer lock free and every parameter is eased per NYA_AUDIO_EFFECTS_BLOCK frames, so moving
 * a knob every frame does not zipper. Delay lines are allocated on the calling thread the first time a unit needing
 * them is switched on, and kept until the audio system goes down.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_audio.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Frames between parameter steps. Short enough that a stepped filter does not zipper. */
#define NYA_AUDIO_EFFECTS_BLOCK 32

/** How long a parameter takes to reach a new setting, milliseconds, unless the unit says otherwise. */
#define NYA_AUDIO_EFFECTS_SMOOTHING_MS 20.0F

/** Speaker channels a chain processes. Wider layouts pass through untouched. */
#define NYA_AUDIO_EFFECTS_MAX_CHANNELS 8

/* The defaults a zero field takes. */

#define NYA_AUDIO_PASS_RESONANCE 0.7071F

#define NYA_AUDIO_EQUALIZER_LOW_HZ  200.0F
#define NYA_AUDIO_EQUALIZER_MID_HZ  1000.0F
#define NYA_AUDIO_EQUALIZER_HIGH_HZ 5000.0F
#define NYA_AUDIO_EQUALIZER_MID_Q   0.7F

#define NYA_AUDIO_COMPRESSOR_THRESHOLD_DB (-18.0F)
#define NYA_AUDIO_COMPRESSOR_RATIO        4.0F
#define NYA_AUDIO_COMPRESSOR_ATTACK_MS    10.0F
#define NYA_AUDIO_COMPRESSOR_RELEASE_MS   120.0F

#define NYA_AUDIO_LIMITER_CEILING_DB (-1.0F)
#define NYA_AUDIO_LIMITER_RELEASE_MS 80.0F

#define NYA_AUDIO_ECHO_DELAY_MS   320.0F
#define NYA_AUDIO_ECHO_FEEDBACK   0.35F
#define NYA_AUDIO_ECHO_WET        0.3F
#define NYA_AUDIO_ECHO_LOWPASS_HZ 3500.0F

/** Frames an echo line holds per channel, a power of two. 680 ms at 48 kHz; longer delays are clamped. */
#define NYA_AUDIO_ECHO_FRAMES 32768

/** Echoes of the listener's surroundings per bus. See NYA_AudioReflections. */
#define NYA_AUDIO_REFLECTION_TAPS 6

/** Frames the reflection line holds, a power of two. 340 ms at 48 kHz, a wall 58 m away. */
#define NYA_AUDIO_REFLECTION_FRAMES 16384

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_AudioPass          NYA_AudioPass;
typedef struct NYA_AudioEqualizer     NYA_AudioEqualizer;
typedef struct NYA_AudioCompressor    NYA_AudioCompressor;
typedef struct NYA_AudioEcho          NYA_AudioEcho;
typedef struct NYA_AudioReverb        NYA_AudioReverb;
typedef struct NYA_AudioLimiter       NYA_AudioLimiter;
typedef struct NYA_AudioEffects       NYA_AudioEffects;
typedef struct NYA_AudioReflectionTap NYA_AudioReflectionTap;
typedef struct NYA_AudioReflections   NYA_AudioReflections;
typedef struct NYA_AudioChain         NYA_AudioChain;

/**
 * Resonant twelve decibel per octave filters at either end of the band. Zero is off for each.
 * */
// @reflect
struct NYA_AudioPass {
    /** Treble above this is rolled off, hertz. */
    f32 lowpass_hz;

    /** Bass below this is rolled off, hertz. */
    f32 highpass_hz;

    /** Q of both. 0.707 is flat to the corner; higher rings. */
    f32 resonance;

    /** How long a new cutoff takes, milliseconds. Zero takes NYA_AUDIO_EFFECTS_SMOOTHING_MS. */
    f32 glide_ms;
};

/**
 * Low shelf, mid bell, high shelf. A zero gain leaves that band flat and costs nothing.
 * */
// @reflect
struct NYA_AudioEqualizer {
    f32 low_db;
    f32 mid_db;
    f32 high_db;

    /* Where each band sits, and the bell's width. Zero takes the NYA_AUDIO_EQUALIZER_* default. */

    f32 low_hz;
    f32 mid_hz;
    f32 high_hz;
    f32 mid_q;
};

/**
 * Turns down whatever is louder than the threshold, linked across channels so the image does not shift.
 * */
// @reflect
struct NYA_AudioCompressor {
    b8 enabled;

    /** Level above which gain is reduced, dBFS. Zero takes the default, so it cannot sit at full scale. */
    f32 threshold_db;

    /** Decibels in per decibel out above the threshold. */
    f32 ratio;

    f32 attack_ms;
    f32 release_ms;

    /** Gain added back after, decibels. Zero adds none. */
    f32 makeup_db;
};

/**
 * A feedback delay: repeats that fade and dull with each pass.
 * */
// @reflect
struct NYA_AudioEcho {
    b8 enabled;

    /** Between repeats, milliseconds, up to NYA_AUDIO_ECHO_FRAMES at the device rate. */
    f32 delay_ms;

    /** Share of each repeat fed into the next, below one. */
    f32 feedback;

    f32 wet;

    /** Each repeat is rolled off above this, hertz. */
    f32 lowpass_hz;
};

/**
 * A room around a bus: how big it sounds and how much of it is heard.
 * */
// @reflect
struct NYA_AudioReverb {
    /** How long the tail rings, roughly 0 to 1. Zero switches the reverb off. */
    f32 room_size;

    /** How fast the high frequencies die away inside the tail, 0 to 1. */
    f32 damping;

    /** How much reverberated signal is added. Zero takes a modest 0.3. */
    f32 wet;

    /** How much of the original passes through. Zero takes 1.0, untouched. */
    f32 dry;

    /** How far apart the two channels' rooms are, 0 to 1. Zero takes 1.0, fully wide. */
    f32 width;
};

/**
 * Holds peaks under a ceiling. Instant attack, so no sample passes it, and a smooth release.
 * */
// @reflect
struct NYA_AudioLimiter {
    b8 enabled;

    /** dBFS. Zero takes -1. */
    f32 ceiling_db;

    f32 release_ms;
};

/**
 * A bus's whole chain. Zeroed is every unit off.
 * */
// @reflect
struct NYA_AudioEffects {
    NYA_AudioPass       pass;
    NYA_AudioEqualizer  equalizer;
    NYA_AudioCompressor compressor;
    NYA_AudioEcho       echo;
    NYA_AudioReverb     reverb;
    NYA_AudioLimiter    limiter;
};

/** One echo off something around the listener. */
struct NYA_AudioReflectionTap {
    /** After the sound, seconds. */
    f32 delay_s;

    /** Zero is silent. */
    f32 gain;

    /** -1 left to +1 right, from where the surface is. */
    f32 pan;

    /** The echo is rolled off above this, hertz: longer paths and softer surfaces lose more treble. */
    f32 lowpass_hz;
};

/** The listener's echoes, written by propagation. */
struct NYA_AudioReflections {
    NYA_AudioReflectionTap taps[NYA_AUDIO_REFLECTION_TAPS];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Sets a bus's chain. Validated: zeroes take their defaults, ranges are clamped. Cheap to call every frame; only a
 * change reaches the mixer.
 * */
NYA_API void nya_audio_bus_effects_set(NYA_AudioBus bus, NYA_AudioEffects effects);

/** What the bus was last set to, validated. The reverb is what was asked for, not what the environment drives. */
NYA_API NYA_AudioEffects nya_audio_bus_effects_get(NYA_AudioBus bus) __attr_no_discard;
