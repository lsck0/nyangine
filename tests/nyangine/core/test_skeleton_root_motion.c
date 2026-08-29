/**
 * Root motion: extracting a clip's travel, pinning the bone that carried it, and surviving the loop.
 *
 * The rig is synthetic and the clips are linear ramps, so every expectation here is arithmetic. A
 * clip that moves its root four metres along +x over one second means a quarter second of it is
 * exactly one metre, and anything else is a bug rather than an art asset.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

enum { BONE_ROOT = 0, BONE_CHILD = 1, BONE_COUNT = 2 };

/** How far the walk clip travels over its one second, and how high its root bobs. */
#define WALK_DISTANCE 4.0F
#define WALK_BOB      1.0F

/** The run clip, deliberately a different speed so a crossfade between the two has something to show. */
#define RUN_DISTANCE 10.0F

static NYA_SkeletonBone bones[BONE_COUNT];
static NYA_Skeleton     skeleton;

static NYA_BoneTransform walk_frames[BONE_COUNT * 2];
static NYA_BoneTransform run_frames[BONE_COUNT * 2];
static NYA_BoneTransform turn_frames[BONE_COUNT * 2];
static NYA_SkeletonClip  clip_walk;
static NYA_SkeletonClip  clip_run;
static NYA_SkeletonClip  clip_turn;

/** A two-frame clip is a straight ramp between its endpoints, which is all these need to be. */
static void ramp(NYA_BoneTransform* frames, f32x3 travel, f32 bob, NYA_Quaternion turn) {
    for (u32 f = 0; f < 2; f++) {
        for (u32 b = 0; b < BONE_COUNT; b++) frames[(f * BONE_COUNT) + b] = bones[b].rest;
    }

    frames[BONE_COUNT + BONE_ROOT].translation = travel + (f32x3){ 0.0F, bob, 0.0F };
    frames[BONE_COUNT + BONE_ROOT].rotation    = turn;
}

static void rig_build(void) {
    bones[BONE_ROOT] =
        (NYA_SkeletonBone){ .parent = -1, .rest = { .translation = { 0, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };
    bones[BONE_CHILD] =
        (NYA_SkeletonBone){ .parent = BONE_ROOT, .rest = { .translation = { 0, 1, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };

    (void)snprintf(bones[BONE_ROOT].name, sizeof(bones[BONE_ROOT].name), "root");
    (void)snprintf(bones[BONE_CHILD].name, sizeof(bones[BONE_CHILD].name), "child");

    skeleton = (NYA_Skeleton){ .bones = bones, .bone_count = BONE_COUNT };

    ramp(walk_frames, (f32x3){ WALK_DISTANCE, 0, 0 }, WALK_BOB, nya_quaternion_identity);
    ramp(run_frames, (f32x3){ RUN_DISTANCE, 0, 0 }, 0.0F, nya_quaternion_identity);
    ramp(turn_frames, (f32x3){ 0, 0, 0 }, 0.0F, nya_quaternion_from_axis_angle((f32x3){ 0, 1, 0 }, 1.5707963F));

    clip_walk = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = walk_frames };
    clip_run  = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = run_frames };
    clip_turn = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = turn_frames };
}

s32 main(void) {
    rig_build();

    // ── The single-bone sampler agrees with sampling the whole pose.
    {
        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_walk, 0.375F, &pose);

        NYA_BoneTransform direct = nya_skeleton_clip_bone(&skeleton, &clip_walk, BONE_ROOT, 0.375F);

        nya_check(fabsf(direct.translation.x - pose.local[BONE_ROOT].translation.x) < 1e-5F,
                  "the bone sampler matches the pose sampler: %f vs %f", (f64)direct.translation.x, (f64)pose.local[BONE_ROOT].translation.x);

        nya_check(fabsf(direct.translation.x - (WALK_DISTANCE * 0.375F)) < 1e-4F, "and is where the ramp says, %f", (f64)direct.translation.x);

        NYA_BoneTransform missing = nya_skeleton_clip_bone(&skeleton, nullptr, BONE_ROOT, 0.0F);
        nya_check(missing.translation.x == 0.0F, "a null clip gives the rest transform");
    }

    // ── Off by default, and a misspelled bone is reported rather than silently ignored.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);

        nya_check(player.root_motion_bone < 0, "root motion is off until asked for");
        nya_check(!nya_skeleton_player_root_motion(&player, "pelvis", (f32x3){ 1, 0, 1 }, false), "a bone that does not exist is refused");
        nya_check(nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false), "and one that does is accepted");
    }

    // ── A quarter of the clip is a quarter of the travel, the bone is pinned, and the bob survives.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false);

        nya_skeleton_player_play(&player, &clip_walk, .looping = true);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 0.25F, &pose);

        NYA_RootMotion delta = nya_skeleton_player_root_delta(&player);

        nya_check(fabsf(delta.translation.x - (WALK_DISTANCE * 0.25F)) < 1e-4F, "a quarter second travels a quarter of the clip: %f",
                  (f64)delta.translation.x);

        nya_check(fabsf(delta.translation.y) < 1e-6F, "the vertical bob is not reported as travel: %f", (f64)delta.translation.y);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x) < 1e-6F, "the extracted axis is pinned to the rest pose: %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.y - (WALK_BOB * 0.25F)) < 1e-4F, "the unextracted axis keeps animating: %f",
                  (f64)pose.local[BONE_ROOT].translation.y);
    }

    // ── The whole point: a loop accumulates distance instead of undoing itself at the seam.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false);

        nya_skeleton_player_play(&player, &clip_walk, .looping = true);

        f32 travelled = 0.0F;
        f32 backwards = 0.0F;

        NYA_SkeletonPose pose = { 0 };

        // Three full cycles at a step that does not divide the duration, so the seam lands mid-step
        // rather than exactly on a frame boundary.
        for (u32 i = 0; i < 300; i++) {
            nya_skeleton_player_update(&player, 0.03F, &pose);

            f32 step = nya_skeleton_player_root_delta(&player).translation.x;
            travelled += step;
            if (step < 0.0F) backwards += step;
        }

        nya_check(backwards == 0.0F, "no step ever goes backwards across the loop seam, worst total %f", (f64)backwards);

        nya_check(fabsf(travelled - (WALK_DISTANCE * 9.0F)) < 0.05F, "nine seconds of a four metre cycle is thirty six metres, got %f",
                  (f64)travelled);
    }

    // ── Playing backwards wraps the other way, and reports negative travel.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false);

        nya_skeleton_player_play(&player, &clip_walk, .looping = true, .speed = -1.0F);

        f32              travelled = 0.0F;
        NYA_SkeletonPose pose      = { 0 };

        for (u32 i = 0; i < 100; i++) {
            nya_skeleton_player_update(&player, 0.03F, &pose);
            travelled += nya_skeleton_player_root_delta(&player).translation.x;
        }

        nya_check(fabsf(travelled + (WALK_DISTANCE * 3.0F)) < 0.05F, "three seconds backwards is minus twelve metres, got %f", (f64)travelled);
    }

    // ── Rotation, when asked for. The root turns a quarter turn over the clip.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, true);

        nya_skeleton_player_play(&player, &clip_turn, .looping = false);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 0.5F, &pose);

        NYA_RootMotion delta = nya_skeleton_player_root_delta(&player);

        // Half of a quarter turn about +y. Compared through the axis the rotation is about, which is
        // the component a quaternion for that rotation puts the sine of the half angle in.
        f32 expected = sinf(1.5707963F * 0.5F * 0.5F);

        nya_check(fabsf(delta.rotation.y - expected) < 1e-3F, "half the clip is half the turn: %f, want %f", (f64)delta.rotation.y, (f64)expected);

        nya_check(fabsf(pose.local[BONE_ROOT].rotation.y) < 1e-6F, "and the bone itself is pinned unrotated: %f",
                  (f64)pose.local[BONE_ROOT].rotation.y);
    }

    // ── A crossfade blends the travel by the same curve as the pose.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false);

        nya_skeleton_player_play(&player, &clip_walk, .looping = true);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 0.1F, &pose);

        nya_skeleton_player_play(&player, &clip_run, .looping = true, .fade_s = 0.4F);
        nya_skeleton_player_update(&player, 0.2F, &pose);

        nya_check(nya_skeleton_player_fading(&player), "the fade is still running");

        // Per second, so the two clips are directly comparable: the walk moves at 4 and the run at 10.
        f32 rate = nya_skeleton_player_root_delta(&player).translation.x / 0.2F;

        nya_check(rate > WALK_DISTANCE && rate < RUN_DISTANCE, "mid-fade the rate is between the two clips: %f", (f64)rate);
    }

    // ── A player with nothing playing reports nothing, rather than the last thing it saw.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false);

        nya_skeleton_player_play(&player, &clip_walk, .looping = true);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 0.25F, &pose);
        nya_check(nya_skeleton_player_root_delta(&player).translation.x > 0.0F, "it moved while it was playing");

        nya_skeleton_player_init(&player, &skeleton);
        nya_skeleton_player_update(&player, 0.25F, &pose);

        nya_check(nya_skeleton_player_root_delta(&player).translation.x == 0.0F, "and stops the moment there is no clip");
    }

    // ── Turning it off puts the bone back under the clip's control.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        (void)nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false);
        (void)nya_skeleton_player_root_motion(&player, nullptr, (f32x3){ 0, 0, 0 }, false);

        nya_skeleton_player_play(&player, &clip_walk, .looping = true);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 0.25F, &pose);

        nya_check(fabsf(pose.local[BONE_ROOT].translation.x - (WALK_DISTANCE * 0.25F)) < 1e-4F, "the root animates in place again: %f",
                  (f64)pose.local[BONE_ROOT].translation.x);

        nya_check(nya_skeleton_player_root_delta(&player).translation.x == 0.0F, "and nothing is reported");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
