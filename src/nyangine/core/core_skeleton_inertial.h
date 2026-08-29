/**
 * @file core_skeleton_inertial.h
 *
 * Inertialization: transitioning between animations by decaying the *difference*, so only one clip is
 * ever evaluated.
 *
 * ```c
 * // Sample whatever should be playing now. One clip, always — including mid-transition.
 * nya_skeleton_pose_sample(skeleton, clip, time_s, &pose);
 *
 * if (clip_changed) {
 *     nya_skeleton_pose_sample(skeleton, clip, time_s - delta_time_s, &previous);
 *     nya_skeleton_inertializer_transition(&inertializer, &pose, &previous, 0.2F);
 * }
 *
 * nya_skeleton_inertializer_update(&inertializer, delta_time_s, &pose);
 * ```
 *
 * **What it is instead of.** A crossfade evaluates both clips for the length of the transition and
 * mixes them. Inertialization records the pose difference at the moment of the switch — position *and*
 * velocity — and drives that difference to zero over the transition while playing only the destination.
 * The character leaves the old pose along the trajectory it was already on and arrives at the new one
 * without a seam. See core_skeleton_layer.h, whose crossfade this does not replace: a crossfade is
 * still what you want when the two clips are genuinely both happening.
 *
 * **Why it is worth the machinery.** The cost of a crossfade is one extra clip evaluation *per frame of
 * the transition*; this is one extra evaluation *on the transition frame only*. With a state machine
 * firing several transitions a second — which is what a locomotion system does — that is the difference
 * between routinely evaluating two clips and routinely evaluating one. The blend is also better: a
 * crossfade interpolates between two poses that disagree about where a limb is going, so a limb can
 * visibly slow down and speed up across the transition. This one matches velocity at the seam by
 * construction.
 *
 * **A quintic, not a spring.** The offset follows the fifth-order polynomial from David Bollo's
 * *Inertialization: High-Performance Animation Transitions in Gears of War* (GDC 2016), which is
 * pinned to reach exactly zero with zero velocity and zero acceleration at the end of the transition.
 * A spring — which this engine also has, in math_spring.h — approaches zero asymptotically and never
 * arrives, so there is no moment at which the transition is over and the offset can stop being
 * computed. Here there is, and after it the whole thing costs nothing.
 *
 * **Per bone, per channel, along a fixed direction.** Each bone's translation, rotation and scale
 * offset is reduced to a direction and a scalar length at the moment of transition; only the length
 * decays. That is what keeps a rotation offset a rotation the whole way down rather than something
 * that has to be renormalised, and it is why the curve is one-dimensional.
 *
 * ⚠ **It transitions from what it last produced.** The inertializer keeps the two poses it wrote last,
 * because the velocity it has to match is the one that was on screen — including any offset still
 * decaying from an earlier transition. That is what makes back-to-back transitions compose instead of
 * fighting. It also means every frame must go through `nya_skeleton_inertializer_update`, even the ones
 * with no transition in them.
 *
 * ⚠ **Roughly 14 KB.** Put one wherever the character lives, not on the stack.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_skeleton.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_InertialChannel      NYA_InertialChannel;
typedef struct NYA_SkeletonInertializer NYA_SkeletonInertializer;

/**
 * One offset on its way to zero: a fixed direction, and the polynomial that walks its length there.
 *
 * `direction` is a unit vector for translation and scale, and a rotation axis for rotation. It is
 * captured once and never moves, so the whole decay is the single scalar `x(t)` below.
 * */
struct NYA_InertialChannel {
    f32x3 direction;

    /**
     * `x(t) = c[0]t⁵ + c[1]t⁴ + c[2]t³ + c[3]t² + c[4]t + c[5]`, solved so that x, x' and x'' are all
     * zero at `duration_s`.
     *
     * Stored as coefficients rather than as the (x₀, v₀, a₀) it was derived from, because evaluating
     * it is Horner and re-deriving the coefficients would be three divides per bone per channel per
     * frame to answer the same question.
     * */
    f32 coefficients[6];

    /** When x reaches zero. Past this the channel contributes nothing and is skipped. */
    f32 duration_s;
};

struct NYA_SkeletonInertializer {
    const NYA_Skeleton* skeleton;

    NYA_InertialChannel translation[NYA_SKELETON_MAX_BONES];
    NYA_InertialChannel rotation[NYA_SKELETON_MAX_BONES];
    NYA_InertialChannel scale[NYA_SKELETON_MAX_BONES];

    /** How far into the current transition. Compared against each channel's own `duration_s`. */
    f32 elapsed_s;

    /** The longest of the channels' durations, so `active` is one compare rather than a scan. */
    f32 longest_s;

    /*
     * ── What was on screen, for the velocity a transition has to match ──
     *
     * Two poses, because velocity is a difference: the pose written last frame and the one before it.
     * Recorded by nya_skeleton_inertializer_update, which is why it has to run on every frame rather
     * than only during a transition.
     */
    NYA_SkeletonPose previous;
    NYA_SkeletonPose before_previous;
    f32              previous_delta_s;
    u32              history_frames;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Points it at a skeleton and clears everything, history included. Safe to call again to reset. */
NYA_API void nya_skeleton_inertializer_init(NYA_SkeletonInertializer* inertializer, const NYA_Skeleton* skeleton);

/**
 * Captures the offset between what was last on screen and `target`, to be decayed over `duration_s`.
 *
 * `target_previous` is the destination sampled one frame earlier — the same clip at `time - delta`.
 * It is what gives the destination's own velocity, and the offset's velocity is the difference between
 * the two. Pass null and the destination is treated as static, which overshoots slightly when both
 * clips are moving a limb the same way; it is the right shortcut for transitioning into a held pose
 * and the wrong one for transitioning between two walks.
 *
 * Calling this while a transition is still running is normal and correct: the offset is re-captured
 * from the pose that is *currently* being shown, which already contains whatever is left of the
 * previous one.
 * */
NYA_API void nya_skeleton_inertializer_transition(NYA_SkeletonInertializer* inertializer, const NYA_SkeletonPose* target,
                                                  const NYA_SkeletonPose* target_previous, f32 duration_s);

/**
 * Adds the decaying offset to `pose` in place, and records it as history.
 *
 * Call it every frame with the destination pose, transition or no transition. With nothing decaying it
 * is a copy into the history buffers and nothing else.
 * */
NYA_API void nya_skeleton_inertializer_update(NYA_SkeletonInertializer* inertializer, f32 delta_time_s, NYA_SkeletonPose* pose);

/** Whether an offset is still being decayed. */
NYA_API b8 nya_skeleton_inertializer_active(const NYA_SkeletonInertializer* inertializer) __attr_no_discard;
