/**
 * Skinning, from the FBX on disk to the matrix palette a shader would multiply by.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define RIG "./assets/models/bender.fbx"

/** How far a matrix is from the identity, as the largest single element difference. */
static f32 identity_error(f32_4x4 matrix) {
  f32 worst = 0.0F;

  for (u32 row = 0; row < 4; row++) {
    for (u32 column = 0; column < 4; column++) {
      f32 expected = row == column ? 1.0F : 0.0F;
      f32 error    = fabsf(matrix[row][column] - expected);

      if (error > worst) worst = error;
    }
  }

  return worst;
}

/** The largest difference between two poses, over every bone's translation and rotation. */
static f32 pose_error(const NYA_SkeletonPose* a, const NYA_SkeletonPose* b) {
  f32 worst = 0.0F;

  for (u32 bone = 0; bone < a->bone_count; bone++) {
    f32x3 offset = a->local[bone].translation - b->local[bone].translation;
    f32   moved  = nya_vector_length(offset);
    f32   turned = 1.0F - fabsf(nya_quaternion_dot(a->local[bone].rotation, b->local[bone].rotation));

    if (moved > worst) worst = moved;
    if (turned > worst) worst = turned;
  }

  return worst;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // the asset system registers an end-of-frame hook, so events come up first, by hand like the other core tests, since nya_app_init wants a window.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = RIG }));

  // Meshes load on a worker, so wait for it rather than assuming.
  u64 deadline = nya_clock_get_monotonic_ms() + 10000;

  NYA_Asset* asset = nullptr;

  while (nya_clock_get_monotonic_ms() < deadline) {
    // The pump is normally driven by the update event; a test has no loop to raise one.
    NYA_Event tick = { .type = NYA_EVENT_UPDATING_STARTED };
    _nya_asset_loading_process(&tick);

    asset = nya_asset_get(RIG);

    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) break;
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_FAILED) break;
  }

  nya_assert(asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED, "the rig did not load");

  // TEST: the skeleton came out of the file
  printf("TEST: extraction\n");

  const NYA_Skeleton* skeleton = asset->as_mesh.skeleton;

  nya_assert(skeleton != nullptr, "a rigged model produced no skeleton");
  nya_assert(skeleton->bone_count == 2, "expected two bones, got %u", skeleton->bone_count);
  nya_assert(asset->as_mesh.skinned_vertices != nullptr, "a rigged model produced no skinned vertices");

  s32 lower = nya_skeleton_bone_index(skeleton, "lower");
  s32 upper = nya_skeleton_bone_index(skeleton, "upper");

  nya_assert(lower >= 0 && upper >= 0, "the bones are not named as authored");

  // The hierarchy: upper hangs off lower, and lower hangs off nothing.
  nya_assert(skeleton->bones[upper].parent == lower, "'upper' is not parented to 'lower'");
  nya_assert(skeleton->bones[lower].parent == -1, "'lower' should be a root");

  // Parents before children, which nya_skeleton_palette composes in one pass on the strength of.
  for (u32 i = 0; i < skeleton->bone_count; i++) {
    nya_assert(skeleton->bones[i].parent < (s32)i, "bone %u comes before its parent", i);
  }

  printf("  %u bones, %u clips\n", skeleton->bone_count, skeleton->clip_count);
  printf("  PASSED\n");

  // TEST: weights are normalised and in range
  printf("TEST: weights\n");
  {
    u32 blended = 0;

    for (u32 v = 0; v < asset->as_mesh.vertex_count; v++) {
      const NYA_VertexSkinned3D* vertex = &asset->as_mesh.skinned_vertices[v];

      u32 sum        = 0;
      u32 influences = 0;

      for (u32 w = 0; w < NYA_SKELETON_WEIGHTS_PER_VERTEX; w++) {
        nya_assert(vertex->bones[w] < skeleton->bone_count, "vertex %u references bone %u of %u", v, vertex->bones[w],
                   skeleton->bone_count);

        sum += vertex->weights[w];
        if (vertex->weights[w] > 0) influences++;
      }

      // The raw file runs as low as 0.982, so an exact byte sum says both normalisation and quantisation held.
      nya_assert(sum == 255, "vertex %u has weights summing to %u of 255", v, sum);

      if (influences > 1) blended++;
    }

    // the joint is weighted to both bones, so rounding to bytes must not snap every vertex to one.
    nya_assert(blended > 0, "no vertex kept more than one influence");

    printf("  every sum is 255, %u of %u vertices blend two bones or more\n", blended, asset->as_mesh.vertex_count);
    printf("  PASSED\n");
  }

  // TEST: the rig comes out y up, like a static mesh from the same file would
  printf("TEST: axes\n");
  {
    // modelled z up: a bar two metres long and half a metre thick.
    f32x3 low  = asset->as_mesh.positions[0];
    f32x3 high = low;

    for (u32 v = 1; v < asset->as_mesh.vertex_count; v++) {
      f32x3 position = asset->as_mesh.positions[v];

      low  = (f32x3){ nya_min(low.x, position.x), nya_min(low.y, position.y), nya_min(low.z, position.z) };
      high = (f32x3){ nya_max(high.x, position.x), nya_max(high.y, position.y), nya_max(high.z, position.z) };
    }

    f32x3 extent = high - low;

    nya_assert(fabsf(extent.y - 2.0F) < 0.01F, "the bar is %.3f tall on y, so the vertices were not converted", (f64)extent.y);
    nya_assert(extent.x < 0.6F && extent.z < 0.6F, "the bar is %.3f by %.3f across", (f64)extent.x, (f64)extent.z);

    // the root bone converted too: the child sits a metre above it, not beside it.
    NYA_SkeletonPose pose = { 0 };
    nya_skeleton_pose_rest(skeleton, &pose);

    f32_4x4 model[NYA_SKELETON_MAX_BONES];
    nya_skeleton_model_transforms(skeleton, &pose, model);

    f32 rise = model[upper][1][3] - model[lower][1][3];

    nya_assert(fabsf(rise - 1.0F) < 0.01F, "'upper' is %.3f above 'lower'", (f64)rise);

    printf("  extent " FMTf32x3 ", upper %.3f above lower\n", FMTf32x3_ARG(extent), (f64)rise);
    printf("  PASSED\n");
  }

  // TEST: the rest palette is the identity
  printf("TEST: rest pose\n");
  {
    NYA_SkeletonPose pose = { 0 };
    nya_skeleton_pose_rest(skeleton, &pose);

    nya_assert(pose.bone_count == skeleton->bone_count);

    f32_4x4 palette[NYA_SKELETON_MAX_BONES];
    nya_skeleton_palette(skeleton, &pose, palette);

    /* The key assertion here. */
    for (u32 i = 0; i < skeleton->bone_count; i++) {
      f32 error = identity_error(palette[i]);

      nya_assert(error < 0.001F, "bone %u ('%s') is %.6f from identity at rest", i, skeleton->bones[i].name, (f64)error);
    }

    printf("  both bones within 0.001 of identity\n");
    printf("  PASSED\n");
  }

  // TEST: the clip moves the upper bone and returns it
  printf("TEST: clip\n");
  {
    nya_assert(skeleton->clip_count > 0, "no clip was baked");

    const NYA_SkeletonClip* clip = &skeleton->clips[0];

    nya_assert(clip->frame_count > 1, "the clip baked to %u frames", clip->frame_count);
    nya_assert(clip->duration_s > 0.0F, "the clip has no duration");

    NYA_SkeletonPose pose = { 0 };
    f32_4x4          palette[NYA_SKELETON_MAX_BONES];

    // Mid clip, where the rig is bent hardest.
    nya_skeleton_pose_sample(skeleton, clip, clip->duration_s * 0.5F, &pose);
    nya_skeleton_palette(skeleton, &pose, palette);

    f32 moved = identity_error(palette[upper]);

    nya_assert(moved > 0.05F, "the upper bone barely moved mid-clip (%.6f from identity)", (f64)moved);

    // And the root should stay put, since only the upper bone was keyed.
    f32 root_moved = identity_error(palette[lower]);
    nya_assert(root_moved < 0.05F, "the root bone moved (%.6f) but nothing keyed it", (f64)root_moved);

    printf("  mid-clip: upper %.4f from identity, lower %.4f\n", (f64)moved, (f64)root_moved);

    // The clip swings out and back, so the end should look like the start.
    NYA_SkeletonPose ending = { 0 };
    f32_4x4          end_palette[NYA_SKELETON_MAX_BONES];

    nya_skeleton_pose_sample(skeleton, clip, clip->duration_s, &ending);
    nya_skeleton_palette(skeleton, &ending, end_palette);

    nya_assert(identity_error(end_palette[upper]) < 0.1F, "the clip did not return to its start");

    printf("  PASSED\n");
  }

  // TEST: the animator, and a pose written by hand
  printf("TEST: animator and procedural\n");
  {
    NYA_SkeletonAnimator animator = { 0 };
    NYA_SkeletonPose     pose     = { 0 };

    nya_skeleton_animator_play(&animator, skeleton, &skeleton->clips[0], false);

    nya_assert(animator.playing);
    nya_assert(!animator.finished);

    // Past the end, so a non-looping clip has to stop and latch.
    nya_skeleton_animator_update(&animator, skeleton->clips[0].duration_s + 1.0F, &pose);

    nya_assert(!animator.playing, "a non-looping clip kept playing past its end");
    nya_assert(animator.finished, "the clip ended without latching finished");

    // Looping instead: the clock must wrap rather than stick.
    nya_skeleton_animator_play(&animator, skeleton, &skeleton->clips[0], true);
    nya_skeleton_animator_update(&animator, skeleton->clips[0].duration_s * 2.5F, &pose);

    nya_assert(animator.playing, "a looping clip stopped");
    nya_assert(animator.time_s < skeleton->clips[0].duration_s, "the looping clock did not wrap");

    /* The claim that ragdoll and procedural animation need no new feature: a pose is a plain array, so writing a bone directly is the same thing the sampler does, and the palette does not care. */
    NYA_SkeletonPose hand_written = { 0 };
    nya_skeleton_pose_rest(skeleton, &hand_written);

    hand_written.local[upper].rotation = nya_quaternion_from_axis_angle((f32x3){ 1.0F, 0.0F, 0.0F }, 1.0F);

    f32_4x4 palette[NYA_SKELETON_MAX_BONES];
    nya_skeleton_palette(skeleton, &hand_written, palette);

    nya_assert(identity_error(palette[upper]) > 0.05F, "writing a bone by hand did not move it");
    nya_assert(identity_error(palette[lower]) < 0.001F, "writing one bone moved another");

    printf("  PASSED\n");
  }

  // TEST: a pose drawn between ticks samples the clip between the clock's two times
  printf("TEST: render pose between ticks\n");
  {
    const NYA_SkeletonClip* clip     = &skeleton->clips[0];
    f32                     duration = clip->duration_s;

    NYA_SkeletonAnimator animator = { 0 };
    NYA_SkeletonPose     drawn    = { 0 };
    NYA_SkeletonPose     expected = { 0 };

    nya_skeleton_animator_play(&animator, skeleton, clip, true);

    // a null pose only advances the clock.
    nya_skeleton_animator_update(&animator, duration * 0.2F, nullptr);
    nya_skeleton_animator_update(&animator, duration * 0.2F, nullptr);

    nya_assert(fabsf(animator.time_s - (duration * 0.4F)) < 0.0001F, "the clock advanced without a pose");
    nya_assert(fabsf(animator.time_previous_s - (duration * 0.2F)) < 0.0001F, "and kept where the tick began");

    // halfway to the next tick.
    _NYA_APP_INSTANCE.options.time_step_ns       = 16'000'000;
    _NYA_APP_INSTANCE.frame_stats.time_behind_ns = 8'000'000;

    nya_skeleton_animator_render_pose(&animator, &drawn);
    nya_skeleton_pose_sample(skeleton, clip, duration * 0.3F, &expected);

    nya_assert(pose_error(&drawn, &expected) < 0.001F, "halfway between ticks draws the clip halfway, off by %f", (f64)pose_error(&drawn, &expected));

    // a tick that wraps the loop, from 90% to 10%: a quarter of the way is 95%, not back through the middle.
    nya_skeleton_animator_play(&animator, skeleton, clip, true);
    nya_skeleton_animator_update(&animator, duration * 0.9F, nullptr);
    nya_skeleton_animator_update(&animator, duration * 0.2F, nullptr);

    _NYA_APP_INSTANCE.frame_stats.time_behind_ns = 4'000'000;

    nya_skeleton_animator_render_pose(&animator, &drawn);
    nya_skeleton_pose_sample(skeleton, clip, duration * 0.95F, &expected);

    nya_assert(pose_error(&drawn, &expected) < 0.001F, "a wrapped tick draws across the seam, off by %f", (f64)pose_error(&drawn, &expected));

    // a clip that just started draws its first frame, never the previous clip's time.
    nya_skeleton_animator_play(&animator, skeleton, clip, false);

    nya_skeleton_animator_render_pose(&animator, &drawn);
    nya_skeleton_pose_sample(skeleton, clip, 0.0F, &expected);

    nya_assert(pose_error(&drawn, &expected) < 0.001F, "a fresh clip draws from its start");

    _NYA_APP_INSTANCE.options.time_step_ns       = 0;
    _NYA_APP_INSTANCE.frame_stats.time_behind_ns = 0;

    printf("  PASSED\n");
  }

  // TEST: nya_skeleton_bone_model agrees with nya_skeleton_model_transforms
  {
    printf("TEST: the one-bone socket walk matches the whole-rig pass\n");

    NYA_SkeletonPose pose;
    nya_skeleton_pose_rest(skeleton, &pose);

    // Moved, so the comparison is not two identities agreeing with each other.
    pose.local[upper].translation = (f32x3){ 0.3F, 1.1F, -0.4F };
    pose.local[upper].rotation    = nya_quaternion_from_axis_angle((f32x3){ 0.0F, 1.0F, 0.0F }, 0.7F);

    f32_4x4 model[NYA_SKELETON_MAX_BONES];
    nya_skeleton_model_transforms(skeleton, &pose, model);

    for (u32 bone = 0; bone < skeleton->bone_count; bone++) {
      f32_4x4 one;
      nya_assert(nya_skeleton_bone_model(skeleton, &pose, (s32)bone, &one), "bone %u should resolve", bone);

      for (u32 row = 0; row < 4; row++) {
        for (u32 column = 0; column < 4; column++) {
          f32 difference = fabsf(one[row][column] - model[bone][row][column]);
          nya_assert(difference < 0.0005F, "bone %u disagreed at [%u][%u] by %f", bone, row, column, (f64)difference);
        }
      }
    }

    // A bone that does not exist is refused rather than answered with whatever was in the slot.
    f32_4x4 untouched = model[0];
    f32_4x4 refused   = untouched;
    nya_assert(!nya_skeleton_bone_model(skeleton, &pose, -1, &refused), "a negative bone index should be refused");
    nya_assert(!nya_skeleton_bone_model(skeleton, &pose, (s32)skeleton->bone_count, &refused), "a bone past the end should be refused");

    printf("  PASSED\n");
  }

  printf("PASSED: test_skeleton\n");

  return 0;
}
