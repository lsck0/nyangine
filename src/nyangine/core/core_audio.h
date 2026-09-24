/**
 * @file core_audio.h
 *
 * ```c
 * // Once, at load time. A sound effect wants predecoding so it starts instantly.
 * NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
 *     .type = NYA_ASSET_TYPE_SOUND, .handle = NYA_ASSET_SFX_JUMP_WAV, .as_sound = { .predecode = true },
 * }));
 *
 * nya_audio_play_sound(NYA_ASSET_SFX_JUMP_WAV, 1.0F);
 * nya_audio_play_music(NYA_ASSET_MUSIC_THEME_OGG, true, 0);
 * ```
 *
 * ```c
 * NYA_SoundVoice engine = nya_audio_play_sound_with(NYA_ASSET_SFX_ENGINE_WAV, (NYA_SoundParams){
 *     .gain = 0.8F, .pitch = 1.0F, .loop = true,
 * });
 *
 * // Later, every frame: the engine note rises with speed, and pans with the car.
 * nya_audio_voice_set_pitch(engine, 1.0F + (speed * 0.5F));
 * nya_audio_voice_set_pan(engine, relative_x);
 * ```
 *
 * ```c
 * nya_audio_listener_set((NYA_AudioListener){ .position = player_position, .reference_distance = 8.0F });
 * nya_audio_play_sound_at(NYA_ASSET_SFX_ROCKFALL_WAV, rock_position, (NYA_SoundParams){ .gain = 0.9F });
 * ```
 *
 *
 * Bus effects are in core_audio_effects.h, and how sound travels through the world in core_audio_propagation.h.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Sound effects that can play at once, music aside.
 * */
#ifndef NYA_AUDIO_VOICES
#define NYA_AUDIO_VOICES 16
#endif

/**
 * How far nya_audio_play_sound_varied detunes, in semitones either side.
 * */
#ifndef NYA_AUDIO_PITCH_VARIATION_SEMITONES
#define NYA_AUDIO_PITCH_VARIATION_SEMITONES 1.0F
#endif

/**
 * How far nya_audio_play_sound_varied varies the level, in decibels either side.
 * */
#ifndef NYA_AUDIO_GAIN_VARIATION_DB
#define NYA_AUDIO_GAIN_VARIATION_DB 2.0F
#endif

/**
 * Speaker channels a bus filter will process.
 * */
#define NYA_AUDIO_FILTER_MAX_CHANNELS 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SoundVoice  NYA_SoundVoice;
typedef struct NYA_SoundParams NYA_SoundParams;

/**
 * A sound that is playing, and can still be changed.
 * */
struct NYA_SoundVoice {
    u32 index;

    /** Bumped every time the slot is reused. Zero is never a live generation. */
    u32 generation;
};

/** The handle a failed play returns. Safe to pass to every function here; they all ignore it. */
#define NYA_SOUND_VOICE_NONE ((NYA_SoundVoice){ .index = 0, .generation = 0 })

/**
 * Everything a sound can be started with, so it does not have to be corrected on the next frame.
 * */
struct NYA_SoundParams {
    /** Linear, 1.0 unchanged. Multiplied by the effect and master gains. */
    f32 gain;

    /**
     * Varies this instance's level by a random offset in ±this many decibels, on top of `gain`. The loud half
     * amplifies (about 1.26 at ±2 dB), and gain above 1.0 clips, so leave headroom on sounds played this way.
     * */
    f32 gain_variation_db;

    /**
     * Playback rate, 1.0 unchanged. 2.0 is an octave up and twice as fast.
     * */
    f32 pitch;

    /**
     * Detunes this instance by a random offset in ±this many semitones, on top of `pitch`.
     * */
    f32 pitch_variation_semitones;

    /** -1 hard left, 0 centred, +1 hard right. */
    f32 pan;

    b8  loop;
    u32 fade_in_ms;

    /**
     * Who wins when every voice is busy.
     * */
    s32 priority;

    /**
     * How wide the source is, world units: a bonfire is not a point. Propagation spreads its rays over it, so a thin
     * post cannot hide it. Zero takes NYA_AudioPropagation.radius.
     * */
    f32 radius;
};

typedef enum NYA_AudioBus          NYA_AudioBus;
typedef struct NYA_AudioFilter     NYA_AudioFilter;
typedef enum NYA_AudioPlane        NYA_AudioPlane;
typedef struct NYA_AudioListener   NYA_AudioListener;
typedef struct NYA_AudioListener3D NYA_AudioListener3D;

/**
 * What effects are attached to.
 * */
enum NYA_AudioBus {
    /** Every sound effect. The one a "player is underwater" filter usually wants. */
    NYA_AUDIO_BUS_SOUND,

    /** Music, both tracks, including a crossfade in progress. */
    NYA_AUDIO_BUS_MUSIC,

    /** Everything, applied after the other two have had their turn. */
    NYA_AUDIO_BUS_MASTER,

    NYA_AUDIO_BUS_COUNT,
};

/**
 * A one pole low pass on a voice. Zeroed is no filtering.
 * */
struct NYA_AudioFilter {
    /**
     * Frequency above which the voice is rolled off, in hertz. Zero is off.
     * */
    f32 lowpass_hz;

    /**
     * How long to take reaching a new setting, in milliseconds. Zero snaps.
     * */
    f32 glide_ms;
};

/**
 * How a 2D world position is laid into the mixer's 3D space.
 * */
enum NYA_AudioPlane {
    /**
     * Side on, the platformer case: the screen is a wall, so y is height.
     * */
    NYA_AUDIO_PLANE_SIDE,

    /**
     * Top down: the screen is the ground, so y is depth away from the listener.
     * */
    NYA_AUDIO_PLANE_TOP_DOWN,

    NYA_AUDIO_PLANE_COUNT,
};

/**
 * Where the player hears from, in world units.
 * */
struct NYA_AudioListener {
    /** World units, the same space nya_audio_play_sound_at is given. */
    f32x2 position;

    /**
     * How far a sound gets before it starts fading, in world units.
     * */
    f32 reference_distance;

    /** Side on unless said otherwise, which is the zero value. */
    NYA_AudioPlane plane;
};

/**
 * The ear in a 3D scene: a point *and* the direction it faces.
 * */
struct NYA_AudioListener3D {
    /** Where the ear is, in world units. Usually the camera, in a scene with no avatar to hear from. */
    f32x3 position;

    /** Where it looks. Zero is read as unspecified and becomes -z, which is the graphics convention. */
    f32x3 forward;

    /** Which way is up for the ear. Zero becomes +y. Only its component across `forward` is used. */
    f32x3 up;

    /**
     * How far a sound gets before it starts fading, in world units. See NYA_AudioListener for the model.
     * */
    f32 reference_distance;
};

/** How music is started. Same zero-means-default convention as NYA_SoundParams. */
typedef struct NYA_MusicParams NYA_MusicParams;

struct NYA_MusicParams {
    /** Linear, 1.0 unchanged. Zero is read as unspecified and becomes 1.0. */
    f32 gain;

    b8 loop;

    /**
     * Where a loop returns to, in milliseconds from the start.
     * */
    u32 loop_start_ms;

    /** Ramps up from silence. Zero starts at full gain, which on a track with no lead-in clicks. */
    u32 fade_in_ms;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Creates the voice pool and the music track.
 * */
NYA_API NYA_Error nya_system_audio_init(void) __attr_no_discard;
NYA_API void      nya_system_audio_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * SOUND EFFECTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Plays a sound asset once, on the first free voice.
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound(NYA_ConstCString sound_handle, f32 gain);

/**
 * Plays a sound asset once, detuned by ±NYA_AUDIO_PITCH_VARIATION_SEMITONES and levelled by
 * ±NYA_AUDIO_GAIN_VARIATION_DB.
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound_varied(NYA_ConstCString sound_handle, f32 gain);

/** Plays with effects already applied, so the first audible sample is already correct. */
NYA_API NYA_SoundVoice nya_audio_play_sound_with(NYA_ConstCString sound_handle, NYA_SoundParams params);

/** Stops every sound effect. Music is untouched; see nya_audio_stop_music. */
NYA_API void nya_audio_stop_sounds(void);

/*
 * ─────────────────────────────────────────────────────────
 * POSITION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Moves the ear. See NYA_AudioListener.
 * */
NYA_API void              nya_audio_listener_set(NYA_AudioListener listener);
NYA_API NYA_AudioListener nya_audio_listener_get(void) __attr_no_discard;

/**
 * Moves the ear in a 3D scene, position and facing. See NYA_AudioListener3D.
 * */
NYA_API void                nya_audio_listener_3d_set(NYA_AudioListener3D listener);
NYA_API NYA_AudioListener3D nya_audio_listener_3d_get(void) __attr_no_discard;

/**
 * Plays a sound at a point in the world, heard from wherever the listener is.
 *
 * ```c
 * nya_audio_listener_set((NYA_AudioListener){ .position = player_position, .reference_distance = 8.0F });
 *
 * nya_audio_play_sound_at(NYA_ASSET_SFX_PICKAXE_WAV, ore_position, (NYA_SoundParams){
 *     .gain = 0.8F, .pitch_variation_semitones = 1.0F, .gain_variation_db = 2.0F,
 * });
 * ```
 *
 * SDL_mixer mixes positional playback down to mono. For a clip whose stereo image matters, use
 * nya_audio_play_sound_with and a pan.
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound_at(NYA_ConstCString sound_handle, f32x2 world_position, NYA_SoundParams params);

/**
 * Plays a sound at a point in a 3D world, heard from wherever the 3D listener is and however it faces.
 *
 * ```c
 * nya_audio_listener_3d_set((NYA_AudioListener3D){
 *     .position = camera_position, .forward = target - camera_position, .up = { 0, 1, 0 },
 *     .reference_distance = 8.0F,
 * });
 *
 * nya_audio_play_sound_at_3d(NYA_ASSET_SOUNDS_HIT_WAV, impact_point, (NYA_SoundParams){ .gain = 0.8F });
 * ```
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound_at_3d(NYA_ConstCString sound_handle, f32x3 world_position, NYA_SoundParams params);

/**
 * Turns our own stereo panner on for positioned sounds. Off by default, where SDL_mixer places them as
 * before. On, and on a stereo device, nya_audio_play_sound_at, nya_audio_play_sound_at_3d and the
 * nya_audio_voice_set_world_position setters are placed by the panner instead: an interaural level and time
 * difference and a head shadow, from the source's azimuth. See core_audio_panner.h for the model. The
 * explicit nya_audio_voice_set_pan and nya_audio_voice_set_position stay SDL_mixer's, and a non-stereo
 * device falls back to it too.
 * */
NYA_API void nya_audio_panner_set_enabled(b8 enabled);
NYA_API b8   nya_audio_panner_enabled(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * MUSIC
 * ─────────────────────────────────────────────────────────
 */

/**
 * Starts `music_handle` on the music track, replacing whatever was playing.
 *
 * @lua(AUDIO)
 * */
NYA_API void nya_audio_play_music(NYA_ConstCString music_handle, b8 loop, u32 fade_in_ms);

/** Starts music with loop points and a gain, for anything the three argument form cannot say. */
NYA_API void nya_audio_play_music_with(NYA_ConstCString music_handle, NYA_MusicParams params);

/**
 * Fades the current track out while fading the new one in, over `duration_ms`.
 * */
NYA_API void nya_audio_crossfade_music(NYA_ConstCString music_handle, NYA_MusicParams params, u32 duration_ms);

/**
 * The music track as a voice, so the effect functions below apply to it.
 * */
NYA_API NYA_SoundVoice nya_audio_music_voice(void) __attr_no_discard;

/**
 * Stops the music, fading out over `fade_out_ms`. Zero stops immediately.
 *
 * @lua(AUDIO)
 * */
NYA_API void nya_audio_stop_music(u32 fade_out_ms);

/*
 * Pause and resume keep the track's position, unlike stop and play. What a pause menu wants.
 */

/**
 * Pauses the music where it is.
 *
 * @lua(AUDIO)
 * */
NYA_API void nya_audio_pause_music(void);

/**
 * Resumes it from there.
 *
 * @lua(AUDIO)
 * */
NYA_API void nya_audio_resume_music(void);

/**
 * Whether the music track is sounding. False while paused, as well as when stopped.
 *
 * @lua(AUDIO)
 * */
NYA_API b8 nya_audio_music_playing(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * EFFECTS
 * ─────────────────────────────────────────────────────────
 */

/*
 * All of these take a voice from nya_audio_play_sound or nya_audio_music_voice and ignore a handle whose sound
 * has finished, so adjusting a looping sound every frame needs no liveness check.
 */

/** Whether the sound this handle names is still the one in that slot, and still sounding. */
NYA_API b8 nya_audio_voice_valid(NYA_SoundVoice voice) __attr_no_discard;

/**
 * This voice's own gain, on top of the category and master gains.
 * */
NYA_API void nya_audio_voice_set_gain(NYA_SoundVoice voice, f32 gain);

/** Playback rate, 1.0 unchanged. This resamples rather than pitch shifts. */
NYA_API void nya_audio_voice_set_pitch(NYA_SoundVoice voice, f32 ratio);

/**
 * Stereo placement, -1 hard left through 0 centred to +1 hard right.
 * */
NYA_API void nya_audio_voice_set_pan(NYA_SoundVoice voice, f32 pan);

/**
 * Places the sound in 3D relative to the listener, who is at the origin facing -z.
 * */
NYA_API void nya_audio_voice_set_position(NYA_SoundVoice voice, f32x3 position);

/**
 * Puts a low pass on one voice alone. See NYA_AudioFilter for what the filter is and is not.
 * */
NYA_API void nya_audio_voice_filter_set(NYA_SoundVoice voice, NYA_AudioFilter filter);

/** Moves a playing sound to a point in the world, through the listener. */
NYA_API void nya_audio_voice_set_world_position(NYA_SoundVoice voice, f32x2 world_position);

/** Re-places a playing voice at a 3D world point, against the 3D listener as it is now. */
NYA_API void nya_audio_voice_set_world_position_3d(NYA_SoundVoice voice, f32x3 world_position);

/** Stops this voice, fading out over `fade_out_ms`. Zero stops immediately. */
NYA_API void nya_audio_voice_stop(NYA_SoundVoice voice, u32 fade_out_ms);

/*
 * ─────────────────────────────────────────────────────────
 * GAIN
 * ─────────────────────────────────────────────────────────
 */

/*
 * Linear multipliers, 1.0 being unchanged. Clamped at zero; values above one amplify and may clip.
 */

NYA_API void nya_audio_set_master_gain(f32 gain);
NYA_API void nya_audio_set_sound_gain(f32 gain);
NYA_API void nya_audio_set_music_gain(f32 gain);

NYA_API f32 nya_audio_master_gain(void) __attr_no_discard;
NYA_API f32 nya_audio_sound_gain(void) __attr_no_discard;
NYA_API f32 nya_audio_music_gain(void) __attr_no_discard;
