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

#include "nyangine-std/base/base_error.h"
#include "nyangine-std/math/math_matrix.h"
#include "nyangine-std/math/math_quaternion.h"
#include "nyangine-std/math/math_vector.h"

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

    /** Model space to bone space at bind time: the inverse bind matrix. */
    f32_4x4 inverse_bind;

    /** The bone's own transform in the rest pose, for nya_skeleton_pose_rest. */
    NYA_BoneTransform rest;
};

/** One animation, sampled onto a uniform grid at load. See NYA_ASSET_SKELETON_BAKE_RATE. */
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

    /** The clock before the last update, which a draw between ticks starts from. */
    f32 time_previous_s;

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

/**
 * Advances the clock and writes the pose. Does nothing to `out_pose` when there is no clip. A null `out_pose` only
 * advances the clock, for a pose drawn with nya_skeleton_animator_render_pose.
 * */
NYA_API void nya_skeleton_animator_update(NYA_SkeletonAnimator* animator, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose);

/**
 * The pose to draw this frame: the clip sampled between the clock's last tick and its current time, by
 * nya_app_tick_alpha, so a clip advanced per tick moves smoothly at any frame rate.
 *
 * ```c
 * void on_update(NYA_Window* window, f32 delta_time_s) { nya_skeleton_animator_update(&hero, delta_time_s, nullptr); }
 *
 * void on_render(NYA_Window* window) {
 *     NYA_SkeletonPose pose;
 *     nya_skeleton_animator_render_pose(&hero, &pose);
 *     nya_skeleton_palette(skeleton, &pose, palette);
 * }
 * ```
 * */
NYA_API void nya_skeleton_animator_render_pose(const NYA_SkeletonAnimator* animator, OUT NYA_SkeletonPose* out_pose);

/**
 * Every bone's model-space transform for `pose`. `out_model` holds NYA_SKELETON_MAX_BONES entries.
 *
 * Where each bone is, for sockets. A palette entry has the inverse bind folded in and is the identity
 * for a bone that has not moved, so it cannot place an accessory.
 *
 * ```c
 * // a hat on the head bone.
 * f32_4x4 model[NYA_SKELETON_MAX_BONES];
 * nya_skeleton_model_transforms(skeleton, &pose, model);
 *
 * f32_4x4 head = character_transform * model[nya_skeleton_bone_index(skeleton, "head")];
 * ```
 *
 * For one bone use nya_skeleton_bone_model, which walks only that bone's chain.
 * */
NYA_API void nya_skeleton_model_transforms(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, OUT f32_4x4* out_model);

/**
 * Composes `pose` down the hierarchy and folds in each bone's inverse bind, into `out_palette`, which holds
 * NYA_SKELETON_MAX_BONES entries. What nya_render3d_skinned_mesh takes.
 * */
NYA_API void nya_skeleton_palette(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, OUT f32_4x4* out_palette);

/**
 * One bone's model-space transform. False when the bone does not resolve, leaving `out_transform` alone.
 *
 * The socket primitive: draw an accessory through this matrix times the character's own transform.
 * Cheaper than nya_skeleton_model_transforms for a handful of bones.
 * */
NYA_API b8 nya_skeleton_bone_model(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, s32 bone,
                                   OUT f32_4x4* out_transform) __attr_no_discard;
