/**
 * @file core_audio.h
 *
 * Playing sound effects and music, in terms of sound assets.
 *
 * Two kinds of playback, because they want opposite things. A **sound effect** is fire and forget
 * and polyphonic: several can overlap, none is addressable once started, and the same footstep clip
 * playing four times at once is normal. **Music** is a single stream that something wants to stop,
 * pause and swap later, so there is exactly one of it and it is controlled by name rather than by
 * handle.
 *
 * ```c
 * // Once, at load time. A sound effect wants predecoding so it starts instantly.
 * NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
 *     .type = NYA_ASSET_TYPE_SOUND, .handle = NYA_ASSET_SFX_JUMP_WAV, .as_sound = { .predecode = true },
 * }));
 *
 * nya_audio_play_sound(NYA_ASSET_SFX_JUMP_WAV, 1.0F);
 * // Looping, with no fade in. The third argument is not optional; this example omitted it.
 * nya_audio_play_music(NYA_ASSET_MUSIC_THEME_OGG, true, 0);
 * ```
 *
 * **Predecode sound effects, stream music.** `predecode` on the asset holds the whole clip decoded
 * in memory, which is right for something short that has to start on the frame it is asked for and
 * wrong for a several minute track, where it means holding the entire decoded stream.
 *
 * ## Gain
 *
 * Three levels, multiplied together: a master, one for effects and one for music. That is the shape
 * an options menu needs — a player turning music down should not also turn the effects down — and it
 * is why gain is not simply a parameter on each call. The per call gain is a fourth multiplier, for
 * a quieter instance of a sound rather than a quieter category.
 *
 * Gains are linear multipliers where 1.0 is unchanged, not decibels and not a percentage. Values
 * above 1.0 amplify and will clip.
 *
 * ## Voices
 *
 * Effects share a fixed pool of tracks, so NYA_AUDIO_VOICES is how many can sound at once. Asking
 * for one more than that drops the new sound rather than cutting off a playing one, on the grounds
 * that a missing footstep is less noticeable than a truncated explosion.
 *
 * ## Variation
 *
 * The same clip played twice in a row is recognisably the same clip, and a game that fires one every
 * few steps turns it into a machine noise. Nudging the pitch and the level of each instance is what
 * breaks that up, and is cheap enough to be the default for anything repetitive:
 *
 * ```c
 * // ±1 semitone and ±2dB, rolled per instance. No second clip, no extra memory.
 * nya_audio_play_sound_varied(NYA_ASSET_SFX_FOOTSTEP_WAV, 0.8F);
 *
 * // Or spelled out, when the amounts matter or they ride on other parameters.
 * nya_audio_play_sound_with(NYA_ASSET_SFX_HIT_WAV, (NYA_SoundParams){
 *     .gain = 0.9F, .pitch_variation_semitones = 2.5F, .gain_variation_db = 3.0F, .priority = 4,
 * });
 * ```
 *
 * Either can be used without the other, and both are off unless asked for. Of the two, the level is
 * what does most of the work — a run of footsteps reads as mechanical because they are all the same
 * loudness before it reads as mechanical because they are the same pitch.
 *
 * Semitones and decibels rather than linear multipliers, because the ear hears both logarithmically
 * and a linear range is not symmetric: ±0.06 around a pitch of 1.0 is a wider step down than up, so
 * naive jitter drifts a crowd of sounds flat and quiet. An exponent is symmetric by construction.
 *
 * Both are rolled once, when the sound starts, so a looping sound is a variant of the clip rather
 * than something that warbles and breathes.
 *
 * The variation draws from the audio system's own generator, not from any RNG the game holds. A
 * seeded run therefore produces the same world however many footsteps it played, which it would not
 * if sound effects advanced the same stream the simulation reads.
 *
 * ## Effects
 *
 * Playing returns an NYA_SoundVoice, which is what an effect is applied to. Gain, pitch, pan and 3D
 * position can all be changed while the sound is running:
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
 * Music is a voice too — `nya_audio_music_voice()` hands back its handle — so the same effects apply
 * to it without a second set of functions. That is why there is no `nya_audio_music_set_pitch`.
 *
 * ## Position
 *
 * A sound can be placed in the world rather than panned by hand. Tell the engine where the player
 * hears from, then play sounds at world coordinates:
 *
 * ```c
 * // Once a frame, from whatever the game considers the ear.
 * nya_audio_listener_set((NYA_AudioListener){ .position = player_position, .reference_distance = 8.0F });
 *
 * // Anywhere, in the same world units.
 * nya_audio_play_sound_at(NYA_ASSET_SFX_ROCKFALL_WAV, rock_position, (NYA_SoundParams){ .gain = 0.9F });
 * ```
 *
 * The mixer's listener cannot be moved off the origin, so this subtracts the listener and maps the
 * remainder onto the axes NYA_AudioPlane names — which is the one thing about the game's world the
 * engine cannot guess, since a side on game wants y to be height and a top down one wants it to be
 * depth.
 *
 * Distance attenuation is `reference_distance / distance` past the reference and full gain inside
 * it. There is no rolloff curve to choose and no cutoff: a far away sound is quiet, never silent.
 * Nothing here is occlusion, reverb or doppler; SDL_mixer does not have them.
 *
 * ## Buses and filters
 *
 * Effects and music mix into separate buses, and a bus can carry a filter — which is how everything
 * goes muffled at once without touching any individual sound:
 *
 * ```c
 * // The world dulls; music and UI carry on untouched because they are on another bus.
 * nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .lowpass_hz = 700.0F, .glide_ms = 120.0F });
 * ```
 *
 * A one pole low pass is the only filter, because it is the one that reads as muffled and because
 * SDL_mixer supplies no DSP at all — this is hand written into its post-mix callback. Dropping the
 * gain instead does not work: quieter sounds quieter, while what the ear identifies as muffled is
 * the missing treble.
 *
 * The filtering runs on the mixer's thread, so a change made here reaches the audio within a buffer
 * rather than instantly, and `glide_ms` is what keeps that transition from clicking.
 *
 * A voice handle is **generational**. When a voice finishes, its slot is reused and every handle to
 * the old sound stops resolving, so a stale handle held across frames does nothing rather than
 * quietly retuning whatever sound took its place. Check with nya_audio_voice_valid.
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
 *
 * Each is one MIX_Track held for the life of the process, so this is a small fixed cost rather than
 * something allocated per sound. Sixteen is generous for a 2D game; override with
 * -DNYA_AUDIO_VOICES=<n>.
 * */
#ifndef NYA_AUDIO_VOICES
#define NYA_AUDIO_VOICES 16
#endif

/**
 * How far nya_audio_play_sound_varied detunes, in semitones either side.
 *
 * One semitone is subtle by design: enough that consecutive footsteps stop sounding stamped from the
 * same die, little enough that the clip is still recognisably itself and a tuned sound does not go
 * out of key. Percussive noise takes two or three happily; anything pitched, or any voice line,
 * wants less than this rather than more. Override with -DNYA_AUDIO_PITCH_VARIATION_SEMITONES=<n>.
 * */
#ifndef NYA_AUDIO_PITCH_VARIATION_SEMITONES
#define NYA_AUDIO_PITCH_VARIATION_SEMITONES 1.0F
#endif

/**
 * How far nya_audio_play_sound_varied varies the level, in decibels either side.
 *
 * Two decibels is about the smallest step that reads as "another one of those" rather than as the
 * same sample again, and stays well short of sounding like a volume fault. Push it much past three
 * and the quiet instances start being missed in a mix.
 *
 * Symmetric, so the loud half multiplies the gain by roughly 1.26 — see NYA_SoundParams.gain_variation_db
 * for what that means at full gain. Override with -DNYA_AUDIO_GAIN_VARIATION_DB=<n>.
 * */
#ifndef NYA_AUDIO_GAIN_VARIATION_DB
#define NYA_AUDIO_GAIN_VARIATION_DB 2.0F
#endif

/**
 * Speaker channels a bus filter will process.
 *
 * One filter state per channel, and eight covers 7.1. A device with more than this is left
 * unfiltered rather than filtered wrongly — see nya_audio_bus_filter_set.
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
 *
 * Generational, like an entity handle and for the same reason: the slot is reused when the sound
 * ends, and a handle that outlived its sound must not steer the next one that lands there. An
 * all-zero handle is the "no voice" value that a failed play returns.
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
 *
 * Zero initialising gives silence, because `gain` and `pitch` default to zero rather than one — which
 * is why nya_audio_play_sound_with fills those in when they are left at zero. Say `.gain = 0.0F`
 * explicitly only if silence is what you meant, and prefer simply not playing the sound.
 * */
struct NYA_SoundParams {
    /** Linear, 1.0 unchanged. Multiplied by the effect and master gains. */
    f32 gain;

    /**
     * Varies this instance's level by a random offset in ±this many decibels, on top of `gain`.
     *
     * The other half of breaking up a repeated clip, and the one that matters more: a crowd of
     * identical footsteps reads as mechanical because they are all the same *loudness* before it
     * reads as one because they are the same pitch. Off unless asked for, like the pitch variation.
     *
     * Decibels rather than a linear fraction, for the reason the pitch is in semitones — loudness is
     * heard logarithmically, so ±0.2 linear is a much bigger step down than up.
     *
     * **The loud half amplifies.** Symmetric in decibels means the top of the range multiplies the
     * gain, by about 1.26 at the default ±2dB, and gain above 1.0 clips exactly as the note on the
     * gain functions says. Leave headroom — a base gain of 0.8 rather than 1.0 — on anything played
     * this way at full level.
     *
     * Folded into the voice's remembered gain, so it survives a later master or category change
     * rather than being lost the first time a slider moves. Rolled once, when the sound starts.
     * */
    f32 gain_variation_db;

    /**
     * Playback rate, 1.0 unchanged. 2.0 is an octave up and twice as fast.
     *
     * Resampling, not a pitch shift: the sound gets shorter as it gets higher, the way speeding up a
     * record does. Right for engine notes and impact variation, wrong for keeping a voice line
     * intelligible.
     * */
    f32 pitch;

    /**
     * Detunes this instance by a random offset in ±this many semitones, on top of `pitch`.
     *
     * Zero plays the clip exactly as authored, which is why it is off unless asked for — a menu
     * click or a voice line wants to sound identical every time. The point of it is repetition:
     * footsteps, impacts, gunfire, anything the player hears often enough to notice is one recording.
     *
     * Twelve is an octave, so the useful range is small. NYA_AUDIO_PITCH_VARIATION_SEMITONES is the
     * subtle default nya_audio_play_sound_varied uses.
     *
     * Applied once, when the sound starts, so a looping sound keeps whatever detune it was given
     * rather than warbling. Resampling like `pitch`, so a detuned instance is also slightly shorter
     * or longer than the clip.
     * */
    f32 pitch_variation_semitones;

    /** -1 hard left, 0 centred, +1 hard right. */
    f32 pan;

    b8  loop;
    u32 fade_in_ms;

    /**
     * Who wins when every voice is busy.
     *
     * Higher is more important. A sound arriving at a full pool takes the voice of the lowest
     * priority sound playing, but only if it outranks it — so equal priorities never steal from each
     * other and the pool degrades by dropping new sounds of the same rank, which is the behaviour a
     * crowd of footsteps wants.
     *
     * Zero for ambient noise, higher for anything the player needs to hear. Without this a critical
     * cue can lose its voice to sixteen footsteps that happened to start first.
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
 *
 * Sound effects and music mix into their own buses before being combined, so an effect can be put
 * on one without touching the other — which is the difference between the world going muffled and
 * the whole soundtrack going with it.
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
 *
 * One pole low pass and nothing else, deliberately: it is what "muffled" means, and SDL_mixer has no
 * DSP of its own, so everything here is hand written into its post-mix callback. There is no reverb,
 * no occlusion and no doppler to reach for.
 * */
struct NYA_AudioFilter {
    /**
     * Frequency above which the bus is rolled off, in hertz. Zero is off.
     *
     * A gentle 6dB per octave slope rather than a wall, so this is the point where the sound starts
     * to dull rather than a hard edge. Roughly: 4000 and up is barely noticeable, 800 is a wall
     * between you and the room, 300 is underwater or unconscious.
     *
     * Off is exactly off — a bus with no filter passes samples through untouched rather than through
     * a wide open filter — so leaving this at zero costs nothing but a compare.
     * */
    f32 lowpass_hz;

    /**
     * How long to take reaching a new setting, in milliseconds. Zero snaps.
     *
     * Snapping is audible as a click, because the filter's response jumps between one buffer and the
     * next, so anything the player triggers wants a glide. 50 to 150 is the usual range; longer
     * sounds like the effect is fading in, which is sometimes the point.
     *
     * The time to sweep the whole range, so a small change gets there proportionally sooner.
     * */
    f32 glide_ms;
};

/**
 * How a 2D world position is laid into the mixer's 3D space.
 *
 * The engine has no opinion on what the screen means, and the two common answers need opposite
 * things from the y axis, so the game says which it is once rather than at every call site.
 *
 * Both assume the renderer's convention, y down — see render2d.h. x is the same in either.
 * */
enum NYA_AudioPlane {
    /**
     * Side on, the platformer case: the screen is a wall, so y is height.
     *
     * World y is negated on the way in, because down the screen is down in the world while the
     * mixer's y is up. Nothing lands on z, so sounds are placed purely left, right and above.
     * */
    NYA_AUDIO_PLANE_SIDE,

    /**
     * Top down: the screen is the ground, so y is depth away from the listener.
     *
     * World y goes to z, where positive is behind — which is what down the screen means when the
     * camera looks along the ground. Nothing lands on the mixer's y, since nothing is overhead.
     * */
    NYA_AUDIO_PLANE_TOP_DOWN,

    NYA_AUDIO_PLANE_COUNT,
};

/**
 * Where the player hears from, in world units.
 *
 * The mixer's own listener is nailed to the origin and cannot be moved, so this is what world
 * positions are made relative to before they are handed over. Set it once per frame, from whatever
 * the game considers the ear.
 *
 * **Usually the player, not the camera.** They are the same thing in a game that keeps the player
 * centred, and they diverge exactly when it matters — a camera that leads, pans to a vista, or
 * shakes would drag the whole soundscape with it. The camera is also per window while audio is
 * global, so there is no one camera to read even if that were wanted.
 * */
struct NYA_AudioListener {
    /** World units, the same space nya_audio_play_sound_at is given. */
    f32x2 position;

    /**
     * How far a sound gets before it starts fading, in world units.
     *
     * Inside this radius a sound plays at full gain; past it the gain falls off as
     * `reference_distance / distance`, so at twice the reference it is half as loud, and at ten
     * times a tenth. That is the mixer's whole distance model — OpenAL's inverse-distance-clamped
     * with a rolloff of one — and there is no cutoff, so a distant sound gets quiet but never
     * silent. Pair it with `priority` if far away should mean droppable.
     *
     * Sets the scale of the world for audio, so it is worth thinking of in tiles: a value of a few
     * tiles keeps sound local, and a value of a hundred makes almost everything sound close.
     *
     * Zero is read as unspecified and becomes 1.0, which is only sensible if a world unit is already
     * the intended earshot.
     * */
    f32 reference_distance;

    /** Side on unless said otherwise, which is the zero value. */
    NYA_AudioPlane plane;
};

/**
 * The ear in a 3D scene: a point *and* the direction it faces.
 *
 * Separate from NYA_AudioListener rather than an extra field on it, because the two answer different
 * questions. The 2D listener has a position and a plane, and the plane is a fixed statement about which
 * world axis maps to which audio axis — right for a game whose camera never turns, and unable to express
 * one that does. An orbiting camera moves the *whole frame* every mouse drag: a sound to the player's
 * left has to cross to the right as the camera swings past it, and no choice of plane produces that.
 *
 * So this one carries an orientation. Set it every frame from wherever the scene is viewed from; see
 * nya_audio_listener_3d_set for what the two setters do to each other.
 *
 * `forward` and `up` need not be unit vectors and need not be exactly perpendicular — they are
 * orthonormalised on the way in, the way a look-at matrix does it, so handing over a camera's aim
 * direction and a world up works without any preparation.
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
     *
     * The same inverse-distance falloff and the same absence of a cutoff. Zero is read as unspecified
     * and becomes 1.0.
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
     *
     * The reason a track can have an intro: play from zero, and on repeat go back to here rather
     * than to the beginning. Zero loops the whole piece, which is what most tracks want.
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
 *
 * Called by the app; a game does not call this. Runs after the asset system, which is what owns the
 * mixer these tracks belong to.
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
 *
 * `gain` scales this instance alone, on top of the effect and master gains — 1.0 for unchanged. Use
 * it for a quieter variant of the same clip, not as a volume setting.
 *
 * Does nothing, quietly, when the asset is missing or still loading, or when every voice is busy.
 * All three are ordinary rather than exceptional: assets load asynchronously, and a game that fires
 * more sounds than it has voices during an explosion is working as intended.
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound(NYA_ConstCString sound_handle, f32 gain);

/**
 * Plays a sound asset once, detuned by ±NYA_AUDIO_PITCH_VARIATION_SEMITONES and levelled by
 * ±NYA_AUDIO_GAIN_VARIATION_DB.
 *
 * What to reach for on anything the player hears repeatedly, where the same waveform every time is
 * what makes a footstep sound like a machine. Identical to nya_audio_play_sound otherwise, failure
 * modes included.
 *
 * `gain` is the centre of the range rather than a ceiling, so the loud half of it goes above what is
 * passed. Give it a value with some headroom — 0.8 rather than 1.0 — or the top of the range clips.
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
 *
 * Cheap and idempotent, so the ordinary use is to set it every frame from the player's position
 * rather than to track when it changed. Sounds already playing are not re-placed by this — a voice
 * keeps the position it was given until something sets it again, which is what
 * nya_audio_voice_set_world_position is for.
 * */
NYA_API void              nya_audio_listener_set(NYA_AudioListener listener);
NYA_API NYA_AudioListener nya_audio_listener_get(void) __attr_no_discard;

/**
 * Moves the ear in a 3D scene, position and facing. See NYA_AudioListener3D.
 *
 * Cheap and idempotent like the 2D setter, so the ordinary use is to call it every frame from the
 * camera rather than to track when it changed.
 *
 * **This and nya_audio_listener_set are two ears, not one.** Each is read only by the placement function
 * that belongs to it — nya_audio_play_sound_at uses the 2D listener, nya_audio_play_sound_at_3d uses this
 * one — so a game with both a 2D and a 3D scene sets whichever the scene it is showing uses, and neither
 * disturbs the other. What it must not do is place a sound through the function whose listener it did not
 * set, which is a sound measured against an ear that was left wherever it last was.
 * */
NYA_API void                nya_audio_listener_3d_set(NYA_AudioListener3D listener);
NYA_API NYA_AudioListener3D nya_audio_listener_3d_get(void) __attr_no_discard;

/**
 * Plays a sound at a point in the world, heard from wherever the listener is.
 *
 * The positional form of nya_audio_play_sound_with, and identical to it otherwise — same failure
 * modes, same zero-means-default parameters, same variation fields. The placement is applied before
 * the first sample is mixed rather than corrected afterwards, so a sound never starts centred and
 * jumps.
 *
 * ```c
 * nya_audio_listener_set((NYA_AudioListener){ .position = player_position, .reference_distance = 8.0F });
 *
 * nya_audio_play_sound_at(NYA_ASSET_SFX_PICKAXE_WAV, ore_position, (NYA_SoundParams){
 *     .gain = 0.8F, .pitch_variation_semitones = 1.0F, .gain_variation_db = 2.0F,
 * });
 * ```
 *
 * `params.pan` is ignored here: panning and 3D placement decide the same thing, and this is the one
 * that was asked for.
 *
 * Positional playback is mixed down to mono by SDL_mixer, since a sound coming from somewhere has to
 * be put back across the speakers. A clip whose stereo image is the point of it — most music, some
 * ambience — wants nya_audio_play_sound_with and a pan instead.
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound_at(NYA_ConstCString sound_handle, f32x2 world_position, NYA_SoundParams params);

/**
 * Plays a sound at a point in a 3D world, heard from wherever the 3D listener is and however it faces.
 *
 * The 3D counterpart of nya_audio_play_sound_at, identical in every respect but which listener it
 * measures against — same parameters, same failure modes, same mixing to mono.
 *
 * ```c
 * nya_audio_listener_3d_set((NYA_AudioListener3D){
 *     .position = camera_position, .forward = target - camera_position, .up = { 0, 1, 0 },
 *     .reference_distance = 8.0F,
 * });
 *
 * nya_audio_play_sound_at_3d(NYA_ASSET_SOUNDS_HIT_WAV, impact_point, (NYA_SoundParams){ .gain = 0.8F });
 * ```
 *
 * What this gets that no arrangement of the 2D call could: the sound is placed in the listener's own
 * frame, so orbiting the camera swings it across the speakers, and a sound overhead is overhead rather
 * than being folded onto whichever axis a plane named.
 *
 * What it still does not get, because SDL_mixer does not have it: occlusion, reverb and doppler. A sound
 * behind a hill is as loud as one in front of it. See the note at the top of this file.
 * */
NYA_API NYA_SoundVoice nya_audio_play_sound_at_3d(NYA_ConstCString sound_handle, f32x3 world_position, NYA_SoundParams params);

/*
 * ─────────────────────────────────────────────────────────
 * MUSIC
 * ─────────────────────────────────────────────────────────
 */

/**
 * Starts `music_handle` on the music track, replacing whatever was playing.
 *
 * One track, so this is a swap rather than a second stream. `loop` repeats it indefinitely, which is
 * what a background track normally wants; false plays it once and stops.
 *
 * `fade_in_ms` ramps the gain up from silence, and pairs with the fade on nya_audio_stop_music. Zero
 * starts at full gain, which on a track with no lead-in is an audible click.
 * */
NYA_API void nya_audio_play_music(NYA_ConstCString music_handle, b8 loop, u32 fade_in_ms);

/** Starts music with loop points and a gain, for anything the three argument form cannot say. */
NYA_API void nya_audio_play_music_with(NYA_ConstCString music_handle, NYA_MusicParams params);

/**
 * Fades the current track out while fading the new one in, over `duration_ms`.
 *
 * Two music tracks exist for exactly this, so the pieces genuinely overlap rather than one starting
 * where the other stopped. What an area transition wants; a hard cut is nya_audio_play_music.
 *
 * Calling this again while a crossfade is still running starts a new one from wherever the two
 * tracks currently are, which cuts the outgoing piece short but never leaves three playing.
 * */
NYA_API void nya_audio_crossfade_music(NYA_ConstCString music_handle, NYA_MusicParams params, u32 duration_ms);

/**
 * The music track as a voice, so the effect functions below apply to it.
 *
 * Valid only while music is playing. Re-read it after each nya_audio_play_music rather than holding
 * one across tracks — starting a new piece bumps the generation, exactly as a sound voice does.
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
 *
 * Remembered, so a later change to the master or category gain keeps this voice's relative level
 * rather than resetting it to full.
 * */
NYA_API void nya_audio_voice_set_gain(NYA_SoundVoice voice, f32 gain);

/** Playback rate, 1.0 unchanged. See NYA_SoundParams.pitch — this resamples rather than pitch shifts. */
NYA_API void nya_audio_voice_set_pitch(NYA_SoundVoice voice, f32 ratio);

/**
 * Stereo placement, -1 hard left through 0 centred to +1 hard right.
 *
 * Equal power rather than linear: a linear pan sounds like it dips in volume as it crosses the
 * middle, because two half-gain channels carry less power than one at full. This holds the perceived
 * loudness steady across the sweep.
 *
 * Mutually exclusive with nya_audio_voice_set_position — both decide the same thing, so the last one
 * called wins.
 * */
NYA_API void nya_audio_voice_set_pan(NYA_SoundVoice voice, f32 pan);

/**
 * Places the sound in 3D relative to the listener, who is at the origin facing -z.
 *
 * x is right, y is up, z is back, in units where 1.0 is where attenuation begins — the raw form of
 * what NYA_AudioListener.reference_distance scales world units into. Overrides any pan on this voice.
 *
 * The escape hatch, for a game whose space neither NYA_AudioPlane describes. Anything working in
 * world coordinates wants nya_audio_voice_set_world_position, which is this with the listener
 * subtracted off first.
 * */
NYA_API void nya_audio_voice_set_position(NYA_SoundVoice voice, f32x3 position);

/**
 * Moves a playing sound to a point in the world, through the listener.
 *
 * What a sound attached to something that moves needs, called each frame with the emitter's
 * position: a mine cart, a dripping ceiling the player walks past, a machine that was started as a
 * looping voice. Ignores a handle whose sound has finished, like every other effect here, so a
 * caller updating one every frame needs no liveness check.
 *
 * Re-reads the listener on every call, so walking past a stationary emitter works by updating the
 * listener alone — but the emitter's own voice still has to be told when *it* moves.
 * */
/**
 * Puts a low pass on one voice alone. See NYA_AudioFilter for what the filter is and is not.
 *
 * The per-voice counterpart of nya_audio_bus_filter_set, and the piece the engine was missing: a bus
 * filter muffles *everything* on it, which is right for the player going underwater and useless for the
 * one sound that happens to be behind a wall. It runs on SDL_mixer's per-track DSP hook, so it is applied
 * before the voice reaches its bus and both filters compose.
 *
 * A zero cutoff is wide open, which is the resting state and is how a voice is un-muffled.
 *
 * Overwritten wholesale by nya_audio_occlusion_update on any voice that automatic occlusion is driving.
 * The two are the same slot; a caller wanting manual control over a particular sound should leave it out
 * of whatever their occlusion callback reports on.
 * */
NYA_API void nya_audio_voice_filter_set(NYA_SoundVoice voice, NYA_AudioFilter filter);

/**
 * How a sound is treated when something is between it and the listener.
 *
 * Muffling rather than only quietening, for the reason NYA_AudioFilter gives: the ear identifies a
 * blocked sound by its missing treble, and a wall that merely turns the volume down reads as distance.
 * Both are applied, because a real obstruction does both.
 * */
struct NYA_AudioOcclusion {
    /**
     * Cutoff in hertz at full occlusion. Zero disables the filtering half.
     *
     * Somewhere around 700 is a solid wall and 2000 is a curtain. Interpolated from wide open toward this
     * as the occlusion factor rises, so a partly blocked sound is partly muffled.
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
 * ## Why a callback rather than a raycast in here
 *
 * The engine's audio layer has no business knowing about its physics layer, and the dependency would run
 * the wrong way — physics is built on entities, which are built on core. More usefully, "what is between
 * these two points" is not one question: a 3D game raycasts a solver, a 2D one walks a tilemap, and a
 * game with a portal system has its own answer entirely.
 *
 * The callback is handed a world position and returns how occluded it is, from 0 for clear line of sight
 * to 1 for fully blocked. Anything in between is honoured, so casting several rays and returning the
 * fraction that hit gives a soft edge for free.
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
 *
 * Nothing is called until nya_audio_occlusion_update runs, so the callback is never invoked from the
 * mixer's thread and may do whatever a normal frame may do.
 * */
NYA_API void nya_audio_occlusion_set(NYA_AudioOcclusionFn function, void* user_data, NYA_AudioOcclusion occlusion);

/**
 * Re-evaluates occlusion for every positional voice that is still playing. Call it once a frame.
 *
 * Costs one callback per live positional voice, which is bounded by NYA_AUDIO_VOICES — so the expensive
 * part is whatever the callback does, and the engine's contribution is a loop over sixteen slots.
 *
 * Does nothing when no callback is installed, so it is safe to call unconditionally from a frame loop
 * that does not know whether the current scene uses occlusion.
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
 *
 * The master applies to everything. The other two are the categories an options menu exposes
 * separately, and each multiplies the master rather than replacing it.
 */
/*
 * ─────────────────────────────────────────────────────────
 * BUS FILTERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Puts a filter on a whole bus, or takes it off.
 *
 * The global effect: everything on that bus is rolled off together, whatever is playing and whatever
 * starts later. What "the world went muffled" is made of.
 *
 * ```c
 * // Headphones on: the world dulls over a tenth of a second, the UI is on music and stays crisp.
 * nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .lowpass_hz = 700.0F, .glide_ms = 120.0F });
 *
 * // Headphones off. Zero is off, and it glides back rather than snapping open.
 * nya_audio_bus_filter_set(NYA_AUDIO_BUS_SOUND, (NYA_AudioFilter){ .glide_ms = 120.0F });
 * ```
 *
 * Cheap and idempotent, so setting it every frame from game state is fine — it stores two numbers,
 * and the work happens on the mixer's thread regardless.
 *
 * Negative values are clamped to zero rather than refused. A device with more channels than
 * NYA_AUDIO_FILTER_MAX_CHANNELS is passed through unfiltered, on the grounds that unfiltered audio
 * beats audio filtered on the wrong channels.
 *
 * Does nothing when there is no audio device, like everything else here.
 * */
/**
 * A room around a bus: how big it sounds and how much of it is heard.
 *
 * A Schroeder network — four parallel comb filters into two series allpasses, per channel. It is the
 * oldest design there is and the right one here for two reasons: it needs no allocation on the audio
 * thread, which a convolution reverb does, and it produces a *smooth, characterless* tail rather than a
 * recognisable room, which is what a game wants when one setting has to serve every cave in it.
 *
 * What it is not: a convolution of a measured impulse response, which is what "this exact hall" needs;
 * and not per-source, since it sits on a bus. A sound that should be dry while everything else is wet
 * wants its own bus, and there are three.
 *
 * The tail is fed by everything on the bus, so a filter set on the same bus is heard *before* it — a
 * muffled sound reverberates muffled, which is what a room does.
 * */
struct NYA_AudioReverb {
    /**
     * How long the tail rings, roughly 0 to 1. Zero switches the reverb off entirely.
     *
     * This is the comb filters' feedback, so it is not a time in seconds and does not scale like one:
     * the last tenth of the range is most of the length. Around 0.5 is a room, 0.8 a hall, and past
     * about 0.98 the network stops decaying and rings forever.
     * */
    f32 room_size;

    /**
     * How fast the high frequencies die away inside the tail, 0 to 1.
     *
     * Zero is a bright, tiled, unnatural room. Real rooms absorb treble faster than bass, so anything
     * meant to sound like a place wants this well above zero — around 0.5 is soft furnishings.
     * */
    f32 damping;

    /** How much reverberated signal is added. Zero is unspecified and becomes a modest 0.3. */
    f32 wet;

    /**
     * How much of the original passes through. Zero is unspecified and becomes 1.0, i.e. untouched.
     *
     * Worth lowering only for something meant to sound distant — a dry level of zero is the sound heard
     * *entirely* through the room, which is what a source in another chamber sounds like.
     * */
    f32 dry;

    /**
     * How far apart the two channels' rooms are, 0 to 1. Zero becomes 1.0, fully wide.
     *
     * The two channels run networks of slightly different lengths, which is what stops the tail
     * collapsing into the middle of the stereo image and sounding like a mono effect bolted on.
     * */
    f32 width;
};

/**
 * Puts a reverb on a bus, or takes it off. See NYA_AudioReverb.
 *
 * Cheap and idempotent like the filter setter, and safe before the audio device exists — so a game can
 * configure its rooms at startup and switch between them by calling this again.
 *
 * A `room_size` of zero disables it, and disabling costs nothing per buffer: the whole network is skipped
 * rather than run with a feedback of nothing.
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
 *
 * The target, not where a glide has currently reached — this reads back what was asked for, so
 * setting and getting round trips. Where the glide actually is belongs to the mixer's thread and is
 * not observable.
 * */
NYA_API NYA_AudioFilter nya_audio_bus_filter_get(NYA_AudioBus bus) __attr_no_discard;

NYA_API void nya_audio_set_master_gain(f32 gain);
NYA_API void nya_audio_set_sound_gain(f32 gain);
NYA_API void nya_audio_set_music_gain(f32 gain);

NYA_API f32 nya_audio_master_gain(void) __attr_no_discard;
NYA_API f32 nya_audio_sound_gain(void) __attr_no_discard;
NYA_API f32 nya_audio_music_gain(void) __attr_no_discard;
