/**
 * @file core_skeleton_inertial.h
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
 * */
struct NYA_InertialChannel {
    f32x3 direction;

    /**
     * `x(t) = c[0]t⁵ + c[1]t⁴ + c[2]t³ + c[3]t² + c[4]t + c[5]`, solved so that x, x' and x'' are all
     * zero at `duration_s`.
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
 * */
NYA_API void nya_skeleton_inertializer_transition(NYA_SkeletonInertializer* inertializer, const NYA_SkeletonPose* target,
                                                  const NYA_SkeletonPose* target_previous, f32 duration_s);

/**
 * Adds the decaying offset to `pose` in place, and records it as history.
 * */
NYA_API void nya_skeleton_inertializer_update(NYA_SkeletonInertializer* inertializer, f32 delta_time_s, NYA_SkeletonPose* pose);

/** Whether an offset is still being decayed. */
NYA_API b8 nya_skeleton_inertializer_active(const NYA_SkeletonInertializer* inertializer) __attr_no_discard;
