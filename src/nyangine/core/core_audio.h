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
 * ```c
 * // The world dulls; music and UI carry on untouched because they are on another bus.
 * nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .lowpass_hz = 700.0F, .glide_ms = 120.0F });
 * ```
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
     * Varies this instance's level by a random offset in ±this many decibels, on top of `gain`.
     *
     * ⚠ **The loud half amplifies**, by about 1.26 at the default ±2dB, and gain above 1.0 clips. Leave
     * headroom — a base gain of 0.8 rather than 1.0 — on anything played this way at full level.
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
};

typedef enum NYA_AudioBus        NYA_AudioBus;
typedef struct NYA_AudioFilter   NYA_AudioFilter;
typedef enum NYA_AudioPlane        NYA_AudioPlane;
typedef struct NYA_AudioOcclusion  NYA_AudioOcclusion;
typedef struct NYA_AudioReverb     NYA_AudioReverb;
typedef f32 (*NYA_AudioOcclusionFn)(f32x3 world_position, void* user_data);
typedef struct NYA_AudioListener   NYA_AudioListener;
typedef struct NYA_AudioListener3D NYA_AudioListener3D;

/**
 * What a filter is attached to.
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
 * A filter on a bus. Zeroed is no filtering, which is where every bus starts.
 * */
struct NYA_AudioFilter {
    /**
     * Frequency above which the bus is rolled off, in hertz. Zero is off.
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

/** Stops every sound effect. Music is untouched — that is what nya_audio_stop_music is for. */
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
 * ⚠ Positional playback is mixed down to **mono** by SDL_mixer. A clip whose stereo image is the point of
 * it — most music, some ambience — wants nya_audio_play_sound_with and a pan instead.
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

/*
 * ─────────────────────────────────────────────────────────
 * MUSIC
 * ─────────────────────────────────────────────────────────
 */

/**
 * Starts `music_handle` on the music track, replacing whatever was playing.
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

/** Stops the music, fading out over `fade_out_ms`. Zero stops immediately. */
NYA_API void nya_audio_stop_music(u32 fade_out_ms);

/*
 * Pause and resume, which are not stop and play: the track keeps its position, so resuming carries on
 * rather than starting the piece again. What a pause menu wants.
 */
NYA_API void nya_audio_pause_music(void);
NYA_API void nya_audio_resume_music(void);

/** Whether the music track is sounding. False while paused, as well as when stopped. */
NYA_API b8 nya_audio_music_playing(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * EFFECTS
 * ─────────────────────────────────────────────────────────
 */

/*
 * All of these take a voice from nya_audio_play_sound or nya_audio_music_voice, and all of them
 * ignore a handle whose sound has finished. That means a caller adjusting a looping sound every frame
 * needs no "is it still alive" check — it simply stops having an effect.
 */

/** Whether the sound this handle names is still the one in that slot, and still sounding. */
NYA_API b8 nya_audio_voice_valid(NYA_SoundVoice voice) __attr_no_discard;

/**
 * This voice's own gain, on top of the category and master gains.
 * */
NYA_API void nya_audio_voice_set_gain(NYA_SoundVoice voice, f32 gain);

/** Playback rate, 1.0 unchanged. See NYA_SoundParams.pitch — this resamples rather than pitch shifts. */
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
 * Moves a playing sound to a point in the world, through the listener.
 * */
/**
 * Puts a low pass on one voice alone. See NYA_AudioFilter for what the filter is and is not.
 * */
NYA_API void nya_audio_voice_filter_set(NYA_SoundVoice voice, NYA_AudioFilter filter);

/**
 * How a sound is treated when something is between it and the listener.
 * */
struct NYA_AudioOcclusion {
    /**
     * Cutoff in hertz at full occlusion. Zero disables the filtering half.
     * */
    f32 lowpass_hz;

    /** Gain multiplier at full occlusion. Zero is read as unspecified and becomes 0.5. */
    f32 gain;

    /** How long the filter takes to reach a new cutoff. Zero becomes a short glide; see NYA_AudioFilter. */
    f32 glide_ms;
};

/**
 * Installs the function that decides how blocked a point is, and how blocked sounds. Null disables it.
 *
 * ```c
 * NYA_INTERNAL f32 occlusion_of(f32x3 source, void* user_data) {
 *     nya_unused(user_data);
 *
 *     f32x3 ear = nya_audio_listener_3d_get().position;
 *
 *     return nya_entity_is_valid(nya_physics3d_raycast(ear, source - ear, nullptr, nullptr)) ? 1.0F : 0.0F;
 * }
 * ```
 * */
NYA_API void nya_audio_occlusion_set(NYA_AudioOcclusionFn function, void* user_data, NYA_AudioOcclusion occlusion);

/**
 * Re-evaluates occlusion for every positional voice that is still playing. Call it once a frame.
 * */
NYA_API void nya_audio_occlusion_update(void);

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
/*
 * ─────────────────────────────────────────────────────────
 * BUS FILTERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Puts a filter on a whole bus, or takes it off.
 *
 * ```c
 * // Headphones on: the world dulls over a tenth of a second, the UI is on music and stays crisp.
 * nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .lowpass_hz = 700.0F, .glide_ms = 120.0F });
 *
 * // Headphones off. Zero is off, and it glides back rather than snapping open.
 * nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .glide_ms = 120.0F });
 * ```
 * */
/**
 * A room around a bus: how big it sounds and how much of it is heard.
 * */
struct NYA_AudioReverb {
    /**
     * How long the tail rings, roughly 0 to 1. Zero switches the reverb off entirely.
     * */
    f32 room_size;

    /**
     * How fast the high frequencies die away inside the tail, 0 to 1.
     * */
    f32 damping;

    /** How much reverberated signal is added. Zero is unspecified and becomes a modest 0.3. */
    f32 wet;

    /**
     * How much of the original passes through. Zero is unspecified and becomes 1.0, i.e. untouched.
     * */
    f32 dry;

    /**
     * How far apart the two channels' rooms are, 0 to 1. Zero becomes 1.0, fully wide.
     * */
    f32 width;
};

/**
 * Puts a reverb on a bus, or takes it off. See NYA_AudioReverb.
 *
 * ```c
 * // A cave: long, dark, and mostly what you hear.
 * nya_audio_bus_reverb_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioReverb){
 *     .room_size = 0.86F, .damping = 0.35F, .wet = 0.5F, .dry = 0.7F,
 * });
 * ```
 * */
NYA_API void nya_audio_bus_reverb_set(NYA_AudioBus bus, NYA_AudioReverb reverb);

/** What the bus is currently reverberating with. A zeroed struct when it has none. */
NYA_API NYA_AudioReverb nya_audio_bus_reverb_get(NYA_AudioBus bus) __attr_no_discard;

NYA_API void nya_audio_bus_filter_set(NYA_AudioBus bus, NYA_AudioFilter filter);

/**
 * What that bus was last set to.
 * */
NYA_API NYA_AudioFilter nya_audio_bus_filter_get(NYA_AudioBus bus) __attr_no_discard;

NYA_API void nya_audio_set_master_gain(f32 gain);
NYA_API void nya_audio_set_sound_gain(f32 gain);
NYA_API void nya_audio_set_music_gain(f32 gain);

NYA_API f32 nya_audio_master_gain(void) __attr_no_discard;
NYA_API f32 nya_audio_sound_gain(void) __attr_no_discard;
NYA_API f32 nya_audio_music_gain(void) __attr_no_discard;
