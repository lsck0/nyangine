/**
 * @file core_skeleton_layer.h
 *
 * What makes a 3D character stop looking terrible: crossfades, layers with bone masks, animation
 * events, and two-bone IK.
 *
 * ```c
 * // A player that owns the transition rather than snapping between clips.
 * nya_skeleton_player_play(&player, skeleton, run_clip, .looping = true, .fade_s = 0.2F);
 *
 * // An upper-body layer, so aiming plays over whatever the legs are doing.
 * nya_skeleton_mask_from_bone(skeleton, "spine", &upper_body);
 * nya_skeleton_player_layer(&player, aim_clip, &upper_body, 1.0F);
 *
 * nya_skeleton_player_update(&player, delta_time_s, &pose);
 *
 * // Then anything procedural, straight into the pose, before the palette is built.
 * nya_skeleton_ik_two_bone(skeleton, &pose, shoulder, elbow, hand, target, pole);
 * nya_skeleton_palette(skeleton, &pose, palette);
 * ```
 *
 * **Everything here writes into `NYA_SkeletonPose`, which is a plain array of local bone transforms.**
 * That is core_skeleton.h's one design idea and this is what it was for: a layer, a mask and an IK
 * solver are all "write some bones after the clip did", and none of them needed the clip system to know
 * they existed.
 *
 * **Crossfade, not a state machine.** A player holds an outgoing clip and an incoming one and blends
 * between them over a fade. Two clips are evaluated during a transition and one the rest of the time,
 * which is the honest cost. `nya_skeleton_player_inertial` swaps that for the technique that evaluates
 * one throughout — see core_skeleton_inertial.h — which is what a game with several transitions a
 * second wants. Both are here because they are not the same thing: a crossfade is still right when the
 * two clips are genuinely both happening.
 *
 * **A mask is a weight per bone, not a set.** Partial weights are what let a layer taper out along the
 * spine instead of ending at one joint, and a hard 0/1 mask makes the seam between an aiming torso and
 * a running pelvis visible as a crease.
 *
 * **Root motion is extracted at playback, not baked.** Clips are already sampled onto a fixed grid, so
 * the distance a clip moves its root over one step is two reads and a subtraction — no importer change,
 * and it stays a per-character decision rather than a property of the asset. See
 * nya_skeleton_player_root_motion.
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
 * How far the root bone moved this update, in the character's own space.
 *
 * What the game applies to whatever actually owns the character's position — a physics body, an
 * entity transform, a controller. The animation decides how far a step goes; the game decides
 * whether the wall in front stops it. That division is the whole point of extracting it.
 * */
struct NYA_RootMotion {
    f32x3          translation;
    NYA_Quaternion rotation;
};

/**
 * A weight per bone, in [0, 1]. Zero means the layer does not touch that bone at all.
 *
 * Weights rather than a bitmask so a layer can *taper*: an aiming layer at full strength on the chest
 * and half on the waist bends the whole torso, where a hard cutoff creases at one joint.
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

    /** Clock multiplier. Zero means one. Negative plays backwards. */
    f32 speed;

    /** Restart from the beginning even if this clip is already playing. */
    b8 restart;
};

/** A frame marker on a clip, mirroring NYA_SpriteAnimationEvent so 2D and 3D read alike. */
struct NYA_SkeletonEvent {
    /** Seconds into the clip. Fired when playback crosses it. */
    f32 time_s;

    /** The game's own identifier — a footstep, a hit frame, a sound. */
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
     *
     * Deferred because capturing needs the destination's velocity, and that needs a frame delta —
     * which `play` does not have and `update` does. Nothing is lost by waiting: the transition begins
     * on the frame the new clip first appears either way.
     * */
    f32 pending_inertial_s;

    NYA_SkeletonLayer layers[NYA_SKELETON_LAYERS];

    /*
     * Events are attached to the player rather than to the clip.
     *
     * A clip is baked asset data shared by every character playing it, and its event list is a game's
     * annotation — two characters can want different markers on the same walk. Keeping them here also
     * means adding events needs no change to the asset pipeline.
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
     *
     * Per axis rather than all or nothing because vertical is usually not movement: a walk cycle's
     * root bobs up and down, and handing that bob to the game as displacement makes the character
     * hop. A jump clip is the case that wants { 1, 1, 1 }.
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
 *
 * The usual way to build an upper-body mask: name the spine, get the spine and every arm, hand and head
 * hanging off it. Returns false when the bone does not exist, rather than producing an empty mask that
 * would silently make the layer do nothing.
 * */
NYA_API b8 nya_skeleton_mask_from_bone(const NYA_Skeleton* skeleton, NYA_ConstCString root, OUT NYA_SkeletonMask* out_mask);

/** Sets one bone's weight, and optionally everything below it. */
NYA_API void nya_skeleton_mask_set(const NYA_Skeleton* skeleton, NYA_SkeletonMask* mask, s32 bone, f32 weight, b8 include_descendants);

/**
 * Blends `to` over `from` per bone, scaled by `mask`. A null mask is every bone at full weight.
 *
 * The masked counterpart of nya_skeleton_pose_blend, which mixes every bone by one scalar and so cannot
 * express a layer.
 * */
NYA_API void nya_skeleton_pose_blend_masked(const NYA_SkeletonPose* from, const NYA_SkeletonPose* to, f32 amount,
                                            const NYA_SkeletonMask* mask, OUT NYA_SkeletonPose* out_pose);

/** Points the player at a skeleton and clears it. Safe to call again to reset. */
NYA_API void nya_skeleton_player_init(NYA_SkeletonPlayer* player, const NYA_Skeleton* skeleton);

/**
 * Plays `clip`, crossfading from whatever was playing.
 *
 * Playing the clip that is already current is a no-op unless `.restart` is set, so a caller can drive
 * this from a state check every frame without restarting the animation every frame.
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
 *
 * From here on each update reports how far that bone moved and then pins it, so the character
 * animates in place and the *game* is what moves it. Both halves matter: reporting without pinning
 * moves it twice, and pinning without reporting leaves it running on the spot.
 *
 * `translation_axes` is a per component multiplier — { 1, 0, 1 } is the usual one, taking the
 * horizontal travel and leaving the vertical bob in the pose. `rotation` takes the root's turn as
 * well, which turn-in-place clips need and strafing clips do not.
 *
 * Returns false when the bone does not exist, rather than silently leaving it off.
 *
 * Pinning restores the bone's *rest* transform on the extracted axes. Not the clip's first frame:
 * during a crossfade there are two first frames and no reason to prefer either, and the rest pose is
 * the one thing both clips are expressed relative to.
 * */
NYA_API b8 nya_skeleton_player_root_motion(NYA_SkeletonPlayer* player, NYA_ConstCString bone, f32x3 translation_axes, b8 rotation);

/**
 * What the last update extracted. Zero when root motion is off or nothing is playing.
 *
 * Blended across a crossfade by the same curve as the pose, so a character transitioning from walk
 * to run accelerates rather than stepping between two speeds.
 * */
NYA_API NYA_RootMotion nya_skeleton_player_root_delta(const NYA_SkeletonPlayer* player) __attr_no_discard;

/** Advances everything and writes the composed pose. Signals are readable afterwards. */
NYA_API void nya_skeleton_player_update(NYA_SkeletonPlayer* player, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose);

/**
 * Transitions through `inertializer` instead of crossfading. Null restores the crossfade.
 *
 * `.fade_s` keeps meaning the same thing — how long the transition takes — but the mechanism changes:
 * the outgoing clip stops being evaluated the moment the new one starts, and the difference between
 * them is decayed instead. See core_skeleton_inertial.h for why that is usually the better trade.
 *
 * The inertializer is not copied and must outlive the player. Attaching one mid-transition is fine;
 * the crossfade in flight finishes first, because it is still what is producing the pose.
 * */
NYA_API void nya_skeleton_player_inertial(NYA_SkeletonPlayer* player, NYA_SkeletonInertializer* inertializer);

/** Whether a transition is in progress, crossfade or inertialization. */
NYA_API b8 nya_skeleton_player_fading(const NYA_SkeletonPlayer* player) __attr_no_discard;

/**
 * Bends a two-bone chain so `end` reaches `target`, writing rotations into `pose`.
 *
 * Analytic rather than iterative: two bones and a target is a triangle, and the law of cosines gives the
 * answer exactly, in constant time, with no convergence to tune. FABRIK is for chains longer than this.
 *
 * `pole` decides which way the joint bends — the direction the elbow or knee should point. Without one a
 * solution exists but the limb is free to spin around the line from shoulder to hand, and it will.
 *
 * Returns false when the bones are not a chain or the pose is not for this skeleton. An unreachable
 * target is *not* a failure: the limb straightens toward it, which is what a real arm does.
 * */
NYA_API b8 nya_skeleton_ik_two_bone(const NYA_Skeleton* skeleton, NYA_SkeletonPose* pose, s32 root_bone, s32 mid_bone, s32 end_bone,
                                    f32x3 target, f32x3 pole);
