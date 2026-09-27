#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Cutoff above which a one pole barely touches the signal, so the panner reports it as open. */
#define _NYA_AUDIO_PAN_OPEN_HZ 20000.0F

/** The more restrictive of two cutoffs, treating zero as open rather than as a filter clamped shut. */
NYA_INTERNAL f32 _nya_audio_pan_narrower(f32 a, f32 b) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_StereoPan nya_audio_pan_compute(NYA_StereoPanParams params) {
    f32 head_radius = params.head_radius_m > 0.0F ? params.head_radius_m : NYA_AUDIO_PAN_HEAD_RADIUS_M;
    f32 speed       = params.speed_of_sound_mps > 0.0F ? params.speed_of_sound_mps : NYA_AUDIO_PAN_SPEED_OF_SOUND_MPS;
    f32 shadow_hz   = params.shadow_hz > 0.0F ? params.shadow_hz : NYA_AUDIO_PAN_SHADOW_HZ;

    // wrapped into [-π, π), so a caller need not normalise a running angle.
    f32 two_pi = 2.0F * (f32)M_PI;
    f32 az     = params.azimuth_radians - (two_pi * floorf((params.azimuth_radians + (f32)M_PI) / two_pi));

    // where the source sits across the ears, -1 hard left through 0 to +1 hard right. A source dead ahead
    // and one dead behind both sit at zero: with one angle there is no telling them apart by level or delay,
    // which is the real cone-of-confusion and why the shadow below leans on the front-back term instead.
    f32 lateral = sinf(az);
    f32 side    = fabsf(lateral);

    NYA_StereoPan pan = { 0 };

    // equal power: the two gains square-sum to one, so swinging across the front holds the loudness steady.
    // The swing stops short of an ear, where the far gain over the near is the side level difference.
    f32 side_ratio = powf(10.0F, -NYA_AUDIO_PAN_SIDE_LEVEL_DB / 20.0F);
    f32 swing      = (0.25F * (f32)M_PI) - atanf(side_ratio);
    f32 angle      = (0.25F * (f32)M_PI) + (lateral * swing);
    pan.left_gain  = cosf(angle);
    pan.right_gain = sinf(angle);

    // Woodworth's interaural delay for a spherical head, off the angle from the median plane so it peaks at
    // the side and is the same in front and behind. The far ear is the delayed one.
    f32 lateral_angle = asinf(nya_clamp(side, 0.0F, 1.0F));
    f32 itd           = (head_radius / speed) * (lateral_angle + sinf(lateral_angle));

    if (lateral > 0.0F) {
        pan.left_delay_s = itd;   // source on the right: the left ear is the far one.
    } else if (lateral < 0.0F) {
        pan.right_delay_s = itd;
    }

    // the side shadow rolls the far ear off, harder the further to the side; at the centre it is open.
    f32 far_side_hz = side > 0.0F ? nya_lerp(_NYA_AUDIO_PAN_OPEN_HZ, shadow_hz, side) : 0.0F;

    // the rear shadow rolls both ears off, since behind the listener neither outer ear faces the source. It
    // rises through the back half and is what tells a front source from a rear one.
    f32 abs_az   = fabsf(az);
    f32 rear     = nya_clamp((abs_az - (0.5F * (f32)M_PI)) / (0.5F * (f32)M_PI), 0.0F, 1.0F);
    f32 rear_hz  = rear > 0.0F ? nya_lerp(_NYA_AUDIO_PAN_OPEN_HZ, NYA_AUDIO_PAN_REAR_SHADOW_HZ, rear) : 0.0F;

    b8 left_is_far  = lateral > 0.0F;
    b8 right_is_far = lateral < 0.0F;

    pan.left_lowpass_hz  = _nya_audio_pan_narrower(rear_hz, left_is_far ? far_side_hz : 0.0F);
    pan.right_lowpass_hz = _nya_audio_pan_narrower(rear_hz, right_is_far ? far_side_hz : 0.0F);

    return pan;
}

f32 nya_audio_pan_azimuth(f32x3 listener_relative) {
    // +x right, -z ahead. atan2(x, -z): 0 ahead, +π/2 right, ±π behind. Height plays no part.
    f32 x = listener_relative[0];
    f32 z = listener_relative[2];

    // a source on the listener has no direction; call it ahead rather than let atan2(0,0) decide.
    if (x == 0.0F && z == 0.0F) return 0.0F;

    return atan2f(x, -z);
}

void nya_audio_pan_render_reset(NYA_AudioPanRender* render) {
    if (render == nullptr) return;

    *render = (NYA_AudioPanRender){ 0 };

    // one, not zero: a zero coefficient is a one pole clamped shut. The gains snap on the first buffer.
    render->coefficient[0] = 1.0F;
    render->coefficient[1] = 1.0F;
    render->gain[0]        = 1.0F;
    render->gain[1]        = 1.0F;
}

void nya_audio_pan_render(NYA_AudioPanRender* render, f32 sample_rate_hz, s32 channels, NYA_StereoPan target, f32* pcm, s32 samples) {
    if (render == nullptr || pcm == nullptr) return;

    // a two-ear model: mono and surround pass through rather than get half a stereo image.
    if (channels != 2) return;
    if (sample_rate_hz <= 0.0F || samples <= 0) return;

    s32 frames = samples / channels;
    if (frames <= 0) return;

    // the ring wraps with a mask, and the fractional read reaches one frame past the whole delay.
    static_assert((NYA_AUDIO_PAN_MAX_DELAY_FRAMES & (NYA_AUDIO_PAN_MAX_DELAY_FRAMES - 1)) == 0, "the delay line wraps with a mask");
    const u32 mask      = NYA_AUDIO_PAN_MAX_DELAY_FRAMES - 1;
    const f32 delay_max = (f32)(NYA_AUDIO_PAN_MAX_DELAY_FRAMES - 2);

    f32 target_gain[2]  = { target.left_gain, target.right_gain };
    f32 target_cut[2]   = { target.left_lowpass_hz, target.right_lowpass_hz };
    f32 target_delay[2] = { target.left_delay_s, target.right_delay_s };

    // one pole coefficient per ear, 1 open. a = 1 - e^(-2π·fc/fs). The delay stays fractional, so a source
    // sweeping round the head glides through sub-frame steps instead of clicking a whole frame at a time.
    f32 target_coeff[2];
    f32 target_frames[2];
    for (s32 ear = 0; ear < 2; ear++) {
        target_coeff[ear]  = target_cut[ear] > 0.0F ? nya_clamp(1.0F - expf(-2.0F * (f32)M_PI * target_cut[ear] / sample_rate_hz), 0.0F, 1.0F) : 1.0F;
        target_frames[ear] = nya_clamp(target_delay[ear] * sample_rate_hz, 0.0F, delay_max);
    }

    // the first buffer starts on the target; later ones ease so a turning head glides rather than steps.
    if (!render->primed) {
        for (s32 ear = 0; ear < 2; ear++) {
            render->gain[ear]         = target_gain[ear];
            render->coefficient[ear]  = target_coeff[ear];
            render->delay_frames[ear] = target_frames[ear];
        }
        render->primed = true;
    }

    f32 gain_step[2];
    f32 coeff_step[2];
    f32 delay_step[2];
    for (s32 ear = 0; ear < 2; ear++) {
        gain_step[ear]  = (target_gain[ear] - render->gain[ear]) / (f32)frames;
        coeff_step[ear] = (target_coeff[ear] - render->coefficient[ear]) / (f32)frames;
        delay_step[ear] = (target_frames[ear] - render->delay_frames[ear]) / (f32)frames;
    }

    for (s32 frame = 0; frame < frames; frame++) {
        f32* out = &pcm[frame * 2];

        // one signal from a point source: a stereo clip keeps its loudness but not its image, as in SDL_mixer's
        // own 3D path. A mono clip arrives upmixed to two equal channels and comes back unchanged.
        render->ring[render->write_index] = 0.5F * (out[0] + out[1]);

        for (s32 ear = 0; ear < 2; ear++) {
            // linear interpolation between the two frames either side of the fractional delay.
            f32 delay = render->delay_frames[ear];
            u32 whole = (u32)delay;
            f32 frac  = delay - (f32)whole;
            f32 newer = render->ring[(render->write_index + NYA_AUDIO_PAN_MAX_DELAY_FRAMES - whole) & mask];
            f32 older = render->ring[(render->write_index + NYA_AUDIO_PAN_MAX_DELAY_FRAMES - whole - 1) & mask];
            f32 heard = newer + (frac * (older - newer));

            // head shadow: one pole toward the delayed signal. A coefficient of one passes it through.
            render->shadow_state[ear] += render->coefficient[ear] * (heard - render->shadow_state[ear]);

            out[ear] = render->gain[ear] * render->shadow_state[ear];

            render->gain[ear]         += gain_step[ear];
            render->coefficient[ear]  += coeff_step[ear];
            render->delay_frames[ear] += delay_step[ear];
        }

        render->write_index = (render->write_index + 1) & mask;
    }

    // land exactly on the target, since the per-frame sum drifts, and flush the one pole's denormals.
    for (s32 ear = 0; ear < 2; ear++) {
        render->gain[ear]         = target_gain[ear];
        render->coefficient[ear]  = target_coeff[ear];
        render->delay_frames[ear] = target_frames[ear];
        if (fabsf(render->shadow_state[ear]) < 1e-20F) render->shadow_state[ear] = 0.0F;
    }

    nya_assert(render->write_index <= mask);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _nya_audio_pan_narrower(f32 a, f32 b) {
    if (a <= 0.0F) return b;
    if (b <= 0.0F) return a;

    return nya_min(a, b);
}
