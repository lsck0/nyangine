#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One clip's share of the mix, collected while the tree is walked. */
typedef struct {
    const NYA_SkeletonClip* clip;
    f32                     weight;
} _NYA_BlendContribution;

/** What a walk of the tree fills in. Clips accumulate here; blend nodes only route weight. */
typedef struct {
    _NYA_BlendContribution clips[NYA_BLEND_MAX_NODES];
    u32                    clip_count;
} _NYA_BlendMix;

NYA_INTERNAL s32  _nya_blend_node_add(NYA_BlendTree* tree, NYA_BlendNodeKind kind);
NYA_INTERNAL void _nya_blend_walk(NYA_BlendTree* tree, s32 node, f32 weight, u32 depth, _NYA_BlendMix* mix);
NYA_INTERNAL void _nya_blend_weights_1d(const NYA_BlendNode* node, f32 parameter, OUT f32* out_weights);

/**
 * Adds `clip` at `weight` to the mix, merging a clip that is already in it.
 *
 * The merge matters: the same clip can be reached twice through a tree — an idle at the centre of a
 * 2D strafe space is the obvious one — and two entries for it would sample and blend it against
 * itself, which is wasted work and, worse, wrong once the weights are normalised.
 * */
NYA_INTERNAL void _nya_blend_contribute(_NYA_BlendMix* mix, const NYA_SkeletonClip* clip, f32 weight) {
    if (clip == nullptr || weight <= 0.0F) return;

    for (u32 i = 0; i < mix->clip_count; i++) {
        if (mix->clips[i].clip != clip) continue;

        mix->clips[i].weight += weight;
        return;
    }

    if (mix->clip_count >= NYA_BLEND_MAX_NODES) return;

    mix->clips[mix->clip_count++] = (_NYA_BlendContribution){ .clip = clip, .weight = weight };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_blend_tree_init(NYA_BlendTree* tree, const NYA_Skeleton* skeleton) {
    if (tree == nullptr) return;

    *tree = (NYA_BlendTree){
        .skeleton = skeleton,
        .root     = -1,
        .speed    = 1.0F,
        .looping  = true,
    };
}

s32 nya_blend_tree_clip(NYA_BlendTree* tree, const NYA_SkeletonClip* clip) {
    s32 index = _nya_blend_node_add(tree, NYA_BLEND_NODE_CLIP);
    if (index < 0) return -1;

    tree->nodes[index].clip = clip;
    return index;
}

s32 nya_blend_tree_1d(NYA_BlendTree* tree, u32 parameter) {
    s32 index = _nya_blend_node_add(tree, NYA_BLEND_NODE_1D);
    if (index < 0) return -1;

    tree->nodes[index].parameter_x = (u8)(parameter < NYA_BLEND_MAX_PARAMETERS ? parameter : 0);
    return index;
}

s32 nya_blend_tree_2d(NYA_BlendTree* tree, u32 parameter_x, u32 parameter_y) {
    s32 index = _nya_blend_node_add(tree, NYA_BLEND_NODE_2D);
    if (index < 0) return -1;

    tree->nodes[index].parameter_x = (u8)(parameter_x < NYA_BLEND_MAX_PARAMETERS ? parameter_x : 0);
    tree->nodes[index].parameter_y = (u8)(parameter_y < NYA_BLEND_MAX_PARAMETERS ? parameter_y : 0);
    return index;
}

b8 nya_blend_tree_child(NYA_BlendTree* tree, s32 parent, s32 child, f32x2 position) {
    if (tree == nullptr) return false;
    if (parent < 0 || (u32)parent >= tree->node_count) return false;
    if (child < 0 || (u32)child >= tree->node_count) return false;

    // A clip has nothing to mix, and a node that is its own child is a cycle the depth limit would
    // only paper over.
    if (tree->nodes[parent].kind == NYA_BLEND_NODE_CLIP) return false;
    if (parent == child) return false;

    NYA_BlendNode* node = &tree->nodes[parent];
    if (node->child_count >= NYA_BLEND_MAX_CHILDREN) return false;

    /*
     * Inserted in order of position.x rather than appended.
     *
     * The 1D weighting walks the children looking for the pair that brackets the parameter, and that
     * only means anything if they are sorted. Doing it here rather than asking the caller to add them
     * in order is the difference between a wrong tree failing loudly and a wrong tree animating
     * slightly incorrectly.
     */
    u32 at = node->child_count;
    while (at > 0 && node->positions[at - 1].x > position.x) {
        node->positions[at] = node->positions[at - 1];
        node->children[at]  = node->children[at - 1];
        at--;
    }

    node->positions[at] = position;
    node->children[at]  = (u8)child;
    node->child_count++;

    return true;
}

void nya_blend_tree_root(NYA_BlendTree* tree, s32 node) {
    if (tree == nullptr || node < 0 || (u32)node >= tree->node_count) return;

    tree->root = node;
}

void nya_blend_tree_parameter(NYA_BlendTree* tree, u32 parameter, f32 value) {
    if (tree == nullptr || parameter >= NYA_BLEND_MAX_PARAMETERS) return;

    tree->parameters[parameter] = value;
}

void nya_blend_tree_evaluate(NYA_BlendTree* tree) {
    if (tree == nullptr) return;

    nya_memset(tree->weights, 0, sizeof(tree->weights));

    if (tree->root < 0 || (u32)tree->root >= tree->node_count) return;

    _NYA_BlendMix mix = { 0 };
    _nya_blend_walk(tree, tree->root, 1.0F, 0, &mix);

    /*
     * The duration the phase runs against, and the reason the whole tree shares one.
     *
     * Weighted by the same weights the pose is: at 82% walk the cycle is 82% of the way from the
     * walk's length to the run's, so the transition through the middle is continuous in *time* as
     * well as in pose. Taking the dominant clip's duration instead makes the cadence jump the moment
     * the majority changes hands.
     */
    f32 duration = 0.0F;
    for (u32 i = 0; i < mix.clip_count; i++) {
        if (mix.clips[i].clip == nullptr) continue;
        duration += mix.clips[i].weight * mix.clips[i].clip->duration_s;
    }

    tree->duration_s = duration;
}

void nya_blend_tree_update(NYA_BlendTree* tree, f32 delta_time_s, OUT NYA_SkeletonPose* out_pose) {
    if (tree == nullptr || out_pose == nullptr || tree->skeleton == nullptr) return;

    nya_memset(tree->weights, 0, sizeof(tree->weights));

    if (tree->root < 0 || (u32)tree->root >= tree->node_count) {
        nya_skeleton_pose_rest(tree->skeleton, out_pose);
        return;
    }

    _NYA_BlendMix mix = { 0 };
    _nya_blend_walk(tree, tree->root, 1.0F, 0, &mix);

    f32 duration = 0.0F;
    for (u32 i = 0; i < mix.clip_count; i++) {
        if (mix.clips[i].clip == nullptr) continue;
        duration += mix.clips[i].weight * mix.clips[i].clip->duration_s;
    }

    tree->duration_s = duration;

    if (mix.clip_count == 0 || duration <= 0.0F) {
        nya_skeleton_pose_rest(tree->skeleton, out_pose);
        return;
    }

    f32 speed = tree->speed != 0.0F ? tree->speed : 1.0F;

    tree->phase += (delta_time_s * speed) / duration;

    if (tree->looping) {
        tree->phase = fmodf(tree->phase, 1.0F);
        if (tree->phase < 0.0F) tree->phase += 1.0F;
    } else {
        tree->phase = nya_clamp(tree->phase, 0.0F, 1.0F);
    }

    /*
     * Mixed by successive blends rather than by averaging every bone at once.
     *
     * `running` is the weight already folded in, so blending the next contribution at
     * `weight / (running + weight)` leaves the result correctly proportioned after each step. Doing
     * it this way means the whole thing is built out of nya_skeleton_pose_blend, which slerps — an
     * N-way component-wise average of quaternions does not, and shrinks rotations toward the centre.
     */
    f32 running = 0.0F;

    for (u32 i = 0; i < mix.clip_count; i++) {
        const _NYA_BlendContribution* contribution = &mix.clips[i];
        if (contribution->clip == nullptr || contribution->weight <= 0.0F) continue;

        NYA_SkeletonPose sampled = { 0 };
        nya_skeleton_pose_sample(tree->skeleton, contribution->clip, tree->phase * contribution->clip->duration_s, &sampled);

        if (running <= 0.0F) {
            *out_pose = sampled;
            running   = contribution->weight;
            continue;
        }

        NYA_SkeletonPose blended = { 0 };
        nya_skeleton_pose_blend(out_pose, &sampled, contribution->weight / (running + contribution->weight), &blended);

        *out_pose = blended;
        running += contribution->weight;
    }
}

void nya_blend_gradient_band(const f32x2* positions, u32 count, f32x2 parameter, OUT f32* out_weights) {
    if (positions == nullptr || out_weights == nullptr || count == 0) return;

    f32 total = 0.0F;

    for (u32 i = 0; i < count; i++) {
        f32 weight = 1.0F;

        for (u32 j = 0; j < count; j++) {
            if (j == i) continue;

            f32x2 to_neighbour = positions[j] - positions[i];
            f32   span         = nya_vector_dot(to_neighbour, to_neighbour);

            // Two samples in the same place say nothing about each other's influence, and dividing by
            // the distance between them would say it very loudly.
            if (span <= NYA_EPSILON) continue;

            f32x2 to_parameter = parameter - positions[i];

            // How far along the line from i to j the parameter has travelled. One at i, zero at j.
            f32 along = 1.0F - (nya_vector_dot(to_parameter, to_neighbour) / span);

            if (along < weight) weight = along;
        }

        out_weights[i] = weight > 0.0F ? weight : 0.0F;
        total += out_weights[i];
    }

    if (total > 0.0F) {
        for (u32 i = 0; i < count; i++) out_weights[i] /= total;
        return;
    }

    /*
     * Every weight zero, which happens when the parameter is outside the samples' hull far enough
     * that each of them is past somebody else's neighbour.
     *
     * The nearest sample takes all of it. Leaving the weights at zero would produce no pose at all,
     * and a character that stops animating because a speed went slightly negative is worse than one
     * that plays its slowest clip.
     */
    u32 nearest          = 0;
    f32 nearest_distance = 0.0F;

    for (u32 i = 0; i < count; i++) {
        f32x2 offset   = parameter - positions[i];
        f32   distance = nya_vector_dot(offset, offset);

        if (i == 0 || distance < nearest_distance) {
            nearest          = i;
            nearest_distance = distance;
        }

        out_weights[i] = 0.0F;
    }

    out_weights[nearest] = 1.0F;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL s32 _nya_blend_node_add(NYA_BlendTree* tree, NYA_BlendNodeKind kind) {
    if (tree == nullptr || tree->node_count >= NYA_BLEND_MAX_NODES) return -1;

    s32 index          = (s32)tree->node_count++;
    tree->nodes[index] = (NYA_BlendNode){ .kind = kind };

    // The last node added, which for a tree built bottom up is the one that mixes everything else.
    // Overridable, and worth overriding the moment a tree stops being built in that order.
    tree->root = index;

    return index;
}

NYA_INTERNAL void _nya_blend_walk(NYA_BlendTree* tree, s32 node, f32 weight, u32 depth, _NYA_BlendMix* mix) {
    if (node < 0 || (u32)node >= tree->node_count || weight <= 0.0F) return;

    // A cycle would otherwise recurse forever. Silently, because the alternative is asserting in the
    // middle of a frame over a tree that was built wrong once, at startup.
    if (depth >= NYA_BLEND_MAX_DEPTH) return;

    NYA_BlendNode* current = &tree->nodes[node];

    tree->weights[node] += weight;

    if (current->kind == NYA_BLEND_NODE_CLIP) {
        _nya_blend_contribute(mix, current->clip, weight);
        return;
    }

    if (current->child_count == 0) return;

    f32 child_weights[NYA_BLEND_MAX_CHILDREN] = { 0 };

    if (current->kind == NYA_BLEND_NODE_1D) {
        _nya_blend_weights_1d(current, tree->parameters[current->parameter_x], child_weights);
    } else {
        f32x2 parameter = { tree->parameters[current->parameter_x], tree->parameters[current->parameter_y] };
        nya_blend_gradient_band(current->positions, current->child_count, parameter, child_weights);
    }

    for (u32 i = 0; i < current->child_count; i++) {
        _nya_blend_walk(tree, (s32)current->children[i], weight * child_weights[i], depth + 1, mix);
    }
}

NYA_INTERNAL void _nya_blend_weights_1d(const NYA_BlendNode* node, f32 parameter, OUT f32* out_weights) {
    u32 count = node->child_count;
    if (count == 0) return;

    // Outside the range, the end child takes everything. Clamping rather than extrapolating: a
    // character standing still should play the idle, not a negative amount of the walk.
    if (count == 1 || parameter <= node->positions[0].x) {
        out_weights[0] = 1.0F;
        return;
    }

    if (parameter >= node->positions[count - 1].x) {
        out_weights[count - 1] = 1.0F;
        return;
    }

    // Children are sorted by nya_blend_tree_child, so the first one past the parameter and the one
    // before it are the pair that bracket it.
    for (u32 i = 1; i < count; i++) {
        if (node->positions[i].x < parameter) continue;

        f32 span = node->positions[i].x - node->positions[i - 1].x;

        // Two children at the same position: the second wins, arbitrarily but deterministically,
        // rather than dividing by zero.
        f32 t = span > NYA_EPSILON ? (parameter - node->positions[i - 1].x) / span : 1.0F;

        out_weights[i - 1] = 1.0F - t;
        out_weights[i]     = t;
        return;
    }
}
