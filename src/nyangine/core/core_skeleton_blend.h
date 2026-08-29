/**
 * @file core_skeleton_blend.h
 *
 * Blend trees: many clips mixed by continuous parameters, instead of one clip chosen by a state.
 *
 * ```c
 * nya_blend_tree_init(&tree, skeleton);
 *
 * s32 idle = nya_blend_tree_clip(&tree, nya_skeleton_clip(skeleton, "idle"));
 * s32 walk = nya_blend_tree_clip(&tree, nya_skeleton_clip(skeleton, "walk"));
 * s32 run  = nya_blend_tree_clip(&tree, nya_skeleton_clip(skeleton, "run"));
 *
 * s32 locomotion = nya_blend_tree_1d(&tree, NYA_BLEND_SPEED);
 * nya_blend_tree_child(&tree, locomotion, idle, (f32x2){ 0.0F, 0.0F });
 * nya_blend_tree_child(&tree, locomotion, walk, (f32x2){ 1.5F, 0.0F });
 * nya_blend_tree_child(&tree, locomotion, run,  (f32x2){ 5.0F, 0.0F });
 * nya_blend_tree_root(&tree, locomotion);
 *
 * nya_blend_tree_parameter(&tree, NYA_BLEND_SPEED, character_speed);
 * nya_blend_tree_update(&tree, delta_time_s, &pose);
 * ```
 *
 * **Why this instead of a crossfade.** A crossfade answers "I was doing that, now I am doing this".
 * A blend tree answers "I am doing this *much* of it" — a character moving at 2.3 m/s is not walking
 * or running, it is 82% walk and 18% run, and it has to still be that on the next frame when the
 * number moves slightly. Fading between the two every time the speed crosses a threshold produces a
 * character that lurches at exactly the speed it spends most of its time near.
 *
 * **One clock for the whole tree, and that is the point.** Every clip is sampled at the same
 * normalised phase, and the phase advances at the rate of the weighted average duration. That is what
 * keeps the feet together: a walk is 1.2 s and a run 0.8 s, and running them off their own clocks
 * puts them out of step within a second, at which point the blend is one foot averaged against the
 * other and the character skates. The cost of the rule is that a tree is for clips that *are* phases
 * of the same motion. A reload animation does not belong in one; that is a layer.
 *
 * **Weights.** 1D interpolates between the two children bracketing the parameter, so exactly two clips
 * are ever sampled. 2D uses gradient band interpolation, which is the standard answer and behaves
 * where inverse distance weighting does not: it gives a sample full weight when the parameter sits on
 * it, falls to zero at its neighbours, and does not care that the samples are unevenly spread.
 *
 * Fixed size and allocation free, like everything else here. A tree is a value; put one in whatever
 * owns the character.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_skeleton.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Nodes one tree may hold, clips and blend nodes together. */
#ifndef NYA_BLEND_MAX_NODES
#define NYA_BLEND_MAX_NODES 32
#endif

/** Children one blend node may mix. */
#ifndef NYA_BLEND_MAX_CHILDREN
#define NYA_BLEND_MAX_CHILDREN 8
#endif

/** Parameters one tree may be driven by. Speed, direction, lean, whatever the game means. */
#ifndef NYA_BLEND_MAX_PARAMETERS
#define NYA_BLEND_MAX_PARAMETERS 8
#endif

/**
 * How deep a tree may nest.
 *
 * A bound rather than a guess: evaluation recurses, and a tree built with a cycle in it would
 * otherwise recurse until the stack runs out. Four is already a 2D blend of 1D blends of clips.
 * */
#ifndef NYA_BLEND_MAX_DEPTH
#define NYA_BLEND_MAX_DEPTH 4
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_BlendNodeKind NYA_BlendNodeKind;
typedef struct NYA_BlendNode   NYA_BlendNode;
typedef struct NYA_BlendTree   NYA_BlendTree;

enum NYA_BlendNodeKind {
    /** A clip. The leaves, and the only nodes that produce a pose. */
    NYA_BLEND_NODE_CLIP = 0,

    /** Children on a line, mixed by one parameter. Idle to walk to run. */
    NYA_BLEND_NODE_1D,

    /** Children on a plane, mixed by two. Forward and strafe, or speed against turn. */
    NYA_BLEND_NODE_2D,

    NYA_BLEND_NODE_KIND_COUNT,
};

struct NYA_BlendNode {
    NYA_BlendNodeKind kind;

    /** NYA_BLEND_NODE_CLIP only. */
    const NYA_SkeletonClip* clip;

    /** Which parameters drive this node. 1D reads only `parameter_x`. */
    u8 parameter_x;
    u8 parameter_y;

    u8 children[NYA_BLEND_MAX_CHILDREN];

    /**
     * Where each child sits in parameter space. 1D uses only `.x`.
     *
     * In the parameter's own units — metres per second, degrees — rather than normalised, because the
     * thing setting the parameter is the game's own speed and asking it to normalise first is asking
     * it to duplicate the thresholds.
     * */
    f32x2 positions[NYA_BLEND_MAX_CHILDREN];

    u8 child_count;
};

struct NYA_BlendTree {
    const NYA_Skeleton* skeleton;

    NYA_BlendNode nodes[NYA_BLEND_MAX_NODES];
    u32           node_count;

    /** Which node evaluation starts from. The last one added by default. */
    s32 root;

    f32 parameters[NYA_BLEND_MAX_PARAMETERS];

    /**
     * Where the whole tree is in its cycle, in [0, 1).
     *
     * One phase, not one clock per clip. See the note at the top of this file — this field is the
     * mechanism the "feet stay together" argument is about.
     * */
    f32 phase;

    /** Multiplies the clock. Negative runs the cycle backwards. Zero means one. */
    f32 speed;

    b8 looping;

    /** Per node weight from the last update, for debugging and for tests. */
    f32 weights[NYA_BLEND_MAX_NODES];

    /** The weighted average clip length the phase is currently advancing against. */
    f32 duration_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Empties the tree and points it at a skeleton. Looping, speed one. */
NYA_API void nya_blend_tree_init(NYA_BlendTree* tree, const NYA_Skeleton* skeleton);

/** Adds a clip leaf. Returns its node index, or -1 when the tree is full. */
NYA_API s32 nya_blend_tree_clip(NYA_BlendTree* tree, const NYA_SkeletonClip* clip);

/** Adds a 1D blend node driven by `parameter`. Returns its index, or -1. */
NYA_API s32 nya_blend_tree_1d(NYA_BlendTree* tree, u32 parameter);

/** Adds a 2D blend node driven by two parameters. Returns its index, or -1. */
NYA_API s32 nya_blend_tree_2d(NYA_BlendTree* tree, u32 parameter_x, u32 parameter_y);

/**
 * Hangs `child` under `parent` at `position` in parameter space.
 *
 * 1D children are kept sorted by `position.x` as they are added, so they may be given in any order and
 * the bracketing search stays a scan. Returns false when either index is bad, the parent is a clip, or
 * it is full.
 * */
NYA_API b8 nya_blend_tree_child(NYA_BlendTree* tree, s32 parent, s32 child, f32x2 position);

/** Sets which node evaluation starts from. Defaults to the most recently added. */
NYA_API void nya_blend_tree_root(NYA_BlendTree* tree, s32 node);

/** Sets one parameter. Out of range indices are ignored. */
NYA_API void nya_blend_tree_parameter(NYA_BlendTree* tree, u32 parameter, f32 value);

/**
 * Recomputes the weights from the parameters without advancing anything or building a pose.
 *
 * What a test asserts on and what a debug overlay draws. `nya_blend_tree_update` calls it first.
 * */
NYA_API void nya_blend_tree_evaluate(NYA_BlendTree* tree);

/** Advances the shared phase and writes the mixed pose. */
NYA_API void nya_blend_tree_update(NYA_BlendTree* tree, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose);

/**
 * The weights a 2D blend node would give, by gradient band interpolation.
 *
 * Public because it is the piece worth testing directly, and because it is useful on its own for
 * anything that mixes N things by a 2D parameter — a footstep sound set, a facial expression.
 *
 * For each sample, the weight is one minus how far the parameter has travelled from it toward its
 * nearest-influencing neighbour, taken as the minimum over every other sample and clamped at zero.
 * That is what gives a sample full weight when the parameter is on it, zero when it reaches any
 * neighbour, and a sensible gradient in between regardless of how unevenly the samples are spread.
 * Weights are normalised to sum to one; when every one of them lands at zero the nearest sample takes
 * everything, which is what happens when the parameter sits well outside the samples' hull.
 * */
NYA_API void nya_blend_gradient_band(const f32x2* positions, u32 count, f32x2 parameter, OUT f32* out_weights);
