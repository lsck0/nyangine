/**
 * Blend trees: 1D bracketing, 2D gradient bands, the shared phase, and nesting.
 *
 * The rig carries one bone whose x translation is the clip's own identity — clip A holds x at 1, B at
 * 2, C at 3 — so the *pose* reads back as the weighted average of whichever clips are mixed. That is
 * what lets a blend be asserted on directly rather than inferred from the weights that produced it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

enum { BONE_ROOT = 0, BONE_COUNT = 1 };
enum { PARAM_SPEED = 0, PARAM_TURN = 1 };

static NYA_SkeletonBone bones[BONE_COUNT];
static NYA_Skeleton     skeleton;

static NYA_BoneTransform idle_frames[BONE_COUNT * 2];
static NYA_BoneTransform walk_frames[BONE_COUNT * 2];
static NYA_BoneTransform run_frames[BONE_COUNT * 2];
static NYA_SkeletonClip  clip_idle;
static NYA_SkeletonClip  clip_walk;
static NYA_SkeletonClip  clip_run;

/** A clip that holds its bone at `marker` for its whole length. Constant, so phase cannot confuse it. */
static void marker_clip(NYA_BoneTransform* frames, NYA_SkeletonClip* clip, f32 marker, f32 duration_s, NYA_ConstCString name) {
    for (u32 f = 0; f < 2; f++) {
        frames[(f * BONE_COUNT) + BONE_ROOT] =
            (NYA_BoneTransform){ .translation = { marker, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } };
    }

    *clip = (NYA_SkeletonClip){ .duration_s = duration_s, .frame_count = 2, .frame_rate = 1.0F / duration_s, .frames = frames };
    (void)snprintf(clip->name, sizeof(clip->name), "%s", name);
}

static void rig_build(void) {
    bones[BONE_ROOT] =
        (NYA_SkeletonBone){ .parent = -1, .rest = { .translation = { 0, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };
    (void)snprintf(bones[BONE_ROOT].name, sizeof(bones[BONE_ROOT].name), "root");

    skeleton = (NYA_Skeleton){ .bones = bones, .bone_count = BONE_COUNT };

    // Different durations on purpose: the weighted average is what the shared phase runs against.
    marker_clip(idle_frames, &clip_idle, 1.0F, 2.0F, "idle");
    marker_clip(walk_frames, &clip_walk, 2.0F, 1.0F, "walk");
    marker_clip(run_frames, &clip_run, 3.0F, 0.5F, "run");
}

/** Builds idle/walk/run on one 1D node at 0, 2 and 6. */
static s32 locomotion(NYA_BlendTree* tree) {
    nya_blend_tree_init(tree, &skeleton);

    s32 idle = nya_blend_tree_clip(tree, &clip_idle);
    s32 walk = nya_blend_tree_clip(tree, &clip_walk);
    s32 run  = nya_blend_tree_clip(tree, &clip_run);

    s32 node = nya_blend_tree_1d(tree, PARAM_SPEED);

    // Deliberately out of order, to prove nya_blend_tree_child sorts them.
    (void)nya_blend_tree_child(tree, node, run, (f32x2){ 6.0F, 0.0F });
    (void)nya_blend_tree_child(tree, node, idle, (f32x2){ 0.0F, 0.0F });
    (void)nya_blend_tree_child(tree, node, walk, (f32x2){ 2.0F, 0.0F });

    nya_blend_tree_root(tree, node);
    return node;
}

s32 main(void) {
    rig_build();

    // ── 1D: the parameter picks a pair, and only that pair.
    {
        NYA_BlendTree tree = { 0 };
        (void)locomotion(&tree);

        NYA_SkeletonPose pose = { 0 };

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 1.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 1.5F) < 1e-4F, "halfway between idle and walk is 1.5, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_check(fabsf(tree.duration_s - 1.5F) < 1e-4F, "and the duration is the same average, got %f", (f64)tree.duration_s);

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 4.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 2.5F) < 1e-4F, "halfway between walk and run is 2.5, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);
    }

    // ── 1D: outside the range clamps to an end rather than extrapolating.
    {
        NYA_BlendTree tree = { 0 };
        (void)locomotion(&tree);

        NYA_SkeletonPose pose = { 0 };

        nya_blend_tree_parameter(&tree, PARAM_SPEED, -5.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);
        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 1.0F) < 1e-4F, "below the first child is the first child, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 100.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);
        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 3.0F) < 1e-4F, "above the last child is the last child, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);
    }

    // ── The shared phase advances against the blended duration, not any one clip's.
    {
        NYA_BlendTree tree = { 0 };
        (void)locomotion(&tree);

        NYA_SkeletonPose pose = { 0 };

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 2.0F); // pure walk, one second long
        nya_blend_tree_update(&tree, 0.25F, &pose);

        nya_check(fabsf(tree.phase - 0.25F) < 1e-4F, "a quarter second of a one second cycle is a quarter phase, got %f", (f64)tree.phase);

        nya_blend_tree_init(&tree, &skeleton);
        (void)locomotion(&tree);

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 0.0F); // pure idle, two seconds long
        nya_blend_tree_update(&tree, 0.25F, &pose);

        nya_check(fabsf(tree.phase - 0.125F) < 1e-4F, "the same step against a two second cycle is half as much phase, got %f", (f64)tree.phase);
    }

    // ── The phase wraps rather than running away, and stops at the end when not looping.
    {
        NYA_BlendTree tree = { 0 };
        (void)locomotion(&tree);
        nya_blend_tree_parameter(&tree, PARAM_SPEED, 2.0F);

        NYA_SkeletonPose pose = { 0 };
        for (u32 i = 0; i < 40; i++) nya_blend_tree_update(&tree, 0.1F, &pose);

        nya_check(tree.phase >= 0.0F && tree.phase < 1.0F, "a looping tree stays inside its cycle, got %f", (f64)tree.phase);

        tree.looping = false;
        tree.phase   = 0.0F;
        for (u32 i = 0; i < 40; i++) nya_blend_tree_update(&tree, 0.1F, &pose);

        nya_check(tree.phase == 1.0F, "a non-looping tree stops at the end, got %f", (f64)tree.phase);
    }

    // ── Gradient band: on a sample it takes everything, and the weights always sum to one.
    {
        f32x2 positions[3] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
        f32   weights[3]   = { 0 };

        nya_blend_gradient_band(positions, 3, (f32x2){ 0, 0 }, weights);
        nya_check(fabsf(weights[0] - 1.0F) < 1e-4F, "the parameter on a sample gives it everything, got %f", (f64)weights[0]);

        nya_blend_gradient_band(positions, 3, (f32x2){ 1, 0 }, weights);
        nya_check(fabsf(weights[1] - 1.0F) < 1e-4F, "and the same for the second, got %f", (f64)weights[1]);

        // A sweep, because a gradient that sums to one only at the samples is a gradient that dims.
        for (u32 i = 0; i <= 20; i++) {
            f32x2 parameter = { (f32)i / 20.0F, (f32)(20 - i) / 40.0F };

            nya_blend_gradient_band(positions, 3, parameter, weights);

            f32 total = weights[0] + weights[1] + weights[2];
            nya_check(fabsf(total - 1.0F) < 1e-4F, "weights sum to one at " FMTf32x2 ", got %f", FMTf32x2_ARG(parameter), (f64)total);

            for (u32 w = 0; w < 3; w++) nya_check(weights[w] >= 0.0F, "and none of them is negative, weight %u is %f", w, (f64)weights[w]);
        }

        // Far outside the hull every band is exhausted, and the nearest sample has to take it.
        nya_blend_gradient_band(positions, 3, (f32x2){ -50.0F, -50.0F }, weights);
        nya_check(fabsf(weights[0] - 1.0F) < 1e-4F, "well outside the hull the nearest sample takes everything, got %f", (f64)weights[0]);
    }

    // ── 2D: three clips on a plane, mixed by two parameters.
    {
        NYA_BlendTree tree = { 0 };
        nya_blend_tree_init(&tree, &skeleton);

        s32 idle = nya_blend_tree_clip(&tree, &clip_idle);
        s32 walk = nya_blend_tree_clip(&tree, &clip_walk);
        s32 run  = nya_blend_tree_clip(&tree, &clip_run);

        s32 node = nya_blend_tree_2d(&tree, PARAM_SPEED, PARAM_TURN);
        (void)nya_blend_tree_child(&tree, node, idle, (f32x2){ 0, 0 });
        (void)nya_blend_tree_child(&tree, node, walk, (f32x2){ 1, 0 });
        (void)nya_blend_tree_child(&tree, node, run, (f32x2){ 0, 1 });
        nya_blend_tree_root(&tree, node);

        NYA_SkeletonPose pose = { 0 };

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 1.0F);
        nya_blend_tree_parameter(&tree, PARAM_TURN, 0.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 2.0F) < 1e-3F, "sitting on the walk sample gives the walk, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_blend_tree_parameter(&tree, PARAM_SPEED, 0.0F);
        nya_blend_tree_parameter(&tree, PARAM_TURN, 1.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 3.0F) < 1e-3F, "and sitting on the run sample gives the run, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);
    }

    // ── Nesting: a 2D node whose children are 1D nodes, weights routed all the way down.
    {
        NYA_BlendTree tree = { 0 };
        nya_blend_tree_init(&tree, &skeleton);

        s32 idle = nya_blend_tree_clip(&tree, &clip_idle);
        s32 walk = nya_blend_tree_clip(&tree, &clip_walk);
        s32 run  = nya_blend_tree_clip(&tree, &clip_run);

        s32 slow = nya_blend_tree_1d(&tree, PARAM_SPEED);
        (void)nya_blend_tree_child(&tree, slow, idle, (f32x2){ 0, 0 });
        (void)nya_blend_tree_child(&tree, slow, walk, (f32x2){ 1, 0 });

        s32 fast = nya_blend_tree_1d(&tree, PARAM_SPEED);
        (void)nya_blend_tree_child(&tree, fast, walk, (f32x2){ 0, 0 });
        (void)nya_blend_tree_child(&tree, fast, run, (f32x2){ 1, 0 });

        s32 top = nya_blend_tree_2d(&tree, PARAM_TURN, PARAM_TURN);
        (void)nya_blend_tree_child(&tree, top, slow, (f32x2){ 0, 0 });
        (void)nya_blend_tree_child(&tree, top, fast, (f32x2){ 1, 1 });
        nya_blend_tree_root(&tree, top);

        NYA_SkeletonPose pose = { 0 };

        nya_blend_tree_parameter(&tree, PARAM_TURN, 0.0F);
        nya_blend_tree_parameter(&tree, PARAM_SPEED, 1.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 2.0F) < 1e-3F, "the slow branch at its top is the walk, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_blend_tree_parameter(&tree, PARAM_TURN, 1.0F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 3.0F) < 1e-3F, "the fast branch at its top is the run, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);
    }

    // ── A clip reached twice is merged rather than blended against itself.
    {
        NYA_BlendTree tree = { 0 };
        nya_blend_tree_init(&tree, &skeleton);

        s32 walk = nya_blend_tree_clip(&tree, &clip_walk);

        s32 node = nya_blend_tree_2d(&tree, PARAM_SPEED, PARAM_TURN);
        (void)nya_blend_tree_child(&tree, node, walk, (f32x2){ 0, 0 });
        (void)nya_blend_tree_child(&tree, node, walk, (f32x2){ 1, 0 });
        nya_blend_tree_root(&tree, node);

        NYA_SkeletonPose pose = { 0 };
        nya_blend_tree_parameter(&tree, PARAM_SPEED, 0.5F);
        nya_blend_tree_update(&tree, 0.0F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - 2.0F) < 1e-4F, "the same clip twice is still that clip, got %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_check(fabsf(tree.duration_s - clip_walk.duration_s) < 1e-4F, "and its duration is not counted twice, got %f", (f64)tree.duration_s);
    }

    // ── Malformed trees are refused at build time rather than at frame time.
    {
        NYA_BlendTree tree = { 0 };
        nya_blend_tree_init(&tree, &skeleton);

        s32 walk = nya_blend_tree_clip(&tree, &clip_walk);
        s32 node = nya_blend_tree_1d(&tree, PARAM_SPEED);

        nya_check(!nya_blend_tree_child(&tree, walk, node, (f32x2){ 0, 0 }), "a clip cannot have children");
        nya_check(!nya_blend_tree_child(&tree, node, node, (f32x2){ 0, 0 }), "a node cannot be its own child");
        nya_check(!nya_blend_tree_child(&tree, node, 99, (f32x2){ 0, 0 }), "a child that does not exist is refused");
        nya_check(!nya_blend_tree_child(&tree, -1, walk, (f32x2){ 0, 0 }), "and so is a parent that does not");
    }

    // ── An empty tree is the rest pose, not an uninitialised one.
    {
        NYA_BlendTree tree = { 0 };
        nya_blend_tree_init(&tree, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        nya_blend_tree_update(&tree, 0.1F, &pose);

        nya_check(pose.bone_count == BONE_COUNT, "an empty tree still produces a pose");
        nya_check(pose.local[BONE_ROOT].translation.x == 0.0F, "and it is the rest pose");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
