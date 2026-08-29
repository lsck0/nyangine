#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The coefficients of one implicit integration step, shared by every spring below.
 *
 * Implicit rather than explicit or semi-implicit Euler, because only this one is *unconditionally*
 * stable. The others have a step limit around `omega * dt < 2`, and both an overdamped spring and a
 * stiff one cross it at an ordinary sixty hertz — a damping ratio of 3 at 4 Hz, or 20 Hz at any
 * damping, produce NaN within a few frames. Solving the step implicitly has no such limit: a huge
 * step lands on the target instead of diverging, which is the behaviour a stall should have anyway.
 *
 * This is the formulation Erin Catto uses for soft constraints, rearranged for one degree of freedom.
 */
typedef struct {
    f32 det_inv;
    f32 f;
    f32 h;
    f32 hoo;
    f32 hhoo;
} _NYA_SpringStep;

NYA_INTERNAL _NYA_SpringStep _nya_spring_step(f32 frequency, f32 damping, f32 delta_time_s) {
    if (frequency <= 0.0F) frequency = NYA_SPRING_DEFAULT_FREQUENCY;
    if (damping <= 0.0F) damping = 1.0F;

    // Clamped for the sake of what a stall *means* rather than for stability, which the step no longer
    // needs: a frame longer than this is a debugger pause, and snapping to the target is the honest answer.
    f32 h = nya_min(delta_time_s, NYA_SPRING_MAX_STEP);

    // M_PI, as math_complex.c does; the engine has no pi constant of its own. Angular frequency, so
    // `frequency` can be stated in oscillations per second.
    f32 omega = frequency * 2.0F * (f32)M_PI;

    f32 f    = 1.0F + (2.0F * h * damping * omega);
    f32 oo   = omega * omega;
    f32 hoo  = h * oo;
    f32 hhoo = h * hoo;

    return (_NYA_SpringStep){
        .det_inv = 1.0F / (f + hhoo),
        .f       = f,
        .h       = h,
        .hoo     = hoo,
        .hhoo    = hhoo,
    };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_spring_f32(NYA_SpringF32* spring, f32 target, f32 delta_time_s) {
    nya_assert(spring != nullptr);
    if (delta_time_s <= 0.0F) return spring->value;

    _NYA_SpringStep s = _nya_spring_step(spring->frequency, spring->damping, delta_time_s);

    f32 det_x = (s.f * spring->value) + (s.h * spring->velocity) + (s.hhoo * target);
    f32 det_v = spring->velocity + (s.hoo * (target - spring->value));

    spring->value    = det_x * s.det_inv;
    spring->velocity = det_v * s.det_inv;

    return spring->value;
}

f32x2 nya_spring_f32x2(NYA_SpringF32x2* spring, f32x2 target, f32 delta_time_s) {
    nya_assert(spring != nullptr);
    if (delta_time_s <= 0.0F) return spring->value;

    _NYA_SpringStep s = _nya_spring_step(spring->frequency, spring->damping, delta_time_s);

    f32x2 det_x = (spring->value * s.f) + (spring->velocity * s.h) + (target * s.hhoo);
    f32x2 det_v = spring->velocity + ((target - spring->value) * s.hoo);

    spring->value    = det_x * s.det_inv;
    spring->velocity = det_v * s.det_inv;

    return spring->value;
}

f32x3 nya_spring_f32x3(NYA_SpringF32x3* spring, f32x3 target, f32 delta_time_s) {
    nya_assert(spring != nullptr);
    if (delta_time_s <= 0.0F) return spring->value;

    _NYA_SpringStep s = _nya_spring_step(spring->frequency, spring->damping, delta_time_s);

    f32x3 det_x = (spring->value * s.f) + (spring->velocity * s.h) + (target * s.hhoo);
    f32x3 det_v = spring->velocity + ((target - spring->value) * s.hoo);

    spring->value    = det_x * s.det_inv;
    spring->velocity = det_v * s.det_inv;

    return spring->value;
}

void nya_spring_f32_reset(NYA_SpringF32* spring, f32 value) {
    nya_assert(spring != nullptr);
    spring->value    = value;
    spring->velocity = 0.0F;
}

void nya_spring_f32x2_reset(NYA_SpringF32x2* spring, f32x2 value) {
    nya_assert(spring != nullptr);
    spring->value    = value;
    spring->velocity = f32x2_zero;
}

void nya_spring_f32x3_reset(NYA_SpringF32x3* spring, f32x3 value) {
    nya_assert(spring != nullptr);
    spring->value    = value;
    spring->velocity = f32x3_zero;
}

b8 nya_spring_f32_settled(const NYA_SpringF32* spring, f32 target, f32 epsilon) {
    nya_assert(spring != nullptr);
    if (epsilon <= 0.0F) epsilon = 0.001F;

    // Both, not just position: a spring passing through its target at speed is not settled.
    return fabsf(target - spring->value) < epsilon && fabsf(spring->velocity) < epsilon;
}
