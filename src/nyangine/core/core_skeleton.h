/**
 * @file core_skeleton.h
 *
 * ```c
 * nya_skeleton_animator_update(&animator, delta_time_s, &pose);   // the clip's opinion
 * pose.local[head].rotation = look_at_rotation;                   // yours, layered on top
 * nya_skeleton_palette(skeleton, &pose, palette);                 // whatever it now says
 * ```
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/math/math_matrix.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Most bones one skeleton may have.
 * */
#define NYA_SKELETON_MAX_BONES 64

/**
 * Influences kept per vertex, largest first.
 * */
#define NYA_SKELETON_WEIGHTS_PER_VERTEX 4

/** Bytes of name kept per bone and per clip. Long enough for exporter names, short enough to inline. */
#define NYA_SKELETON_NAME_MAX 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_BoneTransform  NYA_BoneTransform;
typedef struct NYA_SkeletonBone   NYA_SkeletonBone;
typedef struct NYA_SkeletonClip   NYA_SkeletonClip;
typedef struct NYA_Skeleton       NYA_Skeleton;
typedef struct NYA_SkeletonPose   NYA_SkeletonPose;
typedef struct NYA_SkeletonAnimator NYA_SkeletonAnimator;

/**
 * One bone's transform relative to its parent.
 * */
struct NYA_BoneTransform {
    f32x3          translation;
    NYA_Quaternion rotation;
    f32x3          scale;
};

struct NYA_SkeletonBone {
    char name[NYA_SKELETON_NAME_MAX];

    /**
     * Index of this bone's parent, or -1 for a root.
     * */
    s32 parent;

    /**
     * Model space to bone space at bind time — the inverse bind matrix.
     * */
    f32_4x4 inverse_bind;

    /** The bone's own transform in the rest pose, for nya_skeleton_pose_rest. */
    NYA_BoneTransform rest;
};

/** One animation, sampled onto a fixed grid. See the note on baking at the top of this file. */
struct NYA_SkeletonClip {
    char name[NYA_SKELETON_NAME_MAX];

    f32 duration_s;

    u32 frame_count;

    /** Frames per second the bake used. Playback interpolates, so this need not match the display rate. */
    f32 frame_rate;

    /**
     * `frame_count * bone_count` transforms, frame major.
     * */
    NYA_BoneTransform* frames;
};

struct NYA_Skeleton {
    NYA_SkeletonBone* bones;
    u32               bone_count;

    NYA_SkeletonClip* clips;
    u32               clip_count;
};

/**
 * A skeleton's current local transforms. Plain data, and writable on purpose.
 * */
struct NYA_SkeletonPose {
    NYA_BoneTransform local[NYA_SKELETON_MAX_BONES];

    u32 bone_count;
};

/** Playback state for one clip. Shaped like NYA_SpriteAnimator, so 2D and 3D animation read alike. */
struct NYA_SkeletonAnimator {
    const NYA_Skeleton*     skeleton;
    const NYA_SkeletonClip* clip;

    f32 time_s;

    /** Multiplies the clock. Negative plays backward, which loops correctly. */
    f32 speed;

    b8 playing;
    b8 looping;

    /** Set when a non-looping clip reaches its end. Cleared by nya_skeleton_animator_play. */
    b8 finished;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The clip called `name`, or null. */
NYA_API const NYA_SkeletonClip* nya_skeleton_clip(const NYA_Skeleton* skeleton, NYA_ConstCString name) __attr_no_discard;

/** The bone called `name`, or -1. For a game that wants to drive one by hand. */
NYA_API s32 nya_skeleton_bone_index(const NYA_Skeleton* skeleton, NYA_ConstCString name) __attr_no_discard;

/** Fills `out_pose` with the skeleton's rest transforms. What to start from before layering anything on. */
NYA_API void nya_skeleton_pose_rest(const NYA_Skeleton* skeleton, OUT NYA_SkeletonPose* out_pose);

/**
 * Samples `clip` at `time_s` into `out_pose`, interpolating between baked frames.
 * */
NYA_API void nya_skeleton_pose_sample(const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip, f32 time_s,
                                      OUT NYA_SkeletonPose* out_pose);

/**
 * One bone's transform in `clip` at `time_s`, without touching the other sixty three.
 * */
NYA_API NYA_BoneTransform nya_skeleton_clip_bone(const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip, s32 bone,
                                                 f32 time_s) __attr_no_discard;

/**
 * Blends `from` toward `to` by `amount`, into `out_pose`.
 * */
NYA_API void nya_skeleton_pose_blend(const NYA_SkeletonPose* from, const NYA_SkeletonPose* to, f32 amount,
                                     OUT NYA_SkeletonPose* out_pose);

/** Starts a clip. Resets the clock, clears `finished`. */
NYA_API void nya_skeleton_animator_play(NYA_SkeletonAnimator* animator, const NYA_Skeleton* skeleton,
                                        const NYA_SkeletonClip* clip, b8 looping);

/** Advances the clock and writes the pose. Does nothing to `out_pose` when there is no clip. */
NYA_API void nya_skeleton_animator_update(NYA_SkeletonAnimator* animator, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose);

/**
 * Composes `pose` down the hierarchy and folds in each bone's inverse bind, into `out_palette`.
 * */
NYA_API void nya_skeleton_palette(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, OUT f32_4x4* out_palette);
