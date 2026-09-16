/**
 * @file core_skeleton_layer.h
 *
 * ```c
 * // A player that owns the transition rather than snapping between clips.
 * nya_skeleton_player_init(&player, skeleton);
 * nya_skeleton_player_play(&player, run_clip, .looping = true, .fade_s = 0.2F);
 *
 * // An upper-body layer in slot 0, so aiming plays over whatever the legs are doing.
 * nya_skeleton_mask_from_bone(skeleton, "spine", &upper_body);
 * nya_skeleton_player_layer(&player, 0, aim_clip, &upper_body, 1.0F, true);
 *
 * nya_skeleton_player_update(&player, delta_time_s, &pose);
 *
 * // Then anything procedural, straight into the pose, before the palette is built.
 * nya_skeleton_ik_two_bone(skeleton, &pose, shoulder, elbow, hand, target, pole);
 * nya_skeleton_palette(skeleton, &pose, palette);
 * ```
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_skeleton.h"
// The player holds an NYA_SkeletonInertializer*, and a transition can be driven through it instead of
// through the crossfade below. See nya_skeleton_player_inertial.
#include "nyangine/core/core_skeleton_inertial.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Layers one player may stack over its base clip. */
#ifndef NYA_SKELETON_LAYERS
#define NYA_SKELETON_LAYERS 4
#endif

/** Events one clip may carry. */
#ifndef NYA_SKELETON_CLIP_EVENTS
#define NYA_SKELETON_CLIP_EVENTS 16
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SkeletonMask        NYA_SkeletonMask;
typedef struct NYA_SkeletonLayer       NYA_SkeletonLayer;
typedef struct NYA_SkeletonPlayer      NYA_SkeletonPlayer;
typedef struct NYA_SkeletonPlayOptions NYA_SkeletonPlayOptions;
typedef struct NYA_SkeletonEvent       NYA_SkeletonEvent;
typedef struct NYA_SkeletonSignal      NYA_SkeletonSignal;
typedef struct NYA_RootMotion          NYA_RootMotion;

/**
 * How far the root bone moved this update, in the frame the character was facing at the start of it.
 * */
struct NYA_RootMotion {
    f32x3          translation;
    NYA_Quaternion rotation;
};

/**
 * A weight per bone, in [0, 1]. Zero means the layer does not touch that bone at all.
 * */
struct NYA_SkeletonMask {
    f32 weights[NYA_SKELETON_MAX_BONES];
    u32 bone_count;
};

/** One clip playing over the base, through a mask. */
struct NYA_SkeletonLayer {
    NYA_SkeletonAnimator animator;

    /** Null means every bone at full weight, which is a layer that simply replaces the base. */
    const NYA_SkeletonMask* mask;

    /** Multiplies the mask. What a fade in or out of a whole layer moves. */
    f32 weight;

    b8 active;
};

/** What `nya_skeleton_player_play` accepts beyond the clip. Every zero is a usable default. */
struct NYA_SkeletonPlayOptions {
    b8 looping;

    /** Seconds to blend from whatever is playing. Zero cuts, which is right for a hit reaction. */
    f32 fade_s;

    /** Clock multiplier. Zero means one. Negative plays backwards, starting from the clip's end. */
    f32 speed;

    /** Restart from the beginning even if this clip is already playing. */
    b8 restart;
};

/** A frame marker on a clip, mirroring NYA_SpriteAnimationEvent so 2D and 3D read alike. */
struct NYA_SkeletonEvent {
    /** Seconds into the clip. Fired when playback crosses it. */
    f32 time_s;

    /** The game's own identifier, such as a footstep, a hit frame or a sound. */
    u32 id;
};

/** What a player reports happened this update. */
typedef enum NYA_SkeletonSignalKind {
    NYA_SKELETON_SIGNAL_EVENT = 0,
    NYA_SKELETON_SIGNAL_LOOPED,
    NYA_SKELETON_SIGNAL_FINISHED,

    NYA_SKELETON_SIGNAL_KIND_COUNT,
} NYA_SkeletonSignalKind;

struct NYA_SkeletonSignal {
    NYA_SkeletonSignalKind kind;

    /** The event's id, for NYA_SKELETON_SIGNAL_EVENT. Zero otherwise. */
    u32 id;

    /** Which clip it came from, so a caller need not guess during a crossfade. */
    const NYA_SkeletonClip* clip;
};

struct NYA_SkeletonPlayer {
    const NYA_Skeleton* skeleton;

    /** What is playing now, and what it is fading from. */
    NYA_SkeletonAnimator current;
    NYA_SkeletonAnimator previous;

    f32 fade_elapsed_s;
    f32 fade_duration_s;
    b8  fading;

    /** Set by `play` until the first update, which is the one step whose start point counts for events. */
    b8 current_fresh;

    /*
     * ── Inertialization, when one is attached ──
     *
     * A pointer to something the caller owns rather than a member: it is about fourteen kilobytes,
     * and a player that crossfades should not pay for it. Null means crossfade, which is the default
     * and what every existing caller keeps getting.
     */
    NYA_SkeletonInertializer* inertializer;

    /**
     * Set by `play` and consumed by the next `update`, which is where the transition is captured.
     * */
    f32 pending_inertial_s;

    NYA_SkeletonLayer layers[NYA_SKELETON_LAYERS];

    /*
     * Events are attached to the player rather than to the clip.
     */
    const NYA_SkeletonEvent* events;
    u32                      event_count;

    /** What the last update produced. Valid until the next one. */
    NYA_SkeletonSignal signals[NYA_SKELETON_CLIP_EVENTS];
    u32                signal_count;

    /*
     * ── Root motion ──
     *
     * Off until nya_skeleton_player_root_motion names a bone, because extracting it from a rig that
     * animates in place removes movement that was never there and pins a bone that wanted to move.
     */

    /** Which bone carries the movement, or -1 for off. */
    s32 root_motion_bone;

    /**
     * Which translation axes to take, one per component, normally { 1, 0, 1 }.
     * */
    f32x3 root_motion_axes;

    /** Whether to take the root's rotation too. What turn-in-place animations need. */
    b8 root_motion_rotation;

    /** What the last update extracted. Zero when nothing is playing. */
    NYA_RootMotion root_motion;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A mask with every bone at `weight`. The starting point for carving one out. */
NYA_API void nya_skeleton_mask_fill(const NYA_Skeleton* skeleton, f32 weight, OUT NYA_SkeletonMask* out_mask);

/**
 * A mask covering `root` and everything below it, at full weight, and nothing else.
 * */
NYA_API b8 nya_skeleton_mask_from_bone(const NYA_Skeleton* skeleton, NYA_ConstCString root, OUT NYA_SkeletonMask* out_mask);

/** Sets one bone's weight, and optionally everything below it. */
NYA_API void nya_skeleton_mask_set(const NYA_Skeleton* skeleton, NYA_SkeletonMask* mask, s32 bone, f32 weight, b8 include_descendants);

/**
 * Blends `to` over `from` per bone, scaled by `mask`. A null mask is every bone at full weight.
 * */
NYA_API void nya_skeleton_pose_blend_masked(const NYA_SkeletonPose* from, const NYA_SkeletonPose* to, f32 amount,
                                            const NYA_SkeletonMask* mask, OUT NYA_SkeletonPose* out_pose);

/** Points the player at a skeleton and clears it. Safe to call again to reset. */
NYA_API void nya_skeleton_player_init(NYA_SkeletonPlayer* player, const NYA_Skeleton* skeleton);

/**
 * Plays `clip`, crossfading from whatever was playing.
 * */
NYA_API void nya_skeleton_player_play_with_options(NYA_SkeletonPlayer* player, const NYA_SkeletonClip* clip,
                                                   NYA_SkeletonPlayOptions options);

#define nya_skeleton_player_play(player, clip, ...)                                                                                                  \
    nya_skeleton_player_play_with_options(player, clip, (NYA_SkeletonPlayOptions){ __VA_ARGS__ })

/** Attaches the event list a player fires from. Not copied: it must outlive the player. */
NYA_API void nya_skeleton_player_events(NYA_SkeletonPlayer* player, const NYA_SkeletonEvent* events, u32 count);

/** Starts a layer in `slot`, over the base. A null mask means it replaces every bone. */
NYA_API b8 nya_skeleton_player_layer(NYA_SkeletonPlayer* player, u32 slot, const NYA_SkeletonClip* clip,
                                     const NYA_SkeletonMask* mask, f32 weight, b8 looping);

/** Stops a layer. Its bones return to whatever the base pose says. */
NYA_API void nya_skeleton_player_layer_stop(NYA_SkeletonPlayer* player, u32 slot);

/** Sets a running layer's weight, for fading one in or out by hand. */
NYA_API void nya_skeleton_player_layer_weight(NYA_SkeletonPlayer* player, u32 slot, f32 weight);

/**
 * Turns root motion on for `bone`, or off when `bone` is null.
 * */
NYA_API b8 nya_skeleton_player_root_motion(NYA_SkeletonPlayer* player, NYA_ConstCString bone, f32x3 translation_axes, b8 rotation);

/**
 * What the last update extracted. Zero when root motion is off or nothing is playing.
 * */
NYA_API NYA_RootMotion nya_skeleton_player_root_delta(const NYA_SkeletonPlayer* player) __attr_no_discard;

/** Advances everything and writes the composed pose. Signals are readable afterwards. */
NYA_API void nya_skeleton_player_update(NYA_SkeletonPlayer* player, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose);

/**
 * Transitions through `inertializer` instead of crossfading. Null restores the crossfade.
 * */
NYA_API void nya_skeleton_player_inertial(NYA_SkeletonPlayer* player, NYA_SkeletonInertializer* inertializer);

/** Whether a transition is in progress, crossfade or inertialization. */
NYA_API b8 nya_skeleton_player_fading(const NYA_SkeletonPlayer* player) __attr_no_discard;

/**
 * Bends a two-bone chain so `end` reaches `target`, writing rotations into `pose`.
 * */
NYA_API b8 nya_skeleton_ik_two_bone(const NYA_Skeleton* skeleton, NYA_SkeletonPose* pose, s32 root_bone, s32 mid_bone, s32 end_bone,
                                    f32x3 target, f32x3 pole);
