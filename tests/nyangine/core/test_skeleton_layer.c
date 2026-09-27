/**
 * Crossfades, bone masks, animation events and two-bone IK.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

enum { BONE_ROOT = 0, BONE_MID = 1, BONE_END = 2, BONE_COUNT = 3 };

#define UPPER 2.0F
#define LOWER 1.5F

static NYA_SkeletonBone bones[BONE_COUNT];
static NYA_Skeleton     skeleton;

/** Two clips over the same rig: one holding the rest pose, one rotating the root a quarter turn. */
static NYA_BoneTransform rest_frames[BONE_COUNT * 2];
static NYA_BoneTransform bend_frames[BONE_COUNT * 2];
static NYA_SkeletonClip  clip_rest;
static NYA_SkeletonClip  clip_bend;

/** A clip that moves over time: the root walks along x while the middle and the end turn. */
static NYA_BoneTransform swing_frames[BONE_COUNT * 2];
static NYA_SkeletonClip  clip_swing;

static void rig_build(void) {
    bones[BONE_ROOT] = (NYA_SkeletonBone){ .parent = -1, .rest = { .translation = { 0, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };
    bones[BONE_MID]  = (NYA_SkeletonBone){ .parent = BONE_ROOT, .rest = { .translation = { UPPER, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };
    bones[BONE_END]  = (NYA_SkeletonBone){ .parent = BONE_MID, .rest = { .translation = { LOWER, 0, 0 }, .rotation = nya_quaternion_identity, .scale = { 1, 1, 1 } } };

    (void)snprintf(bones[BONE_ROOT].name, sizeof(bones[BONE_ROOT].name), "root");
    (void)snprintf(bones[BONE_MID].name, sizeof(bones[BONE_MID].name), "mid");
    (void)snprintf(bones[BONE_END].name, sizeof(bones[BONE_END].name), "end");

    skeleton = (NYA_Skeleton){ .bones = bones, .bone_count = BONE_COUNT };

    for (u32 f = 0; f < 2; f++) {
        for (u32 b = 0; b < BONE_COUNT; b++) {
            rest_frames[(f * BONE_COUNT) + b] = bones[b].rest;
            bend_frames[(f * BONE_COUNT) + b] = bones[b].rest;
        }
        // The bend clip turns the root a quarter turn about z, so the two clips visibly disagree.
        bend_frames[(f * BONE_COUNT) + BONE_ROOT].rotation = nya_quaternion_from_axis_angle((f32x3){ 0, 0, 1 }, 1.5707963F);
    }

    for (u32 b = 0; b < BONE_COUNT; b++) {
        swing_frames[b]              = bones[b].rest;
        swing_frames[BONE_COUNT + b] = bones[b].rest;
    }
    swing_frames[BONE_COUNT + BONE_ROOT].translation = (f32x3){ 1.0F, 0.0F, 0.0F };
    swing_frames[BONE_COUNT + BONE_MID].rotation     = nya_quaternion_from_axis_angle((f32x3){ 0, 0, 1 }, 1.5707963F);
    swing_frames[BONE_COUNT + BONE_END].rotation     = nya_quaternion_from_axis_angle((f32x3){ 1, 0, 0 }, 1.0F);

    clip_swing = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = swing_frames };
    (void)snprintf(clip_swing.name, sizeof(clip_swing.name), "swing");

    clip_rest = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = rest_frames };
    clip_bend = (NYA_SkeletonClip){ .duration_s = 1.0F, .frame_count = 2, .frame_rate = 1.0F, .frames = bend_frames };

    (void)snprintf(clip_rest.name, sizeof(clip_rest.name), "rest");
    (void)snprintf(clip_bend.name, sizeof(clip_bend.name), "bend");
}

/** The world position of a bone under a pose, mirroring what the solver composes internally. */
static f32x3 world_of(const NYA_SkeletonPose* pose, s32 bone) {
    f32x3 position = { 0, 0, 0 };
    for (s32 walk = bone; walk >= 0; walk = bones[walk].parent) {
        position = pose->local[walk].translation + nya_quaternion_rotate(pose->local[walk].rotation, position * pose->local[walk].scale);
    }
    return position;
}

/** The largest difference between two poses, over every bone's translation and rotation. */
static f32 pose_error(const NYA_SkeletonPose* a, const NYA_SkeletonPose* b) {
    f32 worst = a->bone_count == b->bone_count ? 0.0F : 1.0F;

    for (u32 i = 0; i < a->bone_count && i < b->bone_count; i++) {
        worst = nya_max(worst, nya_vector_length(a->local[i].translation - b->local[i].translation));
        // componentwise rather than by angle, which acos turns rounding into a milliradian of; q and -q are one rotation.
        NYA_Quaternion p    = a->local[i].rotation;
        NYA_Quaternion q    = b->local[i].rotation;
        f32            sign = nya_quaternion_dot(p, q) < 0.0F ? -1.0F : 1.0F;
        f32x4          gap  = { p.x - (sign * q.x), p.y - (sign * q.y), p.z - (sign * q.z), p.w - (sign * q.w) };

        worst = nya_max(worst, sqrtf((gap.x * gap.x) + (gap.y * gap.y) + (gap.z * gap.z) + (gap.w * gap.w)));
    }

    return worst;
}

/** Puts the frame `alpha` of the way from the last tick to the next, as the app's clock would. */
static void alpha_set(f32 alpha) {
    _NYA_APP_INSTANCE.options.time_step_ns       = 16'000'000;
    _NYA_APP_INSTANCE.frame_stats.time_behind_ns = (s64)(alpha * 16'000'000.0F);
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    rig_build();

    // Masks: fill, carve from a bone, and the missing-bone case being reported.
    {
        NYA_SkeletonMask mask = { 0 };

        nya_skeleton_mask_fill(&skeleton, 1.0F, &mask);
        nya_check(mask.bone_count == BONE_COUNT, "a filled mask covers the skeleton");
        nya_check(mask.weights[BONE_ROOT] == 1.0F && mask.weights[BONE_END] == 1.0F, "at the weight asked for");

        nya_check(nya_skeleton_mask_from_bone(&skeleton, "mid", &mask), "a real bone should build a mask");
        nya_check(mask.weights[BONE_ROOT] == 0.0F, "the root is above 'mid' and must be excluded");
        nya_check(mask.weights[BONE_MID] == 1.0F, "'mid' itself is included");
        nya_check(mask.weights[BONE_END] == 1.0F, "and everything below it");

        // Reported, not an empty mask: a layer through an all-zero mask silently does nothing, which is far harder to trace back to a misspelled bone name.
        nya_check(!nya_skeleton_mask_from_bone(&skeleton, "no such bone", &mask), "a missing bone must be reported");

        nya_skeleton_mask_set(&skeleton, &mask, BONE_MID, 0.5F, false);
        nya_check(mask.weights[BONE_MID] == 0.5F, "a single bone can be set");
        nya_check(mask.weights[BONE_END] == 1.0F, "without touching its children");
    }

    // Masked blending only moves the bones the mask names.
    {
        NYA_SkeletonPose from = { .bone_count = BONE_COUNT };
        NYA_SkeletonPose to   = { .bone_count = BONE_COUNT };
        for (u32 b = 0; b < BONE_COUNT; b++) {
            from.local[b] = bones[b].rest;
            to.local[b]   = bones[b].rest;
            to.local[b].translation = (f32x3){ 99.0F, 99.0F, 99.0F };
        }

        NYA_SkeletonMask mask = { 0 };
        nya_skeleton_mask_fill(&skeleton, 0.0F, &mask);
        nya_skeleton_mask_set(&skeleton, &mask, BONE_END, 1.0F, false);

        NYA_SkeletonPose out = { 0 };
        nya_skeleton_pose_blend_masked(&from, &to, 1.0F, &mask, &out);

        nya_check(out.local[BONE_ROOT].translation.x == from.local[BONE_ROOT].translation.x, "a zero-weight bone is untouched");
        nya_check(out.local[BONE_MID].translation.x == from.local[BONE_MID].translation.x, "and so is the middle");
        nya_check(fabsf(out.local[BONE_END].translation.x - 99.0F) < 0.001F, "the masked bone takes the new value");

        // A null mask must behave like the unmasked blend, since that is what layers with no mask rely on.
        nya_skeleton_pose_blend_masked(&from, &to, 1.0F, nullptr, &out);
        nya_check(fabsf(out.local[BONE_ROOT].translation.x - 99.0F) < 0.001F, "a null mask blends every bone");
    }

    // A player with no clip yields the rest pose rather than garbage.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 1.0F / 60.0F, &pose);

        nya_check(pose.bone_count == BONE_COUNT, "an idle player still produces a pose");
        nya_check(fabsf(pose.local[BONE_MID].translation.x - UPPER) < 0.001F, "and it is the rest pose");
    }

    // Crossfade: the pose moves from one clip to the other and the fade ends.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);

        nya_skeleton_player_play(&player, &clip_rest, .looping = true);

        NYA_SkeletonPose pose = { 0 };
        nya_skeleton_player_update(&player, 1.0F / 60.0F, &pose);
        nya_check(!nya_skeleton_player_fading(&player), "the first clip does not fade from nothing");

        f32x3 before = world_of(&pose, BONE_END);

        nya_skeleton_player_play(&player, &clip_bend, .looping = true, .fade_s = 0.25F);
        nya_check(nya_skeleton_player_fading(&player), "switching with a fade should start one");

        // Halfway: the end bone must be somewhere between the two clips' answers.
        for (u32 i = 0; i < 7; i++) nya_skeleton_player_update(&player, 1.0F / 60.0F, &pose);
        f32x3 midway = world_of(&pose, BONE_END);
        nya_check(nya_vector_length(midway - before) > 0.01F, "it should have moved off the first pose");
        nya_check(nya_skeleton_player_fading(&player), "and still be fading");

        for (u32 i = 0; i < 30; i++) nya_skeleton_player_update(&player, 1.0F / 60.0F, &pose);
        nya_check(!nya_skeleton_player_fading(&player), "the fade should finish");

        f32x3 after = world_of(&pose, BONE_END);
        nya_check(nya_vector_length(after - midway) > 0.01F, "and it should have kept going to the new clip");

        // Re-playing the current clip is a no-op, so a per-frame state check does not restart it.
        nya_skeleton_player_play(&player, &clip_bend, .looping = true, .fade_s = 0.25F);
        nya_check(!nya_skeleton_player_fading(&player), "replaying the current clip should not start a fade");
    }

    // Events fire once when crossed, and a loop does not swallow one near the end.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);

        static const NYA_SkeletonEvent events[] = {
            { .time_s = 0.25F, .id = 100 },
            { .time_s = 0.95F, .id = 200 },
        };
        nya_skeleton_player_events(&player, events, nya_carray_length(events));

        nya_skeleton_player_play(&player, &clip_rest, .looping = true);

        u32 first  = 0;
        u32 second = 0;
        u32 loops  = 0;

        for (u32 i = 0; i < 180; i++) {
            nya_skeleton_player_update(&player, 1.0F / 60.0F, &(NYA_SkeletonPose){ 0 });

            for (u32 s = 0; s < player.signal_count; s++) {
                if (player.signals[s].kind == NYA_SKELETON_SIGNAL_LOOPED) loops++;
                if (player.signals[s].kind != NYA_SKELETON_SIGNAL_EVENT) continue;
                if (player.signals[s].id == 100) first++;
                if (player.signals[s].id == 200) second++;
            }
        }

        // Three seconds of a one second loop.
        nya_check(loops >= 2, "the clip should have looped, got %u", loops);
        nya_check(first >= 2 && first <= 4, "the early event should fire once per loop, got %u", first);
        nya_check(second >= 2 && second <= 4, "and so should the one at 0.95s, got %u", second);
    }

    // An event on the start point fires on the first update, and backward playback does not report a loop every frame. Quarter second steps, so every crossing is exact.
    {
        static const NYA_SkeletonEvent events[] = {
            { .time_s = 0.0F, .id = 1 },
            { .time_s = 0.5F, .id = 2 },
        };

        for (u32 direction = 0; direction < 2; direction++) {
            NYA_SkeletonPlayer player = { 0 };
            nya_skeleton_player_init(&player, &skeleton);
            nya_skeleton_player_events(&player, events, nya_carray_length(events));
            nya_skeleton_player_play(&player, &clip_rest, .looping = true, .speed = direction == 0 ? 1.0F : -1.0F);

            u32 at_start = 0;
            u32 at_half  = 0;
            u32 loops    = 0;

            for (u32 i = 0; i < 8; i++) {
                nya_skeleton_player_update(&player, 0.25F, &(NYA_SkeletonPose){ 0 });

                for (u32 s = 0; s < player.signal_count; s++) {
                    if (player.signals[s].kind == NYA_SKELETON_SIGNAL_LOOPED) loops++;
                    if (player.signals[s].kind == NYA_SKELETON_SIGNAL_EVENT && player.signals[s].id == 1) at_start++;
                    if (player.signals[s].kind == NYA_SKELETON_SIGNAL_EVENT && player.signals[s].id == 2) at_half++;
                }
            }

            // forward: 0 at the start and at both wraps. backward from the end: 0 as the clock reaches it, twice, and one wrap between.
            u32 want_start = direction == 0 ? 3 : 2;
            u32 want_loops = direction == 0 ? 2 : 1;

            nya_check(at_start == want_start, "direction %u: the event at 0 fired %u times, want %u", direction, at_start, want_start);
            nya_check(at_half == 2, "direction %u: the event at 0.5 fired %u times, want 2", direction, at_half);
            nya_check(loops == want_loops, "direction %u: %u loops reported, want %u", direction, loops, want_loops);
        }
    }

    // A non-looping clip reports finishing, once.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        nya_skeleton_player_play(&player, &clip_rest, .looping = false);

        u32 finished = 0;
        for (u32 i = 0; i < 180; i++) {
            nya_skeleton_player_update(&player, 1.0F / 60.0F, &(NYA_SkeletonPose){ 0 });
            for (u32 s = 0; s < player.signal_count; s++) {
                if (player.signals[s].kind == NYA_SKELETON_SIGNAL_FINISHED) finished++;
            }
        }
        nya_check(finished == 1, "finishing should be reported exactly once, got %u", finished);
    }

    // Layers stack over the base, through their mask.
    {
        NYA_SkeletonPlayer player = { 0 };
        nya_skeleton_player_init(&player, &skeleton);
        nya_skeleton_player_play(&player, &clip_rest, .looping = true);

        NYA_SkeletonPose base = { 0 };
        nya_skeleton_player_update(&player, 1.0F / 60.0F, &base);
        f32x3 unlayered = world_of(&base, BONE_END);

        NYA_SkeletonMask mask = { 0 };
        nya_check(nya_skeleton_mask_from_bone(&skeleton, "mid", &mask), "mask built");

        nya_check(nya_skeleton_player_layer(&player, 0, &clip_bend, &mask, 1.0F, true), "a layer should start");

        NYA_SkeletonPose layered = { 0 };
        nya_skeleton_player_update(&player, 1.0F / 60.0F, &layered);

        // the layer's clip rotates the root, which the mask excludes, so the layer must change nothing.
        nya_check(nya_vector_length(world_of(&layered, BONE_END) - unlayered) < 0.001F,
                  "a layer masked away from the bone its clip moves should change nothing");

        // Widen the mask to include the root and it must now take effect.
        nya_skeleton_mask_set(&skeleton, &mask, BONE_ROOT, 1.0F, false);
        nya_skeleton_player_update(&player, 1.0F / 60.0F, &layered);
        nya_check(nya_vector_length(world_of(&layered, BONE_END) - unlayered) > 0.01F, "and with the root in the mask it should");

        nya_skeleton_player_layer_weight(&player, 0, 0.0F);
        nya_skeleton_player_update(&player, 1.0F / 60.0F, &layered);
        nya_check(nya_vector_length(world_of(&layered, BONE_END) - unlayered) < 0.001F, "a zero-weight layer is inert");

        nya_skeleton_player_layer_stop(&player, 0);
        nya_check(!nya_skeleton_player_layer(&player, NYA_SKELETON_LAYERS, &clip_bend, nullptr, 1.0F, true),
                  "a slot past the end must be refused");
    }

    // Two-bone IK: a reachable target is reached.
    {
        NYA_SkeletonPose pose = { .bone_count = BONE_COUNT };
        for (u32 b = 0; b < BONE_COUNT; b++) pose.local[b] = bones[b].rest;

        // Well inside reach (UPPER + LOWER = 3.5), and off-axis so the limb has to bend.
        f32x3 target = { 2.0F, 1.5F, 0.0F };
        f32x3 pole   = { 1.0F, 4.0F, 0.0F };

        nya_check(nya_skeleton_ik_two_bone(&skeleton, &pose, BONE_ROOT, BONE_MID, BONE_END, target, pole), "the solve should succeed");

        f32x3 reached = world_of(&pose, BONE_END);
        nya_check(nya_vector_length(reached - target) < 0.01F, "the end bone should land on the target, off by %f",
                  (f64)nya_vector_length(reached - target));

        // Bone lengths are rotations only and must not have changed.
        f32x3 root_pos = world_of(&pose, BONE_ROOT);
        f32x3 mid_pos  = world_of(&pose, BONE_MID);
        nya_check(fabsf(nya_vector_length(mid_pos - root_pos) - UPPER) < 0.01F, "the upper bone must keep its length");
        nya_check(fabsf(nya_vector_length(reached - mid_pos) - LOWER) < 0.01F, "and so must the lower");
    }

    // An unreachable target straightens the limb toward it rather than failing.
    {
        NYA_SkeletonPose pose = { .bone_count = BONE_COUNT };
        for (u32 b = 0; b < BONE_COUNT; b++) pose.local[b] = bones[b].rest;

        f32x3 target = { 0.0F, 20.0F, 0.0F };
        nya_check(nya_skeleton_ik_two_bone(&skeleton, &pose, BONE_ROOT, BONE_MID, BONE_END, target, (f32x3){ 1, 1, 0 }),
                  "an out-of-reach target is not an error");

        f32x3 reached = world_of(&pose, BONE_END);
        f32   extent  = nya_vector_length(reached);

        nya_check(extent > (UPPER + LOWER) * 0.99F, "the limb should be nearly straight, at %f of %f", (f64)extent, (f64)(UPPER + LOWER));
        nya_check(reached.y > 3.0F, "and pointing at the target, y=%f", (f64)reached.y);
    }

    // Malformed chains are refused.
    {
        NYA_SkeletonPose pose = { .bone_count = BONE_COUNT };
        for (u32 b = 0; b < BONE_COUNT; b++) pose.local[b] = bones[b].rest;

        f32x3 target = { 1.0F, 1.0F, 0.0F };
        f32x3 pole   = { 0.0F, 1.0F, 0.0F };

        nya_check(!nya_skeleton_ik_two_bone(&skeleton, &pose, BONE_END, BONE_MID, BONE_ROOT, target, pole), "a reversed chain is refused");
        nya_check(!nya_skeleton_ik_two_bone(&skeleton, &pose, BONE_ROOT, BONE_END, BONE_MID, target, pole), "a scrambled chain is refused");
        nya_check(!nya_skeleton_ik_two_bone(&skeleton, &pose, -1, BONE_MID, BONE_END, target, pole), "a negative bone is refused");
        nya_check(!nya_skeleton_ik_two_bone(&skeleton, &pose, 0, 1, 99, target, pole), "a bone past the pose is refused");
        nya_check(!nya_skeleton_ik_two_bone(nullptr, &pose, 0, 1, 2, target, pole), "a null skeleton is refused");
    }

    // Drawn between ticks: alpha zero is the tick before, one the tick just run, through a crossfade, a layer, root
    // motion and an inertializer alike.
    {
        static NYA_SkeletonInertializer inertializer;

        NYA_SkeletonMask end_only = { 0 };
        nya_check(nya_skeleton_mask_from_bone(&skeleton, "end", &end_only), "the end bone builds a mask");

        for (u32 inertial = 0; inertial < 2; inertial++) {
            NYA_SkeletonPlayer player = { 0 };
            nya_skeleton_player_init(&player, &skeleton);
            nya_check(nya_skeleton_player_root_motion(&player, "root", (f32x3){ 1, 0, 1 }, false), "root motion turns on");
            nya_check(nya_skeleton_player_layer(&player, 0, &clip_swing, &end_only, 0.5F, true), "a layer starts");

            if (inertial == 1) {
                nya_skeleton_inertializer_init(&inertializer, &skeleton);
                nya_skeleton_player_inertial(&player, &inertializer);
            }

            NYA_SkeletonPose before = { 0 };
            NYA_SkeletonPose after  = { 0 };
            NYA_SkeletonPose drawn  = { 0 };

            nya_skeleton_player_play(&player, &clip_bend, .looping = true);
            nya_skeleton_player_update(&player, 0.1F, &before);

            nya_skeleton_player_play(&player, &clip_swing, .looping = true, .fade_s = 0.5F);
            nya_skeleton_player_update(&player, 0.1F, &before);
            nya_skeleton_player_update(&player, 0.1F, &after);

            nya_check(nya_skeleton_player_fading(&player), "still mid transition, pass %u", inertial);
            nya_check(pose_error(&before, &after) > 0.01F, "the two ticks differ, pass %u", inertial);

            alpha_set(0.0F);
            nya_skeleton_player_render_pose(&player, &drawn);
            nya_check(pose_error(&drawn, &before) < 0.0001F, "alpha zero draws the tick before, off by %f, pass %u", (f64)pose_error(&drawn, &before), inertial);

            alpha_set(1.0F);
            nya_skeleton_player_render_pose(&player, &drawn);
            nya_check(pose_error(&drawn, &after) < 0.0001F, "alpha one draws the tick just run, off by %f, pass %u", (f64)pose_error(&drawn, &after), inertial);

            alpha_set(0.5F);
            nya_skeleton_player_render_pose(&player, &drawn);
            nya_check(pose_error(&drawn, &before) > 0.001F && pose_error(&drawn, &after) > 0.001F, "halfway is neither tick, pass %u", inertial);
            nya_check(nya_vector_length(drawn.local[BONE_ROOT].translation - bones[BONE_ROOT].rest.translation) < 0.0001F,
                      "the root stays pinned between ticks, pass %u", inertial);
        }

        _NYA_APP_INSTANCE.options.time_step_ns       = 0;
        _NYA_APP_INSTANCE.frame_stats.time_behind_ns = 0;
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
