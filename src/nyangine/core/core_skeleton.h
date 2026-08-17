/**
 * @file core_skeleton.h
 *
 * Skeletons, baked animation clips, and the pose that drives skinning.
 *
 * ## The one idea
 *
 * A **pose is just an array of local bone transforms**, and nothing here cares where it came from.
 * Sampling a clip writes one. So does a game writing bone rotations itself. So would a ragdoll
 * reading rigid body orientations back out of the physics solver.
 *
 * That is deliberate and it is the whole design. Procedural animation and ragdoll are not features
 * that have to be added later — they are what you get for free by making the pose a plain writable
 * buffer instead of something a clip privately owns:
 *
 * ```c
 * nya_skeleton_animator_update(&animator, delta_time_s, &pose);   // the clip's opinion
 * pose.local[head].rotation = look_at_rotation;                   // yours, layered on top
 * nya_skeleton_palette(skeleton, &pose, palette);                 // whatever it now says
 * ```
 *
 * ## Clips are baked at load, not evaluated
 *
 * A clip is sampled onto a fixed grid when the model loads and the FBX scene is then thrown away.
 * Playing it is a lerp between two frames.
 *
 * The alternative — keeping the parsed scene and calling into the importer every frame — means the
 * importer's memory and its curve evaluation live in the frame budget forever, in exchange for
 * accuracy nothing in this art style can see. Baking is why nothing in this header mentions FBX.
 *
 * ## Linear blend skinning, four weights
 *
 * The usual approximation, and enough here: the test rig peaks at two influences per vertex, and a
 * low-poly character has no fine deformation for dual quaternion to preserve. What linear blend gets
 * wrong is the volume of a limb twisted along its own axis, which is not a shape this art style has.
 *
 * Weights are normalised at load, which is not optional — the exporter's own weights on the test rig
 * sum to as little as 0.982, and a vertex weighted to 0.982 sits 1.8% of the way toward the origin.
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
 *
 * The cap is the shader's, not the loader's: the palette is a vertex uniform and every bone costs
 * forty eight bytes in it whether or not the mesh uses it. Sixty four is three kibibytes, which is
 * comfortable, and is more bones than a low-poly character has any use for — a humanoid game rig is
 * usually in the twenties.
 * */
#define NYA_SKELETON_MAX_BONES 64

/**
 * Influences kept per vertex, largest first.
 *
 * Four is the standard cap and it is generous here. The importer sorts weights by descending
 * influence, so dropping the tail is dropping the least important — and the test rig never exceeds
 * two, so nothing is being dropped at all yet.
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
 *
 * Kept as translation, rotation and scale rather than as a matrix because that is what can be
 * *interpolated*: blending two matrices component-wise shears them, while blending a quaternion is
 * a rotation the whole way through. The matrix is built once at the end, in nya_skeleton_palette.
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
     *
     * Bones are stored so that a parent always comes before its children, which is what lets
     * nya_skeleton_palette compose the hierarchy in one forward pass instead of recursing.
     * */
    s32 parent;

    /**
     * Model space to bone space at bind time — the inverse bind matrix.
     *
     * This is what makes skinning work: a vertex is authored in model space, this takes it into the
     * bone's space, and the bone's animated world transform takes it back out to wherever the bone
     * has moved to. Straight from the importer's cluster; it is not derived from the rest pose.
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
     *
     * Frame major because sampling reads *every bone of two adjacent frames*, so this is the layout
     * where those two reads are each one contiguous run.
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
 *
 * See the header note: this being an ordinary array is what makes procedural animation and ragdoll
 * possible without either being a feature. Anything that can decide where a bone should be can write
 * here, and nya_skeleton_palette does not ask who did.
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
 *
 * Rotations blend as quaternions and the rest linearly. Time outside the clip clamps rather than
 * wrapping — looping is the animator's decision, not the sampler's, so that a game driving the clock
 * itself gets exactly the frame it asked for.
 * */
NYA_API void nya_skeleton_pose_sample(const NYA_Skeleton* skeleton, const NYA_SkeletonClip* clip, f32 time_s,
                                      OUT NYA_SkeletonPose* out_pose);

/**
 * Blends `from` toward `to` by `amount`, into `out_pose`.
 *
 * The whole of transitioning between animations, and the piece procedural work leans on hardest: a
 * hand-authored pose blended over a walk cycle at 0.3 is a character that is mostly walking.
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
 *
 * The result is what a skinning shader multiplies a vertex by: model space in, model space out, with
 * the bone's movement applied in between. `out_palette` must hold at least the skeleton's bone count.
 *
 * One forward pass, no recursion, because bones are ordered parents first. See NYA_SkeletonBone.parent.
 * */
NYA_API void nya_skeleton_palette(const NYA_Skeleton* skeleton, const NYA_SkeletonPose* pose, OUT f32_4x4* out_palette);
