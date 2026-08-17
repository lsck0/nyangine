#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_AudioSystem      NYA_AudioSystem;
typedef struct NYA_AudioFilterState NYA_AudioFilterState;
typedef struct NYA_AudioReverbState NYA_AudioReverbState;

/*
 * ─────────────────────────────────────────────────────────
 * REVERB
 * ─────────────────────────────────────────────────────────
 *
 * A Schroeder network: four comb filters in parallel, summed, then two allpasses in series. Per channel,
 * with the second channel's delays offset so the two rooms are not the same room.
 *
 * The delay lengths are the classic Freeverb set, in samples at 44.1 kHz. They are mutually prime on
 * purpose — lengths sharing a factor make their echoes land on top of each other, which is heard as a
 * metallic ring rather than as a room.
 */

/** Comb delays at 44.1 kHz, in samples. */
#define _NYA_AUDIO_REVERB_COMBS 4

NYA_INTERNAL const u32 _NYA_AUDIO_REVERB_COMB_LENGTHS[_NYA_AUDIO_REVERB_COMBS] = { 1116, 1188, 1277, 1356 };

/** Allpass delays at 44.1 kHz, in samples. */
#define _NYA_AUDIO_REVERB_ALLPASSES 2

NYA_INTERNAL const u32 _NYA_AUDIO_REVERB_ALLPASS_LENGTHS[_NYA_AUDIO_REVERB_ALLPASSES] = { 556, 441 };

/**
 * How far the right channel's delays are offset from the left's, in samples at 44.1 kHz.
 *
 * Twenty-three, and prime for the same reason the lengths are: an offset sharing a factor with a comb
 * length puts the two channels' echoes back in step at intervals, which is heard as the stereo image
 * pulsing rather than as width.
 * */
#define _NYA_AUDIO_REVERB_STEREO_SPREAD 23

/**
 * The longest delay any line can need, in samples.
 *
 * The lengths above are at 44.1 kHz and scale with the sample rate: the longest comb plus the stereo
 * offset is 1379 there, so this covers rates up to about 96 kHz. A device running faster than that gets
 * lengths clamped to this, which is a slightly smaller room and inaudible.
 *
 * Fixed rather than allocated because this is touched only from the mixer's thread, where allocating is
 * not allowed at all.
 * */
#define _NYA_AUDIO_REVERB_MAX_DELAY 3072

/**
 * How many networks are run, regardless of how many speakers there are.
 *
 * Two, always. A reverb tail is a stereo impression rather than a per-speaker signal, and running six
 * independent networks for a 5.1 device would cost three times as much to produce something *less*
 * coherent — six uncorrelated tails do not localise, they smear.
 *
 * So the bus is downmixed into a left and a right feed, the two networks run on those, and the result is
 * added back across every channel by parity. A surround device therefore hears one room from all its
 * speakers, which is what a room is.
 * */
#define _NYA_AUDIO_REVERB_NETWORKS 2

/** One comb filter: a delay line with a damped feedback path. */
typedef struct {
    f32 buffer[_NYA_AUDIO_REVERB_MAX_DELAY];
    u32 length;
    u32 cursor;

    /** The one-pole state that makes the feedback lose treble on each pass. See NYA_AudioReverb.damping. */
    f32 damped;
} NYA_AudioReverbComb;

/** One allpass: a delay line that scatters phase without colouring the magnitude response. */
typedef struct {
    f32 buffer[_NYA_AUDIO_REVERB_MAX_DELAY];
    u32 length;
    u32 cursor;
} NYA_AudioReverbAllpass;

/**
 * One bus's reverb: what the game asked for, and the network the mixer's thread runs.
 *
 * Split exactly like NYA_AudioFilterState, and for the same reason: the atomics are written by whoever
 * calls the setter and read in the callback, and everything below them belongs to the audio thread alone.
 * Nothing is shared in both directions, so there is no lock on the mixing path.
 * */
struct NYA_AudioReverbState {
    /* ── Written by the game, read by the mixer ── */

    atomic f32 room_size;
    atomic f32 damping;
    atomic f32 wet;
    atomic f32 dry;
    atomic f32 width;

    /* ── The mixer's thread alone ── */

    NYA_AudioReverbComb    combs[_NYA_AUDIO_REVERB_NETWORKS][_NYA_AUDIO_REVERB_COMBS];
    NYA_AudioReverbAllpass allpasses[_NYA_AUDIO_REVERB_NETWORKS][_NYA_AUDIO_REVERB_ALLPASSES];

    /** The rate the delay lengths were computed for. A change re-derives them and clears the lines. */
    s32 configured_rate;
};

/**
 * One bus's filter: what the game asked for, and where the mixer's thread has got to.
 *
 * The split down the middle is the whole design. The two atomics are written by whoever calls
 * nya_audio_bus_filter_set and read by the audio thread; everything below them is touched only by
 * the audio thread, inside the callback. Nothing is shared in both directions, so there is no lock
 * on the mixing path — which there could not be anyway, since blocking in a post-mix callback
 * drops audio.
 * */
struct NYA_AudioFilterState {
    /*
     * ── Written by the game, read by the mixer ──
     *
     * Relaxed on both sides: these are two independent scalars with no ordering between them and
     * nothing else published alongside. The worst a torn pair can do is spend one buffer gliding at
     * the previous rate, which is inaudible and self correcting on the next.
     */
    atomic f32 target_hz;
    atomic f32 glide_ms;

    /*
     * ── The mixer's thread alone ──
     */

    /**
     * The one pole's current coefficient, in [0, 1], where 1 is wide open.
     *
     * Held rather than recomputed because it is what glides: jumping straight to a new cutoff is the
     * click that glide_ms exists to avoid. One is the resting value, and it makes the filter an
     * exact identity rather than a very wide filter.
     * */
    f32 coefficient;

    /** The previous output, per channel, which is all a one pole remembers. */
    f32 state[NYA_AUDIO_FILTER_MAX_CHANNELS];
};

/**
 * One playable slot.
 *
 * `generation` is what makes a handle safe to hold: it is bumped every time the slot starts a new
 * sound, so a handle from the sound before stops resolving. `base_gain` is remembered rather than
 * pushed straight to the track, so that a later change to the category or master gain can be folded
 * in without losing what the caller asked for.
 * */
typedef struct {
    MIX_Track* track;
    u32        generation;
    f32        base_gain;

    /** What this voice has to be outranked by before it can be stolen. See NYA_SoundParams.priority. */
    s32 priority;

    /**
     * This voice's own low pass, run on SDL_mixer's per-track hook before it reaches its bus.
     *
     * The piece that made occlusion possible. A bus filter muffles everything on the bus, which cannot
     * express "that one sound is behind a wall" — and there was nowhere else to put a filter, because a
     * track had no DSP stage of its own until this was hung on one.
     * */
    NYA_AudioFilterState filter;

    /**
     * Where in the world this voice was placed, and whether it was placed at all.
     *
     * Remembered because occlusion has to ask about it every frame and the position was previously
     * pushed straight to the track and forgotten — SDL_mixer stores it listener-relative, so reading it
     * back would give an answer in the wrong space and one frame out of date.
     * */
    f32x3 world_position;
    b8    positional;

    /** The gain occlusion is currently applying, so it can be folded out again when the way clears. */
    f32 occlusion_gain;
} NYA_AudioVoice;

/*
 * Music is the slot past the effect pool rather than a separate field.
 *
 * It behaves identically once playing — same effects, same gain arithmetic — so giving it a slot
 * means one lookup path and one set of effect functions instead of two of each. It is still never
 * handed out by the free-voice search, which only walks the first NYA_AUDIO_VOICES entries.
 */
/*
 * Two music slots, not one, and that is what makes a crossfade possible.
 *
 * A crossfade needs both pieces sounding at once, so a single track could only ever cut. The pair is
 * used alternately: whichever is not currently playing is the one the next track starts on, and the
 * two swap roles. Neither is ever handed out by the free-voice search, which walks only the first
 * NYA_AUDIO_VOICES entries.
 */
#define _NYA_AUDIO_MUSIC_A NYA_AUDIO_VOICES
#define _NYA_AUDIO_MUSIC_B (NYA_AUDIO_VOICES + 1)
#define _NYA_AUDIO_SLOTS   (NYA_AUDIO_VOICES + 2)

struct NYA_AudioSystem {
    NYA_AudioVoice slots[_NYA_AUDIO_SLOTS];

    /**
     * Drives per instance pitch variation, and nothing else.
     *
     * Its own generator rather than one the game shares, because how many sounds played is not
     * something a seeded run should be able to feel: a footstep drawing from the simulation's stream
     * would shift every value after it, and the same seed would stop reproducing the same world.
     *
     * By value in a static, which is what keeps NYA_RNG's 32 byte alignment satisfied for free —
     * see nya_rng_create_in for the arena case, where it is not free at all.
     * */
    NYA_RNG rng;

    /** Where world positions are heard from. See NYA_AudioListener. */
    NYA_AudioListener listener;

    /** The 3D scene's ear. Independent of `listener`; see nya_audio_listener_3d_set. */
    NYA_AudioListener3D listener_3d;

    /** One room per bus. Runs after that bus's filter, so a muffled sound reverberates muffled. */
    NYA_AudioReverbState reverbs[NYA_AUDIO_BUS_COUNT];

    /** How blocked sounds, and what decides it. Null function means occlusion is off. */
    NYA_AudioOcclusionFn occlusion_function;
    void*                occlusion_user_data;
    NYA_AudioOcclusion   occlusion;

    /**
     * The buses effects and music mix through, indexed by NYA_AudioBus.
     *
     * The master entry stays null: it is the mixer itself rather than a group, and its filter hangs
     * off MIX_SetPostMixCallback instead. Indexing by bus anyway keeps one array and one loop.
     * */
    MIX_Group* groups[NYA_AUDIO_BUS_COUNT];

    NYA_AudioFilterState filters[NYA_AUDIO_BUS_COUNT];

    /** Which of the two music slots is the one currently considered "the music". */
    u32 music_slot;

    f32 master_gain;
    f32 sound_gain;
    f32 music_gain;

    b8 ready;
};

NYA_INTERNAL NYA_AudioSystem _nya_audio_system = {
    .music_slot  = _NYA_AUDIO_MUSIC_A,
    .master_gain = 1.0F,
    .sound_gain  = 1.0F,
    .music_gain  = 1.0F,
};

/** The sound asset behind a handle, or null when it is missing, failed or still loading. */
NYA_INTERNAL MIX_Audio* _nya_audio_get(NYA_ConstCString handle);

/** Reapplies the music gain, which is the product of the master and music gains. */
NYA_INTERNAL void _nya_audio_apply_music_gain(void);

/** The slot a handle names, or null when the handle is stale, out of range, or the system is down. */
NYA_INTERNAL NYA_AudioVoice* _nya_audio_resolve(NYA_SoundVoice voice);

/** Pushes a slot's remembered gain through its category and the master onto the track. */
NYA_INTERNAL void _nya_audio_apply_gain(u32 slot);

/**
 * Everything nya_audio_play_sound_with does, with an optional placement applied before the first
 * sample is mixed. Null plays unplaced, which is the non positional case.
 * */
NYA_INTERNAL NYA_SoundVoice _nya_audio_play(NYA_ConstCString sound_handle, NYA_SoundParams params, const f32x3* position);

/** Records where a voice is in the world, for occlusion to ask about later. A no-op for a dead handle. */
NYA_INTERNAL void _nya_audio_voice_remember_position(NYA_SoundVoice voice, f32x3 world_position);

/**
 * A world point as the mixer wants it: relative to the listener, on the axes its plane names, and
 * scaled so that 1.0 is where attenuation starts.
 * */
NYA_INTERNAL f32x3 _nya_audio_world_to_audio(f32x2 world_position) __attr_no_discard;

/** The same, for a 3D world point against the 3D listener: into its own frame, then scaled. */
NYA_INTERNAL f32x3 _nya_audio_world_to_audio_3d(f32x3 world_position) __attr_no_discard;

/*
 * Pan and placement against a track rather than a voice handle.
 *
 * Both are needed from inside the play path, where the voice does not exist yet — its generation is
 * bumped only once the sound is actually running, so a handle built there resolves against the
 * *previous* sound and every setter called with it does nothing. That is not hypothetical: it is
 * what silently dropped NYA_SoundParams.pan on the floor before these existed.
 */
NYA_INTERNAL void _nya_audio_track_set_pan(MIX_Track* track, f32 pan);
NYA_INTERNAL void _nya_audio_track_set_position(MIX_Track* track, f32x3 position);

/** Puts a filter back to wide open with no history. The resting state, which is not all zeroes. */
NYA_INTERNAL void _nya_audio_filter_reset(NYA_AudioFilterState* filter);

/**
 * Rolls `pcm` off in place, gliding toward whatever cutoff was last asked for.
 *
 * **Runs on the mixer's thread.** No allocation, no logging, no locks, and nothing that could block:
 * this is called with a deadline, and overrunning it is a dropout. `samples` counts floats rather
 * than sample frames, which is SDL_mixer's convention and not the obvious reading.
 * */
NYA_INTERNAL void _nya_audio_filter_apply(NYA_AudioFilterState* filter, const SDL_AudioSpec* spec, f32* pcm, s32 samples);

/** Sizes the delay lines for a sample rate and clears them. Called when the rate is first seen or changes. */
NYA_INTERNAL void _nya_audio_reverb_configure(NYA_AudioReverbState* reverb, s32 rate);

/**
 * Adds a reverberated copy of `pcm` back into it, in place.
 *
 * **Runs on the mixer's thread**, under the same rules the filter does: no allocation, no logging, no
 * locks, nothing that could block. It is the heavier of the two — six delay lines per channel per frame —
 * which is why it returns immediately when the room size is zero.
 * */
NYA_INTERNAL void _nya_audio_reverb_apply(NYA_AudioReverbState* reverb, const SDL_AudioSpec* spec, f32* pcm, s32 samples);

/* The three shapes SDL_mixer wants that filter in. All are the same call with a different owner. */
NYA_INTERNAL void SDLCALL _nya_audio_track_mix_callback(void* userdata, MIX_Track* track, const SDL_AudioSpec* spec, float* pcm, int samples);
NYA_INTERNAL void SDLCALL _nya_audio_group_mix_callback(void* userdata, MIX_Group* group, const SDL_AudioSpec* spec, float* pcm, int samples);
NYA_INTERNAL void SDLCALL _nya_audio_post_mix_callback(void* userdata, MIX_Mixer* mixer, const SDL_AudioSpec* spec, float* pcm, int samples);

/** A uniform draw from ±`half_range`, and exactly zero when that is not positive. */
NYA_INTERNAL f32 _nya_audio_jitter(f32 half_range) __attr_no_discard;

/**
 * `pitch` detuned by a random offset within ±`semitones`. Returns it unchanged when that is zero.
 *
 * The offset is drawn in semitones and converted, rather than drawn as a ratio, because a ratio
 * range is not symmetric about 1.0: ±0.06 is 1.07 semitones up and 1.14 down, so uniform linear
 * jitter is biased flat and a crowd of sounds detunes downward on average. An exponent keeps the two
 * directions the same musical distance.
 * */
NYA_INTERNAL f32 _nya_audio_vary_pitch(f32 pitch, f32 semitones) __attr_no_discard;

/**
 * `gain` moved by a random offset within ±`db`. Returns it unchanged when that is zero.
 *
 * Decibels for the same reason the pitch is in semitones, and with the same shape: an offset drawn
 * uniformly in the exponent, so the two directions are the same perceived step. The result can
 * exceed `gain` — that is what symmetric means — and is not clamped, because a caller who wanted a
 * ceiling would have passed a lower gain.
 * */
NYA_INTERNAL f32 _nya_audio_vary_gain(f32 gain, f32 db) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_system_audio_init(void) {
    NYA_AudioSystem* system = &_nya_audio_system;

    // Ahead of the mixer check, so the generator is seeded on every path rather than only the one
    // that has a device. Unseeded it would hand every run the same sequence of detunes, which is the
    // one thing this is meant to avoid.
    system->rng = nya_rng_create();

    // A listener at the origin, hearing one world unit as the falloff distance. Set here rather than
    // left zeroed because a reference distance of zero is a division by it, and a game is entitled to
    // play a positional sound before it has said where the player is.
    system->listener = (NYA_AudioListener){ .reference_distance = 1.0F };

    // The graphics convention for an unset orientation: looking down -z with +y up, at the origin.
    system->listener_3d = (NYA_AudioListener3D){
        .forward            = { 0.0F, 0.0F, -1.0F },
        .up                 = { 0.0F, 1.0F, 0.0F },
        .reference_distance = 1.0F,
    };

    // Reached through the app rather than an accessor: the mixer belongs to the asset system,
    // which owns it because a MIX_Audio cannot outlive the mixer that decoded it.
    MIX_Mixer* mixer = nya_app_get()->asset_system.mixer;

    /*
     * No mixer means no audio device, which is the normal state in CI, on a headless build, and on a
     * machine whose sound server is not running. Recoverable rather than fatal: every call below
     * turns into a no-op and the game runs silently, which is a better outcome than refusing to
     * start.
     */
    if (mixer == nullptr) {
        nya_info("Audio system initialized (no mixer: nothing will be heard).");
        return NYA_OK;
    }

    for (u32 i = 0; i < NYA_AUDIO_BUS_COUNT; i++) _nya_audio_filter_reset(&system->filters[i]);

    /*
     * The effect and music buses, so a filter can be put on one without the other.
     *
     * Master is not among them: it is the mixer itself, and its filter runs from
     * MIX_SetPostMixCallback below, after every group has been mixed together.
     */
    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) {
        if (bus == NYA_AUDIO_BUS_MASTER) continue;

        system->groups[bus] = MIX_CreateGroup(mixer);
        if (system->groups[bus] == nullptr) return nya_error(NYA_ERROR_NOT_OK, "MIX_CreateGroup() failed for bus %u: %s", bus, SDL_GetError());

        if (!MIX_SetGroupPostMixCallback(system->groups[bus], _nya_audio_group_mix_callback, &system->filters[bus])) {
            return nya_error(NYA_ERROR_NOT_OK, "MIX_SetGroupPostMixCallback() failed for bus %u: %s", bus, SDL_GetError());
        }
    }

    if (!MIX_SetPostMixCallback(mixer, _nya_audio_post_mix_callback, &system->filters[NYA_AUDIO_BUS_MASTER])) {
        return nya_error(NYA_ERROR_NOT_OK, "MIX_SetPostMixCallback() failed: %s", SDL_GetError());
    }

    for (u32 i = 0; i < _NYA_AUDIO_SLOTS; i++) {
        system->slots[i] = (NYA_AudioVoice){ .track = MIX_CreateTrack(mixer), .generation = 0, .base_gain = 1.0F };
        if (system->slots[i].track == nullptr) return nya_error(NYA_ERROR_NOT_OK, "MIX_CreateTrack() failed for slot %u: %s", i, SDL_GetError());

        // Which bus a slot belongs to is fixed for the life of the process, since a slot is only
        // ever an effect voice or one of the two music tracks. A track left unassigned would mix
        // through SDL_mixer's internal default group, where no callback of ours can reach it.
        MIX_Group* group = i >= NYA_AUDIO_VOICES ? system->groups[NYA_AUDIO_BUS_MUSIC] : system->groups[NYA_AUDIO_BUS_SOUND];
        if (!MIX_SetTrackGroup(system->slots[i].track, group)) {
            return nya_error(NYA_ERROR_NOT_OK, "MIX_SetTrackGroup() failed for slot %u: %s", i, SDL_GetError());
        }

        /*
         * Every slot gets a filter, wide open, for the life of the process.
         *
         * Attached once rather than when something needs one, because installing a callback on a track
         * that is already playing races the mixer's thread — and an identity filter costs one multiply
         * and one add per sample, which is nothing against the decode it follows.
         */
        _nya_audio_filter_reset(&system->slots[i].filter);

        // One, not zero: it is a multiplier, and a zeroed slot would silence every voice. See
        // _nya_audio_apply_gain, which reads a non-positive value as "not occluded" for the same reason.
        system->slots[i].occlusion_gain = 1.0F;

        if (!MIX_SetTrackCookedCallback(system->slots[i].track, _nya_audio_track_mix_callback, &system->slots[i].filter)) {
            return nya_error(NYA_ERROR_NOT_OK, "MIX_SetTrackCookedCallback() failed for slot %u: %s", i, SDL_GetError());
        }
    }

    system->ready = true;

    nya_info("Audio system initialized (%d voices).", NYA_AUDIO_VOICES);
    return NYA_OK;
}

void nya_system_audio_deinit(void) {
    NYA_AudioSystem* system = &_nya_audio_system;

    if (!system->ready) {
        nya_info("Audio system deinitialized (no mixer).");
        return;
    }

    /*
     * Callbacks come off first, before anything they point at is torn down.
     *
     * They run on the mixer's thread, which is still going at this moment, and every one of them was
     * handed the address of a filter in this struct. Detaching them first is what makes the rest of
     * this teardown safe rather than a race against the next buffer.
     */
    MIX_Mixer* mixer = nya_app_get()->asset_system.mixer;
    if (mixer != nullptr) MIX_SetPostMixCallback(mixer, nullptr, nullptr);

    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) {
        if (system->groups[bus] != nullptr) MIX_SetGroupPostMixCallback(system->groups[bus], nullptr, nullptr);
    }

    // Tracks first, then the mixer they belong to — which the asset system destroys, and which is
    // why this runs before it.
    for (u32 i = 0; i < _NYA_AUDIO_SLOTS; i++) {
        if (system->slots[i].track != nullptr) MIX_DestroyTrack(system->slots[i].track);
        system->slots[i] = (NYA_AudioVoice){ 0 };
    }

    // After the tracks, so nothing is being reassigned to the default group on its way out.
    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) {
        if (system->groups[bus] != nullptr) MIX_DestroyGroup(system->groups[bus]);
        system->groups[bus] = nullptr;
    }

    system->ready = false;

    nya_info("Audio system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * SOUND EFFECTS
 * ─────────────────────────────────────────────────────────
 */

NYA_SoundVoice nya_audio_play_sound(NYA_ConstCString sound_handle, f32 gain) {
    return nya_audio_play_sound_with(sound_handle, (NYA_SoundParams){ .gain = gain, .pitch = 1.0F });
}

NYA_SoundVoice nya_audio_play_sound_varied(NYA_ConstCString sound_handle, f32 gain) {
    return nya_audio_play_sound_with(
        sound_handle,
        (NYA_SoundParams){
            .gain                      = gain,
            .gain_variation_db         = NYA_AUDIO_GAIN_VARIATION_DB,
            .pitch                     = 1.0F,
            .pitch_variation_semitones = NYA_AUDIO_PITCH_VARIATION_SEMITONES,
        }
    );
}

NYA_SoundVoice nya_audio_play_sound_with(NYA_ConstCString sound_handle, NYA_SoundParams params) {
    return _nya_audio_play(sound_handle, params, nullptr);
}

NYA_SoundVoice nya_audio_play_sound_at(NYA_ConstCString sound_handle, f32x2 world_position, NYA_SoundParams params) {
    f32x3 position = _nya_audio_world_to_audio(world_position);

    // Through the same path rather than "play, then place": handing the position down means it is on
    // the track before MIX_PlayTrack, so the sound cannot be heard centred for the first buffer and
    // then snap into place.
    NYA_SoundVoice voice = _nya_audio_play(sound_handle, params, &position);

    /*
     * The *world* position recorded afterwards, because it does not survive the call.
     *
     * What is handed down above is already listener-relative and divided by the reference distance —
     * which is what the mixer wants and is not a place occlusion can ask about. Recording it here keeps
     * one source of truth for where the sound actually is.
     */
    _nya_audio_voice_remember_position(voice, (f32x3){ world_position.x, world_position.y, 0.0F });

    return voice;
}

NYA_SoundVoice nya_audio_play_sound_at_3d(NYA_ConstCString sound_handle, f32x3 world_position, NYA_SoundParams params) {
    f32x3 position = _nya_audio_world_to_audio_3d(world_position);

    NYA_SoundVoice voice = _nya_audio_play(sound_handle, params, &position);

    _nya_audio_voice_remember_position(voice, world_position);

    return voice;
}

NYA_SoundVoice _nya_audio_play(NYA_ConstCString sound_handle, NYA_SoundParams params, const f32x3* position) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return NYA_SOUND_VOICE_NONE;

    MIX_Audio* audio = _nya_audio_get(sound_handle);
    if (audio == nullptr) return NYA_SOUND_VOICE_NONE;

    // First slot that is not sounding. Linear over sixteen entries, cheaper than maintaining a free
    // list and not on any hot path — a frame starts a handful of sounds.
    u32 slot = _NYA_AUDIO_SLOTS;
    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) {
        if (!MIX_TrackPlaying(system->slots[i].track)) {
            slot = i;
            break;
        }
    }

    /*
     * Every voice busy, so the lowest priority one is a candidate to be taken.
     *
     * Strictly outranked, not merely equal: a crowd of same-priority footsteps then never cuts each
     * other off, and the pool degrades by dropping new sounds of that rank rather than by chopping
     * playing ones. Cutting a sound mid-waveform is an audible click, so it is worth doing only when
     * something genuinely more important needs the voice.
     */
    if (slot == _NYA_AUDIO_SLOTS) {
        u32 weakest = 0;
        for (u32 i = 1; i < NYA_AUDIO_VOICES; i++) {
            if (system->slots[i].priority < system->slots[weakest].priority) weakest = i;
        }

        if (params.priority <= system->slots[weakest].priority) return NYA_SOUND_VOICE_NONE;

        MIX_StopTrack(system->slots[weakest].track, 0);
        slot = weakest;
    }

    MIX_Track* track = system->slots[slot].track;
    if (!MIX_SetTrackAudio(track, audio)) return NYA_SOUND_VOICE_NONE;

    /*
     * A reused slot starts un-occluded and unplaced.
     *
     * Both are state from whatever this slot last played, and both are wrong for the new sound: a voice
     * that inherits the previous one's occlusion starts muffled for no reason, and one that inherits
     * `positional` gets its predecessor's world position raycast against until it is overwritten below.
     */
    system->slots[slot].occlusion_gain = 1.0F;
    system->slots[slot].positional     = false;

    atomic_store_explicit(&system->slots[slot].filter.target_hz, 0.0F, memory_order_relaxed);


    // A zero-initialised NYA_SoundParams means silence at zero speed, which nobody intends. Treated
    // as "not specified" so the struct can be filled in partially, which is the point of it.
    //
    // The variation lands in the remembered gain rather than on the track, so a later master or
    // category change folds in on top of it instead of resetting this instance to its centre.
    system->slots[slot].base_gain = _nya_audio_vary_gain(params.gain > 0.0F ? params.gain : 1.0F, params.gain_variation_db);
    system->slots[slot].priority  = params.priority;
    _nya_audio_apply_gain(slot);

    // Rolled once, here, rather than per frame: a looping sound keeps the detune it started with,
    // which is a variant of the clip rather than a warble.
    MIX_SetTrackFrequencyRatio(track, _nya_audio_vary_pitch(params.pitch > 0.0F ? params.pitch : 1.0F, params.pitch_variation_semitones));

    /*
     * Spatialisation is cleared before the new sound, not merely left alone.
     *
     * A voice is a reused track, and everything set on it persists. Without this, a slot that was
     * panned hard left for one sound plays the *next* one hard left too — and since pan cannot be
     * read back, the only symptom is sounds drifting to one side as the pool cycles. Null disables
     * placement, which is what both of these mean.
     */
    MIX_SetTrackStereo(track, nullptr);
    MIX_SetTrack3DPosition(track, nullptr);

    /*
     * Against the track, not through a voice handle.
     *
     * The voice does not exist yet — its generation is bumped below, once the sound is running — so
     * a handle built here names the *previous* sound in this slot and every setter given it resolves
     * to nothing. Placement wins over pan rather than combining with it: both decide where the sound
     * sits, and a caller who passed a world position said which they meant.
     */
    if (position != nullptr) {
        _nya_audio_track_set_position(track, *position);
    } else if (params.pan != 0.0F) {
        _nya_audio_track_set_pan(track, params.pan);
    }

    SDL_PropertiesID options = SDL_CreateProperties();
    SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, params.loop ? -1 : 0);
    if (params.fade_in_ms > 0) SDL_SetNumberProperty(options, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, (s64)params.fade_in_ms);

    b8 started = MIX_PlayTrack(track, options);
    SDL_DestroyProperties(options);

    if (!started) return NYA_SOUND_VOICE_NONE;

    // Bumped only once the sound is actually running, so a failed play cannot invalidate the handle
    // someone is still holding for the previous sound in this slot.
    system->slots[slot].generation++;

    return (NYA_SoundVoice){ .index = slot, .generation = system->slots[slot].generation };
}

void nya_audio_listener_set(NYA_AudioListener listener) {
    // Same zero-means-unspecified convention the params structs use, so a listener built with only a
    // position does not silently get a reference distance of zero — which would divide by it.
    if (listener.reference_distance <= 0.0F) listener.reference_distance = 1.0F;

    // Out of range is a caller mistake rather than a state to handle, and the mapping below would
    // otherwise fall through to its default and quietly behave as side on.
    nya_assert(listener.plane < NYA_AUDIO_PLANE_COUNT, "unknown audio plane %d", (s32)listener.plane);

    _nya_audio_system.listener = listener;
}

NYA_AudioListener nya_audio_listener_get(void) {
    return _nya_audio_system.listener;
}

void nya_audio_listener_3d_set(NYA_AudioListener3D listener) {
    // Same zero-means-unspecified convention as everywhere else here, so a listener built with only a
    // position is a usable listener rather than one with a falloff of zero and no facing.
    if (listener.reference_distance <= 0.0F) listener.reference_distance = 1.0F;

    if (nya_vector_length(listener.forward) < NYA_EPSILON) listener.forward = (f32x3){ 0.0F, 0.0F, -1.0F };
    if (nya_vector_length(listener.up) < NYA_EPSILON) listener.up = (f32x3){ 0.0F, 1.0F, 0.0F };

    /*
     * Rejected rather than silently repaired.
     *
     * A `forward` parallel to `up` has no unique right vector — the cross product is zero — and the
     * frame built from it collapses, which comes out as every sound arriving dead centre however the
     * camera turns. Substituting some other up would hide that behind a listener quietly facing
     * somewhere the caller did not ask for. The camera code has the same degeneracy at the poles and
     * clamps short of them; see nya_matrix_look_at.
     */
    f32x3 right = nya_vector_cross(listener.forward, listener.up);

    if (nya_vector_length(right) < NYA_EPSILON) {
        nya_log_error("The 3D audio listener was given a forward parallel to its up; keeping the previous one.");
        return;
    }

    _nya_audio_system.listener_3d = listener;
}

NYA_AudioListener3D nya_audio_listener_3d_get(void) {
    return _nya_audio_system.listener_3d;
}

void nya_audio_stop_sounds(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) MIX_StopTrack(system->slots[i].track, 0);
}

/*
 * ─────────────────────────────────────────────────────────
 * MUSIC
 * ─────────────────────────────────────────────────────────
 */

void nya_audio_play_music(NYA_ConstCString music_handle, b8 loop, u32 fade_in_ms) {
    nya_audio_play_music_with(music_handle, (NYA_MusicParams){ .loop = loop, .fade_in_ms = fade_in_ms });
}

void nya_audio_crossfade_music(NYA_ConstCString music_handle, NYA_MusicParams params, u32 duration_ms) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    // Nothing to fade from, so this is simply a fade in. Also the path a game hits on its first
    // track, which would otherwise sound like a bug the first time and be right ever after.
    if (!MIX_TrackPlaying(system->slots[system->music_slot].track)) {
        params.fade_in_ms = duration_ms;
        nya_audio_play_music_with(music_handle, params);
        return;
    }

    MIX_Audio* audio = _nya_audio_get(music_handle);
    if (audio == nullptr) return;

    u32 outgoing = system->music_slot;
    u32 incoming = outgoing == _NYA_AUDIO_MUSIC_A ? _NYA_AUDIO_MUSIC_B : _NYA_AUDIO_MUSIC_A;

    // The incoming slot may still be finishing an earlier crossfade. Stopped outright rather than
    // faded, because there is nowhere for a third piece to go.
    MIX_StopTrack(system->slots[incoming].track, 0);

    if (!MIX_SetTrackAudio(system->slots[incoming].track, audio)) {
        nya_warn("could not set the music track to '%s': %s", music_handle, SDL_GetError());
        return;
    }

    system->slots[incoming].base_gain = params.gain > 0.0F ? params.gain : 1.0F;
    _nya_audio_apply_gain(incoming);

    SDL_PropertiesID options = SDL_CreateProperties();
    SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, params.loop ? -1 : 0);
    SDL_SetNumberProperty(options, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, (s64)duration_ms);
    if (params.loop_start_ms > 0) SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOP_START_MILLISECOND_NUMBER, (s64)params.loop_start_ms);

    b8 started = MIX_PlayTrack(system->slots[incoming].track, options);
    SDL_DestroyProperties(options);

    if (!started) {
        nya_warn("could not play the music '%s': %s", music_handle, SDL_GetError());
        return;
    }

    // Only once the incoming piece is actually running: fading the old one out first would leave
    // silence if the new track failed to start.
    MIX_StopTrack(system->slots[outgoing].track, (s64)duration_ms);

    system->slots[incoming].generation++;
    system->music_slot = incoming;
}

void nya_audio_play_music_with(NYA_ConstCString music_handle, NYA_MusicParams params) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    MIX_Audio* audio = _nya_audio_get(music_handle);
    if (audio == nullptr) return;

    // A hard cut stops both slots, not just the current one — a crossfade may still be running, and
    // leaving its outgoing half sounding would layer the old track under the new one.
    MIX_StopTrack(system->slots[_NYA_AUDIO_MUSIC_A].track, 0);
    MIX_StopTrack(system->slots[_NYA_AUDIO_MUSIC_B].track, 0);

    if (!MIX_SetTrackAudio(system->slots[system->music_slot].track, audio)) {
        nya_warn("could not set the music track to '%s': %s", music_handle, SDL_GetError());
        return;
    }

    system->slots[system->music_slot].base_gain = params.gain > 0.0F ? params.gain : 1.0F;
    _nya_audio_apply_gain(system->music_slot);

    /*
     * Options go through a property set rather than parameters, which is how SDL_mixer models
     * anything optional. Created and destroyed per call: this happens on a track change, not per
     * frame, so the allocation is not worth caching.
     */
    SDL_PropertiesID options = SDL_CreateProperties();

    // -1 is "forever" in SDL_mixer's counting, where the number is how many times to *repeat* after
    // the first play. Zero therefore means play once, not play nothing.
    SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, params.loop ? -1 : 0);

    if (params.fade_in_ms > 0) SDL_SetNumberProperty(options, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, (s64)params.fade_in_ms);

    // The intro-then-loop case: play from the start, repeat from here. See NYA_MusicParams.
    if (params.loop_start_ms > 0) SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOP_START_MILLISECOND_NUMBER, (s64)params.loop_start_ms);

    b8 started = MIX_PlayTrack(system->slots[system->music_slot].track, options);
    if (!started) nya_warn("could not play the music '%s': %s", music_handle, SDL_GetError());

    SDL_DestroyProperties(options);

    // A new piece is a new sound in that slot, so handles to the previous one must stop resolving —
    // otherwise a pan left on the old track would still be steering the new one.
    if (started) system->slots[system->music_slot].generation++;
}

void nya_audio_stop_music(u32 fade_out_ms) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    // Both, because a crossfade in flight has two pieces sounding and stopping only the current one
    // would leave the outgoing half playing on alone.
    MIX_StopTrack(system->slots[_NYA_AUDIO_MUSIC_A].track, (s64)fade_out_ms);
    MIX_StopTrack(system->slots[_NYA_AUDIO_MUSIC_B].track, (s64)fade_out_ms);
}

void nya_audio_pause_music(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    MIX_PauseTrack(system->slots[_NYA_AUDIO_MUSIC_A].track);
    MIX_PauseTrack(system->slots[_NYA_AUDIO_MUSIC_B].track);
}

void nya_audio_resume_music(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    MIX_ResumeTrack(system->slots[_NYA_AUDIO_MUSIC_A].track);
    MIX_ResumeTrack(system->slots[_NYA_AUDIO_MUSIC_B].track);
}

b8 nya_audio_music_playing(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return false;

    // Paused counts as not playing, which is what a caller asking "is music sounding" means. A
    // caller that wants to distinguish the two wants MIX_TrackPaused, which is not exposed yet.
    return MIX_TrackPlaying(system->slots[system->music_slot].track) && !MIX_TrackPaused(system->slots[system->music_slot].track);
}

NYA_SoundVoice nya_audio_music_voice(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return NYA_SOUND_VOICE_NONE;

    return (NYA_SoundVoice){ .index = system->music_slot, .generation = system->slots[system->music_slot].generation };
}

/*
 * ─────────────────────────────────────────────────────────
 * EFFECTS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_audio_voice_valid(NYA_SoundVoice voice) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);

    if (slot == nullptr) return false;

    // Paused counts. The setters accept a paused voice — they only need the handle to still name
    // this sound — so reporting it invalid here would make "check then set" behave differently from
    // "just set", which is the kind of inconsistency that only shows up in a pause menu.
    return MIX_TrackPlaying(slot->track) || MIX_TrackPaused(slot->track);
}

void nya_audio_voice_set_gain(NYA_SoundVoice voice, f32 gain) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // Remembered rather than pushed straight through, so a later master or category change keeps
    // this voice's relative level instead of resetting it to full.
    slot->base_gain = nya_max(0.0F, gain);
    _nya_audio_apply_gain(voice.index);
}

void nya_audio_voice_set_pitch(NYA_SoundVoice voice, f32 ratio) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // A ratio of zero would stop the playhead rather than silence it, and a negative one is not
    // defined at all, so both are refused rather than passed through.
    if (ratio <= 0.0F) return;

    MIX_SetTrackFrequencyRatio(slot->track, ratio);
}

void nya_audio_voice_set_pan(NYA_SoundVoice voice, f32 pan) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    _nya_audio_track_set_pan(slot->track, pan);
}

void nya_audio_voice_set_position(NYA_SoundVoice voice, f32x3 position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    _nya_audio_track_set_position(slot->track, position);
}

void nya_audio_voice_set_world_position(NYA_SoundVoice voice, f32x2 world_position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // Remembered as well as pushed, so occlusion has a world position to ask about. SDL_mixer stores it
    // listener-relative, so reading it back would give an answer in the wrong space.
    slot->world_position = (f32x3){ world_position.x, world_position.y, 0.0F };
    slot->positional     = true;

    // Read fresh rather than remembered from when the sound started, so a listener that moved since
    // is the one this is measured against.
    _nya_audio_track_set_position(slot->track, _nya_audio_world_to_audio(world_position));
}

void nya_audio_voice_set_world_position_3d(NYA_SoundVoice voice, f32x3 world_position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    slot->world_position = world_position;
    slot->positional     = true;

    // Read fresh, like the 2D version, so a listener that has orbited since is the one this is measured
    // against — which for a 3D scene is the whole point: the camera turns far more often than the source
    // moves.
    _nya_audio_track_set_position(slot->track, _nya_audio_world_to_audio_3d(world_position));
}

void nya_audio_voice_stop(NYA_SoundVoice voice, u32 fade_out_ms) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    MIX_StopTrack(slot->track, (s64)fade_out_ms);
}

/*
 * ─────────────────────────────────────────────────────────
 * GAIN
 * ─────────────────────────────────────────────────────────
 */

void nya_audio_voice_filter_set(NYA_SoundVoice voice, NYA_AudioFilter filter) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // Two relaxed stores into a struct the mixer's thread reads, exactly as the bus filter does. See
    // NYA_AudioFilterState for why nothing here needs a lock.
    atomic_store_explicit(&slot->filter.target_hz, nya_max(0.0F, filter.lowpass_hz), memory_order_relaxed);
    atomic_store_explicit(&slot->filter.glide_ms, nya_max(0.0F, filter.glide_ms), memory_order_relaxed);
}

void nya_audio_occlusion_set(NYA_AudioOcclusionFn function, void* user_data, NYA_AudioOcclusion occlusion) {
    NYA_AudioSystem* system = &_nya_audio_system;

    // Zero means unspecified, matching every other options struct here. Half gain is about what a solid
    // wall does before the filtering is taken into account.
    if (occlusion.gain <= 0.0F) occlusion.gain = 0.5F;
    if (occlusion.glide_ms <= 0.0F) occlusion.glide_ms = 80.0F;

    system->occlusion_function  = function;
    system->occlusion_user_data = user_data;
    system->occlusion           = occlusion;

    /*
     * Turning it off clears what it was applying.
     *
     * Without this, every voice muffled at the moment occlusion was disabled would stay muffled for as
     * long as it played — the automatic driver is the only thing that would have cleared it, and it is
     * exactly what has just been removed.
     */
    if (function != nullptr) return;

    for (u32 i = 0; i < _NYA_AUDIO_SLOTS; i++) {
        NYA_AudioVoice* slot = &system->slots[i];

        atomic_store_explicit(&slot->filter.target_hz, 0.0F, memory_order_relaxed);

        b8 was_occluded = slot->occlusion_gain > 0.0F && slot->occlusion_gain < 1.0F;

        slot->occlusion_gain = 1.0F;

        if (was_occluded && slot->track != nullptr) _nya_audio_apply_gain(i);
    }
}

void nya_audio_occlusion_update(void) {
    NYA_AudioSystem* system = &_nya_audio_system;

    if (!system->ready || system->occlusion_function == nullptr) return;

    for (u32 i = 0; i < _NYA_AUDIO_SLOTS; i++) {
        NYA_AudioVoice* slot = &system->slots[i];

        if (slot->track == nullptr || !slot->positional) continue;

        /*
         * Only voices that are actually sounding.
         *
         * A finished voice keeps its slot until something reuses it, so without this the callback would
         * be asked about the position of every sound the scene has ever played — and a raycast is not
         * something to spend on a voice nobody can hear.
         */
        if (!MIX_TrackPlaying(slot->track)) {
            slot->positional = false;
            continue;
        }

        f32 occlusion = nya_clamp(system->occlusion_function(slot->world_position, system->occlusion_user_data), 0.0F, 1.0F);

        /*
         * Interpolated from wide open toward the configured cutoff, rather than switched between them.
         *
         * A callback that reports a fraction — several rays, some blocked — then gets a proportionally
         * muffled sound for free, which is what makes a doorway sound like a doorway rather than like a
         * switch. A callback that only ever answers zero or one loses nothing by this.
         *
         * Zero occlusion stores a cutoff of zero, which the filter reads as wide open rather than as
         * shut. That distinction is the one thing NYA_AudioFilterState's resting value exists for.
         */
        f32 cutoff = 0.0F;

        if (system->occlusion.lowpass_hz > 0.0F && occlusion > 0.0F) {
            // From the top of the audible band down toward the configured cutoff, so that partial
            // occlusion rolls the treble off gradually instead of jumping to the fully blocked value.
            cutoff = nya_lerp(20000.0F, system->occlusion.lowpass_hz, occlusion);
        }

        atomic_store_explicit(&slot->filter.target_hz, cutoff, memory_order_relaxed);
        atomic_store_explicit(&slot->filter.glide_ms, system->occlusion.glide_ms, memory_order_relaxed);

        f32 gain = nya_lerp(1.0F, system->occlusion.gain, occlusion);

        /*
         * Applied on top of the voice's remembered gain rather than replacing it.
         *
         * `base_gain` is what the caller asked for and the category gain is what the settings say; this
         * is a third multiplier, and folding it into either of the others would lose it the next time a
         * volume slider moved.
         */
        if (fabsf(gain - slot->occlusion_gain) > 0.001F) {
            slot->occlusion_gain = gain;

            // Through the shared formula, so the category and master gains are reapplied with it rather
            // than being dropped. See _nya_audio_apply_gain.
            _nya_audio_apply_gain(i);
        }
    }
}

void nya_audio_bus_reverb_set(NYA_AudioBus bus, NYA_AudioReverb reverb) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    NYA_AudioReverbState* state = &_nya_audio_system.reverbs[bus];

    // Zero means unspecified for the mix controls, matching every other options struct here. `room_size`
    // and `damping` are deliberately not defaulted: a zero room is how the reverb is switched off, and a
    // zero damping is a real, if bright, setting.
    if (reverb.wet <= 0.0F) reverb.wet = 0.3F;
    if (reverb.dry <= 0.0F) reverb.dry = 1.0F;
    if (reverb.width <= 0.0F) reverb.width = 1.0F;

    /*
     * Clamped short of one rather than at it.
     *
     * A comb feedback of exactly one does not decay — the tail rings at constant amplitude forever, and
     * anything above one grows without bound until the buffer is full of infinities. This is the whole
     * of the stability argument for a Schroeder network, and it is worth enforcing rather than
     * documenting: the failure is silent for a second and then permanent.
     */
    atomic_store_explicit(&state->room_size, nya_clamp(reverb.room_size, 0.0F, 0.97F), memory_order_relaxed);
    atomic_store_explicit(&state->damping, nya_clamp(reverb.damping, 0.0F, 1.0F), memory_order_relaxed);
    atomic_store_explicit(&state->wet, nya_max(reverb.wet, 0.0F), memory_order_relaxed);
    atomic_store_explicit(&state->dry, nya_max(reverb.dry, 0.0F), memory_order_relaxed);
    atomic_store_explicit(&state->width, nya_clamp(reverb.width, 0.0F, 1.0F), memory_order_relaxed);
}

NYA_AudioReverb nya_audio_bus_reverb_get(NYA_AudioBus bus) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    const NYA_AudioReverbState* state = &_nya_audio_system.reverbs[bus];

    return (NYA_AudioReverb){
        .room_size = atomic_load_explicit(&state->room_size, memory_order_relaxed),
        .damping   = atomic_load_explicit(&state->damping, memory_order_relaxed),
        .wet       = atomic_load_explicit(&state->wet, memory_order_relaxed),
        .dry       = atomic_load_explicit(&state->dry, memory_order_relaxed),
        .width     = atomic_load_explicit(&state->width, memory_order_relaxed),
    };
}

void nya_audio_bus_filter_set(NYA_AudioBus bus, NYA_AudioFilter filter) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    NYA_AudioFilterState* state = &_nya_audio_system.filters[bus];

    // Not guarded on `ready`: storing two numbers into a struct that exists either way is harmless,
    // and it means a game can set up its filters before the audio device is up without them being
    // silently dropped.
    atomic_store_explicit(&state->target_hz, nya_max(0.0F, filter.lowpass_hz), memory_order_relaxed);
    atomic_store_explicit(&state->glide_ms, nya_max(0.0F, filter.glide_ms), memory_order_relaxed);
}

NYA_AudioFilter nya_audio_bus_filter_get(NYA_AudioBus bus) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    NYA_AudioFilterState* state = &_nya_audio_system.filters[bus];

    // The target rather than where the glide has reached, so this round trips with the setter. The
    // live coefficient belongs to the mixer's thread and reading it here would be a race for a
    // number nobody can act on.
    return (NYA_AudioFilter){
        .lowpass_hz = atomic_load_explicit(&state->target_hz, memory_order_relaxed),
        .glide_ms   = atomic_load_explicit(&state->glide_ms, memory_order_relaxed),
    };
}

void nya_audio_set_master_gain(f32 gain) {
    _nya_audio_system.master_gain = nya_max(0.0F, gain);

    /*
     * Music is updated now; effects are not.
     *
     * A voice's gain is set when it starts, so a change here reaches the next sound rather than the
     * ones already sounding. That is the right behaviour for something a fraction of a second long,
     * and fixing it would mean walking the pool on every slider movement. Music is a stream that may
     * run for minutes, so it has to follow the slider immediately.
     */
    _nya_audio_apply_music_gain();
}

void nya_audio_set_sound_gain(f32 gain) {
    _nya_audio_system.sound_gain = nya_max(0.0F, gain);
}

void nya_audio_set_music_gain(f32 gain) {
    _nya_audio_system.music_gain = nya_max(0.0F, gain);
    _nya_audio_apply_music_gain();
}

f32 nya_audio_master_gain(void) {
    return _nya_audio_system.master_gain;
}

f32 nya_audio_sound_gain(void) {
    return _nya_audio_system.sound_gain;
}

f32 nya_audio_music_gain(void) {
    return _nya_audio_system.music_gain;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

MIX_Audio* _nya_audio_get(NYA_ConstCString handle) {
    if (handle == nullptr) return nullptr;

    NYA_Asset* asset = nya_asset_get((NYA_CString)handle);

    // Missing or still loading. Assets resolve asynchronously, so this is the normal state for the
    // first frames after a load rather than something to report every time.
    if (asset == nullptr) return nullptr;
    if (asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;

    // The wrong kind of asset is a caller mistake rather than a timing one, so it is worth saying:
    // playing a texture is a typo somewhere, and silence gives no clue where.
    if (asset->type != NYA_ASSET_TYPE_SOUND) {
        /*
         * Said once per handle, not once per call.
         *
         * This is a programming mistake rather than a transient state, so it has to be reported —
         * but the call that makes it is usually in a per-frame path, and an unthrottled warning
         * there buries every other line in the log. Remembering the last one caught covers the
         * realistic case of one wrong handle being hammered.
         */
        NYA_INTERNAL NYA_ConstCString last_warned = nullptr;

        if (last_warned != handle) {
            last_warned = handle;
            nya_warn("'%s' is not a sound asset, so it cannot be played", handle);
        }

        return nullptr;
    }

    return asset->as_sound.audio;
}

NYA_AudioVoice* _nya_audio_resolve(NYA_SoundVoice voice) {
    NYA_AudioSystem* system = &_nya_audio_system;

    if (!system->ready) return nullptr;
    if (voice.index >= _NYA_AUDIO_SLOTS) return nullptr;

    // Zero is never a live generation, so NYA_SOUND_VOICE_NONE falls out here rather than resolving
    // to slot zero — which is a real voice, and would make every failed play steer it.
    if (voice.generation == 0) return nullptr;

    if (system->slots[voice.index].generation != voice.generation) return nullptr;

    return &system->slots[voice.index];
}

void _nya_audio_voice_remember_position(NYA_SoundVoice voice, f32x3 world_position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    slot->world_position = world_position;
    slot->positional     = true;
}

void _nya_audio_apply_gain(u32 slot) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    // Music and effects have separate category gains, and which one applies is decided by the slot
    // rather than being stored — there is exactly one music slot.
    // Both music slots take the music gain, including the one fading out during a crossfade.
    f32 category = slot >= NYA_AUDIO_VOICES ? system->music_gain : system->sound_gain;

    /*
     * Occlusion is a fourth multiplier here rather than a separate MIX_SetTrackGain elsewhere.
     *
     * There is one formula for a voice's gain and it has to stay one. An occlusion driver writing the
     * track gain directly would be overwritten by the next volume slider, and a volume slider would
     * un-muffle every occluded sound — each correct on its own and wrong together.
     *
     * One at rest, so a voice nothing is occluding multiplies by one.
     */
    f32 occlusion = system->slots[slot].occlusion_gain > 0.0F ? system->slots[slot].occlusion_gain : 1.0F;

    MIX_SetTrackGain(system->slots[slot].track, system->slots[slot].base_gain * category * system->master_gain * occlusion);
}

void _nya_audio_track_set_pan(MIX_Track* track, f32 pan) {
    pan = nya_clamp(pan, -1.0F, 1.0F);

    /*
     * Equal power, not linear.
     *
     * Mapping pan onto a quarter turn and taking cosine and sine keeps left² + right² at one across
     * the sweep, which is what the ear tracks. A linear crossfade holds left + right constant
     * instead, and that dips audibly in the middle — two channels at half gain carry less power than
     * one at full.
     */
    f32 angle = (pan + 1.0F) * 0.25F * (f32)M_PI;

    MIX_SetTrackStereo(track, &(MIX_StereoGains){ .left = cosf(angle), .right = sinf(angle) });
}

void _nya_audio_track_set_position(MIX_Track* track, f32x3 position) {
    // Overrides any pan on this track: both decide where the sound sits, and SDL_mixer keeps only
    // the most recent answer.
    MIX_SetTrack3DPosition(track, &(MIX_Point3D){ .x = position[0], .y = position[1], .z = position[2] });
}

f32x3 _nya_audio_world_to_audio(f32x2 world_position) {
    NYA_AudioListener listener = _nya_audio_system.listener;

    // Never zero: the setter substitutes 1.0, and the static initializer starts there. Dividing here
    // is what makes the mixer's fixed reference distance of 1.0 mean reference_distance world units.
    f32x2 offset = (world_position - listener.position) / listener.reference_distance;

    /*
     * The mixer's space is right handed with y up and z back, while the renderer's world is y down.
     * Which axis y belongs on is the thing the engine cannot infer, so NYA_AudioPlane carries it.
     *
     * The default is not a fallback for an unknown plane — nya_audio_listener_set rejects those — it
     * is here because the build compiles with -Wswitch-default.
     */
    switch (listener.plane) {
        case NYA_AUDIO_PLANE_TOP_DOWN:
            // The screen is the ground: down the screen is away behind the listener, and nothing is
            // ever overhead.
            return (f32x3){ offset[0], 0.0F, offset[1] };

        case NYA_AUDIO_PLANE_SIDE:
        case NYA_AUDIO_PLANE_COUNT:
        default:
            // The screen is a wall: down the screen is down in the world, which is the negation, and
            // nothing is ever in front of or behind the player.
            return (f32x3){ offset[0], -offset[1], 0.0F };
    }
}

f32x3 _nya_audio_world_to_audio_3d(f32x3 world_position) {
    NYA_AudioListener3D listener = _nya_audio_system.listener_3d;

    // Never zero: the setter substitutes 1.0, and the initializer starts there. Dividing here is what
    // makes the mixer's fixed reference distance of 1.0 mean reference_distance world units.
    f32x3 offset = (world_position - listener.position) / listener.reference_distance;

    /*
     * An orthonormal frame from the listener's facing, built the way a look-at matrix builds one.
     *
     * `up` is only used to decide which way around the forward axis the ear is rolled; the actual up
     * vector is recomputed across the other two, so a caller can hand over a world up together with a
     * forward that is not perpendicular to it and get a correct frame anyway. The degenerate case —
     * forward parallel to up — cannot arrive here, because nya_audio_listener_3d_set refuses it.
     */
    f32x3 forward = nya_vector_normalize(listener.forward);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, listener.up));
    f32x3 up      = nya_vector_cross(right, forward);

    /*
     * Projected onto that frame, with forward becoming *negative* z.
     *
     * SDL_mixer's space is right handed like OpenGL and OpenAL, where the viewer looks down -z. Getting
     * this sign wrong is the classic positional audio bug and it is not obvious from a speaker test: left
     * and right stay correct, and only front and back are swapped — which on a stereo pair is nearly
     * inaudible and on a surround setup is completely wrong.
     */
    return (f32x3){
        nya_vector_dot(offset, right),
        nya_vector_dot(offset, up),
        -nya_vector_dot(offset, forward),
    };
}

void _nya_audio_filter_reset(NYA_AudioFilterState* filter) {
    atomic_store_explicit(&filter->target_hz, 0.0F, memory_order_relaxed);
    atomic_store_explicit(&filter->glide_ms, 0.0F, memory_order_relaxed);

    // One, not zero. A zeroed coefficient is a filter clamped shut, which would come up silent — the
    // resting state of a one pole is wide open, and that is an identity rather than a wide filter.
    filter->coefficient = 1.0F;

    for (u32 i = 0; i < NYA_AUDIO_FILTER_MAX_CHANNELS; i++) filter->state[i] = 0.0F;
}

void _nya_audio_reverb_configure(NYA_AudioReverbState* reverb, s32 rate) {
    /*
     * The published lengths are for 44.1 kHz, so every other rate scales them.
     *
     * Not scaling would make the room shrink as the device's rate rises — the delays are counted in
     * samples, and the same count is a shorter time at a higher rate. A 96 kHz device would get a room
     * less than half the size of the one the numbers were chosen for.
     */
    f32 scale = (f32)rate / 44100.0F;

    for (u32 network = 0; network < _NYA_AUDIO_REVERB_NETWORKS; network++) {
        // The second network's lines are all offset, which is what stops the two channels being the
        // same room. See _NYA_AUDIO_REVERB_STEREO_SPREAD.
        u32 spread = network == 0 ? 0 : _NYA_AUDIO_REVERB_STEREO_SPREAD;

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
            NYA_AudioReverbComb* comb = &reverb->combs[network][i];

            u32 length = (u32)(((f32)_NYA_AUDIO_REVERB_COMB_LENGTHS[i] + (f32)spread) * scale);

            // Clamped to what the fixed buffer holds, and never zero — a delay line of no length feeds
            // its output straight back into its input, which is not a comb filter, it is a divide by
            // nothing that becomes an infinity on the first sample.
            comb->length = nya_clamp(length, 1U, (u32)_NYA_AUDIO_REVERB_MAX_DELAY);
            comb->cursor = 0;
            comb->damped = 0.0F;

            for (u32 sample = 0; sample < comb->length; sample++) comb->buffer[sample] = 0.0F;
        }

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_ALLPASSES; i++) {
            NYA_AudioReverbAllpass* allpass = &reverb->allpasses[network][i];

            u32 length = (u32)(((f32)_NYA_AUDIO_REVERB_ALLPASS_LENGTHS[i] + (f32)spread) * scale);

            allpass->length = nya_clamp(length, 1U, (u32)_NYA_AUDIO_REVERB_MAX_DELAY);
            allpass->cursor = 0;

            for (u32 sample = 0; sample < allpass->length; sample++) allpass->buffer[sample] = 0.0F;
        }
    }

    reverb->configured_rate = rate;
}

void _nya_audio_reverb_apply(NYA_AudioReverbState* reverb, const SDL_AudioSpec* spec, f32* pcm, s32 samples) {
    if (samples <= 0 || spec->channels <= 0 || spec->freq <= 0) return;

    f32 room_size = atomic_load_explicit(&reverb->room_size, memory_order_relaxed);

    /*
     * No room, no work — and the delay lines are left as they are.
     *
     * Not cleared, deliberately. A reverb switched off mid-tail should stop *feeding* the room, and
     * clearing here would cut the tail dead instead, which is a click. The lines decay to silence on
     * their own the next time it is switched on, and hold stale samples until then — which is what the
     * flush at the bottom of the loop keeps from being denormal.
     */
    if (room_size <= 0.0F) return;

    if (reverb->configured_rate != spec->freq) _nya_audio_reverb_configure(reverb, spec->freq);

    f32 damping = atomic_load_explicit(&reverb->damping, memory_order_relaxed);
    f32 wet     = atomic_load_explicit(&reverb->wet, memory_order_relaxed);
    f32 dry     = atomic_load_explicit(&reverb->dry, memory_order_relaxed);
    f32 width   = atomic_load_explicit(&reverb->width, memory_order_relaxed);

    s32 channels = spec->channels;
    s32 frames   = samples / channels;

    if (frames <= 0) return;

    /*
     * Fed at a fraction of the input level.
     *
     * Four combs in parallel sum their outputs, so an unattenuated feed enters the allpasses at roughly
     * four times the signal and clips. The constant is the usual one for this network; it is a property
     * of there being four combs rather than something to tune.
     */
    const f32 feed = 0.015F;

    // The allpass coefficient is fixed at the classic 0.5. It sets how the phase is scattered and not
    // how long the tail is, which is the comb feedback's job — exposing it would be a knob whose only
    // audible settings are "correct" and "metallic".
    const f32 allpass_feedback = 0.5F;

    for (s32 frame = 0; frame < frames; frame++) {
        f32* row = &pcm[frame * channels];

        /*
         * Downmixed into a left and a right feed by channel parity.
         *
         * Even channels are the left side of any standard layout and odd ones the right, for stereo and
         * for every surround arrangement SDL produces. A mono device has one channel, which is even, so
         * the right feed is silent and the two networks produce the same thing — correct, if wasteful,
         * and not worth a special case for a device that cannot convey width anyway.
         */
        f32 feed_left  = 0.0F;
        f32 feed_right = 0.0F;

        for (s32 channel = 0; channel < channels; channel++) {
            if ((channel & 1) == 0) {
                feed_left += row[channel];
            } else {
                feed_right += row[channel];
            }
        }

        f32 input[_NYA_AUDIO_REVERB_NETWORKS] = { feed_left * feed, feed_right * feed };
        f32 output[_NYA_AUDIO_REVERB_NETWORKS] = { 0.0F, 0.0F };

        for (u32 network = 0; network < _NYA_AUDIO_REVERB_NETWORKS; network++) {
            f32 sum = 0.0F;

            /*
             * The combs, in parallel: each is a delay whose output is fed back through a one-pole.
             *
             * The one-pole is the damping. Without it every pass round the loop returns the same
             * spectrum and the tail rings bright forever; with it the highs lose energy faster than the
             * lows, which is what every real surface does and what makes the tail sound like a room
             * rather than like a delay pedal.
             */
            for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
                NYA_AudioReverbComb* comb = &reverb->combs[network][i];

                f32 delayed = comb->buffer[comb->cursor];

                comb->damped = (delayed * (1.0F - damping)) + (comb->damped * damping);

                comb->buffer[comb->cursor] = input[network] + (comb->damped * room_size);

                comb->cursor++;
                if (comb->cursor >= comb->length) comb->cursor = 0;

                sum += delayed;
            }

            /*
             * The allpasses, in series: they scatter the echo density without changing the tone.
             *
             * Four combs alone give four audible repeats a second — a flutter, not a room. Each allpass
             * multiplies the number of echoes without adding colouration of its own, which is the entire
             * reason Schroeder put them after the combs rather than using more combs.
             */
            for (u32 i = 0; i < _NYA_AUDIO_REVERB_ALLPASSES; i++) {
                NYA_AudioReverbAllpass* allpass = &reverb->allpasses[network][i];

                f32 delayed = allpass->buffer[allpass->cursor];

                allpass->buffer[allpass->cursor] = sum + (delayed * allpass_feedback);

                allpass->cursor++;
                if (allpass->cursor >= allpass->length) allpass->cursor = 0;

                sum = delayed - sum;
            }

            output[network] = sum;
        }

        /*
         * Width as a crossfeed between the two tails.
         *
         * One is fully separate rooms and zero is the same tail in both ears. Mixing toward the average
         * rather than toward a mono sum keeps the level constant as the width changes, so the knob does
         * not double as a volume control.
         */
        f32 average = (output[0] + output[1]) * 0.5F;

        f32 left  = nya_lerp(average, output[0], width);
        f32 right = nya_lerp(average, output[1], width);

        for (s32 channel = 0; channel < channels; channel++) {
            f32 tail = (channel & 1) == 0 ? left : right;

            row[channel] = (row[channel] * dry) + (tail * wet);
        }
    }

    /*
     * Flush what is left to zero once it stops mattering.
     *
     * Same reasoning as the filter's: these decay geometrically and land in denormal territory after the
     * audio goes quiet, where some CPUs take a large per-operation penalty — on the audio thread, for
     * values far below anything audible.
     *
     * The comb one-poles were the only thing flushed here, on the reasoning that the delay lines are
     * written from values that have already been flushed. True of the *combs*, whose buffers take
     * `input + damped * room_size` and go to exactly zero once both do. Not true of the **allpasses**: an
     * allpass writes `sum + delayed * 0.5`, so once the combs fall silent it halves its own contents
     * forever and never reaches zero. A couple of seconds after the last sound the whole network is
     * denormal and stays that way until something plays again.
     *
     * One tap per line rather than the whole buffer, at the cursor that is about to be read next. That
     * walks the line at one sample per buffer, which clears it in well under a second — far faster than
     * the decay that fills it, and a fixed cost instead of memsetting a hundred kilobytes on the audio
     * thread.
     */
    for (u32 network = 0; network < _NYA_AUDIO_REVERB_NETWORKS; network++) {
        for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
            NYA_AudioReverbComb* comb = &reverb->combs[network][i];

            if (fabsf(comb->damped) < 1e-20F) comb->damped = 0.0F;

            if (fabsf(comb->buffer[comb->cursor]) < 1e-20F) comb->buffer[comb->cursor] = 0.0F;
        }

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_ALLPASSES; i++) {
            NYA_AudioReverbAllpass* allpass = &reverb->allpasses[network][i];

            if (fabsf(allpass->buffer[allpass->cursor]) < 1e-20F) allpass->buffer[allpass->cursor] = 0.0F;
        }
    }
}

void _nya_audio_filter_apply(NYA_AudioFilterState* filter, const SDL_AudioSpec* spec, f32* pcm, s32 samples) {
    if (samples <= 0 || spec->channels <= 0 || spec->freq <= 0) return;

    // More speakers than there is filter state for. Passed through rather than filtered across the
    // channels that do fit, because half a filtered surround field is worse than none.
    if (spec->channels > NYA_AUDIO_FILTER_MAX_CHANNELS) return;

    f32 target_hz = atomic_load_explicit(&filter->target_hz, memory_order_relaxed);
    f32 glide_ms  = atomic_load_explicit(&filter->glide_ms, memory_order_relaxed);

    /*
     * The one pole coefficient for that cutoff: a = 1 - e^(-2π·fc/fs).
     *
     * A cutoff of zero is off, and off is a coefficient of exactly one — `y += 1·(x - y)` leaves y
     * equal to x, so an unfiltered bus is bit for bit what came in rather than something that went
     * through a very wide filter and came back nearly the same.
     */
    f32 target = 1.0F;
    if (target_hz > 0.0F) {
        target = 1.0F - expf(-2.0F * (f32)M_PI * target_hz / (f32)spec->freq);

        // A cutoff at or above Nyquist lands at or past one, which is simply open.
        if (target > 1.0F) target = 1.0F;
        if (target < 0.0F) target = 0.0F;
    }

    s32 channels = spec->channels;
    s32 frames   = samples / channels;
    if (frames <= 0) return;

    /*
     * How far the coefficient may move this buffer.
     *
     * glide_ms is the time to cross the whole range, so the limit is the fraction of that range one
     * buffer represents. Rate limiting rather than interpolating between two endpoints means no
     * transition state has to be remembered, and a target that changes mid-glide is simply followed
     * from wherever the coefficient currently is.
     */
    f32 end = target;
    if (glide_ms > 0.0F) {
        f32 glide_frames = (glide_ms / 1000.0F) * (f32)spec->freq;

        if (glide_frames > 0.0F) {
            f32 max_delta = (f32)frames / glide_frames;
            f32 remaining = target - filter->coefficient;

            if (remaining > max_delta)
                end = filter->coefficient + max_delta;
            else if (remaining < -max_delta)
                end = filter->coefficient - max_delta;
        }
    } else {
        /*
         * No glide asked for, so the coefficient is already there when the buffer starts rather than
         * arriving at its far end.
         *
         * Without this the sweep below still ramps across one whole buffer, which makes an unglided
         * filter reach its setting some tens of milliseconds late and sound like a short glide — the
         * one thing a caller passing zero said they did not want. Stepping the coefficient is safe
         * on its own; it is the *state* that has to stay continuous, and that is untouched here.
         */
        filter->coefficient = target;
    }

    // Wide open, staying open, and no history left to bleed out: the filter is an identity, so skip
    // it. This is the common case — most buses carry no filter most of the time.
    if (filter->coefficient == 1.0F && end == 1.0F) {
        for (s32 channel = 0; channel < channels; channel++) filter->state[channel] = 0.0F;
        return;
    }

    // Swept across the buffer rather than applied as a step, because a coefficient that jumps once
    // per buffer is a zipper noise at the buffer rate.
    f32 coefficient = filter->coefficient;
    f32 increment   = (end - coefficient) / (f32)frames;

    for (s32 frame = 0; frame < frames; frame++) {
        for (s32 channel = 0; channel < channels; channel++) {
            f32* sample = &pcm[(frame * channels) + channel];

            filter->state[channel] += coefficient * (*sample - filter->state[channel]);
            *sample                 = filter->state[channel];
        }

        coefficient += increment;
    }

    filter->coefficient = end;

    /*
     * Flush what is left to zero once it stops mattering.
     *
     * A one pole decays toward its input, so after the audio goes quiet the state keeps halving
     * forever and lands in denormal territory — where some CPUs take a large penalty per operation,
     * on the audio thread, for values thousands of times below anything audible.
     */
    for (s32 channel = 0; channel < channels; channel++) {
        if (fabsf(filter->state[channel]) < 1e-20F) filter->state[channel] = 0.0F;
    }
}

void SDLCALL _nya_audio_track_mix_callback(void* userdata, MIX_Track* track, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(track);

    /*
     * The *cooked* hook, not the raw one.
     *
     * Cooked runs after SDL_mixer has resampled the track and placed it across the speakers, so the
     * buffer here is in the mixer's own format and channel count — which is what _nya_audio_filter_apply
     * assumes, and what makes one filter implementation serve a track, a bus and the master alike.
     * The raw hook would hand over the clip's native format, which varies per asset.
     */
    _nya_audio_filter_apply((NYA_AudioFilterState*)userdata, spec, pcm, samples);
}

void SDLCALL _nya_audio_group_mix_callback(void* userdata, MIX_Group* group, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(group);

    NYA_AudioFilterState* filter = userdata;

    /*
     * Filter first, then reverb, and the order is audible.
     *
     * The filter stands in for the air and the walls between the source and the ear; the reverb is what
     * those walls send back. Reverberating first and filtering afterwards muffles the *tail* as well,
     * which is what a room heard through a door sounds like — a different effect, and not the one either
     * setting is named after.
     */
    _nya_audio_filter_apply(filter, spec, pcm, samples);

    // The filter lives inside the system's array, so its index is the bus, and the two are parallel.
    u64 bus = (u64)(filter - &_nya_audio_system.filters[0]);

    if (bus < NYA_AUDIO_BUS_COUNT) _nya_audio_reverb_apply(&_nya_audio_system.reverbs[bus], spec, pcm, samples);
}

void SDLCALL _nya_audio_post_mix_callback(void* userdata, MIX_Mixer* mixer, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(mixer);

    // The master bus, after every group has been mixed together. Same order as a group's: the filter
    // stands in for what is between the ear and the world, the reverb for what the world sends back.
    _nya_audio_filter_apply((NYA_AudioFilterState*)userdata, spec, pcm, samples);
    _nya_audio_reverb_apply(&_nya_audio_system.reverbs[NYA_AUDIO_BUS_MASTER], spec, pcm, samples);
}

f32 _nya_audio_jitter(f32 half_range) {
    // Zero is the common case — every call that did not ask for variation — so it costs a compare
    // rather than a sample. Negative is read as "none" too, since a range cannot be inverted.
    if (half_range <= 0.0F) return 0.0F;

    NYA_RNGDistribution range = {
        .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
        .uniform = { .min = -(f64)half_range, .max = (f64)half_range },
    };

    return nya_rng_sample_f32(&_nya_audio_system.rng, range);
}

f32 _nya_audio_vary_pitch(f32 pitch, f32 semitones) {
    // Not merely an early out for the sample: it is what makes "no variation" bit exact rather than
    // whatever the exponential returns for zero, so a sound nobody asked to vary plays as authored.
    if (semitones <= 0.0F) return pitch;

    // Twelve semitones to the octave, and an octave is a doubling of the rate. Uniform in this
    // exponent rather than in the ratio it produces, which is the symmetry the declaration explains.
    return pitch * exp2f(_nya_audio_jitter(semitones) / 12.0F);
}

f32 _nya_audio_vary_gain(f32 gain, f32 db) {
    if (db <= 0.0F) return gain;

    // Twenty rather than ten: decibels of amplitude, which is what a gain multiplier is, against
    // decibels of power. Getting this wrong would make every range quietly twice what it says.
    return gain * powf(10.0F, _nya_audio_jitter(db) / 20.0F);
}

void _nya_audio_apply_music_gain(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    _nya_audio_apply_gain(system->music_slot);
}
