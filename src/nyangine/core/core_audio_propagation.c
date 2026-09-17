#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where a cutoff eases from: at or above this the low pass is open. */
#define _NYA_AUDIO_PROPAGATION_OPEN_HZ 20000.0F

/** The lowest cutoff a thick blocker reaches. Below it everything is rumble. */
#define _NYA_AUDIO_PROPAGATION_FLOOR_HZ 80.0F

/* How the room estimate becomes a reverb. A small closed room rings short and bright, a large one long and dark. */
#define _NYA_AUDIO_PROPAGATION_ROOM_SMALL   0.35F
#define _NYA_AUDIO_PROPAGATION_ROOM_LARGE   0.88F
#define _NYA_AUDIO_PROPAGATION_DAMPING_SMALL 0.2F
#define _NYA_AUDIO_PROPAGATION_DAMPING_LARGE 0.7F
#define _NYA_AUDIO_PROPAGATION_WET          0.45F

/** Below this room size the reverb is switched off rather than run inaudibly. */
#define _NYA_AUDIO_PROPAGATION_ROOM_SILENT 0.02F

/** How bright an echo off a surface beside the ear is, and off one at the edge of range. Air and surfaces eat treble. */
#define _NYA_AUDIO_PROPAGATION_ECHO_NEAR_HZ 9000.0F
#define _NYA_AUDIO_PROPAGATION_ECHO_FAR_HZ  1800.0F

/** The frequency diffraction loss is judged at, hertz. The middle of what a game's sounds carry. */
#define _NYA_AUDIO_PROPAGATION_DIFFRACTION_HZ 500.0F

/** A diffraction loss this many decibels deep is as dull as one thin surface. */
#define _NYA_AUDIO_PROPAGATION_DIFFRACTION_DULL_DB 15.0F

/** How much quieter an echo from the edge of range is than one from beside the ear. */
#define _NYA_AUDIO_PROPAGATION_ECHO_FAR_GAIN 0.3F

/** Listener directions in 3D: the six axes, then the eight cube corners, all unit length. */
NYA_INTERNAL const f32x3 _NYA_AUDIO_PROPAGATION_DIRECTIONS_3D[NYA_AUDIO_PROPAGATION_ENVIRONMENT_RAYS] = {
    { 1.0F, 0.0F, 0.0F },          { -1.0F, 0.0F, 0.0F },          { 0.0F, 1.0F, 0.0F },          { 0.0F, -1.0F, 0.0F },
    { 0.0F, 0.0F, 1.0F },          { 0.0F, 0.0F, -1.0F },          { 0.577F, 0.577F, 0.577F },    { -0.577F, 0.577F, 0.577F },
    { 0.577F, -0.577F, 0.577F },   { -0.577F, -0.577F, 0.577F },   { 0.577F, 0.577F, -0.577F },   { -0.577F, 0.577F, -0.577F },
    { 0.577F, -0.577F, -0.577F },  { -0.577F, -0.577F, -0.577F },
};

/** In 2D: the four axes and four diagonals of the plane. */
#define _NYA_AUDIO_PROPAGATION_DIRECTIONS_2D_COUNT 8

NYA_INTERNAL const f32x3 _NYA_AUDIO_PROPAGATION_DIRECTIONS_2D[_NYA_AUDIO_PROPAGATION_DIRECTIONS_2D_COUNT] = {
    { 1.0F, 0.0F, 0.0F },       { -1.0F, 0.0F, 0.0F },       { 0.0F, 1.0F, 0.0F },        { 0.0F, -1.0F, 0.0F },
    { 0.7071F, 0.7071F, 0.0F }, { -0.7071F, 0.7071F, 0.0F }, { 0.7071F, -0.7071F, 0.0F }, { -0.7071F, -0.7071F, 0.0F },
};

/** One voice as the tracer sees it. */
typedef struct {
    f32x3 position;

    /** World units. Zero takes NYA_AudioPropagation.radius. */
    f32 radius;

    b8 active;
} NYA_AudioEmitter;

/** Where one voice's rays sit in the batch, and the geometry needed to read them back. */
typedef struct {
    u32 voice;
    u32 first;

    /** The path's end, pulled back from the source by its radius so a sound on a surface is not hidden by it. */
    f32x3 end;
    f32   length;

    /** Zero when no probes were cast this update. */
    u32   probe_count;
    f32x3 probes[NYA_AUDIO_PROPAGATION_PROBES];
} NYA_AudioTrace;

/** Zero counts defaulted, counts clamped, the budget raised to fit at least one voice and the room slice. */
NYA_INTERNAL NYA_AudioPropagation _nya_audio_propagation_validate(NYA_AudioPropagation propagation) __attr_no_discard;

/** Every path back to untraced and open, and the room back to open air. */
NYA_INTERNAL void _nya_audio_tracer_reset(NYA_AudioTracer* tracer);

/** One path back to untraced and open. */
NYA_INTERNAL void _nya_audio_path_reset(NYA_AudioPath* path);

/**
 * One update: casts the room slice and as many voices as the budget holds, reads the results into targets, and eases
 * every active path toward its target. `emitters` is NYA_AUDIO_VOICES long, `right` the ear's right, for panning
 * echoes. Pure: the scene is only reached through `trace`.
 * */
NYA_INTERNAL void _nya_audio_tracer_step(
    NYA_AudioTracer* tracer, const NYA_AudioPropagation* propagation, NYA_AudioRayFn trace, void* user_data, f32x3 ear,
    f32x3 right, const NYA_AudioEmitter* emitters, f32 delta_time_s
);

/** Rays tracing `path` costs this update, given what it found last time. */
NYA_INTERNAL u32 _nya_audio_path_cost(const NYA_AudioPath* path, const NYA_AudioPropagation* propagation) __attr_no_discard;

/** Appends one voice's rays to the batch and records where they went. */
NYA_INTERNAL void _nya_audio_trace_build(
    NYA_AudioTracer* tracer, NYA_AudioTrace* out_trace, const NYA_AudioPropagation* propagation, f32x3 ear, f32x3 source,
    f32 radius, u32* ray_count
);

/** Reads one voice's results into its path's targets. */
NYA_INTERNAL void _nya_audio_trace_read(
    NYA_AudioTracer* tracer, const NYA_AudioTrace* trace, const NYA_AudioPropagation* propagation, f32x3 ear, f32x3 source,
    f32 radius
);

/** Turns the nearest surfaces the room probes hit into the sound bus's echoes. */
NYA_INTERNAL void _nya_audio_environment_reflect(NYA_AudioTracer* tracer, const NYA_AudioPropagation* propagation, f32x3 right);

/** Recomputes the room estimate from every direction's last fraction and eases the reverb toward it. */
NYA_INTERNAL void _nya_audio_environment_estimate(NYA_AudioTracer* tracer, const NYA_AudioPropagation* propagation, f32 ease);

/** The room directions for a space. */
NYA_INTERNAL const f32x3* _nya_audio_propagation_directions(NYA_AudioSpace space, u32* out_count) __attr_no_discard;

/** A path's muffle as a cutoff for the voice filter. Zero, which the filter reads as open, when barely muffled. */
NYA_INTERNAL f32 _nya_audio_muffle_to_hz(f32 muffle, f32 lowpass_hz) __attr_no_discard;

/** Puts every voice back as placed and the sound bus back on its requested reverb. */
NYA_INTERNAL void _nya_audio_propagation_restore(NYA_AudioSystem* system);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_audio_update(f32 delta_time_s) {
    NYA_AudioSystem*            system      = &_nya_audio_system;
    const NYA_AudioPropagation* propagation = &system->propagation;

    if (!propagation->enabled || !system->ready) return;

    nya_perf_time_this_scope("audio_propagation");

    NYA_AudioRayFn trace = system->ray_functions[propagation->space];
    if (trace == nullptr) return;

    b8    planar = propagation->space == NYA_AUDIO_SPACE_2D;
    f32x3 ear    = planar ? (f32x3){ system->listener.position.x, system->listener.position.y, 0.0F } : system->listener_3d.position;
    f32x3 right  = planar ? (f32x3){ 1.0F, 0.0F, 0.0F } : nya_vector_normalize(nya_vector_cross(system->listener_3d.forward, system->listener_3d.up));

    NYA_AudioEmitter emitters[NYA_AUDIO_VOICES];

    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) {
        NYA_AudioVoice* slot = &system->slots[i];

        // a finished voice keeps its slot until reused; it stops being traced now.
        if (slot->positional && !MIX_TrackPlaying(slot->track)) slot->positional = false;

        emitters[i] = (NYA_AudioEmitter){
            .position = slot->world_position,
            .radius   = slot->radius,
            .active   = slot->positional && slot->planar == planar,
        };
    }

    _nya_audio_tracer_step(&system->tracer, propagation, trace, system->ray_user_data[propagation->space], ear, right, emitters, delta_time_s);

    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) {
        if (!emitters[i].active) continue;

        NYA_AudioVoice*      slot = &system->slots[i];
        const NYA_AudioPath* path = &system->tracer.paths[i];

        if (fabsf(path->gain - slot->propagation_gain) > 0.001F) {
            slot->propagation_gain = path->gain;
            _nya_audio_apply_gain(i);
        }

        // the path is already eased, so the filter only has to bridge one frame.
        atomic_store_explicit(&slot->filter.target_hz, _nya_audio_muffle_to_hz(path->muffle, propagation->lowpass_hz), memory_order_relaxed);
        atomic_store_explicit(&slot->filter.glide_ms, 50.0F, memory_order_relaxed);

        _nya_audio_voice_place(i);
    }

    // the room's tail and echoes, over whatever the sound bus was set to. unchanged settings publish nothing.
    if (propagation->environment) _nya_audio_bus_publish(NYA_AUDIO_BUS_SOUND);
}

void nya_audio_propagation_set(NYA_AudioPropagation propagation) {
    NYA_AudioSystem* system = &_nya_audio_system;

    NYA_AudioPropagation previous  = system->propagation;
    NYA_AudioPropagation validated = _nya_audio_propagation_validate(propagation);

    system->propagation = validated;

    // called every frame with the same settings, so only a change of shape does any work.
    b8 reshaped = previous.enabled != validated.enabled || previous.space != validated.space || previous.environment != validated.environment
               || (previous.reflections > 0.0F) != (validated.reflections > 0.0F);

    if (reshaped) _nya_audio_propagation_restore(system);
}

NYA_AudioPropagation nya_audio_propagation_get(void) {
    return _nya_audio_system.propagation;
}

void nya_audio_rays_set(NYA_AudioSpace space, NYA_AudioRayFn function, void* user_data) {
    nya_assert(space < NYA_AUDIO_SPACE_COUNT, "unknown audio space %d", (s32)space);

    _nya_audio_system.ray_functions[space] = function;
    _nya_audio_system.ray_user_data[space] = user_data;
}

NYA_AudioEnvironment nya_audio_environment_get(void) {
    const NYA_AudioSystem* system = &_nya_audio_system;

    if (!system->propagation.enabled || !system->propagation.environment) return (NYA_AudioEnvironment){ 0 };

    return system->tracer.environment;
}

NYA_AudioPath nya_audio_voice_path_get(NYA_SoundVoice voice) {
    NYA_AudioVoice* slot = _nya_audio_resolve(voice);

    if (slot == nullptr || voice.index >= NYA_AUDIO_VOICES || !slot->positional || !_nya_audio_system.propagation.enabled) return (NYA_AudioPath){ 0 };

    return _nya_audio_system.tracer.paths[voice.index];
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_AudioPropagation _nya_audio_propagation_validate(NYA_AudioPropagation propagation) {
    if (propagation.space >= NYA_AUDIO_SPACE_COUNT) propagation.space = NYA_AUDIO_SPACE_3D;

    if (propagation.voice_rays == 0) propagation.voice_rays = NYA_AUDIO_PROPAGATION_VOICE_RAYS;
    propagation.voice_rays = nya_min(propagation.voice_rays, (u32)NYA_AUDIO_PROPAGATION_VOICE_RAYS_MAX);

    if (propagation.radius <= 0.0F) propagation.radius = NYA_AUDIO_PROPAGATION_RADIUS;
    if (propagation.lowpass_hz <= 0.0F) propagation.lowpass_hz = NYA_AUDIO_PROPAGATION_LOWPASS_HZ;
    if (propagation.transmission <= 0.0F) propagation.transmission = NYA_AUDIO_PROPAGATION_TRANSMISSION;
    if (propagation.thickness <= 0.0F) propagation.thickness = NYA_AUDIO_PROPAGATION_THICKNESS;
    if (propagation.smoothing_ms <= 0.0F) propagation.smoothing_ms = NYA_AUDIO_PROPAGATION_SMOOTHING_MS;
    if (propagation.diffraction_reach <= 0.0F) propagation.diffraction_reach = NYA_AUDIO_PROPAGATION_DIFFRACTION_REACH;
    if (propagation.environment_range <= 0.0F) propagation.environment_range = NYA_AUDIO_PROPAGATION_ENVIRONMENT_RANGE;
    if (propagation.speed_of_sound <= 0.0F) propagation.speed_of_sound = NYA_AUDIO_PROPAGATION_SPEED_OF_SOUND;

    propagation.lowpass_hz   = nya_clamp(propagation.lowpass_hz, _NYA_AUDIO_PROPAGATION_FLOOR_HZ, _NYA_AUDIO_PROPAGATION_OPEN_HZ);
    propagation.transmission = nya_min(propagation.transmission, 1.0F);
    propagation.reflections  = nya_clamp(propagation.reflections, 0.0F, 1.0F);

    // reflections are placed off the room probes' hits, so they cannot run without them.
    if (!propagation.environment) propagation.reflections = 0.0F;

    if (propagation.ray_budget == 0) propagation.ray_budget = NYA_AUDIO_PROPAGATION_RAY_BUDGET;

    // at least the room slice and one voice at its dearest, or a hidden voice could never be traced again.
    u32 probes   = propagation.space == NYA_AUDIO_SPACE_2D ? NYA_AUDIO_PROPAGATION_PROBES / 2 : NYA_AUDIO_PROPAGATION_PROBES;
    u32 smallest = (propagation.environment ? NYA_AUDIO_PROPAGATION_ENVIRONMENT_SLICE : 0) + propagation.voice_rays + 1 + (propagation.diffraction ? 2 * probes : 0);

    propagation.ray_budget = nya_clamp(propagation.ray_budget, smallest, (u32)NYA_AUDIO_PROPAGATION_RAYS_MAX);

    return propagation;
}

void _nya_audio_path_reset(NYA_AudioPath* path) {
    // open: an untraced voice plays as placed until its first result.
    *path = (NYA_AudioPath){ .gain = 1.0F, .target_gain = 1.0F, .blocker = 0.5F };
}

void _nya_audio_tracer_reset(NYA_AudioTracer* tracer) {
    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) _nya_audio_path_reset(&tracer->paths[i]);

    // open air until the first sweep says otherwise, so the reverb fades in rather than starting at full.
    for (u32 i = 0; i < NYA_AUDIO_PROPAGATION_ENVIRONMENT_RAYS; i++) tracer->environment_fractions[i] = 1.0F;

    tracer->environment        = (NYA_AudioEnvironment){ 0 };
    tracer->reflections        = (NYA_AudioReflections){ 0 };
    tracer->environment_cursor = 0;
    tracer->voice_cursor       = 0;
    tracer->rays_cast          = 0;
}

void _nya_audio_tracer_step(
    NYA_AudioTracer* tracer, const NYA_AudioPropagation* propagation, NYA_AudioRayFn trace, void* user_data, f32x3 ear,
    f32x3 right, const NYA_AudioEmitter* emitters, f32 delta_time_s
) {
    nya_assert(tracer != nullptr && propagation != nullptr && trace != nullptr && emitters != nullptr);
    nya_assert(propagation->ray_budget <= NYA_AUDIO_PROPAGATION_RAYS_MAX, "unvalidated propagation settings");

    u32 ray_count = 0;
    u32 budget    = propagation->ray_budget;

    // ── the room: a slice of the fixed directions, resumed where the last update stopped ──
    u32          direction_count = 0;
    const f32x3* directions      = _nya_audio_propagation_directions(propagation->space, &direction_count);

    u32 environment_first = 0;
    u32 environment_count = 0;

    if (propagation->environment) {
        environment_count = nya_min((u32)NYA_AUDIO_PROPAGATION_ENVIRONMENT_SLICE, direction_count);

        for (u32 i = 0; i < environment_count; i++) {
            u32 direction = (tracer->environment_cursor + i) % direction_count;

            tracer->rays[ray_count++] = (NYA_AudioRay){ .origin = ear, .direction = directions[direction] * propagation->environment_range };
        }
    }

    // ── voices: new ones first, since a one shot is over before the round robin comes back, then in turn ──
    NYA_AudioTrace traces[NYA_AUDIO_VOICES];
    u32            trace_count = 0;
    b8             queued[NYA_AUDIO_VOICES] = { 0 };

    for (u32 pass = 0; pass < 2; pass++) {
        b8 full = false;

        for (u32 step = 0; step < NYA_AUDIO_VOICES && !full; step++) {
            u32 voice = pass == 0 ? step : (tracer->voice_cursor + step) % NYA_AUDIO_VOICES;

            if (!emitters[voice].active || queued[voice]) continue;
            if (pass == 0 && tracer->paths[voice].traced) continue;

            u32 cost = _nya_audio_path_cost(&tracer->paths[voice], propagation);

            if (ray_count + cost > budget) {
                // the round robin resumes here next update, so no voice is starved.
                if (pass == 1) tracer->voice_cursor = voice;
                full = true;
                continue;
            }

            f32 radius = emitters[voice].radius > 0.0F ? emitters[voice].radius : propagation->radius;

            traces[trace_count].voice = voice;
            _nya_audio_trace_build(tracer, &traces[trace_count], propagation, ear, emitters[voice].position, radius, &ray_count);

            trace_count++;
            queued[voice] = true;

            if (pass == 1) tracer->voice_cursor = (voice + 1) % NYA_AUDIO_VOICES;
        }

        if (full) break;
    }

    nya_assert(ray_count <= budget, "cast %u rays against a budget of %u", ray_count, budget);

    // one call for the whole batch, on this thread: a job starts a thread, which costs more than the rays.
    if (ray_count > 0) trace(tracer->rays, tracer->fractions, ray_count, user_data);

    tracer->rays_cast = ray_count;

    if (propagation->environment) {
        for (u32 i = 0; i < environment_count; i++) {
            u32 direction = (tracer->environment_cursor + i) % direction_count;

            tracer->environment_fractions[direction] = nya_clamp(tracer->fractions[environment_first + i], 0.0F, 1.0F);
        }

        tracer->environment_cursor = (tracer->environment_cursor + environment_count) % direction_count;
    }

    for (u32 i = 0; i < trace_count; i++) {
        const NYA_AudioTrace*   voice_trace = &traces[i];
        const NYA_AudioEmitter* emitter     = &emitters[voice_trace->voice];

        f32 radius = emitter->radius > 0.0F ? emitter->radius : propagation->radius;

        _nya_audio_trace_read(tracer, voice_trace, propagation, ear, emitter->position, radius);
    }

    // ── easing, by time rather than per update, so a voice the budget skipped still settles at the same rate ──
    f32 ease             = 1.0F - expf(-(delta_time_s * 1000.0F) / propagation->smoothing_ms);
    f32 environment_ease = 1.0F - expf(-(delta_time_s * 1000.0F) / NYA_AUDIO_PROPAGATION_ENVIRONMENT_GLIDE_MS);

    for (u32 voice = 0; voice < NYA_AUDIO_VOICES; voice++) {
        NYA_AudioPath* path = &tracer->paths[voice];

        // a voice that stopped starts its next sound untraced.
        if (!emitters[voice].active) {
            if (path->traced) _nya_audio_path_reset(path);
            continue;
        }

        if (!path->traced) continue;

        path->gain   += (path->target_gain - path->gain) * ease;
        path->muffle += (path->target_muffle - path->muffle) * ease;
        path->offset += (path->target_offset - path->offset) * ease;
    }

    if (propagation->environment) _nya_audio_environment_estimate(tracer, propagation, environment_ease);
    if (propagation->reflections > 0.0F) _nya_audio_environment_reflect(tracer, propagation, right);

    nya_assert(tracer->voice_cursor < NYA_AUDIO_VOICES);
}

u32 _nya_audio_path_cost(const NYA_AudioPath* path, const NYA_AudioPropagation* propagation) {
    u32 cost = propagation->voice_rays + 1;

    // probes only while something was in the way, or before anything is known.
    b8 probing = propagation->diffraction && (!path->traced || path->occlusion > 0.0F);

    if (probing) cost += 2 * (propagation->space == NYA_AUDIO_SPACE_2D ? NYA_AUDIO_PROPAGATION_PROBES / 2 : NYA_AUDIO_PROPAGATION_PROBES);

    return cost;
}

void _nya_audio_trace_build(
    NYA_AudioTracer* tracer, NYA_AudioTrace* out_trace, const NYA_AudioPropagation* propagation, f32x3 ear, f32x3 source,
    f32 radius, u32* ray_count
) {
    const NYA_AudioPath* path = &tracer->paths[out_trace->voice];

    b8 planar = propagation->space == NYA_AUDIO_SPACE_2D;
    u32 first = *ray_count;
    u32 n     = propagation->voice_rays;

    f32x3 to_source = source - ear;
    f32   distance  = nya_vector_length(to_source);

    // a source inside its own radius of the ear has no path to block. its rays are cast anyway, degenerate, so the
    // cost the scheduler charged is what was spent.
    f32x3 direction = distance > NYA_EPSILON ? to_source / distance : (f32x3){ 0.0F, 0.0F, -1.0F };

    f32x3 right;
    f32x3 up;

    if (planar) {
        right = (f32x3){ -direction.y, direction.x, 0.0F };
        up    = (f32x3){ 0.0F, 0.0F, 0.0F };
    } else {
        f32x3 reference = fabsf(direction.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

        right = nya_vector_normalize(nya_vector_cross(direction, reference));
        up    = nya_vector_cross(right, direction);
    }

    f32   length = nya_max(distance - radius, 0.0F);
    f32x3 end    = ear + (direction * length);

    out_trace->first  = first;
    out_trace->end    = end;
    out_trace->length = length;

    // the centre first: its fraction and the reverse ray's give the blocker's thickness.
    tracer->rays[first] = (NYA_AudioRay){ .origin = ear, .direction = end - ear };

    for (u32 k = 1; k < n; k++) {
        f32x3 spread;

        if (planar) {
            // alternating sides, widening, so two rays cover both edges.
            u32 ring  = (k + 1) / 2;
            u32 rings = n / 2;
            f32 side  = (k & 1) != 0 ? 1.0F : -1.0F;
            f32 reach = (f32)ring / (f32)rings;
            spread    = right * (side * reach * radius);
        } else {
            f32 angle = 2.0F * (f32)M_PI * (f32)(k - 1) / (f32)(n - 1);
            spread    = ((right * cosf(angle)) + (up * sinf(angle))) * radius;
        }

        // aimed at the rim of the extent, stopping a radius short for the same reason as the centre.
        f32x3 target  = source + spread;
        f32x3 toward  = target - ear;
        f32   reach   = nya_vector_length(toward);
        f32   trimmed = reach > NYA_EPSILON ? nya_max(reach - radius, 0.0F) / reach : 0.0F;

        tracer->rays[first + k] = (NYA_AudioRay){ .origin = ear, .direction = toward * trimmed };
    }

    tracer->rays[first + n] = (NYA_AudioRay){ .origin = end, .direction = ear - end };

    *ray_count = first + n + 1;

    out_trace->probe_count = 0;

    if (!propagation->diffraction || (path->traced && path->occlusion <= 0.0F)) return;

    // beside where the blocker started last time, so the probes straddle its edge rather than the midpoint.
    f32x3 blocker = ear + (direction * (length * path->blocker));

    const f32x3 axes[NYA_AUDIO_PROPAGATION_PROBES] = { right, -right, up, -up };

    u32 probe_count = planar ? NYA_AUDIO_PROPAGATION_PROBES / 2 : NYA_AUDIO_PROPAGATION_PROBES;

    for (u32 p = 0; p < probe_count; p++) {
        f32x3 corner = blocker + (axes[p] * propagation->diffraction_reach);

        out_trace->probes[p] = corner;

        tracer->rays[*ray_count]     = (NYA_AudioRay){ .origin = ear, .direction = corner - ear };
        tracer->rays[*ray_count + 1] = (NYA_AudioRay){ .origin = corner, .direction = end - corner };

        *ray_count += 2;
    }

    out_trace->probe_count = probe_count;
}

void _nya_audio_trace_read(
    NYA_AudioTracer* tracer, const NYA_AudioTrace* trace, const NYA_AudioPropagation* propagation, f32x3 ear, f32x3 source,
    f32 radius
) {
    NYA_AudioPath* path = &tracer->paths[trace->voice];

    const f32* fractions = &tracer->fractions[trace->first];
    u32        n         = propagation->voice_rays;

    u32 blocked = 0;
    for (u32 k = 0; k < n; k++) {
        if (fractions[k] < 1.0F) blocked++;
    }

    f32 occlusion = (f32)blocked / (f32)n;

    // the solid between the first surface each end sees. two walls read as one thick one: no materials, no layers.
    b8  centre_blocked = fractions[0] < 1.0F;
    f32 thickness      = 0.0F;

    if (centre_blocked) {
        f32 back  = nya_clamp(fractions[n], 0.0F, 1.0F);
        thickness = nya_max(trace->length * (1.0F - fractions[0] - back), 0.0F);
    }

    f32   blocked_gain   = propagation->transmission * expf(-thickness / propagation->thickness);
    f32   blocked_muffle = 1.0F + (thickness / propagation->thickness);
    f32x3 heard          = source;

    // a way around: both legs of a probe clear. the shortest detour wins, heard from the corner at the detour's length.
    for (u32 p = 0; p < trace->probe_count; p++) {
        f32 to_corner   = fractions[n + 1 + (2 * p)];
        f32 from_corner = fractions[n + 2 + (2 * p)];

        if (to_corner < 1.0F || from_corner < 1.0F) continue;

        f32x3 first_leg  = trace->probes[p] - ear;
        f32x3 second_leg = trace->end - trace->probes[p];

        f32 first_length  = nya_vector_length(first_leg);
        f32 second_length = nya_vector_length(second_leg);

        if (first_length < NYA_EPSILON || second_length < NYA_EPSILON) continue;

        // Maekawa's screen: the detour in half wavelengths sets the loss, about 5 dB grazing the edge and 13 dB a
        // wavelength around it. Treble bends worse, so the loss dulls it too.
        f32 detour  = nya_max(first_length + second_length - trace->length, 0.0F);
        f32 fresnel = 2.0F * detour * _NYA_AUDIO_PROPAGATION_DIFFRACTION_HZ / propagation->speed_of_sound;
        f32 loss_db = 10.0F * log10f(3.0F + (20.0F * fresnel));
        f32 gain    = powf(10.0F, -loss_db / 20.0F);

        if (gain <= blocked_gain) continue;

        blocked_gain   = gain;
        blocked_muffle = loss_db / _NYA_AUDIO_PROPAGATION_DIFFRACTION_DULL_DB;
        heard          = ear + ((first_leg / first_length) * (first_length + second_length + radius));
    }

    f32 gain = (1.0F - occlusion) + (occlusion * blocked_gain);

    // what reaches the ear hidden, as a share of all that does. it decides how dull and how displaced the voice is.
    f32 hidden_share = gain > NYA_EPSILON ? (occlusion * blocked_gain) / gain : 1.0F;

    path->occlusion     = occlusion;
    path->target_gain   = gain;
    path->target_muffle = hidden_share * blocked_muffle;
    path->target_offset = (heard - source) * hidden_share;

    if (centre_blocked) path->blocker = nya_clamp(fractions[0], 0.0F, 1.0F);

    // a voice's first result is taken as is: it has barely been heard, and easing in from open would be audible.
    if (!path->traced) {
        path->gain   = path->target_gain;
        path->muffle = path->target_muffle;
        path->offset = path->target_offset;
        path->traced = true;
    }

    nya_assert(path->target_gain >= 0.0F && path->target_gain <= 1.0F + NYA_EPSILON, "gain %f out of range", (f64)path->target_gain);
}

void _nya_audio_environment_reflect(NYA_AudioTracer* tracer, const NYA_AudioPropagation* propagation, f32x3 right) {
    u32          direction_count = 0;
    const f32x3* directions      = _nya_audio_propagation_directions(propagation->space, &direction_count);

    // the nearest hits, in order: they return first and loudest. insertion into a handful is cheaper than a sort.
    u32 nearest[NYA_AUDIO_REFLECTION_TAPS];
    u32 count = 0;

    for (u32 i = 0; i < direction_count; i++) {
        f32 fraction = tracer->environment_fractions[i];
        if (fraction >= 1.0F) continue;

        u32 slot = count;
        while (slot > 0 && tracer->environment_fractions[nearest[slot - 1]] > fraction) slot--;

        if (slot >= NYA_AUDIO_REFLECTION_TAPS) continue;

        for (u32 later = nya_min(count, (u32)NYA_AUDIO_REFLECTION_TAPS - 1); later > slot; later--) nearest[later] = nearest[later - 1];

        nearest[slot] = i;
        count         = nya_min(count + 1, (u32)NYA_AUDIO_REFLECTION_TAPS);
    }

    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) {
        NYA_AudioReflectionTap* echo = &tracer->reflections.taps[tap];

        if (tap >= count) {
            echo->gain = 0.0F;
            continue;
        }

        u32 direction = nearest[tap];
        f32 fraction  = tracer->environment_fractions[direction];

        // there and back.
        echo->delay_s    = 2.0F * fraction * propagation->environment_range / propagation->speed_of_sound;
        echo->gain       = propagation->reflections * nya_lerp(1.0F, _NYA_AUDIO_PROPAGATION_ECHO_FAR_GAIN, fraction);
        echo->pan        = nya_clamp(nya_vector_dot(directions[direction], right), -1.0F, 1.0F);
        echo->lowpass_hz = nya_lerp(_NYA_AUDIO_PROPAGATION_ECHO_NEAR_HZ, _NYA_AUDIO_PROPAGATION_ECHO_FAR_HZ, fraction);
    }
}

void _nya_audio_environment_estimate(NYA_AudioTracer* tracer, const NYA_AudioPropagation* propagation, f32 ease) {
    u32 direction_count = 0;
    (void)_nya_audio_propagation_directions(propagation->space, &direction_count);

    u32 hits = 0;
    f32 sum  = 0.0F;

    for (u32 i = 0; i < direction_count; i++) {
        if (tracer->environment_fractions[i] >= 1.0F) continue;

        hits++;
        sum += tracer->environment_fractions[i] * propagation->environment_range;
    }

    f32 enclosure = (f32)hits / (f32)direction_count;
    f32 distance  = hits > 0 ? sum / (f32)hits : propagation->environment_range;
    f32 size      = nya_clamp(distance / propagation->environment_range, 0.0F, 1.0F);

    // squared, so a floor alone, about half the probes, is a hint of room rather than half of one.
    NYA_AudioReverb target = {
        .room_size = enclosure * nya_lerp(_NYA_AUDIO_PROPAGATION_ROOM_SMALL, _NYA_AUDIO_PROPAGATION_ROOM_LARGE, size),
        .damping   = nya_lerp(_NYA_AUDIO_PROPAGATION_DAMPING_SMALL, _NYA_AUDIO_PROPAGATION_DAMPING_LARGE, size),
        .wet       = _NYA_AUDIO_PROPAGATION_WET * enclosure * enclosure,
        .dry       = 1.0F,
        .width     = 1.0F,
    };

    NYA_AudioEnvironment* environment = &tracer->environment;

    environment->enclosure += (enclosure - environment->enclosure) * ease;
    environment->distance  += (distance - environment->distance) * ease;

    NYA_AudioReverb* reverb = &environment->reverb;

    reverb->room_size += (target.room_size - reverb->room_size) * ease;
    reverb->damping   += (target.damping - reverb->damping) * ease;
    reverb->wet       += (target.wet - reverb->wet) * ease;
    reverb->dry        = target.dry;
    reverb->width      = target.width;

    // settled to nothing: switched off, so open air runs no reverb at all.
    if (target.room_size < _NYA_AUDIO_PROPAGATION_ROOM_SILENT && reverb->room_size < _NYA_AUDIO_PROPAGATION_ROOM_SILENT) reverb->room_size = 0.0F;
}

const f32x3* _nya_audio_propagation_directions(NYA_AudioSpace space, u32* out_count) {
    if (space == NYA_AUDIO_SPACE_2D) {
        *out_count = _NYA_AUDIO_PROPAGATION_DIRECTIONS_2D_COUNT;
        return _NYA_AUDIO_PROPAGATION_DIRECTIONS_2D;
    }

    *out_count = NYA_AUDIO_PROPAGATION_ENVIRONMENT_RAYS;
    return _NYA_AUDIO_PROPAGATION_DIRECTIONS_3D;
}

f32 _nya_audio_muffle_to_hz(f32 muffle, f32 lowpass_hz) {
    if (muffle < 0.01F) return 0.0F;

    // geometric, so each step of muffle takes off the same share of the band.
    f32 hz = _NYA_AUDIO_PROPAGATION_OPEN_HZ * powf(lowpass_hz / _NYA_AUDIO_PROPAGATION_OPEN_HZ, muffle);

    return nya_max(hz, _NYA_AUDIO_PROPAGATION_FLOOR_HZ);
}

void _nya_audio_propagation_restore(NYA_AudioSystem* system) {
    _nya_audio_tracer_reset(&system->tracer);

    // the reset tracer has no echoes, and the bus takes back its own reverb unless the environment still drives it.
    _nya_audio_bus_publish(NYA_AUDIO_BUS_SOUND);

    if (!system->ready) return;

    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) {
        NYA_AudioVoice* slot = &system->slots[i];

        atomic_store_explicit(&slot->filter.target_hz, 0.0F, memory_order_relaxed);

        if (slot->propagation_gain != 1.0F) {
            slot->propagation_gain = 1.0F;
            _nya_audio_apply_gain(i);
        }

        if (slot->positional) _nya_audio_voice_place(i);
    }
}
