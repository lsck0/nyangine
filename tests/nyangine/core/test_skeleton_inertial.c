/**
 * Inertialization: the quintic's endpoint conditions, continuity across a switch, and composition.
 *
 * The properties asserted here are deliberately **scale invariant** — the seam jump as a fraction of
 * the cut it replaces, rather than an absolute tolerance. That is not a convenience. Bollo's curve
 * has a large initial acceleration by construction (a₀ is what stops it overshooting), so over one
 * 16 ms frame of a 200 ms transition the offset has already curved measurably, and a finite difference
 * across that frame does *not* recover x'(0). Measured on this rig: the true v₀ is -4.0 and the
 * one-frame difference reads -36.5, converging only as the step shrinks, and still 30% out at 0.5 ms.
 * An absolute tolerance on a one-frame velocity would therefore be asserting the sampling interval,
 * not the maths. What can be checked exactly is that the offset **reaches zero and stays there**, that
 * it **never crosses zero on the way**, and that the seam is a **small fraction of the pop it replaces**.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

enum { BONE_ROOT = 0, BONE_CHILD = 1, BONE_COUNT = 2 };

#define STEP 0.016F

static NYA_SkeletonBone bones[BONE_COUNT];
static NYA_Skeleton     skeleton;

static NYA_BoneTransform left_frames[BONE_COUNT * 2];
static NYA_BoneTransform right_frames[BONE_COUNT * 2];
static NYA_SkeletonClip  clip_left;
static NYA_SkeletonClip  clip_right;

/** A clip whose child bone slides from `start` to `end` along x, and rotates about z by `turn`. */
static void slide_clip(NYA_BoneTransform* frames, NYA_SkeletonClip* clip, f32 start, f32 end, f32 turn, NYA_ConstCString name) {
    for (u32 f = 0; f < 2; f++) {
        for (u32 b = 0; b < BONE_COUNT; b++) frames[(f * BONE_COUNT) + b] = bones[b].rest;
    }

    frames[BONE_CHILD].translation             = (f32x3){ start, 0, 0 };
    frames[BONE_COUNT + BONE_CHILD].translation = (f32x3){ end, 0, 0 };
    frames[BONE_COUNT + BONE_CHILD].rotation    = nya_quaternion_from_axis_angle((f32x3){ 0, 0, 1 }, turn);

    *clip = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = frames };
    (void)snprintf(clip->name, sizeof(clip->name), "%s", name);
}

static void rig_build(void) {
    bones[BONE_ROOT] =
        (NYA_SkeletonBone){ .parent = -1, .rest = { .translation = { 0, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };
    bones[BONE_CHILD] =
        (NYA_SkeletonBone){ .parent = BONE_ROOT, .rest = { .translation = { 0, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };

    (void)snprintf(bones[BONE_ROOT].name, sizeof(bones[BONE_ROOT].name), "root");
    (void)snprintf(bones[BONE_CHILD].name, sizeof(bones[BONE_CHILD].name), "child");

    skeleton = (NYA_Skeleton){ .bones = bones, .bone_count = BONE_COUNT };

    // Deliberately far apart and moving in opposite directions, so a transition between them has both
    // a large offset and a large *relative* velocity to reconcile.
    slide_clip(left_frames, &clip_left, 0.0F, 2.0F, 0.0F, "left");
    slide_clip(right_frames, &clip_right, 10.0F, 8.0F, 1.0F, "right");
}

static NYA_SkeletonInertializer inertializer;

s32 main(void) {
    rig_build();

    // ── The curve's defining property: it reaches exactly zero, and stays there.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPose pose = { 0 };

        // Two frames of the left clip, so there is a history with a velocity in it.
        for (u32 i = 0; i < 2; i++) {
            nya_skeleton_pose_sample(&skeleton, &clip_left, (f32)i * STEP, &pose);
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        NYA_SkeletonPose target   = { 0 };
        NYA_SkeletonPose previous = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.0F, &target);
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.0F, &previous);

        nya_skeleton_inertializer_transition(&inertializer, &target, &previous, 0.2F);
        nya_check(nya_skeleton_inertializer_active(&inertializer), "a transition starts one");

        // Well past the duration.
        f32 elapsed = 0.0F;
        while (elapsed < 0.4F) {
            nya_skeleton_pose_sample(&skeleton, &clip_right, elapsed, &pose);

            NYA_SkeletonPose clean = pose;
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);

            if (elapsed > 0.25F) {
                f32 residue = fabsf(pose.local[BONE_CHILD].translation.x - clean.local[BONE_CHILD].translation.x);
                nya_check(residue == 0.0F, "past the duration the offset is exactly zero, not small: %f at t=%f", (f64)residue, (f64)elapsed);
            }

            elapsed += STEP;
        }

        nya_check(!nya_skeleton_inertializer_active(&inertializer), "and it reports itself finished");
    }

    // ── The seam: the first inertialized frame matches what was on screen, not the new clip.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        for (u32 i = 0; i < 3; i++) {
            nya_skeleton_pose_sample(&skeleton, &clip_left, (f32)i * STEP, &pose);
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        f32 shown_before = pose.local[BONE_CHILD].translation.x;

        NYA_SkeletonPose target   = { 0 };
        NYA_SkeletonPose previous = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_right, STEP, &target);
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.0F, &previous);

        nya_skeleton_inertializer_transition(&inertializer, &target, &previous, 0.2F);

        pose = target;
        nya_skeleton_inertializer_update(&inertializer, STEP, &pose);

        f32 shown_after = pose.local[BONE_CHILD].translation.x;
        f32 raw         = target.local[BONE_CHILD].translation.x;

        // What a cut would have done, which is the thing being removed.
        f32 cut  = fabsf(raw - shown_before);
        f32 seam = fabsf(shown_after - shown_before);

        nya_check(seam < cut * 0.15F, "the seam is a fraction of the cut it replaces: %f against %f", (f64)seam, (f64)cut);

        nya_check(fabsf(shown_after - raw) > cut * 0.8F, "and the pose is still nowhere near the raw destination %f", (f64)raw);
    }

    // ── The offset never crosses zero and never runs away, which is what a₀ is chosen for.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        for (u32 i = 0; i < 3; i++) {
            nya_skeleton_pose_sample(&skeleton, &clip_left, (f32)i * STEP, &pose);
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        NYA_SkeletonPose target   = { 0 };
        NYA_SkeletonPose previous = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_right, STEP, &target);
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.0F, &previous);

        f32 initial = fabsf(pose.local[BONE_CHILD].translation.x - target.local[BONE_CHILD].translation.x);

        nya_skeleton_inertializer_transition(&inertializer, &target, &previous, 0.2F);

        f32 largest = 0.0F;
        f32 elapsed = 0.0F;

        while (elapsed < 0.25F) {
            nya_skeleton_pose_sample(&skeleton, &clip_right, elapsed, &pose);

            NYA_SkeletonPose clean = pose;
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);

            f32 offset = pose.local[BONE_CHILD].translation.x - clean.local[BONE_CHILD].translation.x;

            // The offset started negative. Crossing zero would mean the limb swung past the
            // destination and came back, which reads worse than the pop this exists to remove.
            nya_check(offset <= 1e-5F, "the offset never crosses zero, %f at t=%f", (f64)offset, (f64)elapsed);

            largest = nya_max(largest, fabsf(offset));
            elapsed += STEP;
        }

        nya_check(largest <= initial * 1.05F, "and never grows past where it started: %f against %f", (f64)largest, (f64)initial);
    }

    // ── Rotation goes the same way, and comes back to the destination exactly.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        for (u32 i = 0; i < 3; i++) {
            nya_skeleton_pose_sample(&skeleton, &clip_left, (f32)i * STEP, &pose);
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        NYA_Quaternion shown_before = pose.local[BONE_CHILD].rotation;

        NYA_SkeletonPose target = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.9F, &target);
        nya_skeleton_inertializer_transition(&inertializer, &target, nullptr, 0.2F);

        pose = target;
        nya_skeleton_inertializer_update(&inertializer, STEP, &pose);

        nya_check(nya_quaternion_angle_between(pose.local[BONE_CHILD].rotation, shown_before) < 0.15F,
                  "the rotation stays near what was on screen, %f rad away",
                  (f64)nya_quaternion_angle_between(pose.local[BONE_CHILD].rotation, shown_before));

        for (u32 i = 0; i < 40; i++) {
            pose = target;
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        nya_check(nya_quaternion_approx_equals(pose.local[BONE_CHILD].rotation, target.local[BONE_CHILD].rotation, 1e-5F),
                  "and lands exactly on the destination");
    }

    // ── Nothing to transition from is a cut, not a rig thrown at the origin.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPose target = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.0F, &target);

        nya_skeleton_inertializer_transition(&inertializer, &target, nullptr, 0.2F);
        nya_check(!nya_skeleton_inertializer_active(&inertializer), "a transition with no history does nothing");

        NYA_SkeletonPose pose = target;
        nya_skeleton_inertializer_update(&inertializer, STEP, &pose);

        nya_check(pose.local[BONE_CHILD].translation.x == target.local[BONE_CHILD].translation.x, "and the pose comes through untouched");
    }

    // ── A second transition during the first composes rather than fighting it.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        for (u32 i = 0; i < 3; i++) {
            nya_skeleton_pose_sample(&skeleton, &clip_left, (f32)i * STEP, &pose);
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        NYA_SkeletonPose right = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_right, 0.0F, &right);
        nya_skeleton_inertializer_transition(&inertializer, &right, nullptr, 0.3F);

        // Halfway through, switch back. The offset in flight is what the second capture reads.
        f32 shown = 0.0F;
        for (u32 i = 0; i < 9; i++) {
            pose = right;
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
            shown = pose.local[BONE_CHILD].translation.x;
        }

        NYA_SkeletonPose left = { 0 };
        nya_skeleton_pose_sample(&skeleton, &clip_left, 0.0F, &left);
        nya_skeleton_inertializer_transition(&inertializer, &left, nullptr, 0.2F);

        pose = left;
        nya_skeleton_inertializer_update(&inertializer, STEP, &pose);

        nya_check(fabsf(pose.local[BONE_CHILD].translation.x - shown) < 0.5F, "the second transition starts from what was shown: %f, was %f",
                  (f64)pose.local[BONE_CHILD].translation.x, (f64)shown);

        for (u32 i = 0; i < 40; i++) {
            pose = left;
            nya_skeleton_inertializer_update(&inertializer, STEP, &pose);
        }

        nya_check(fabsf(pose.local[BONE_CHILD].translation.x - left.local[BONE_CHILD].translation.x) < 1e-6F,
                  "and still arrives exactly, %f", (f64)pose.local[BONE_CHILD].translation.x);
    }

    // ── The player drives it, and stops evaluating the outgoing clip when it does.
    {
        nya_skeleton_inertializer_init(&inertializer, &skeleton);

        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        nya_skeleton_player_inertial(&player, &inertializer);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_play(&player, &clip_left, .looping = true);
        for (u32 i = 0; i < 4; i++) nya_skeleton_player_update(&player, STEP, &pose);

        f32 shown_before = pose.local[BONE_CHILD].translation.x;

        nya_skeleton_player_play(&player, &clip_right, .looping = true, .fade_s = 0.25F);
        nya_skeleton_player_update(&player, STEP, &pose);

        nya_check(player.previous.clip == nullptr, "the outgoing clip is dropped, not kept for a crossfade");
        nya_check(nya_skeleton_player_fading(&player), "and the player reports the transition");

        nya_check(fabsf(pose.local[BONE_CHILD].translation.x - shown_before) < 0.5F, "the pose does not jump: %f, was %f",
                  (f64)pose.local[BONE_CHILD].translation.x, (f64)shown_before);

        for (u32 i = 0; i < 40; i++) nya_skeleton_player_update(&player, STEP, &pose);

        nya_check(!nya_skeleton_player_fading(&player), "the transition ends");
    }

    // ── With no inertializer attached the player still crossfades, exactly as before.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_play(&player, &clip_left, .looping = true);
        nya_skeleton_player_update(&player, STEP, &pose);

        nya_skeleton_player_play(&player, &clip_right, .looping = true, .fade_s = 0.25F);
        nya_skeleton_player_update(&player, STEP, &pose);

        nya_check(player.previous.clip == &clip_left, "the outgoing clip is kept for the crossfade");
        nya_check(player.fading, "and the fade is what is running");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
