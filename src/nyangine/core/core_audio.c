#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_AudioSystem      NYA_AudioSystem;
typedef struct NYA_AudioFilterState NYA_AudioFilterState;

/**
 * One voice's filter. The atomics are written by the game and read by the audio thread; everything after
 * them is the audio thread's alone. No lock on the mixing path: blocking in a mix callback drops audio.
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
     * propagation needs one sound behind a wall.
     * */
    NYA_AudioFilterState filter;

    /**
     * The voice's world position, if placed. SDL_mixer stores positions listener-relative, so propagation needs
     * this copy.
     * */
    f32x3 world_position;
    b8    positional;

    /** Placed in 2D, z zero, rather than in 3D. Decides which listener and which space traces it. */
    b8 planar;

    /** See NYA_SoundParams.radius. */
    f32 radius;

    /** The gain propagation is applying, so it can be folded out again. */
    f32 propagation_gain;

    /**
     * The stereo panner placing this voice, when it is on. See core_audio_panner.h and
     * nya_audio_panner_set_enabled. The render state is the mixer thread's alone once playing.
     * */
    NYA_AudioPanRender pan_render;

    /*
     * Written by the game, read by the mixer, relaxed on both sides like the filter's pair: the source
     * azimuth and whether the panner is the one placing this voice.
     */
    atomic f32 pan_azimuth;
    atomic b8  pan_active;

    /**
     * Distance attenuation the panner path applies in place of the mixer's, since taking over the pan also
     * takes over MIX_SetTrack3DPosition's falloff. One when the panner is off, so the mixer's own applies.
     * */
    f32 spatial_gain;
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

    /**
     * Each bus's effect chain, published the first time the bus needs one. The callbacks read these slots, so a bus
     * nobody set costs one load a buffer.
     * */
    atomic(NYA_AudioChain*) chains[NYA_AUDIO_BUS_COUNT];

    /** What each bus was set to. The sound bus's chain is this with the environment's reverb and echoes laid over. */
    NYA_AudioEffects effects[NYA_AUDIO_BUS_COUNT];

    /** Where chains and their delay lines come from. Kept until deinit: the mixer may be reading them. */
    NYA_Arena* line_allocator;

    /** See nya_audio_propagation_set. Validated. */
    NYA_AudioPropagation propagation;
    NYA_AudioTracer      tracer;

    /** What answers rays, per space. See nya_audio_rays_set. */
    NYA_AudioRayFn ray_functions[NYA_AUDIO_SPACE_COUNT];
    void*          ray_user_data[NYA_AUDIO_SPACE_COUNT];

    /**
     * The buses effects and music mix through, indexed by NYA_AudioBus. The master entry stays null: master is
     * the mixer itself, processed through MIX_SetPostMixCallback.
     * */
    MIX_Group* groups[NYA_AUDIO_BUS_COUNT];

    /** Which of the two music slots is "the music". */
    u32 music_slot;

    /**
     * Whether positioned sounds are placed by our own stereo panner rather than SDL_mixer's. Off by default,
     * so a game that never asks for it hears exactly what it did. See nya_audio_panner_set_enabled.
     * */
    b8 panner_enabled;

    /** The mixer device's channel count, read once at init. The panner engages only on a stereo device. */
    s32 device_channels;

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

/** Records where a voice is in the world, for propagation to trace. A no-op for a dead handle. */
NYA_INTERNAL void _nya_audio_voice_remember_position(NYA_SoundVoice voice, f32x3 world_position, b8 planar);

/** Places a positional voice at its world position, moved by what propagation found, against its listener. */
NYA_INTERNAL void _nya_audio_voice_place(u32 slot);

/**
 * Hands a bus's settings, and for the sound bus propagation's, to its chain, creating the chain the first time any
 * unit is on. Unchanged settings publish nothing.
 * */
NYA_INTERNAL void _nya_audio_bus_publish(NYA_AudioBus bus);

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

/**
 * Places a track at a listener-relative point: through our own panner when it is on and the device is stereo,
 * otherwise through SDL_mixer's positioning. Takes the slot so it can arm the panner and fold in distance.
 * */
NYA_INTERNAL void _nya_audio_place_track(u32 slot, MIX_Track* track, f32x3 position);

/** Takes a voice off the panner and drops its distance gain, for when SDL_mixer's own positioning takes over. */
NYA_INTERNAL void _nya_audio_panner_disarm(NYA_AudioVoice* slot, u32 index);

/** Frees every chain and its lines. Only once no callback can run. */
NYA_INTERNAL void _nya_audio_lines_release(NYA_AudioSystem* system);

/** Resets a filter to wide open with no history, which is not all zeroes. */
NYA_INTERNAL void _nya_audio_filter_reset(NYA_AudioFilterState* filter);

/**
 * Rolls `pcm` off in place, gliding toward the last requested cutoff. Runs on the mixer's thread: no
 * allocation, logging or locks. `samples` counts floats, not frames, as SDL_mixer does.
 * */
NYA_INTERNAL void _nya_audio_filter_apply(NYA_AudioFilterState* filter, const SDL_AudioSpec* spec, f32* pcm, s32 samples);

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

    system->line_allocator = nya_arena_create(.name = "audio_lines");

    // guarded, so restarting the app in one process does not register duplicates.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("audio_rays", NYA_AUDIO_PROPAGATION_RAYS_MAX, &system->tracer.rays_cast);
        ceiling_registered = true;
    }

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

    // the device's channel count, so the panner can bow out on anything but a stereo pair. A failed query
    // leaves it zero, which the panner reads as "not stereo".
    SDL_AudioSpec device_spec = { 0 };
    if (MIX_GetMixerFormat(mixer, &device_spec)) system->device_channels = device_spec.channels;

    // effect and music buses, so each has its own chain. master is the mixer, processed by the post-mix callback.
    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) {
        if (bus == NYA_AUDIO_BUS_MASTER) continue;

        system->groups[bus] = MIX_CreateGroup(mixer);
        if (system->groups[bus] == nullptr) return nya_error(NYA_ERROR_NOT_OK, "MIX_CreateGroup() failed for bus %u: %s", bus, SDL_GetError());

        if (!MIX_SetGroupPostMixCallback(system->groups[bus], _nya_audio_group_mix_callback, (void*)&system->chains[bus])) {
            return nya_error(NYA_ERROR_NOT_OK, "MIX_SetGroupPostMixCallback() failed for bus %u: %s", bus, SDL_GetError());
        }
    }

    if (!MIX_SetPostMixCallback(mixer, _nya_audio_post_mix_callback, (void*)&system->chains[NYA_AUDIO_BUS_MASTER])) {
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
        system->slots[i].propagation_gain = 1.0F;
        system->slots[i].spatial_gain     = 1.0F;

        // the panner starts off and open on every slot; positioned playback arms it per sound.
        atomic_store_explicit(&system->slots[i].pan_active, false, memory_order_relaxed);
        atomic_store_explicit(&system->slots[i].pan_azimuth, 0.0F, memory_order_relaxed);
        nya_audio_pan_render_reset(&system->slots[i].pan_render);

        // the whole voice, not just its filter: the cooked hook now runs the panner too. See the callback.
        if (!MIX_SetTrackCookedCallback(system->slots[i].track, _nya_audio_track_mix_callback, &system->slots[i])) {
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
        _nya_audio_lines_release(system);
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

    // after the callbacks are gone, since the mixer thread reads the lines.
    _nya_audio_lines_release(system);

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
    _nya_audio_voice_remember_position(voice, (f32x3){ world_position.x, world_position.y, 0.0F }, true);

    return voice;
}

NYA_SoundVoice nya_audio_play_sound_at_3d(NYA_ConstCString sound_handle, f32x3 world_position, NYA_SoundParams params) {
    f32x3 position = _nya_audio_world_to_audio_3d(world_position);

    NYA_SoundVoice voice = _nya_audio_play(sound_handle, params, &position);

    _nya_audio_voice_remember_position(voice, world_position, false);

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
    system->slots[slot].propagation_gain = 1.0F;
    system->slots[slot].positional       = false;
    system->slots[slot].radius           = nya_max(params.radius, 0.0F);

    // the panner too: a reset here is safe because the track is stopped, so its cooked callback is not
    // running and cannot be touching the render state.
    system->slots[slot].spatial_gain = 1.0F;
    atomic_store_explicit(&system->slots[slot].pan_active, false, memory_order_relaxed);
    nya_audio_pan_render_reset(&system->slots[slot].pan_render);

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
        _nya_audio_place_track(slot, track, *position);
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

void nya_audio_panner_set_enabled(b8 enabled) {
    // not guarded on `ready`: a game may set it before the device is up, and placement reads it live.
    _nya_audio_system.panner_enabled = enabled;
}

b8 nya_audio_panner_enabled(void) {
    return _nya_audio_system.panner_enabled;
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

    // an explicit pan takes the voice off the panner, or the cooked hook would place it a second time.
    _nya_audio_panner_disarm(slot, voice.index);

    _nya_audio_track_set_pan(slot->track, pan);
}

void nya_audio_voice_set_position(NYA_SoundVoice voice, f32x3 position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // the low-level 3D setter is SDL_mixer's own; take the voice off our panner so the two do not stack.
    _nya_audio_panner_disarm(slot, voice.index);

    _nya_audio_track_set_position(slot->track, position);
}

void nya_audio_voice_set_world_position(NYA_SoundVoice voice, f32x2 world_position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    // remembered too, because SDL_mixer's copy is listener-relative.
    slot->world_position = (f32x3){ world_position.x, world_position.y, 0.0F };
    slot->positional     = true;
    slot->planar         = true;

    // read fresh, against where the listener is now.
    _nya_audio_voice_place(voice.index);
}

void nya_audio_voice_set_world_position_3d(NYA_SoundVoice voice, f32x3 world_position) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    slot->world_position = world_position;
    slot->positional     = true;
    slot->planar         = false;

    // read fresh: the camera turns more often than sources move.
    _nya_audio_voice_place(voice.index);
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

void nya_audio_bus_effects_set(NYA_AudioBus bus, NYA_AudioEffects effects) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    // not guarded on `ready`, so effects set before the device is up are kept.
    _nya_audio_system.effects[bus] = _nya_audio_effects_validate(effects);

    _nya_audio_bus_publish(bus);
}

NYA_AudioEffects nya_audio_bus_effects_get(NYA_AudioBus bus) {
    nya_assert(bus < NYA_AUDIO_BUS_COUNT, "unknown audio bus %d", (s32)bus);

    return _nya_audio_system.effects[bus];
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

void _nya_audio_voice_remember_position(NYA_SoundVoice voice, f32x3 world_position, b8 planar) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);
    if (slot == nullptr) return;

    slot->world_position = world_position;
    slot->positional     = true;
    slot->planar         = planar;
}

void _nya_audio_voice_place(u32 slot) {
    NYA_AudioSystem* system = &_nya_audio_system;
    nya_assert(slot < _NYA_AUDIO_SLOTS);

    NYA_AudioVoice* voice = &system->slots[slot];

    // music has no path.
    f32x3 position = voice->world_position;
    if (slot < NYA_AUDIO_VOICES) position += system->tracer.paths[slot].offset;

    f32x3 heard = voice->planar ? _nya_audio_world_to_audio((f32x2){ position.x, position.y }) : _nya_audio_world_to_audio_3d(position);

    _nya_audio_place_track(slot, voice->track, heard);
}

void _nya_audio_bus_publish(NYA_AudioBus bus) {
    NYA_AudioSystem* system = &_nya_audio_system;
    nya_assert(bus < NYA_AUDIO_BUS_COUNT);

    NYA_AudioChainSettings settings = { 0 };
    settings.effects                = system->effects[bus];

    const NYA_AudioPropagation* propagation = &system->propagation;

    if (bus == NYA_AUDIO_BUS_SOUND && propagation->enabled && propagation->environment) {
        settings.effects.reverb = system->tracer.environment.reverb;
        settings.reflections    = system->tracer.reflections;
    }

    NYA_AudioChain* chain = atomic_load(&system->chains[bus]);

    if (chain == nullptr) {
        // a bus that has never needed a unit keeps costing nothing.
        if (!_nya_audio_chain_settings_active(&settings) || system->line_allocator == nullptr) return;

        chain = _nya_audio_chain_create(system->line_allocator);
        _nya_audio_chain_publish(chain, &settings, system->line_allocator);
        atomic_store(&system->chains[bus], chain);
        return;
    }

    _nya_audio_chain_publish(chain, &settings, system->line_allocator);
}

void _nya_audio_apply_gain(u32 slot) {
    NYA_AudioSystem* system = &_nya_audio_system;
    if (!system->ready) return;

    // the slot decides which category gain applies; both music slots take the music gain, including one fading
    // out.
    f32 category = slot >= NYA_AUDIO_VOICES ? system->music_gain : system->sound_gain;

    /* Propagation is a fourth multiplier here, and the panner's distance falloff a fifth. */
    f32 propagation = system->slots[slot].propagation_gain;
    f32 spatial     = system->slots[slot].spatial_gain;

    MIX_SetTrackGain(system->slots[slot].track, system->slots[slot].base_gain * category * system->master_gain * propagation * spatial);
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

void _nya_audio_place_track(u32 slot, MIX_Track* track, f32x3 position) {
    NYA_AudioSystem* system = &_nya_audio_system;
    nya_assert(slot < _NYA_AUDIO_SLOTS);

    // SDL_mixer's positioning unless our panner is on and the device is a stereo pair: the panner is a
    // two-ear model and has nothing to say to a mono or surround layout.
    if (!system->panner_enabled || system->device_channels != 2) {
        _nya_audio_panner_disarm(&system->slots[slot], slot);
        _nya_audio_track_set_position(track, position);
        return;
    }

    // take SDL_mixer's own spatialisation off this track, so the cooked hook is the only thing panning it.
    MIX_SetTrackStereo(track, nullptr);
    MIX_SetTrack3DPosition(track, nullptr);

    atomic_store_explicit(&system->slots[slot].pan_azimuth, nya_audio_pan_azimuth(position), memory_order_relaxed);
    atomic_store_explicit(&system->slots[slot].pan_active, true, memory_order_relaxed);

    // the panner owns falloff now, since MIX_SetTrack3DPosition is off. Inverse distance past the reference,
    // where the world-to-audio scaling puts 1.0; nearer than that is full.
    f32 distance                   = nya_vector_length(position);
    system->slots[slot].spatial_gain = distance > 1.0F ? 1.0F / distance : 1.0F;

    _nya_audio_apply_gain(slot);
}

void _nya_audio_panner_disarm(NYA_AudioVoice* slot, u32 index) {
    atomic_store_explicit(&slot->pan_active, false, memory_order_relaxed);

    // fold the distance gain back out, so a voice handed to SDL_mixer's positioning is not doubly quiet.
    if (slot->spatial_gain != 1.0F) {
        slot->spatial_gain = 1.0F;
        _nya_audio_apply_gain(index);
    }
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

void _nya_audio_lines_release(NYA_AudioSystem* system) {
    for (u32 bus = 0; bus < NYA_AUDIO_BUS_COUNT; bus++) atomic_store(&system->chains[bus], nullptr);

    if (system->line_allocator != nullptr) nya_arena_destroy(system->line_allocator);
    system->line_allocator = nullptr;
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

    /* The cooked hook, not the raw one: this is the decoded, unspatialised signal for one voice. */
    NYA_AudioVoice* voice = (NYA_AudioVoice*)userdata;

    // the panner first, placing the source in the stereo field, then the voice's own low pass over it. When
    // the panner is off the buffer is whatever SDL_mixer's positioning left, and only the filter runs.
    if (atomic_load_explicit(&voice->pan_active, memory_order_relaxed)) {
        NYA_StereoPan pan = nya_audio_pan_compute((NYA_StereoPanParams){
            .azimuth_radians = atomic_load_explicit(&voice->pan_azimuth, memory_order_relaxed),
        });

        nya_audio_pan_render(&voice->pan_render, (f32)spec->freq, spec->channels, pan, pcm, samples);
    }

    _nya_audio_filter_apply(&voice->filter, spec, pcm, samples);
}

void SDLCALL _nya_audio_group_mix_callback(void* userdata, MIX_Group* group, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(group);

    // the slot, not the chain: the chain is published after this callback is installed.
    NYA_AudioChain* chain = atomic_load((atomic(NYA_AudioChain*)*)userdata);

    if (chain != nullptr) _nya_audio_chain_process(chain, spec, pcm, samples);
}

void SDLCALL _nya_audio_post_mix_callback(void* userdata, MIX_Mixer* mixer, const SDL_AudioSpec* spec, float* pcm, int samples) {
    nya_unused(mixer);

    // master, after every group is mixed.
    NYA_AudioChain* chain = atomic_load((atomic(NYA_AudioChain*)*)userdata);

    if (chain != nullptr) _nya_audio_chain_process(chain, spec, pcm, samples);
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
