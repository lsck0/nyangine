#include "nyangine/nyangine.h"

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
    f32 angle      = (lateral + 1.0F) * 0.25F * (f32)M_PI;
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

    f32 target_gain[2]  = { target.left_gain, target.right_gain };
    f32 target_cut[2]   = { target.left_lowpass_hz, target.right_lowpass_hz };
    f32 target_delay[2] = { target.left_delay_s, target.right_delay_s };

    // one pole coefficient per ear, 1 open. a = 1 - e^(-2π·fc/fs).
    f32 target_coeff[2];
    u32 delay_frames[2];
    for (s32 ear = 0; ear < 2; ear++) {
        if (target_cut[ear] > 0.0F) {
            f32 a            = 1.0F - expf(-2.0F * (f32)M_PI * target_cut[ear] / sample_rate_hz);
            target_coeff[ear] = nya_clamp(a, 0.0F, 1.0F);
        } else {
            target_coeff[ear] = 1.0F;
        }

        f32 d = target_delay[ear] * sample_rate_hz;
        if (d < 0.0F) d = 0.0F;

        u32 di = (u32)(d + 0.5F);
        if (di > NYA_AUDIO_PAN_MAX_DELAY_FRAMES - 1) di = NYA_AUDIO_PAN_MAX_DELAY_FRAMES - 1;
        delay_frames[ear] = di;
    }

    // the first buffer starts on the target; later ones ease so a turning head glides rather than steps.
    if (!render->primed) {
        render->gain[0]        = target_gain[0];
        render->gain[1]        = target_gain[1];
        render->coefficient[0] = target_coeff[0];
        render->coefficient[1] = target_coeff[1];
        render->primed         = true;
    }

    f32 gain_step[2];
    f32 coeff_step[2];
    for (s32 ear = 0; ear < 2; ear++) {
        gain_step[ear]  = (target_gain[ear] - render->gain[ear]) / (f32)frames;
        coeff_step[ear] = (target_coeff[ear] - render->coefficient[ear]) / (f32)frames;
    }

    for (s32 frame = 0; frame < frames; frame++) {
        for (s32 ear = 0; ear < 2; ear++) {
            f32* sample = &pcm[(frame * channels) + ear];

            // write the raw input, then read the delayed frame off the same line.
            render->ring[ear][render->write_index] = *sample;

            u32 read    = (render->write_index + NYA_AUDIO_PAN_MAX_DELAY_FRAMES - delay_frames[ear]) % NYA_AUDIO_PAN_MAX_DELAY_FRAMES;
            f32 delayed = render->ring[ear][read];

            // head shadow: one pole toward the delayed signal. A coefficient of one passes it through.
            render->shadow_state[ear] += render->coefficient[ear] * (delayed - render->shadow_state[ear]);

            *sample = render->gain[ear] * render->shadow_state[ear];

            render->gain[ear]        += gain_step[ear];
            render->coefficient[ear] += coeff_step[ear];
        }

        render->write_index = (render->write_index + 1) % NYA_AUDIO_PAN_MAX_DELAY_FRAMES;
    }

    // land exactly on the target, since the per-frame sum drifts.
    render->gain[0]        = target_gain[0];
    render->gain[1]        = target_gain[1];
    render->coefficient[0] = target_coeff[0];
    render->coefficient[1] = target_coeff[1];

    // a decaying one pole reaches denormals; flush them once inaudible.
    for (s32 ear = 0; ear < 2; ear++) {
        if (fabsf(render->shadow_state[ear]) < 1e-20F) render->shadow_state[ear] = 0.0F;
    }
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
