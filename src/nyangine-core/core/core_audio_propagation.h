/**
 * @file core_audio_propagation.h
 *
 * ```c
 * // Once: the engine already traces against the 3D physics world, so this is the whole setup.
 * nya_audio_propagation_set((NYA_AudioPropagation){ .enabled = true, .diffraction = true, .environment = true });
 *
 * // A bonfire is a few metres wide, so it is never fully hidden by a post.
 * NYA_SoundVoice fire = nya_audio_play_sound_at_3d(NYA_ASSET_SOUNDS_FIRE_WAV, hearth, (NYA_SoundParams){
 *     .loop = true, .radius = 1.5F,
 * });
 * ```
 *
 * ```c
 * // A test, or a game with its own geometry: any function that answers rays.
 * NYA_INTERNAL void walls_trace(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data) {
 *     for (u32 i = 0; i < count; i++) out_fractions[i] = walls_first_hit(user_data, rays[i].origin, rays[i].direction);
 * }
 *
 * nya_audio_rays_set(NYA_AUDIO_SPACE_3D, walls_trace, &walls);
 * ```
 *
 * Every positional voice is traced from the listener: a few rays over the source's extent give how much of it is
 * hidden, a ray back from the source gives how thick the blocker is, and while a voice is hidden a few probes beside
 * the blocker look for a way around it. The result is a gain, a low pass and a shift in where the sound seems to come
 * from, eased over NYA_AudioPropagation.smoothing_ms so nothing clicks. Rays from the listener in fixed directions
 * estimate the room, which drives the sound bus reverb as a late tail, and the surfaces they hit echo everything on
 * the sound bus back from where they are, delayed by the round trip and dulled by distance.
 *
 * Rays are cast on the main thread from nya_system_audio_update, under a fixed budget shared round robin across voices,
 * with no allocation per frame. Off, it casts nothing and touches no voice.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_audio.h"
#include "nyangine-core/core/core_audio_effects.h"
#include "nyangine-std/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The most rays one update may cast, whatever NYA_AudioPropagation.ray_budget asks. Registered as a ceiling. */
#define NYA_AUDIO_PROPAGATION_RAYS_MAX 256

/** Rays per update when NYA_AudioPropagation.ray_budget is zero. */
#define NYA_AUDIO_PROPAGATION_RAY_BUDGET 64

/** The most rays spread over one voice's extent. */
#define NYA_AUDIO_PROPAGATION_VOICE_RAYS_MAX 8

/** Rays over one voice's extent when NYA_AudioPropagation.voice_rays is zero. */
#define NYA_AUDIO_PROPAGATION_VOICE_RAYS 4

/** Places beside a blocker tried for a way around it, two rays each. Right, left, up and down; 2D drops the last two. */
#define NYA_AUDIO_PROPAGATION_PROBES 4

/** Fixed listener directions the room is measured along. 2D uses the first eight, which lie in its plane. */
#define NYA_AUDIO_PROPAGATION_ENVIRONMENT_RAYS 14

/** Of those, how many are re-cast each update. A full sweep takes four updates. */
#define NYA_AUDIO_PROPAGATION_ENVIRONMENT_SLICE 4

/* The defaults a zero field takes. */

/** World units the rays spread over, for a voice played without NYA_SoundParams.radius. */
#define NYA_AUDIO_PROPAGATION_RADIUS 0.25F

/** Cutoff through one thin surface, hertz. Thicker blockers go lower. */
#define NYA_AUDIO_PROPAGATION_LOWPASS_HZ 900.0F

/** Level through one thin surface. */
#define NYA_AUDIO_PROPAGATION_TRANSMISSION 0.4F

/** World units of solid that cut the transmitted level to about a third. */
#define NYA_AUDIO_PROPAGATION_THICKNESS 2.0F

/** How far beside a blocker a way around is looked for, world units. */
#define NYA_AUDIO_PROPAGATION_DIFFRACTION_REACH 2.0F

/** How long a voice's level, tone and direction take to settle, milliseconds. */
#define NYA_AUDIO_PROPAGATION_SMOOTHING_MS 90.0F

/** How far the room probes reach, world units. Nothing within it reads as open air. */
#define NYA_AUDIO_PROPAGATION_ENVIRONMENT_RANGE 30.0F

/** How long the room estimate takes to follow the listener into another space, milliseconds. */
#define NYA_AUDIO_PROPAGATION_ENVIRONMENT_GLIDE_MS 600.0F

/** World units per second. Metres in air. */
#define NYA_AUDIO_PROPAGATION_SPEED_OF_SOUND 343.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_AudioSpace          NYA_AudioSpace;
typedef struct NYA_AudioRay          NYA_AudioRay;
typedef struct NYA_AudioPropagation  NYA_AudioPropagation;
typedef struct NYA_AudioEnvironment  NYA_AudioEnvironment;
typedef struct NYA_AudioPath         NYA_AudioPath;
typedef struct NYA_AudioTracer       NYA_AudioTracer;

/**
 * Casts `count` rays and writes, for each, how far along its direction the first hit is: 0 at the origin, 1 at the
 * end. A miss is 1 or more. Called on the main thread, at most once per update.
 * */
typedef void (*NYA_AudioRayFn)(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data);

/**
 * Which listener is the ear, which voices are traced, and which ray function answers.
 * */
// @reflect
enum NYA_AudioSpace {
    /** nya_audio_listener_3d_set and voices placed with the _3d functions. The engine traces the 3D physics world. */
    NYA_AUDIO_SPACE_3D,

    /** nya_audio_listener_set and voices placed in 2D, z zero. The engine traces the 2D physics world. */
    NYA_AUDIO_SPACE_2D,

    NYA_AUDIO_SPACE_COUNT,
};

/** A segment: `origin` to `origin + direction`. */
struct NYA_AudioRay {
    f32x3 origin;
    f32x3 direction;
};

/**
 * How sound finds its way through the world. Zeroed is off; a zero number takes its NYA_AUDIO_PROPAGATION_* default.
 * */
// @reflect
struct NYA_AudioPropagation {
    b8 enabled;

    NYA_AudioSpace space;

    /** Rays per update across every voice and the room, clamped to NYA_AUDIO_PROPAGATION_RAYS_MAX. */
    u32 ray_budget;

    /** Rays over each voice's extent. The share blocked is how hidden it is. */
    u32 voice_rays;

    /** Extent of a voice played without NYA_SoundParams.radius, world units. */
    f32 radius;

    /** Cutoff through one thin surface, hertz. */
    f32 lowpass_hz;

    /** Level through one thin surface, 0 to 1. */
    f32 transmission;

    /** World units of solid that cut the transmitted level to about a third. */
    f32 thickness;

    /** Eases level, tone and direction, milliseconds. */
    f32 smoothing_ms;

    /** Looks for a way around a blocker, and moves a hidden sound toward the opening. */
    b8 diffraction;

    /** How far beside the blocker, world units. */
    f32 diffraction_reach;

    /** Measures the room around the listener and drives the sound bus reverb with it, over any reverb set there. */
    b8 environment;

    /** How far the room probes reach, world units. */
    f32 environment_range;

    /** Echo level, 0 to 1, off the surfaces the room probes hit. Zero is off. Needs `environment`. */
    f32 reflections;

    /** World units per second, which sets how late each echo returns and how hard a detour is on the level. */
    f32 speed_of_sound;
};

/**
 * The room as the listener's probes see it, and the reverb derived from it.
 * */
struct NYA_AudioEnvironment {
    /** Share of probes that hit something within range, 0 open air to 1 closed. */
    f32 enclosure;

    /** Mean distance to what they hit, world units. The range when nothing is hit. */
    f32 distance;

    /** Eased toward what enclosure and distance ask for. */
    NYA_AudioReverb reverb;
};

/**
 * What propagation does to one voice. Eased values the mixer gets, and the targets the last trace set.
 * */
struct NYA_AudioPath {
    /** Multiplies the voice's gain. */
    f32 gain;

    /** 0 open, 1 as dull as one thin surface, more for thicker. */
    f32 muffle;

    /** Added to the voice's position, toward the way around a blocker. */
    f32x3 offset;

    f32   target_gain;
    f32   target_muffle;
    f32x3 target_offset;

    /** Share of the voice's rays that were blocked. */
    f32 occlusion;

    /** Where along the path the blocker starts, 0 to 1, for placing probes. */
    f32 blocker;

    /** Traced at least once since the voice started. The first result is taken as is, not eased into. */
    b8 traced;
};

/**
 * Propagation's state, embedded in the audio system. Fixed size: an update allocates nothing.
 * */
struct NYA_AudioTracer {
    NYA_AudioPath paths[NYA_AUDIO_VOICES];

    /** The update's batch, and what came back. */
    NYA_AudioRay rays[NYA_AUDIO_PROPAGATION_RAYS_MAX];
    f32          fractions[NYA_AUDIO_PROPAGATION_RAYS_MAX];

    /** Last hit fraction along each room direction. Starts open. */
    f32 environment_fractions[NYA_AUDIO_PROPAGATION_ENVIRONMENT_RAYS];
    u32 environment_cursor;

    NYA_AudioEnvironment environment;

    /** The echoes the room probes' hits give, for the sound bus. */
    NYA_AudioReflections reflections;

    /** The voice the round robin resumes at. */
    u32 voice_cursor;

    /** Rays the last update cast. The ceiling's live count. */
    u32 rays_cast;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Traces every positional voice of the configured space and applies the result. The app calls it once a frame, after
 * rendering, when the listener has been placed. Returns at once while propagation is off.
 * */
NYA_API void nya_system_audio_update(f32 delta_time_s);

/**
 * Turns propagation on, off or retunes it. Validated: counts are clamped and zeroes defaulted. Turning it off puts
 * every voice back where it was placed, unfiltered, and the sound bus back on its own reverb.
 * */
NYA_API void                 nya_audio_propagation_set(NYA_AudioPropagation propagation);
NYA_API NYA_AudioPropagation nya_audio_propagation_get(void) __attr_no_discard;

/**
 * Installs what answers rays for a space. The app installs the physics worlds; null disables tracing there.
 * */
NYA_API void nya_audio_rays_set(NYA_AudioSpace space, NYA_AudioRayFn function, void* user_data);

/** The room estimate, eased. Zeroed while the environment is off. */
NYA_API NYA_AudioEnvironment nya_audio_environment_get(void) __attr_no_discard;

/**
 * What propagation is doing to a voice. Zeroed for a dead handle, an unplaced voice, or while off.
 * */
NYA_API NYA_AudioPath nya_audio_voice_path_get(NYA_SoundVoice voice) __attr_no_discard;
