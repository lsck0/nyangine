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
 * A Schroeder network: four comb filters in parallel, summed, then two allpasses in series, per channel,
 * with the second channel's delays offset so the two rooms are not the same room. Delay lengths are the
 * classic Freeverb set, in samples at 44.1 kHz, and mutually prime — lengths sharing a factor land their
 * echoes on top of each other, heard as a metallic ring rather than a room.
 */

/** Comb delays at 44.1 kHz, in samples. */
#define _NYA_AUDIO_REVERB_COMBS 4

NYA_INTERNAL const u32 _NYA_AUDIO_REVERB_COMB_LENGTHS[_NYA_AUDIO_REVERB_COMBS] = { 1116, 1188, 1277, 1356 };

/** Allpass delays at 44.1 kHz, in samples. */
#define _NYA_AUDIO_REVERB_ALLPASSES 2

NYA_INTERNAL const u32 _NYA_AUDIO_REVERB_ALLPASS_LENGTHS[_NYA_AUDIO_REVERB_ALLPASSES] = { 556, 441 };

/**
 * Offset of the right channel's delays, in samples at 44.1 kHz. Prime, like the lengths, so the two
 * channels' echoes never fall back in step and pulse.
 * */
#define _NYA_AUDIO_REVERB_STEREO_SPREAD 23

/**
 * The longest delay any line needs, in samples. Lengths scale with the sample rate, so this covers up to
 * about 96 kHz; faster rates are clamped, a slightly smaller room. Fixed because only the mixer's thread
 * touches it.
 * */
#define _NYA_AUDIO_REVERB_MAX_DELAY 3072

/**
 * Networks run regardless of speaker count. A reverb tail is a stereo impression, and uncorrelated tails per
 * speaker smear rather than localise. The bus is downmixed to left and right, and the result is added back
 * across channels by parity.
 * */
#define _NYA_AUDIO_REVERB_NETWORKS 2

/** One comb filter: a delay line with a damped feedback path. */
typedef struct {
    f32 buffer[_NYA_AUDIO_REVERB_MAX_DELAY];
    u32 length;
    u32 cursor;

    /** The one-pole state that damps treble on each pass. See NYA_AudioReverb.damping. */
    f32 damped;
} NYA_AudioReverbComb;

/** One allpass: a delay line that scatters phase without colouring the magnitude. */
typedef struct {
    f32 buffer[_NYA_AUDIO_REVERB_MAX_DELAY];
    u32 length;
    u32 cursor;
} NYA_AudioReverbAllpass;

/**
 * One bus's reverb. The atomics are written by the setter and read in the callback; everything after them
 * belongs to the audio thread. Nothing is shared both ways, so the mixing path takes no lock.
 * */
struct NYA_AudioReverbState {
    /* Written by the game, read by the mixer. */

    atomic f32 room_size;
    atomic f32 damping;
    atomic f32 wet;
    atomic f32 dry;
    atomic f32 width;

    /* The mixer's thread alone. */

    NYA_AudioReverbComb    combs[_NYA_AUDIO_REVERB_NETWORKS][_NYA_AUDIO_REVERB_COMBS];
    NYA_AudioReverbAllpass allpasses[_NYA_AUDIO_REVERB_NETWORKS][_NYA_AUDIO_REVERB_ALLPASSES];

    /** The rate the delay lengths were computed for. A change re-derives them and clears the lines. */
    s32 configured_rate;
};

/**
 * One bus's filter. The atomics are written by nya_audio_bus_filter_set and read by the audio thread;
 * everything after them is the audio thread's alone. No lock on the mixing path: blocking in a post-mix
 * callback drops audio.
 * */
struct NYA_AudioFilterState {
    /*
     * Written by the game, read by the mixer. Relaxed on both sides: a torn pair glides at the old rate for one
     * buffer, which is inaudible.
     */
    atomic f32 target_hz;
    atomic f32 glide_ms;

    /* The mixer's thread alone. */

    /**
     * The one pole's current coefficient in [0, 1], 1 being wide open. Held because it glides; jumping to a new
     * cutoff clicks.
     * */
    f32 coefficient;

    /** The previous output per channel, all a one pole remembers. */
    f32 state[NYA_AUDIO_FILTER_MAX_CHANNELS];
};

/**
 * One playable slot. `generation` is bumped for every new sound, so an old handle stops resolving.
 * `base_gain` is kept so category and master changes can be folded in later.
 * */
typedef struct {
    MIX_Track* track;
    u32        generation;
    f32        base_gain;

    /** What this voice has to be outranked by before it can be stolen. See NYA_SoundParams.priority. */
    s32 priority;

    /**
     * This voice's own low pass, on SDL_mixer's per-track hook. A bus filter muffles the whole bus, and
     * occlusion needs one sound behind a wall.
     * */
    NYA_AudioFilterState filter;

    /**
     * The voice's world position, if placed. SDL_mixer stores positions listener-relative, so occlusion needs
     * this copy.
     * */
    f32x3 world_position;
    b8    positional;

    /** The gain occlusion is applying, so it can be folded out again. */
    f32 occlusion_gain;
} NYA_AudioVoice;

/*
 * Music is the slot after the effect pool. It behaves like an effect once playing, so one lookup path
 * serves both. The free-voice search only walks the first NYA_AUDIO_VOICES.
 */
#define _NYA_AUDIO_MUSIC_A NYA_AUDIO_VOICES
#define _NYA_AUDIO_MUSIC_B (NYA_AUDIO_VOICES + 1)
#define _NYA_AUDIO_SLOTS   (NYA_AUDIO_VOICES + 2)

struct NYA_AudioSystem {
    NYA_AudioVoice slots[_NYA_AUDIO_SLOTS];

    /**
     * Drives pitch variation only. Its own generator, so the number of sounds played cannot shift a seeded
     * game's random sequence.
     * */
    NYA_RNG rng;

    /** Where world positions are heard from. See NYA_AudioListener. */
    NYA_AudioListener listener;

    /** The 3D scene's ear. Independent of `listener`; see nya_audio_listener_3d_set. */
    NYA_AudioListener3D listener_3d;

    /** One room per bus, after that bus's filter, so a muffled sound reverberates muffled. */
    NYA_AudioReverbState reverbs[NYA_AUDIO_BUS_COUNT];

    /** How blocked sounds, and what decides it. A null function turns occlusion off. */
    NYA_AudioOcclusionFn occlusion_function;
    void*                occlusion_user_data;
    NYA_AudioOcclusion   occlusion;

    /**
     * The buses effects and music mix through, indexed by NYA_AudioBus. The master entry stays null: master is
     * the mixer itself, filtered through MIX_SetPostMixCallback.
     * */
    MIX_Group* groups[NYA_AUDIO_BUS_COUNT];

    NYA_AudioFilterState filters[NYA_AUDIO_BUS_COUNT];

    /** Which of the two music slots is "the music". */
    u32 music_slot;

    f32 master_gain;
    f32 sound_gain;
    f32 music_gain;

    b8 ready;
};

/* Zero-initialized; the non-zero defaults are set in nya_system_audio_init. */
NYA_INTERNAL NYA_AudioSystem _nya_audio_system;

/** The sound asset behind a handle, or null when it is missing, failed or still loading. */
NYA_INTERNAL MIX_Audio* _nya_audio_get(NYA_ConstCString handle);

/** Reapplies the music gain, which is the product of the master and music gains. */
NYA_INTERNAL void _nya_audio_apply_music_gain(void);

/** The slot a handle names, or null when stale, out of range, or the system is down. */
NYA_INTERNAL NYA_AudioVoice* _nya_audio_resolve(NYA_SoundVoice voice);

/** Pushes a slot's gain through its category and the master onto the track. */
NYA_INTERNAL void _nya_audio_apply_gain(u32 slot);

/**
 * nya_audio_play_sound_with, with an optional placement applied before the first sample. Null plays
 * unplaced.
 * */
NYA_INTERNAL NYA_SoundVoice _nya_audio_play(NYA_ConstCString sound_handle, NYA_SoundParams params, const f32x3* position);

/** Records where a voice is in the world, for occlusion to ask about later. A no-op for a dead handle. */
NYA_INTERNAL void _nya_audio_voice_remember_position(NYA_SoundVoice voice, f32x3 world_position);

/**
 * A world point as the mixer wants it: relative to the listener, on the plane's axes, scaled so 1.0 is
 * where attenuation starts.
 * */
NYA_INTERNAL f32x3 _nya_audio_world_to_audio(f32x2 world_position) __attr_no_discard;

/** The same, for a 3D world point against the 3D listener: into its own frame, then scaled. */
NYA_INTERNAL f32x3 _nya_audio_world_to_audio_3d(f32x3 world_position) __attr_no_discard;

/*
 * Pan and placement against a track. The play path needs them before the voice exists: its generation is
 * only bumped once the sound runs, so a handle would still name the previous sound.
 */
NYA_INTERNAL void _nya_audio_track_set_pan(MIX_Track* track, f32 pan);
NYA_INTERNAL void _nya_audio_track_set_position(MIX_Track* track, f32x3 position);

/** Resets a filter to wide open with no history, which is not all zeroes. */
NYA_INTERNAL void _nya_audio_filter_reset(NYA_AudioFilterState* filter);

/**
 * Rolls `pcm` off in place, gliding toward the last requested cutoff. Runs on the mixer's thread: no
 * allocation, logging or locks. `samples` counts floats, not frames, as SDL_mixer does.
 * */
NYA_INTERNAL void _nya_audio_filter_apply(NYA_AudioFilterState* filter, const SDL_AudioSpec* spec, f32* pcm, s32 samples);

/** Sizes the delay lines for a sample rate and clears them. */
NYA_INTERNAL void _nya_audio_reverb_configure(NYA_AudioReverbState* reverb, s32 rate);

/**
 * Adds a reverberated copy of `pcm` into it. Runs on the mixer's thread under the filter's rules. Returns
 * at once for a zero room size, since six delay lines per channel is the heavy part.
 * */
NYA_INTERNAL void _nya_audio_reverb_apply(NYA_AudioReverbState* reverb, const SDL_AudioSpec* spec, f32* pcm, s32 samples);

/* The three callback shapes SDL_mixer wants, all the same call with a different owner. */
NYA_INTERNAL void SDLCALL _nya_audio_track_mix_callback(void* userdata, MIX_Track* track, const SDL_AudioSpec* spec, float* pcm, int samples);
NYA_INTERNAL void SDLCALL _nya_audio_group_mix_callback(void* userdata, MIX_Group* group, const SDL_AudioSpec* spec, float* pcm, int samples);
NYA_INTERNAL void SDLCALL _nya_audio_post_mix_callback(void* userdata, MIX_Mixer* mixer, const SDL_AudioSpec* spec, float* pcm, int samples);

/** A uniform draw in ±`half_range`, zero when that is not positive. */
NYA_INTERNAL f32 _nya_audio_jitter(f32 half_range) __attr_no_discard;

/**
 * `pitch` detuned by up to ±`semitones`. Drawn in semitones because a ratio range is asymmetric: ±0.06 is
 * 1.07 semitones up and 1.14 down, so linear jitter drifts flat.
 * */
NYA_INTERNAL f32 _nya_audio_vary_pitch(f32 pitch, f32 semitones) __attr_no_discard;

/** `gain` moved by up to ±`db`. Not clamped; a caller wanting a ceiling passes a lower gain. */
NYA_INTERNAL f32 _nya_audio_vary_gain(f32 gain, f32 db) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_system_audio_init(void) {
    NYA_AudioSystem* system = &_nya_audio_system;

    // defaults set before anything runs, since a zeroed gain would silence everything.
    system->music_slot  = _NYA_AUDIO_MUSIC_A;
    system->master_gain = 1.0F;
    system->sound_gain  = 1.0F;
    system->music_gain  = 1.0F;

    // seeded on every path, device or not, or every run detunes identically.
    system->rng = nya_rng_create();

    // a zero reference distance would divide by zero, and a game may play a positional sound before placing the
    // listener.
    system->listener = (NYA_AudioListener){ .reference_distance = 1.0F };

    // the graphics convention: at the origin, looking down -z with +y up.
    system->listener_3d = (NYA_AudioListener3D){
        .forward            = { 0.0F, 0.0F, -1.0F },
        .up                 = { 0.0F, 1.0F, 0.0F },
        .reference_distance = 1.0F,
    };

    // the asset system owns the mixer, because a MIX_Audio cannot outlive the mixer that decoded it.
    MIX_Mixer* mixer = nya_app_get()->asset_system.mixer;

    // no mixer means no audio device, normal on CI and headless builds. every call below becomes a no-op.
    if (mixer == nullptr) {
        nya_log_info("Audio system initialized (no mixer: nothing will be heard).");
        return NYA_OK;
    }

    for (u32 i = 0; i < NYA_AUDIO_BUS_COUNT; i++) _nya_audio_filter_reset(&system->filters[i]);

    // effect and music buses, so each can be filtered alone. master is the mixer, filtered by the post-mix
    // callback.
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

        // fixed for the process: an unassigned track mixes through SDL_mixer's default group, out of reach of our
        // callbacks.
        MIX_Group* group = i >= NYA_AUDIO_VOICES ? system->groups[NYA_AUDIO_BUS_MUSIC] : system->groups[NYA_AUDIO_BUS_SOUND];
        if (!MIX_SetTrackGroup(system->slots[i].track, group)) {
            return nya_error(NYA_ERROR_NOT_OK, "MIX_SetTrackGroup() failed for slot %u: %s", i, SDL_GetError());
        }

        // attached once, wide open: installing a callback on a playing track races the mixer's thread.
        _nya_audio_filter_reset(&system->slots[i].filter);

        // one, not zero: it is a multiplier. see _nya_audio_apply_gain.
        system->slots[i].occlusion_gain = 1.0F;

        if (!MIX_SetTrackCookedCallback(system->slots[i].track, _nya_audio_track_mix_callback, &system->slots[i].filter)) {
            return nya_error(NYA_ERROR_NOT_OK, "MIX_SetTrackCookedCallback() failed for slot %u: %s", i, SDL_GetError());
        }
    }

    system->ready = true;

    nya_log_info("Audio system initialized (%d voices).", NYA_AUDIO_VOICES);
    return NYA_OK;
}

void nya_system_audio_deinit(void) {
    NYA_AudioSystem* system = &_nya_audio_system;

    if (!system->ready) {
        nya_log_info("Audio system deinitialized (no mixer).");
        return;
    }

    // callbacks first: they run on the live mixer thread and point into this struct.
    MIX_Mixer* mixer = nya_app_get()->asset_system.mixer;
    if (mixer != nullptr) MIX_SetPostMixCallback(mixer, nullptr, nullptr);

    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) {
        if (system->groups[bus] != nullptr) MIX_SetGroupPostMixCallback(system->groups[bus], nullptr, nullptr);
    }

    // tracks before the mixer, which the asset system destroys after this.
    for (u32 i = 0; i < _NYA_AUDIO_SLOTS; i++) {
        if (system->slots[i].track != nullptr) MIX_DestroyTrack(system->slots[i].track);
        system->slots[i] = (NYA_AudioVoice){ 0 };
    }

    // after the tracks, so nothing is reassigned to the default group on its way out.
    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) {
        if (system->groups[bus] != nullptr) MIX_DestroyGroup(system->groups[bus]);
        system->groups[bus] = nullptr;
    }

    system->ready = false;

    nya_log_info("Audio system deinitialized.");
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

    // placed before MIX_PlayTrack, so the sound is never heard centred first.
    NYA_SoundVoice voice = _nya_audio_play(sound_handle, params, &position);

    // the world position is recorded after; what went down above is listener-relative.
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

    // first silent slot. linear over sixteen, not on a hot path.
    u32 slot = _NYA_AUDIO_SLOTS;
    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) {
        if (!MIX_TrackPlaying(system->slots[i].track)) {
            slot = i;
            break;
        }
    }

    // every voice busy: the lowest priority one may be taken, but only if strictly outranked, so equal-priority
    // sounds never cut each other off.
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

    // a reused slot starts unoccluded and unplaced; inheriting either from the previous sound is wrong.
    system->slots[slot].occlusion_gain = 1.0F;
    system->slots[slot].positional     = false;

    atomic_store_explicit(&system->slots[slot].filter.target_hz, 0.0F, memory_order_relaxed);


    // zero means unset, since a zeroed struct would be silence at zero speed. variation goes into the remembered
    // gain, so later volume changes keep it.
    system->slots[slot].base_gain = _nya_audio_vary_gain(params.gain > 0.0F ? params.gain : 1.0F, params.gain_variation_db);
    system->slots[slot].priority  = params.priority;
    _nya_audio_apply_gain(slot);

    // rolled once, so a looping sound keeps its detune.
    MIX_SetTrackFrequencyRatio(track, _nya_audio_vary_pitch(params.pitch > 0.0F ? params.pitch : 1.0F, params.pitch_variation_semitones));

    // spatialisation reset first: settings persist on a reused track, and a slot panned left would play the next
    // sound left too.
    MIX_SetTrackStereo(track, nullptr);
    MIX_SetTrack3DPosition(track, nullptr);

    // against the track, since the voice does not exist yet. placement wins over pan.
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

    // bumped only once the sound runs, so a failed play leaves existing handles valid.
    system->slots[slot].generation++;

    return (NYA_SoundVoice){ .index = slot, .generation = system->slots[slot].generation };
}

void nya_audio_listener_set(NYA_AudioListener listener) {
    // zero means unset, so a listener built with only a position does not divide by zero.
    if (listener.reference_distance <= 0.0F) listener.reference_distance = 1.0F;

    // out of range is a caller mistake; the mapping below would quietly treat it as side on.
    nya_assert(listener.plane < NYA_AUDIO_PLANE_COUNT, "unknown audio plane %d", (s32)listener.plane);

    _nya_audio_system.listener = listener;
}

NYA_AudioListener nya_audio_listener_get(void) {
    return _nya_audio_system.listener;
}

void nya_audio_listener_3d_set(NYA_AudioListener3D listener) {
    // zero means unset.
    if (listener.reference_distance <= 0.0F) listener.reference_distance = 1.0F;

    if (nya_vector_length(listener.forward) < NYA_EPSILON) listener.forward = (f32x3){ 0.0F, 0.0F, -1.0F };
    if (nya_vector_length(listener.up) < NYA_EPSILON) listener.up = (f32x3){ 0.0F, 1.0F, 0.0F };

    // rejected: a `forward` parallel to `up` has no right vector, and every sound would sit dead centre.
    // see nya_matrix_look_at for the same degeneracy.
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

    // nothing to fade from, so this is a fade in.
    if (!MIX_TrackPlaying(system->slots[system->music_slot].track)) {
        params.fade_in_ms = duration_ms;
        nya_audio_play_music_with(music_handle, params);
        return;
    }

    MIX_Audio* audio = _nya_audio_get(music_handle);
    if (audio == nullptr) return;

    u32 outgoing = system->music_slot;
    u32 incoming = outgoing == _NYA_AUDIO_MUSIC_A ? _NYA_AUDIO_MUSIC_B : _NYA_AUDIO_MUSIC_A;

    // the incoming slot may still be finishing a crossfade; there is nowhere for a third piece.
    MIX_StopTrack(system->slots[incoming].track, 0);

    if (!MIX_SetTrackAudio(system->slots[incoming].track, audio)) {
        nya_log_warn("could not set the music track to '%s': %s", music_handle, SDL_GetError());
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
        nya_log_warn("could not play the music '%s': %s", music_handle, SDL_GetError());
        return;
    }

    // only once the new piece runs, or a failed start leaves silence.
    MIX_StopTrack(system->slots[outgoing].track, (s64)duration_ms);

    system->slots[incoming].generation++;
    system->music_slot = incoming;
}

void nya_audio_play_music_with(NYA_ConstCString music_handle, NYA_MusicParams params) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    MIX_Audio* audio = _nya_audio_get(music_handle);
    if (audio == nullptr) return;

    // both slots: a crossfade may still be running.
    MIX_StopTrack(system->slots[_NYA_AUDIO_MUSIC_A].track, 0);
    MIX_StopTrack(system->slots[_NYA_AUDIO_MUSIC_B].track, 0);

    if (!MIX_SetTrackAudio(system->slots[system->music_slot].track, audio)) {
        nya_log_warn("could not set the music track to '%s': %s", music_handle, SDL_GetError());
        return;
    }

    system->slots[system->music_slot].base_gain = params.gain > 0.0F ? params.gain : 1.0F;
    _nya_audio_apply_gain(system->music_slot);

    // created per call; track changes are rare.
    SDL_PropertiesID options = SDL_CreateProperties();

    // -1 is forever; the count is repeats after the first play, so zero plays once.
    SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, params.loop ? -1 : 0);

    if (params.fade_in_ms > 0) SDL_SetNumberProperty(options, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, (s64)params.fade_in_ms);

    // intro then loop: play from the start, repeat from here. see NYA_MusicParams.
    if (params.loop_start_ms > 0) SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOP_START_MILLISECOND_NUMBER, (s64)params.loop_start_ms);

    b8 started = MIX_PlayTrack(system->slots[system->music_slot].track, options);
    if (!started) nya_log_warn("could not play the music '%s': %s", music_handle, SDL_GetError());

    SDL_DestroyProperties(options);

    // a new piece is a new sound, so old handles stop resolving.
    if (started) system->slots[system->music_slot].generation++;
}

void nya_audio_stop_music(u32 fade_out_ms) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    // both slots, or a crossfade's outgoing half keeps playing.
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

    // paused counts as not playing.
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

    // paused counts as valid, so check-then-set matches set.
    return MIX_TrackPlaying(slot->track) || MIX_TrackPaused(slot->track);
}

void nya_audio_voice_set_gain(NYA_SoundVoice voice, f32 gain) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // remembered, so later category or master changes keep this voice's level.
    slot->base_gain = nya_max(0.0F, gain);
    _nya_audio_apply_gain(voice.index);
}

void nya_audio_voice_set_pitch(NYA_SoundVoice voice, f32 ratio) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // zero would stop the playhead; negative is undefined.
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

    // remembered too, because SDL_mixer's copy is listener-relative.
    slot->world_position = (f32x3){ world_position.x, world_position.y, 0.0F };
    slot->positional     = true;

    // read fresh, against where the listener is now.
    _nya_audio_track_set_position(slot->track, _nya_audio_world_to_audio(world_position));
}

void nya_audio_voice_set_world_position_3d(NYA_SoundVoice voice, f32x3 world_position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    slot->world_position = world_position;
    slot->positional     = true;

    // read fresh: the camera turns more often than sources move.
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

    // two relaxed stores, as the bus filter does.
    atomic_store_explicit(&slot->filter.target_hz, nya_max(0.0F, filter.lowpass_hz), memory_order_relaxed);
    atomic_store_explicit(&slot->filter.glide_ms, nya_max(0.0F, filter.glide_ms), memory_order_relaxed);
}

void nya_audio_occlusion_set(NYA_AudioOcclusionFn function, void* user_data, NYA_AudioOcclusion occlusion) {
    NYA_AudioSystem* system = &_nya_audio_system;

    // zero means unset. half gain is roughly a solid wall before filtering.
    if (occlusion.gain <= 0.0F) occlusion.gain = 0.5F;
    if (occlusion.glide_ms <= 0.0F) occlusion.glide_ms = 80.0F;

    system->occlusion_function  = function;
    system->occlusion_user_data = user_data;
    system->occlusion           = occlusion;

    // turning it off clears what it applied, or muffled voices stay muffled.
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

        // only sounding voices: finished ones keep their slot until reused.
        if (!MIX_TrackPlaying(slot->track)) {
            slot->positional = false;
            continue;
        }

        f32 occlusion = nya_clamp(system->occlusion_function(slot->world_position, system->occlusion_user_data), 0.0F, 1.0F);

        // interpolated toward the configured cutoff, so a callback reporting a fraction (some rays blocked) gets a
        // partially muffled sound. zero stores a cutoff the filter reads as wide open.
        f32 cutoff = 0.0F;

        if (system->occlusion.lowpass_hz > 0.0F && occlusion > 0.0F) {
            // from the top of the audible band down, so partial occlusion rolls treble off gradually.
            cutoff = nya_lerp(20000.0F, system->occlusion.lowpass_hz, occlusion);
        }

        atomic_store_explicit(&slot->filter.target_hz, cutoff, memory_order_relaxed);
        atomic_store_explicit(&slot->filter.glide_ms, system->occlusion.glide_ms, memory_order_relaxed);

        f32 gain = nya_lerp(1.0F, system->occlusion.gain, occlusion);

        // a third multiplier on the remembered gain, so moving a volume slider cannot lose it.
        if (fabsf(gain - slot->occlusion_gain) > 0.001F) {
            slot->occlusion_gain = gain;

            // through the shared formula, so category and master are reapplied.
            _nya_audio_apply_gain(i);
        }
    }
}

void nya_audio_bus_reverb_set(NYA_AudioBus bus, NYA_AudioReverb reverb) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    NYA_AudioReverbState* state = &_nya_audio_system.reverbs[bus];

    // zero means unset for the mix controls. `room_size` and `damping` are not defaulted: zero room turns the
    // reverb off, and zero damping is a real setting.
    if (reverb.wet <= 0.0F) reverb.wet = 0.3F;
    if (reverb.dry <= 0.0F) reverb.dry = 1.0F;
    if (reverb.width <= 0.0F) reverb.width = 1.0F;

    /*
     * Clamped below one: a comb feedback of one never decays, and above one grows until the buffer is full of
     * infinities. Silent for a second, then permanent.
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

    // not guarded on `ready`, so filters set before the device is up are kept.
    atomic_store_explicit(&state->target_hz, nya_max(0.0F, filter.lowpass_hz), memory_order_relaxed);
    atomic_store_explicit(&state->glide_ms, nya_max(0.0F, filter.glide_ms), memory_order_relaxed);
}

NYA_AudioFilter nya_audio_bus_filter_get(NYA_AudioBus bus) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    NYA_AudioFilterState* state = &_nya_audio_system.filters[bus];

    // the target, so this round trips with the setter. the live coefficient belongs to the mixer's thread.
    return (NYA_AudioFilter){
        .lowpass_hz = atomic_load_explicit(&state->target_hz, memory_order_relaxed),
        .glide_ms   = atomic_load_explicit(&state->glide_ms, memory_order_relaxed),
    };
}

void nya_audio_set_master_gain(f32 gain) {
    _nya_audio_system.master_gain = nya_max(0.0F, gain);

    /* Music updates now; effects on their next change. */
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

    // missing or still loading, normal right after a load.
    if (asset == nullptr) return nullptr;
    if (asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;

    // the wrong asset type is a caller mistake, worth saying since silence gives no clue.
    if (asset->type != NYA_ASSET_TYPE_SOUND) {
        /* Said once per handle, not per call. */
        NYA_INTERNAL NYA_ConstCString last_warned = nullptr;

        if (last_warned != handle) {
            last_warned = handle;
            nya_log_warn("'%s' is not a sound asset, so it cannot be played", handle);
        }

        return nullptr;
    }

    return asset->as_sound.audio;
}

NYA_AudioVoice* _nya_audio_resolve(NYA_SoundVoice voice) {
    NYA_AudioSystem* system = &_nya_audio_system;

    if (!system->ready) return nullptr;
    if (voice.index >= _NYA_AUDIO_SLOTS) return nullptr;

    // zero is never a live generation, so NYA_SOUND_VOICE_NONE does not resolve to slot zero.
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

    // the slot decides which category gain applies; both music slots take the music gain, including one fading
    // out.
    f32 category = slot >= NYA_AUDIO_VOICES ? system->music_gain : system->sound_gain;

    /* Occlusion is a fourth multiplier here. */
    f32 occlusion = system->slots[slot].occlusion_gain > 0.0F ? system->slots[slot].occlusion_gain : 1.0F;

    MIX_SetTrackGain(system->slots[slot].track, system->slots[slot].base_gain * category * system->master_gain * occlusion);
}

void _nya_audio_track_set_pan(MIX_Track* track, f32 pan) {
    pan = nya_clamp(pan, -1.0F, 1.0F);

    /* Equal power, not linear. */
    f32 angle = (pan + 1.0F) * 0.25F * (f32)M_PI;

    MIX_SetTrackStereo(track, &(MIX_StereoGains){ .left = cosf(angle), .right = sinf(angle) });
}

void _nya_audio_track_set_position(MIX_Track* track, f32x3 position) {
    // overrides pan: SDL_mixer keeps only the latest.
    MIX_SetTrack3DPosition(track, &(MIX_Point3D){ .x = position[0], .y = position[1], .z = position[2] });
}

f32x3 _nya_audio_world_to_audio(f32x2 world_position) {
    NYA_AudioListener listener = _nya_audio_system.listener;

    // never zero: the setter substitutes 1.0. dividing maps the mixer's reference distance of 1.0 to
    // reference_distance world units.
    f32x2 offset = (world_position - listener.position) / listener.reference_distance;

    /*
     * The mixer's space is right handed, y up, z back; the renderer's 2D world is y down. NYA_AudioPlane says
     * which axis y maps to.
     */
    switch (listener.plane) {
        case NYA_AUDIO_PLANE_TOP_DOWN:
            // the screen is the ground: down the screen is behind the listener.
            return (f32x3){ offset[0], 0.0F, offset[1] };

        case NYA_AUDIO_PLANE_SIDE:
        case NYA_AUDIO_PLANE_COUNT:
        default:
            // the screen is a wall: down the screen is down.
            return (f32x3){ offset[0], -offset[1], 0.0F };
    }
}

f32x3 _nya_audio_world_to_audio_3d(f32x3 world_position) {
    NYA_AudioListener3D listener = _nya_audio_system.listener_3d;

    // never zero: the setter substitutes 1.0.
    f32x3 offset = (world_position - listener.position) / listener.reference_distance;

    /* An orthonormal frame from the listener's facing, as a look-at matrix builds it. */
    f32x3 forward = nya_vector_normalize(listener.forward);
    f32x3 right   = nya_vector_normalize(nya_vector_cross(forward, listener.up));
    f32x3 up      = nya_vector_cross(right, forward);

    /* Projected onto that frame, forward becoming negative z. */
    return (f32x3){
        nya_vector_dot(offset, right),
        nya_vector_dot(offset, up),
        -nya_vector_dot(offset, forward),
    };
}

void _nya_audio_filter_reset(NYA_AudioFilterState* filter) {
    atomic_store_explicit(&filter->target_hz, 0.0F, memory_order_relaxed);
    atomic_store_explicit(&filter->glide_ms, 0.0F, memory_order_relaxed);

    // one, not zero: a zero coefficient is a filter clamped shut.
    filter->coefficient = 1.0F;

    for (u32 i = 0; i < NYA_AUDIO_FILTER_MAX_CHANNELS; i++) filter->state[i] = 0.0F;
}

void _nya_audio_reverb_configure(NYA_AudioReverbState* reverb, s32 rate) {
    /*
     * The published lengths are for 44.1 kHz, so other rates scale them. Delays count samples, and unscaled
     * lengths would halve the room at 96 kHz.
     */
    f32 scale = (f32)rate / 44100.0F;

    for (u32 network = 0; network < _NYA_AUDIO_REVERB_NETWORKS; network++) {
        // the second network's lines are offset, so the two channels are not the same room.
        u32 spread = network == 0 ? 0 : _NYA_AUDIO_REVERB_STEREO_SPREAD;

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
            NYA_AudioReverbComb* comb = &reverb->combs[network][i];

            u32 length = (u32)(((f32)_NYA_AUDIO_REVERB_COMB_LENGTHS[i] + (f32)spread) * scale);

            // clamped to the buffer and never zero: a zero-length line feeds output straight back into input.
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
     * No room, no work, and the lines are left alone. Clearing them would cut a tail dead with a click; they decay
     * on their own once switched on again.
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

    /* Fed at a fraction of the input: four parallel combs sum, and a full feed would clip the allpasses. */
    const f32 feed = 0.015F;

    // 0.5 is the classic allpass coefficient. it shapes phase, not tail length.
    const f32 allpass_feedback = 0.5F;

    for (s32 frame = 0; frame < frames; frame++) {
        f32* row = &pcm[(ptrdiff_t)frame * channels];

        /*
         * Downmixed by channel parity: even channels are left in every standard layout, odd ones right. Mono is
         * one even channel, so both networks produce the same output.
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
             * Combs in parallel, each with a one-pole in its feedback. The damping makes highs decay faster than lows,
             * like real surfaces, instead of ringing bright forever.
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

            /* Allpasses in series multiply echo density without adding colour. Four combs alone flutter. */
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

        /* Width crossfeeds the two tails toward their average, which keeps the level constant as width changes. */
        f32 average = (output[0] + output[1]) * 0.5F;

        f32 left  = nya_lerp(average, output[0], width);
        f32 right = nya_lerp(average, output[1], width);

        for (s32 channel = 0; channel < channels; channel++) {
            f32 tail = (channel & 1) == 0 ? left : right;

            row[channel] = (row[channel] * dry) + (tail * wet);
        }
    }

    /* Flushed to zero once inaudible: the decaying values reach denormals, which some CPUs process very slowly. */
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

    // more speakers than filter state; passed through, since half a filtered surround field is worse than none.
    if (spec->channels > NYA_AUDIO_FILTER_MAX_CHANNELS) return;

    f32 target_hz = atomic_load_explicit(&filter->target_hz, memory_order_relaxed);
    f32 glide_ms  = atomic_load_explicit(&filter->glide_ms, memory_order_relaxed);

    /*
     * The one-pole coefficient: a = 1 - e^(-2π·fc/fs). A zero cutoff is a coefficient of exactly one, so an
     * unfiltered bus is bit exact.
     */
    f32 target = 1.0F;
    if (target_hz > 0.0F) {
        target = 1.0F - expf(-2.0F * (f32)M_PI * target_hz / (f32)spec->freq);

        // at or above Nyquist is open.
        if (target > 1.0F) target = 1.0F;
        if (target < 0.0F) target = 0.0F;
    }

    s32 channels = spec->channels;
    s32 frames   = samples / channels;
    if (frames <= 0) return;

    /*
     * How far the coefficient may move this buffer. Rate limiting needs no transition state, and a target that
     * changes mid-glide is followed from wherever the coefficient is.
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
         * No glide asked for, so the coefficient jumps now. Sweeping would reach the setting a buffer late and sound
         * like a short glide. Only the state has to stay continuous.
         */
        filter->coefficient = target;
    }

    // wide open with no history: an identity, skipped. the common case.
    if (filter->coefficient == 1.0F && end == 1.0F) {
        for (s32 channel = 0; channel < channels; channel++) filter->state[channel] = 0.0F;
        return;
    }

    // swept across the buffer, since stepping once per buffer makes zipper noise.
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

    /* Flushed to zero once inaudible, since a decaying one pole reaches denormals. */
    for (s32 channel = 0; channel < channels; channel++) {
        if (fabsf(filter->state[channel]) < 1e-20F) filter->state[channel] = 0.0F;
    }
}

void SDLCALL _nya_audio_track_mix_callback(void* userdata, MIX_Track* track, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(track);

    /* The cooked hook, not the raw one. */
    _nya_audio_filter_apply((NYA_AudioFilterState*)userdata, spec, pcm, samples);
}

void SDLCALL _nya_audio_group_mix_callback(void* userdata, MIX_Group* group, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(group);

    NYA_AudioFilterState* filter = userdata;

    /* Filter first, then reverb; the order is audible. */
    _nya_audio_filter_apply(filter, spec, pcm, samples);

    // the filter's index in the system's array is the bus.
    u64 bus = (u64)(filter - &_nya_audio_system.filters[0]);

    if (bus < NYA_AUDIO_BUS_COUNT) _nya_audio_reverb_apply(&_nya_audio_system.reverbs[bus], spec, pcm, samples);
}

void SDLCALL _nya_audio_post_mix_callback(void* userdata, MIX_Mixer* mixer, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(mixer);

    // master, after every group is mixed. same order as a group.
    _nya_audio_filter_apply((NYA_AudioFilterState*)userdata, spec, pcm, samples);
    _nya_audio_reverb_apply(&_nya_audio_system.reverbs[NYA_AUDIO_BUS_MASTER], spec, pcm, samples);
}

f32 _nya_audio_jitter(f32 half_range) {
    // zero costs a compare rather than a sample. negative also means none.
    if (half_range <= 0.0F) return 0.0F;

    NYA_RNGDistribution range = {
        .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
        .uniform = { .min = -(f64)half_range, .max = (f64)half_range },
    };

    return nya_rng_sample_f32(&_nya_audio_system.rng, range);
}

f32 _nya_audio_vary_pitch(f32 pitch, f32 semitones) {
    // keeps "no variation" bit exact.
    if (semitones <= 0.0F) return pitch;

    // twelve semitones to an octave, which doubles the rate. uniform in the exponent.
    return pitch * exp2f(_nya_audio_jitter(semitones) / 12.0F);
}

f32 _nya_audio_vary_gain(f32 gain, f32 db) {
    if (db <= 0.0F) return gain;

    // twenty, not ten: amplitude decibels, which is what a gain multiplier is.
    return gain * powf(10.0F, _nya_audio_jitter(db) / 20.0F);
}

void _nya_audio_apply_music_gain(void) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    _nya_audio_apply_gain(system->music_slot);
}
