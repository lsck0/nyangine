#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * REVERB
 * ─────────────────────────────────────────────────────────
 *
 * A Schroeder network: four parallel comb filters summed, then two allpasses in series, per channel,
 * with the second channel's delays offset so the rooms differ. Delays are the Freeverb set, in
 * samples at 44.1 kHz, and mutually prime, since shared factors stack echoes into a metallic ring.
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
 * about 96 kHz; faster rates are clamped, a slightly smaller room.
 * */
#define _NYA_AUDIO_REVERB_MAX_DELAY 3072

/**
 * Networks run regardless of speaker count. A reverb tail is a stereo impression, and uncorrelated tails per
 * speaker smear rather than localise. The bus is downmixed to left and right, and the result is added back
 * across channels by parity. The echo does the same.
 * */
#define _NYA_AUDIO_STEREO 2

/** Clamped below one: a comb feedback of one never decays, and above one grows until the buffer is infinities. */
#define _NYA_AUDIO_REVERB_ROOM_MAX 0.97F

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

/** A bus's reverb lines, 144 KiB. */
typedef struct {
    NYA_AudioReverbComb    combs[_NYA_AUDIO_STEREO][_NYA_AUDIO_REVERB_COMBS];
    NYA_AudioReverbAllpass allpasses[_NYA_AUDIO_STEREO][_NYA_AUDIO_REVERB_ALLPASSES];
} NYA_AudioReverbLines;

/** A bus's echo, two channels, 250 KiB. */
typedef struct {
    f32 buffer[_NYA_AUDIO_STEREO][NYA_AUDIO_ECHO_FRAMES];
    u32 cursor;
} NYA_AudioEchoLine;

/** The downmixed recent signal the listener's reflections read back, 64 KiB. */
typedef struct {
    f32 buffer[NYA_AUDIO_REFLECTION_FRAMES];
    u32 cursor;
} NYA_AudioReflectionLine;

/** Everything a chain is told. Written by the game, copied by the mixer. */
typedef struct {
    NYA_AudioEffects     effects;
    NYA_AudioReflections reflections;
} NYA_AudioChainSettings;

/** A second order section, transposed direct form II, one state pair per channel. */
typedef struct {
    f32 b0, b1, b2, a1, a2;

    f32 z1[NYA_AUDIO_EFFECTS_MAX_CHANNELS];
    f32 z2[NYA_AUDIO_EFFECTS_MAX_CHANNELS];
} NYA_AudioBiquad;

typedef enum {
    NYA_AUDIO_BIQUAD_LOWPASS,
    NYA_AUDIO_BIQUAD_HIGHPASS,
    NYA_AUDIO_BIQUAD_LOW_SHELF,
    NYA_AUDIO_BIQUAD_PEAK,
    NYA_AUDIO_BIQUAD_HIGH_SHELF,
} NYA_AudioBiquadShape;

/** The equaliser's bands, in the order NYA_AudioEqualizer lists them. */
#define _NYA_AUDIO_EQUALIZER_BANDS 3

/**
 * One bus's chain. Settings cross threads through a sequence lock: the game bumps `version` to odd, writes, bumps it
 * to even, and the mixer copies only an even version that did not move while it read. Neither side ever waits. The
 * lines are published once, before the setting that needs them, and freed with the arena.
 * */
struct NYA_AudioChain {
    /* Written by the game. */

    atomic u32             version;
    NYA_AudioChainSettings published;

    atomic(NYA_AudioReverbLines*)    reverb_lines;
    atomic(NYA_AudioEchoLine*)       echo_line;
    atomic(NYA_AudioReflectionLine*) reflection_line;

    /* The mixer's thread alone. */

    u32                    seen_version;
    NYA_AudioChainSettings target;

    /** The rate everything below was set up for. A change resets it. */
    s32 rate;

    /* Eased toward `target`, one step per block. */

    /**
     * Cutoff, how much of the filtered signal is heard, and the cutoff and resonance the coefficients were computed
     * for. A pass fades in and out rather than switching.
     * */
    f32 lowpass_state[4];
    f32 highpass_state[4];
    f32 resonance;
    f32 band_db[_NYA_AUDIO_EQUALIZER_BANDS];

    NYA_AudioBiquad lowpass;
    NYA_AudioBiquad highpass;
    NYA_AudioBiquad bands[_NYA_AUDIO_EQUALIZER_BANDS];

    /** What the band coefficients were last computed for, so a settled band is not recomputed. */
    f32 band_computed_db[_NYA_AUDIO_EQUALIZER_BANDS];

    /** Linear peak envelope across channels, and the gain applied, makeup included. */
    f32 compressor_envelope;
    f32 compressor_gain;

    f32 echo_delay;
    f32 echo_wet;
    f32 echo_feedback;
    f32 echo_damping[_NYA_AUDIO_STEREO];
    b8  echo_running;

    f32 reverb_room;
    f32 reverb_damping;
    f32 reverb_wet;
    f32 reverb_dry;
    f32 reverb_width;
    u32 reverb_rate;

    f32 limiter_gain;

    f32  tap_delay[NYA_AUDIO_REFLECTION_TAPS];
    f32  tap_gain[NYA_AUDIO_REFLECTION_TAPS];
    f32  tap_pan[NYA_AUDIO_REFLECTION_TAPS];
    f32  tap_coefficient[NYA_AUDIO_REFLECTION_TAPS];
    f32  tap_state[NYA_AUDIO_REFLECTION_TAPS];
    b8   taps_running;
};

/** Zero fields take their defaults and ranges are clamped. */
NYA_INTERNAL NYA_AudioEffects _nya_audio_effects_validate(NYA_AudioEffects effects) __attr_no_discard;

/** Whether any unit would do something with these settings, so a bus that never needs one never gets a chain. */
NYA_INTERNAL b8 _nya_audio_chain_settings_active(const NYA_AudioChainSettings* settings) __attr_no_discard;

/** A new chain from `allocator`, every unit off. */
NYA_INTERNAL NYA_AudioChain* _nya_audio_chain_create(NYA_Arena* allocator) __attr_no_discard;

/**
 * Hands settings to the mixer. Allocates, from `allocator`, any line a newly enabled unit needs before publishing. A
 * no-op when nothing changed.
 * */
NYA_INTERNAL void _nya_audio_chain_publish(NYA_AudioChain* chain, const NYA_AudioChainSettings* settings, NYA_Arena* allocator);

/**
 * Runs the chain over interleaved float `pcm` in place. Mixer thread: no allocation, logging or locks. `samples`
 * counts floats, not frames, as SDL_mixer does.
 * */
NYA_INTERNAL void _nya_audio_chain_process(NYA_AudioChain* chain, const SDL_AudioSpec* spec, f32* pcm, s32 samples);

/** Coefficients for one section at `hz`. `gain_db` is ignored by the passes, `q` by the shelves. */
NYA_INTERNAL void _nya_audio_biquad_configure(NYA_AudioBiquad* biquad, NYA_AudioBiquadShape shape, f32 hz, f32 q, f32 gain_db, f32 rate);

NYA_INTERNAL void _nya_audio_biquad_apply(NYA_AudioBiquad* biquad, f32* pcm, s32 frames, s32 channels);

/** Clears a section's history, leaving its coefficients. */
NYA_INTERNAL void _nya_audio_biquad_reset(NYA_AudioBiquad* biquad);

/** Sizes and clears reverb lines for a sample rate. */
NYA_INTERNAL void _nya_audio_reverb_configure(NYA_AudioReverbLines* lines, s32 rate);

/*
 * The units, each over one block with its parameters already eased. A pass is given its cutoff and mix pair and
 * `target_hz` zero for off; it sweeps from and back to `open_hz` while fading, then skips.
 */
NYA_INTERNAL void _nya_audio_chain_pass(
    f32* state, NYA_AudioBiquad* biquad, NYA_AudioBiquadShape shape, f32 target_hz, f32 open_hz, f32 resonance, f32 ease, f32 rate, f32* pcm,
    s32 frames, s32 channels
);
NYA_INTERNAL void _nya_audio_chain_compress(NYA_AudioChain* chain, f32* pcm, s32 frames, s32 channels, f32 rate);
NYA_INTERNAL void _nya_audio_chain_echo(NYA_AudioChain* chain, NYA_AudioEchoLine* line, f32* pcm, s32 frames, s32 channels, f32 rate);
NYA_INTERNAL void _nya_audio_chain_reflect(NYA_AudioChain* chain, NYA_AudioReflectionLine* line, f32* pcm, s32 frames, s32 channels, f32 rate);
NYA_INTERNAL void _nya_audio_chain_reverberate(NYA_AudioChain* chain, NYA_AudioReverbLines* lines, f32* pcm, s32 frames, s32 channels, f32 ease);
NYA_INTERNAL void _nya_audio_chain_limit(NYA_AudioChain* chain, f32* pcm, s32 frames, s32 channels, f32 rate);

/** The one-pole coefficient reaching 63% of a step in `ms`. */
NYA_INTERNAL f32 _nya_audio_time_coefficient(f32 ms, f32 rate) __attr_no_discard;

/** A one-pole low pass coefficient for a cutoff, 1 open. */
NYA_INTERNAL f32 _nya_audio_one_pole_coefficient(f32 hz, f32 rate) __attr_no_discard;

/** Decibels to a linear gain. */
NYA_INTERNAL f32 _nya_audio_db_to_gain(f32 db) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_AudioEffects _nya_audio_effects_validate(NYA_AudioEffects effects) {
    NYA_AudioPass* pass = &effects.pass;

    pass->lowpass_hz  = nya_max(pass->lowpass_hz, 0.0F);
    pass->highpass_hz = nya_max(pass->highpass_hz, 0.0F);
    pass->glide_ms    = nya_max(pass->glide_ms, 0.0F);
    if (pass->resonance <= 0.0F) pass->resonance = NYA_AUDIO_PASS_RESONANCE;
    if (pass->glide_ms <= 0.0F) pass->glide_ms = NYA_AUDIO_EFFECTS_SMOOTHING_MS;

    NYA_AudioEqualizer* equalizer = &effects.equalizer;

    // past 24 dB a bell is a feedback squeal, not a tone control.
    equalizer->low_db  = nya_clamp(equalizer->low_db, -24.0F, 24.0F);
    equalizer->mid_db  = nya_clamp(equalizer->mid_db, -24.0F, 24.0F);
    equalizer->high_db = nya_clamp(equalizer->high_db, -24.0F, 24.0F);
    if (equalizer->low_hz <= 0.0F) equalizer->low_hz = NYA_AUDIO_EQUALIZER_LOW_HZ;
    if (equalizer->mid_hz <= 0.0F) equalizer->mid_hz = NYA_AUDIO_EQUALIZER_MID_HZ;
    if (equalizer->high_hz <= 0.0F) equalizer->high_hz = NYA_AUDIO_EQUALIZER_HIGH_HZ;
    if (equalizer->mid_q <= 0.0F) equalizer->mid_q = NYA_AUDIO_EQUALIZER_MID_Q;

    NYA_AudioCompressor* compressor = &effects.compressor;

    if (compressor->threshold_db >= 0.0F) compressor->threshold_db = NYA_AUDIO_COMPRESSOR_THRESHOLD_DB;
    if (compressor->ratio < 1.0F) compressor->ratio = NYA_AUDIO_COMPRESSOR_RATIO;
    if (compressor->attack_ms <= 0.0F) compressor->attack_ms = NYA_AUDIO_COMPRESSOR_ATTACK_MS;
    if (compressor->release_ms <= 0.0F) compressor->release_ms = NYA_AUDIO_COMPRESSOR_RELEASE_MS;
    compressor->makeup_db = nya_clamp(compressor->makeup_db, 0.0F, 24.0F);

    NYA_AudioEcho* echo = &effects.echo;

    if (echo->delay_ms <= 0.0F) echo->delay_ms = NYA_AUDIO_ECHO_DELAY_MS;
    if (echo->feedback <= 0.0F) echo->feedback = NYA_AUDIO_ECHO_FEEDBACK;
    if (echo->wet <= 0.0F) echo->wet = NYA_AUDIO_ECHO_WET;
    if (echo->lowpass_hz <= 0.0F) echo->lowpass_hz = NYA_AUDIO_ECHO_LOWPASS_HZ;

    // at one every repeat is as loud as the last, forever.
    echo->feedback = nya_min(echo->feedback, 0.95F);

    NYA_AudioReverb* reverb = &effects.reverb;

    // `room_size` and `damping` are not defaulted: zero room is off, and zero damping is a real setting.
    if (reverb->wet <= 0.0F) reverb->wet = 0.3F;
    if (reverb->dry <= 0.0F) reverb->dry = 1.0F;
    if (reverb->width <= 0.0F) reverb->width = 1.0F;
    reverb->room_size = nya_clamp(reverb->room_size, 0.0F, _NYA_AUDIO_REVERB_ROOM_MAX);
    reverb->damping   = nya_clamp(reverb->damping, 0.0F, 1.0F);
    reverb->width     = nya_min(reverb->width, 1.0F);

    NYA_AudioLimiter* limiter = &effects.limiter;

    if (limiter->ceiling_db >= 0.0F) limiter->ceiling_db = NYA_AUDIO_LIMITER_CEILING_DB;
    if (limiter->release_ms <= 0.0F) limiter->release_ms = NYA_AUDIO_LIMITER_RELEASE_MS;

    return effects;
}

b8 _nya_audio_chain_settings_active(const NYA_AudioChainSettings* settings) {
    const NYA_AudioEffects* effects = &settings->effects;

    b8 any = effects->pass.lowpass_hz > 0.0F || effects->pass.highpass_hz > 0.0F || effects->equalizer.low_db != 0.0F
          || effects->equalizer.mid_db != 0.0F || effects->equalizer.high_db != 0.0F || effects->compressor.enabled || effects->echo.enabled
          || effects->reverb.room_size > 0.0F || effects->limiter.enabled;

    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) any = any || settings->reflections.taps[tap].gain > 0.0F;

    return any;
}

NYA_AudioChain* _nya_audio_chain_create(NYA_Arena* allocator) {
    nya_assert(allocator != nullptr);

    NYA_AudioChain* chain = nya_arena_alloc(allocator, sizeof(NYA_AudioChain));
    nya_memset(chain, 0, sizeof(NYA_AudioChain));

    // the eased state starts where "off" is, so the first enabled setting eases in from nothing.
    chain->compressor_gain = 1.0F;
    chain->limiter_gain    = 1.0F;
    chain->reverb_dry      = 1.0F;

    return chain;
}

void _nya_audio_chain_publish(NYA_AudioChain* chain, const NYA_AudioChainSettings* settings, NYA_Arena* allocator) {
    nya_assert(chain != nullptr && settings != nullptr && allocator != nullptr);

    if (nya_memcmp(&chain->published, settings, sizeof(NYA_AudioChainSettings)) == 0) return;

    // lines first, so the mixer never reads a setting whose line is not there yet.
    if (settings->effects.reverb.room_size > 0.0F && atomic_load(&chain->reverb_lines) == nullptr) {
        NYA_AudioReverbLines* lines = nya_arena_alloc(allocator, sizeof(NYA_AudioReverbLines));
        nya_memset(lines, 0, sizeof(NYA_AudioReverbLines));
        atomic_store(&chain->reverb_lines, lines);
    }

    if (settings->effects.echo.enabled && atomic_load(&chain->echo_line) == nullptr) {
        NYA_AudioEchoLine* line = nya_arena_alloc(allocator, sizeof(NYA_AudioEchoLine));
        nya_memset(line, 0, sizeof(NYA_AudioEchoLine));
        atomic_store(&chain->echo_line, line);
    }

    b8 tapped = false;
    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) tapped = tapped || settings->reflections.taps[tap].gain > 0.0F;

    if (tapped && atomic_load(&chain->reflection_line) == nullptr) {
        NYA_AudioReflectionLine* line = nya_arena_alloc(allocator, sizeof(NYA_AudioReflectionLine));
        nya_memset(line, 0, sizeof(NYA_AudioReflectionLine));
        atomic_store(&chain->reflection_line, line);
    }

    u32 version = atomic_load(&chain->version);
    nya_assert((version & 1U) == 0, "two writers published to one audio chain at once");

    atomic_store(&chain->version, version + 1);
    chain->published = *settings;
    atomic_store(&chain->version, version + 2);
}

void _nya_audio_chain_process(NYA_AudioChain* chain, const SDL_AudioSpec* spec, f32* pcm, s32 samples) {
    nya_trace_scope(NYA_TRACE_AUDIO_EFFECTS);

    if (samples <= 0 || spec->channels <= 0 || spec->freq <= 0) return;
    if (spec->channels > NYA_AUDIO_EFFECTS_MAX_CHANNELS) return;

    // a torn read is thrown away and the previous settings kept; the next buffer tries again.
    u32 version = atomic_load(&chain->version);

    if (version != chain->seen_version && (version & 1U) == 0) {
        NYA_AudioChainSettings copy = chain->published;

        if (atomic_load(&chain->version) == version) {
            chain->target       = copy;
            chain->seen_version = version;
        }
    }

    s32 channels = spec->channels;
    s32 frames   = samples / channels;
    f32 rate     = (f32)spec->freq;

    if (frames <= 0) return;

    const NYA_AudioEffects* target = &chain->target.effects;

    // the lowest pass and the highest are "open": filters settle there and are then skipped.
    f32 open_low  = rate * 0.45F;
    f32 open_high = 10.0F;

    if (chain->rate != spec->freq) {
        chain->rate              = spec->freq;
        chain->lowpass_state[0]  = open_low;
        chain->lowpass_state[1]  = 0.0F;
        chain->lowpass_state[2]  = 0.0F;
        chain->highpass_state[0] = open_high;
        chain->highpass_state[1] = 0.0F;
        chain->highpass_state[2] = 0.0F;
        chain->resonance         = NYA_AUDIO_PASS_RESONANCE;

        _nya_audio_biquad_reset(&chain->lowpass);
        _nya_audio_biquad_reset(&chain->highpass);

        for (u32 band = 0; band < _NYA_AUDIO_EQUALIZER_BANDS; band++) {
            _nya_audio_biquad_reset(&chain->bands[band]);
            chain->band_computed_db[band] = NAN;
        }
    }

    NYA_AudioReverbLines*    reverb_lines    = atomic_load(&chain->reverb_lines);
    NYA_AudioEchoLine*       echo_line       = atomic_load(&chain->echo_line);
    NYA_AudioReflectionLine* reflection_line = atomic_load(&chain->reflection_line);

    if (reverb_lines != nullptr && chain->reverb_rate != (u32)spec->freq) {
        _nya_audio_reverb_configure(reverb_lines, spec->freq);
        chain->reverb_rate = (u32)spec->freq;
    }

    f32 ease      = 1.0F - expf(-(f32)NYA_AUDIO_EFFECTS_BLOCK / (NYA_AUDIO_EFFECTS_SMOOTHING_MS * 0.001F * rate));
    f32 pass_ease = 1.0F - expf(-(f32)NYA_AUDIO_EFFECTS_BLOCK / (nya_max(target->pass.glide_ms, 1.0F) * 0.001F * rate));

    const f32 band_hz[_NYA_AUDIO_EQUALIZER_BANDS] = { target->equalizer.low_hz, target->equalizer.mid_hz, target->equalizer.high_hz };
    const f32 band_db[_NYA_AUDIO_EQUALIZER_BANDS] = { target->equalizer.low_db, target->equalizer.mid_db, target->equalizer.high_db };

    const NYA_AudioBiquadShape band_shape[_NYA_AUDIO_EQUALIZER_BANDS] = { NYA_AUDIO_BIQUAD_LOW_SHELF, NYA_AUDIO_BIQUAD_PEAK, NYA_AUDIO_BIQUAD_HIGH_SHELF };

    for (s32 start = 0; start < frames; start += NYA_AUDIO_EFFECTS_BLOCK) {
        s32  block = nya_min(NYA_AUDIO_EFFECTS_BLOCK, frames - start);
        f32* row   = &pcm[(ptrdiff_t)start * channels];

        // ── passes, eased in the log domain so a sweep sounds even ──
        f32 resonance     = nya_max(target->pass.resonance, 0.1F);
        chain->resonance += (resonance - chain->resonance) * ease;
        if (fabsf(chain->resonance - resonance) < 1e-3F) chain->resonance = resonance;

        f32 lowpass_target  = target->pass.lowpass_hz > 0.0F ? nya_clamp(target->pass.lowpass_hz, 20.0F, open_low) : 0.0F;
        f32 highpass_target = target->pass.highpass_hz > 0.0F ? nya_clamp(target->pass.highpass_hz, open_high, open_low) : 0.0F;

        _nya_audio_chain_pass(chain->lowpass_state, &chain->lowpass, NYA_AUDIO_BIQUAD_LOWPASS, lowpass_target, open_low, chain->resonance, pass_ease, rate, row, block, channels);
        _nya_audio_chain_pass(chain->highpass_state, &chain->highpass, NYA_AUDIO_BIQUAD_HIGHPASS, highpass_target, open_high, chain->resonance, pass_ease, rate, row, block, channels);

        // ── equaliser: a band at zero, settled, is skipped ──
        for (u32 band = 0; band < _NYA_AUDIO_EQUALIZER_BANDS; band++) {
            if (band_db[band] == 0.0F && fabsf(chain->band_db[band]) < 0.01F) {
                if (chain->band_db[band] != 0.0F) {
                    chain->band_db[band] = 0.0F;
                    _nya_audio_biquad_reset(&chain->bands[band]);
                }
                continue;
            }

            chain->band_db[band] += (band_db[band] - chain->band_db[band]) * ease;
            if (fabsf(chain->band_db[band] - band_db[band]) < 0.01F) chain->band_db[band] = band_db[band];

            if (chain->band_db[band] != chain->band_computed_db[band]) {
                f32 hz = nya_clamp(band_hz[band], 20.0F, open_low);
                _nya_audio_biquad_configure(&chain->bands[band], band_shape[band], hz, target->equalizer.mid_q, chain->band_db[band], rate);
                chain->band_computed_db[band] = chain->band_db[band];
            }

            _nya_audio_biquad_apply(&chain->bands[band], row, block, channels);
        }

        if (target->compressor.enabled || chain->compressor_gain != 1.0F) _nya_audio_chain_compress(chain, row, block, channels, rate);

        if (echo_line != nullptr && (target->echo.enabled || chain->echo_running)) _nya_audio_chain_echo(chain, echo_line, row, block, channels, rate);

        if (reflection_line != nullptr) _nya_audio_chain_reflect(chain, reflection_line, row, block, channels, rate);

        if (reverb_lines != nullptr && (target->reverb.room_size > 0.0F || chain->reverb_wet > 0.0F)) {
            _nya_audio_chain_reverberate(chain, reverb_lines, row, block, channels, ease);
        }

        if (target->limiter.enabled || chain->limiter_gain < 1.0F) _nya_audio_chain_limit(chain, row, block, channels, rate);
    }
}

void _nya_audio_chain_pass(
    f32* state, NYA_AudioBiquad* biquad, NYA_AudioBiquadShape shape, f32 target_hz, f32 open_hz, f32 resonance, f32 ease, f32 rate, f32* pcm,
    s32 frames, s32 channels
) {
    nya_assert(frames <= NYA_AUDIO_EFFECTS_BLOCK && channels <= NYA_AUDIO_EFFECTS_MAX_CHANNELS);

    f32 mix_target = target_hz > 0.0F ? 1.0F : 0.0F;

    if (mix_target == 0.0F && state[1] == 0.0F) return;

    // a section near Nyquist is badly conditioned and starts from silence, so a new pass is faded in, not switched.
    f32 goal = target_hz > 0.0F ? target_hz : open_hz;

    // snapped once within a thousandth, so a settled pass stops recomputing.
    if (state[0] != goal) state[0] = expf(nya_lerp(logf(state[0]), logf(goal), ease));
    if (fabsf(state[0] - goal) < goal * 1e-3F) state[0] = goal;

    f32 mix_start = state[1];
    state[1]     += (mix_target - state[1]) * ease;

    if (mix_target == 0.0F && state[1] < 1e-3F) {
        state[0] = open_hz;
        state[1] = 0.0F;
        _nya_audio_biquad_reset(biquad);
        return;
    }

    f32 dry[NYA_AUDIO_EFFECTS_BLOCK * NYA_AUDIO_EFFECTS_MAX_CHANNELS];
    b8  fading = mix_start < 1.0F || state[1] < 1.0F;

    if (fading) {
        for (s32 i = 0; i < frames * channels; i++) dry[i] = pcm[i];
    }

    // a settled cutoff keeps its coefficients: the trigonometry costs more than the filter.
    if (state[2] != state[0] || state[3] != resonance) {
        _nya_audio_biquad_configure(biquad, shape, state[0], resonance, 0.0F, rate);
        state[2] = state[0];
        state[3] = resonance;
    }

    _nya_audio_biquad_apply(biquad, pcm, frames, channels);

    if (!fading) return;

    f32 step = (state[1] - mix_start) / (f32)frames;

    for (s32 frame = 0; frame < frames; frame++) {
        f32 mix = mix_start + (step * (f32)(frame + 1));

        for (s32 channel = 0; channel < channels; channel++) {
            s32 i  = (frame * channels) + channel;
            pcm[i] = nya_lerp(dry[i], pcm[i], mix);
        }
    }

    // settled in: exact from here.
    if (state[1] > 0.9999F) state[1] = 1.0F;
}

void _nya_audio_chain_compress(NYA_AudioChain* chain, f32* pcm, s32 frames, s32 channels, f32 rate) {
    const NYA_AudioCompressor* compressor = &chain->target.effects.compressor;

    f32 attack  = _nya_audio_time_coefficient(compressor->attack_ms, rate);
    f32 release = _nya_audio_time_coefficient(compressor->release_ms, rate);

    // the detector runs per sample; the gain is computed once a block and ramped across it, which cannot zipper.
    for (s32 frame = 0; frame < frames; frame++) {
        f32 peak = 0.0F;
        for (s32 channel = 0; channel < channels; channel++) peak = nya_max(peak, fabsf(pcm[(frame * channels) + channel]));

        f32 coefficient            = peak > chain->compressor_envelope ? attack : release;
        chain->compressor_envelope += (peak - chain->compressor_envelope) * coefficient;
    }

    f32 target = 1.0F;

    if (compressor->enabled) {
        f32 level_db = 20.0F * log10f(chain->compressor_envelope + 1e-9F);
        f32 over_db  = level_db - compressor->threshold_db;
        f32 cut_db   = over_db > 0.0F ? over_db * (1.0F - (1.0F / compressor->ratio)) : 0.0F;

        target = _nya_audio_db_to_gain(compressor->makeup_db - cut_db);
    }

    f32 start = chain->compressor_gain;
    f32 end   = target;
    f32 step  = (end - start) / (f32)frames;

    for (s32 frame = 0; frame < frames; frame++) {
        f32 gain = start + (step * (f32)(frame + 1));
        for (s32 channel = 0; channel < channels; channel++) pcm[(frame * channels) + channel] *= gain;
    }

    chain->compressor_gain = end;

    if (!compressor->enabled && fabsf(chain->compressor_gain - 1.0F) < 1e-4F) {
        chain->compressor_gain     = 1.0F;
        chain->compressor_envelope = 0.0F;
    }
}

void _nya_audio_chain_echo(NYA_AudioChain* chain, NYA_AudioEchoLine* line, f32* pcm, s32 frames, s32 channels, f32 rate) {
    const NYA_AudioEcho* echo = &chain->target.effects.echo;

    f32 delay_target = nya_clamp(echo->delay_ms * 0.001F * rate, 1.0F, (f32)(NYA_AUDIO_ECHO_FRAMES - 2));

    // switched on from silence: start from the setting, over a cleared line, so no old repeat plays back.
    if (!chain->echo_running) {
        nya_memset(line, 0, sizeof(NYA_AudioEchoLine));
        chain->echo_delay      = delay_target;
        chain->echo_damping[0] = 0.0F;
        chain->echo_damping[1] = 0.0F;
        chain->echo_running    = true;
    }

    f32 block_ease = 1.0F - expf(-(f32)frames / (NYA_AUDIO_EFFECTS_SMOOTHING_MS * 0.001F * rate));

    f32 wet_target      = echo->enabled ? echo->wet : 0.0F;
    f32 wet_start       = chain->echo_wet;
    f32 wet_step        = (wet_target - wet_start) * block_ease / (f32)frames;
    chain->echo_feedback += (echo->feedback - chain->echo_feedback) * block_ease;

    // moved at a tape's pace, so a new time bends pitch briefly rather than clicking.
    f32 delay_start = chain->echo_delay;
    f32 delay_step  = (delay_target - delay_start) * block_ease / (f32)frames;

    f32 damping  = _nya_audio_one_pole_coefficient(echo->lowpass_hz, rate);
    u32 mask     = NYA_AUDIO_ECHO_FRAMES - 1;
    f32 feedback = chain->echo_feedback;

    for (s32 frame = 0; frame < frames; frame++) {
        f32* row   = &pcm[(ptrdiff_t)frame * channels];
        f32  wet   = wet_start + (wet_step * (f32)(frame + 1));
        f32  delay = delay_start + (delay_step * (f32)(frame + 1));

        f32 input[_NYA_AUDIO_STEREO] = { 0.0F, 0.0F };
        for (s32 channel = 0; channel < channels; channel++) input[channel & 1] += row[channel];

        // mono feeds both lines, so a mono device hears the same repeats.
        if (channels == 1) input[1] = input[0];

        f32 whole    = floorf(delay);
        f32 fraction = delay - whole;
        // the line length added first, so the index wraps without going below zero.
        u32 back = (line->cursor + mask + 1 - (u32)whole) & mask;

        f32 output[_NYA_AUDIO_STEREO];

        for (u32 side = 0; side < _NYA_AUDIO_STEREO; side++) {
            f32 newer   = line->buffer[side][back];
            f32 older   = line->buffer[side][(back + mask) & mask];
            f32 delayed = newer + ((older - newer) * fraction);

            chain->echo_damping[side] += (delayed - chain->echo_damping[side]) * damping;

            output[side]                            = chain->echo_damping[side];
            line->buffer[side][line->cursor & mask] = (echo->enabled ? input[side] : 0.0F) + (output[side] * feedback);
        }

        line->cursor = (line->cursor + 1) & mask;

        for (s32 channel = 0; channel < channels; channel++) row[channel] += output[channel & 1] * wet;
    }

    chain->echo_wet   = wet_start + (wet_step * (f32)frames);
    chain->echo_delay = delay_start + (delay_step * (f32)frames);

    // off, and the last repeat has faded under the wet: stop running until switched on again.
    if (!echo->enabled && chain->echo_wet < 1e-3F) {
        chain->echo_wet     = 0.0F;
        chain->echo_running = false;
    }

    for (u32 side = 0; side < _NYA_AUDIO_STEREO; side++) {
        if (fabsf(chain->echo_damping[side]) < 1e-20F) chain->echo_damping[side] = 0.0F;
    }
}

void _nya_audio_chain_reflect(NYA_AudioChain* chain, NYA_AudioReflectionLine* line, f32* pcm, s32 frames, s32 channels, f32 rate) {
    const NYA_AudioReflections* reflections = &chain->target.reflections;

    b8 audible = false;
    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) audible = audible || reflections->taps[tap].gain > 0.0F || chain->tap_gain[tap] > 0.0F;

    if (!audible) {
        chain->taps_running = false;
        return;
    }

    if (!chain->taps_running) {
        nya_memset(line, 0, sizeof(NYA_AudioReflectionLine));

        for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) {
            chain->tap_delay[tap] = nya_clamp(reflections->taps[tap].delay_s * rate, 1.0F, (f32)(NYA_AUDIO_REFLECTION_FRAMES - 2));
            chain->tap_state[tap] = 0.0F;
        }

        chain->taps_running = true;
    }

    f32 block_ease = 1.0F - expf(-(f32)frames / (NYA_AUDIO_EFFECTS_SMOOTHING_MS * 0.001F * rate));
    u32 mask       = NYA_AUDIO_REFLECTION_FRAMES - 1;
    f32 scale      = 1.0F / (f32)channels;

    // per block: where each tap reads, how loud, which side and how dull. gains ramp inside the block.
    f32 gain_start[NYA_AUDIO_REFLECTION_TAPS];
    f32 gain_step[NYA_AUDIO_REFLECTION_TAPS];
    f32 delay_step[NYA_AUDIO_REFLECTION_TAPS];
    f32 to_left[NYA_AUDIO_REFLECTION_TAPS];
    f32 to_right[NYA_AUDIO_REFLECTION_TAPS];

    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) {
        const NYA_AudioReflectionTap* setting = &reflections->taps[tap];

        f32 delay_target = nya_clamp(setting->delay_s * rate, 1.0F, (f32)(NYA_AUDIO_REFLECTION_FRAMES - 2));

        gain_start[tap] = chain->tap_gain[tap];
        gain_step[tap]  = (nya_max(setting->gain, 0.0F) - chain->tap_gain[tap]) * block_ease / (f32)frames;
        delay_step[tap] = (delay_target - chain->tap_delay[tap]) * block_ease / (f32)frames;

        chain->tap_pan[tap]         += (nya_clamp(setting->pan, -1.0F, 1.0F) - chain->tap_pan[tap]) * block_ease;
        chain->tap_coefficient[tap] += (_nya_audio_one_pole_coefficient(setting->lowpass_hz, rate) - chain->tap_coefficient[tap]) * block_ease;

        // equal power, so a tap swinging across does not dip in the middle.
        f32 angle     = (chain->tap_pan[tap] + 1.0F) * 0.25F * (f32)M_PI;
        to_left[tap]  = cosf(angle);
        to_right[tap] = sinf(angle);
    }

    for (s32 frame = 0; frame < frames; frame++) {
        f32* row = &pcm[(ptrdiff_t)frame * channels];

        f32 mono = 0.0F;
        for (s32 channel = 0; channel < channels; channel++) mono += row[channel];

        line->buffer[line->cursor & mask] = mono * scale;

        f32 left  = 0.0F;
        f32 right = 0.0F;

        for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) {
            f32 gain = gain_start[tap] + (gain_step[tap] * (f32)(frame + 1));
            if (gain <= 0.0F) continue;

            f32 delay    = chain->tap_delay[tap] + (delay_step[tap] * (f32)(frame + 1));
            f32 whole    = floorf(delay);
            f32 fraction = delay - whole;
            u32 back     = (line->cursor + mask + 1 - (u32)whole) & mask;

            f32 newer   = line->buffer[back];
            f32 older   = line->buffer[(back + mask) & mask];
            f32 delayed = newer + ((older - newer) * fraction);

            chain->tap_state[tap] += (delayed - chain->tap_state[tap]) * chain->tap_coefficient[tap];

            f32 echo  = chain->tap_state[tap] * gain;
            left     += echo * to_left[tap];
            right    += echo * to_right[tap];
        }

        line->cursor = (line->cursor + 1) & mask;

        if (channels == 1) {
            row[0] += (left + right) * 0.7071F;
        } else {
            for (s32 channel = 0; channel < channels; channel++) row[channel] += (channel & 1) == 0 ? left : right;
        }
    }

    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) {
        chain->tap_gain[tap]   = nya_max(gain_start[tap] + (gain_step[tap] * (f32)frames), 0.0F);
        chain->tap_delay[tap] += delay_step[tap] * (f32)frames;

        if (chain->tap_gain[tap] < 1e-5F && reflections->taps[tap].gain <= 0.0F) chain->tap_gain[tap] = 0.0F;
        if (fabsf(chain->tap_state[tap]) < 1e-20F) chain->tap_state[tap] = 0.0F;
    }
}

void _nya_audio_chain_reverberate(NYA_AudioChain* chain, NYA_AudioReverbLines* lines, f32* pcm, s32 frames, s32 channels, f32 ease) {
    const NYA_AudioReverb* target = &chain->target.effects.reverb;

    // off eases the wet out and leaves the lines alone: clearing them would cut a tail dead with a click.
    f32 wet_target = target->room_size > 0.0F ? target->wet : 0.0F;
    f32 dry_target = target->room_size > 0.0F ? target->dry : 1.0F;

    f32 wet_start = chain->reverb_wet;
    f32 dry_start = chain->reverb_dry;
    f32 wet_step  = (wet_target - wet_start) * ease / (f32)frames;
    f32 dry_step  = (dry_target - dry_start) * ease / (f32)frames;

    if (target->room_size > 0.0F) {
        chain->reverb_room    += (target->room_size - chain->reverb_room) * ease;
        chain->reverb_damping += (target->damping - chain->reverb_damping) * ease;
        chain->reverb_width   += (target->width - chain->reverb_width) * ease;
    }

    f32 room_size = chain->reverb_room;
    f32 damping   = chain->reverb_damping;
    f32 width     = chain->reverb_width;

    /* Fed at a fraction of the input: four parallel combs sum, and a full feed would clip the allpasses. */
    const f32 feed = 0.015F;

    // 0.5 is the classic allpass coefficient. it shapes phase, not tail length.
    const f32 allpass_feedback = 0.5F;

    for (s32 frame = 0; frame < frames; frame++) {
        f32* row = &pcm[(ptrdiff_t)frame * channels];

        f32 wet = wet_start + (wet_step * (f32)(frame + 1));
        f32 dry = dry_start + (dry_step * (f32)(frame + 1));

        // even channels are left in every standard layout, odd ones right. mono feeds both networks.
        f32 input[_NYA_AUDIO_STEREO] = { 0.0F, 0.0F };
        for (s32 channel = 0; channel < channels; channel++) input[channel & 1] += row[channel];
        if (channels == 1) input[1] = input[0];

        f32 output[_NYA_AUDIO_STEREO] = { 0.0F, 0.0F };

        for (u32 network = 0; network < _NYA_AUDIO_STEREO; network++) {
            f32 sum = 0.0F;

            /* Combs in parallel, each with a one-pole in its feedback, so highs decay faster than lows. */
            for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
                NYA_AudioReverbComb* comb = &lines->combs[network][i];

                f32 delayed = comb->buffer[comb->cursor];

                comb->damped = (delayed * (1.0F - damping)) + (comb->damped * damping);

                comb->buffer[comb->cursor] = (input[network] * feed) + (comb->damped * room_size);

                comb->cursor++;
                if (comb->cursor >= comb->length) comb->cursor = 0;

                sum += delayed;
            }

            /* Allpasses in series multiply echo density without adding colour. Four combs alone flutter. */
            for (u32 i = 0; i < _NYA_AUDIO_REVERB_ALLPASSES; i++) {
                NYA_AudioReverbAllpass* allpass = &lines->allpasses[network][i];

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

    chain->reverb_wet = wet_start + (wet_step * (f32)frames);
    chain->reverb_dry = dry_start + (dry_step * (f32)frames);

    if (target->room_size <= 0.0F && chain->reverb_wet < 1e-4F) {
        chain->reverb_wet = 0.0F;
        chain->reverb_dry = 1.0F;
    }

    /* Flushed to zero once inaudible: the decaying values reach denormals, which some CPUs process very slowly. */
    for (u32 network = 0; network < _NYA_AUDIO_STEREO; network++) {
        for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
            NYA_AudioReverbComb* comb = &lines->combs[network][i];

            if (fabsf(comb->damped) < 1e-20F) comb->damped = 0.0F;
            if (fabsf(comb->buffer[comb->cursor]) < 1e-20F) comb->buffer[comb->cursor] = 0.0F;
        }

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_ALLPASSES; i++) {
            NYA_AudioReverbAllpass* allpass = &lines->allpasses[network][i];

            if (fabsf(allpass->buffer[allpass->cursor]) < 1e-20F) allpass->buffer[allpass->cursor] = 0.0F;
        }
    }
}

void _nya_audio_chain_limit(NYA_AudioChain* chain, f32* pcm, s32 frames, s32 channels, f32 rate) {
    const NYA_AudioLimiter* limiter = &chain->target.effects.limiter;

    // a hair under, so rounding in the division below cannot land a sample on the wrong side.
    f32 ceiling = _nya_audio_db_to_gain(limiter->ceiling_db) * 0.9999F;
    f32 release = _nya_audio_time_coefficient(limiter->release_ms, rate);

    for (s32 frame = 0; frame < frames; frame++) {
        f32* row = &pcm[(ptrdiff_t)frame * channels];

        f32 peak = 0.0F;
        for (s32 channel = 0; channel < channels; channel++) peak = nya_max(peak, fabsf(row[channel]));

        // recovers smoothly, but never lets a sample through above the ceiling: the attack is this sample.
        chain->limiter_gain += (1.0F - chain->limiter_gain) * release;

        if (limiter->enabled && peak * chain->limiter_gain > ceiling) chain->limiter_gain = ceiling / peak;

        for (s32 channel = 0; channel < channels; channel++) row[channel] *= chain->limiter_gain;
    }

    if (chain->limiter_gain > 0.9999F) chain->limiter_gain = 1.0F;

    nya_assert(!limiter->enabled || chain->limiter_gain <= 1.0F);
}

void _nya_audio_biquad_configure(NYA_AudioBiquad* biquad, NYA_AudioBiquadShape shape, f32 hz, f32 q, f32 gain_db, f32 rate) {
    nya_assert(hz > 0.0F && q > 0.0F && rate > 0.0F);

    // the audio EQ cookbook's formulas, normalised by a0.
    f32 omega  = 2.0F * (f32)M_PI * hz / rate;
    f32 cosine = cosf(omega);
    f32 alpha  = sinf(omega) / (2.0F * q);
    f32 amp    = powf(10.0F, gain_db / 40.0F);

    // shelves use a slope of one, which fixes their alpha.
    f32 shelf = 2.0F * sqrtf(amp) * (sinf(omega) * 0.5F * sqrtf(2.0F));

    f32 b0 = 1.0F;
    f32 b1 = 0.0F;
    f32 b2 = 0.0F;
    f32 a0 = 1.0F;
    f32 a1 = 0.0F;
    f32 a2 = 0.0F;

    switch (shape) {
        case NYA_AUDIO_BIQUAD_LOWPASS:
            b0 = (1.0F - cosine) * 0.5F;
            b1 = 1.0F - cosine;
            b2 = b0;
            a0 = 1.0F + alpha;
            a1 = -2.0F * cosine;
            a2 = 1.0F - alpha;
            break;

        case NYA_AUDIO_BIQUAD_HIGHPASS:
            b0 = (1.0F + cosine) * 0.5F;
            b1 = -(1.0F + cosine);
            b2 = b0;
            a0 = 1.0F + alpha;
            a1 = -2.0F * cosine;
            a2 = 1.0F - alpha;
            break;

        case NYA_AUDIO_BIQUAD_PEAK:
            b0 = 1.0F + (alpha * amp);
            b1 = -2.0F * cosine;
            b2 = 1.0F - (alpha * amp);
            a0 = 1.0F + (alpha / amp);
            a1 = -2.0F * cosine;
            a2 = 1.0F - (alpha / amp);
            break;

        case NYA_AUDIO_BIQUAD_LOW_SHELF:
            b0 = amp * ((amp + 1.0F) - ((amp - 1.0F) * cosine) + shelf);
            b1 = 2.0F * amp * ((amp - 1.0F) - ((amp + 1.0F) * cosine));
            b2 = amp * ((amp + 1.0F) - ((amp - 1.0F) * cosine) - shelf);
            a0 = (amp + 1.0F) + ((amp - 1.0F) * cosine) + shelf;
            a1 = -2.0F * ((amp - 1.0F) + ((amp + 1.0F) * cosine));
            a2 = (amp + 1.0F) + ((amp - 1.0F) * cosine) - shelf;
            break;

        case NYA_AUDIO_BIQUAD_HIGH_SHELF:
            b0 = amp * ((amp + 1.0F) + ((amp - 1.0F) * cosine) + shelf);
            b1 = -2.0F * amp * ((amp - 1.0F) + ((amp + 1.0F) * cosine));
            b2 = amp * ((amp + 1.0F) + ((amp - 1.0F) * cosine) - shelf);
            a0 = (amp + 1.0F) - ((amp - 1.0F) * cosine) + shelf;
            a1 = 2.0F * ((amp - 1.0F) - ((amp + 1.0F) * cosine));
            a2 = (amp + 1.0F) - ((amp - 1.0F) * cosine) - shelf;
            break;

        default: nya_unreachable();
    }

    biquad->b0 = b0 / a0;
    biquad->b1 = b1 / a0;
    biquad->b2 = b2 / a0;
    biquad->a1 = a1 / a0;
    biquad->a2 = a2 / a0;
}

void _nya_audio_biquad_apply(NYA_AudioBiquad* biquad, f32* pcm, s32 frames, s32 channels) {
    for (s32 channel = 0; channel < channels; channel++) {
        f32 z1 = biquad->z1[channel];
        f32 z2 = biquad->z2[channel];

        for (s32 frame = 0; frame < frames; frame++) {
            f32* sample = &pcm[(frame * channels) + channel];

            f32 input  = *sample;
            f32 output = (biquad->b0 * input) + z1;

            z1 = (biquad->b1 * input) - (biquad->a1 * output) + z2;
            z2 = (biquad->b2 * input) - (biquad->a2 * output);

            *sample = output;
        }

        // flushed once inaudible, since a decaying section reaches denormals.
        biquad->z1[channel] = fabsf(z1) < 1e-20F ? 0.0F : z1;
        biquad->z2[channel] = fabsf(z2) < 1e-20F ? 0.0F : z2;
    }
}

void _nya_audio_biquad_reset(NYA_AudioBiquad* biquad) {
    for (u32 channel = 0; channel < NYA_AUDIO_EFFECTS_MAX_CHANNELS; channel++) {
        biquad->z1[channel] = 0.0F;
        biquad->z2[channel] = 0.0F;
    }
}

void _nya_audio_reverb_configure(NYA_AudioReverbLines* lines, s32 rate) {
    // the published lengths are for 44.1 kHz; unscaled they would halve the room at 96 kHz.
    f32 scale = (f32)rate / 44100.0F;

    for (u32 network = 0; network < _NYA_AUDIO_STEREO; network++) {
        u32 spread = network == 0 ? 0 : _NYA_AUDIO_REVERB_STEREO_SPREAD;

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_COMBS; i++) {
            NYA_AudioReverbComb* comb = &lines->combs[network][i];

            u32 length = (u32)(((f32)_NYA_AUDIO_REVERB_COMB_LENGTHS[i] + (f32)spread) * scale);

            // never zero: a zero-length line feeds output straight back into input.
            comb->length = nya_clamp(length, 1U, (u32)_NYA_AUDIO_REVERB_MAX_DELAY);
            comb->cursor = 0;
            comb->damped = 0.0F;

            for (u32 sample = 0; sample < comb->length; sample++) comb->buffer[sample] = 0.0F;
        }

        for (u32 i = 0; i < _NYA_AUDIO_REVERB_ALLPASSES; i++) {
            NYA_AudioReverbAllpass* allpass = &lines->allpasses[network][i];

            u32 length = (u32)(((f32)_NYA_AUDIO_REVERB_ALLPASS_LENGTHS[i] + (f32)spread) * scale);

            allpass->length = nya_clamp(length, 1U, (u32)_NYA_AUDIO_REVERB_MAX_DELAY);
            allpass->cursor = 0;

            for (u32 sample = 0; sample < allpass->length; sample++) allpass->buffer[sample] = 0.0F;
        }
    }
}

f32 _nya_audio_time_coefficient(f32 ms, f32 rate) {
    return 1.0F - expf(-1.0F / (nya_max(ms, 0.01F) * 0.001F * rate));
}

f32 _nya_audio_one_pole_coefficient(f32 hz, f32 rate) {
    if (hz <= 0.0F) return 1.0F;

    return nya_clamp(1.0F - expf(-2.0F * (f32)M_PI * hz / rate), 0.0F, 1.0F);
}

f32 _nya_audio_db_to_gain(f32 db) {
    // twenty, not ten: amplitude decibels.
    return powf(10.0F, db / 20.0F);
}
