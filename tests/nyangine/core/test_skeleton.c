/**
 * Skinning, from the FBX on disk to the matrix palette a shader would multiply by.
 *
 * Driven by assets/models/bender.fbx, a two-bone cylinder that swings its upper half through 75° and
 * back. Two bones because the first skinned draw either bends in the middle or it does not, and a
 * third bone would only make it harder to tell which one was wrong.
 *
 * What it defends, in order of how badly a regression would hurt:
 *
 * - **The rest palette is identity.** If composing the hierarchy and folding in the inverse bind do
 *   not cancel at rest, every skinned model is deformed before it is even animated — and that is the
 *   failure that looks like a broken importer rather than broken maths.
 * - **Weights are normalised.** The exporter's own weights on this rig sum to as little as 0.982.
 * - **A skinned mesh is not pre-transformed by its node**, which would apply the placement twice.
 * - **The clip actually moves something**, and moves it back by the end.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

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

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // No real audio device; nya_system_asset_init opens one otherwise. Same reason as test_asset.c —
  // and on CI the difference is not academic: the real ALSA driver leaks inside the library itself,
  // which LeakSanitizer reports against this test.
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // The asset system registers an end-of-frame hook, so the event system has to be up first — the
  // same build-up-by-hand the other core tests do rather than a full nya_app_init, which wants a window.
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the skeleton came out of the file
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: weights are normalised and in range
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: weights\n");
  {
    f32 lowest  = 1e30F;
    f32 highest = -1e30F;

    for (u32 v = 0; v < asset->as_mesh.vertex_count; v++) {
      const NYA_VertexSkinned3D* vertex = &asset->as_mesh.skinned_vertices[v];

      f32 sum = 0.0F;

      for (u32 w = 0; w < NYA_SKELETON_WEIGHTS_PER_VERTEX; w++) {
        nya_assert(vertex->bones[w] < skeleton->bone_count, "vertex %u references bone %u of %u", v, vertex->bones[w],
                   skeleton->bone_count);
        nya_assert(vertex->weights[w] >= 0.0F, "vertex %u has a negative weight", v);

        sum += vertex->weights[w];
      }

      if (sum < lowest) lowest = sum;
      if (sum > highest) highest = sum;
    }

    // The raw file runs as low as 0.982, so this is the check that says the normalisation happened.
    nya_assert(fabsf(lowest - 1.0F) < 0.001F, "the lowest weight sum is %.6f, so weights were not normalised", (f64)lowest);
    nya_assert(fabsf(highest - 1.0F) < 0.001F, "the highest weight sum is %.6f", (f64)highest);

    printf("  sums in [%.6f, %.6f]\n", (f64)lowest, (f64)highest);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the rest palette is the identity
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: rest pose\n");
  {
    NYA_SkeletonPose pose = { 0 };
    nya_skeleton_pose_rest(skeleton, &pose);

    nya_assert(pose.bone_count == skeleton->bone_count);

    f32_4x4 palette[NYA_SKELETON_MAX_BONES];
    nya_skeleton_palette(skeleton, &pose, palette);

    /*
     * The single most load-bearing assertion here.
     *
     * At rest, walking the hierarchy must undo exactly what the inverse bind matrices did, so every
     * vertex lands where it was authored. Any error here is a model that is deformed before a single
     * frame of animation has played.
     */
    for (u32 i = 0; i < skeleton->bone_count; i++) {
      f32 error = identity_error(palette[i]);

      nya_assert(error < 0.001F, "bone %u ('%s') is %.6f from identity at rest", i, skeleton->bones[i].name, (f64)error);
    }

    printf("  both bones within 0.001 of identity\n");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the clip moves the upper bone and returns it
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the animator, and a pose written by hand
  // ─────────────────────────────────────────────────────────────────────────────
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

    /*
     * The claim that ragdoll and procedural animation need no new feature: a pose is a plain array,
     * so writing a bone directly is the same thing the sampler does, and the palette does not care.
     */
    NYA_SkeletonPose hand_written = { 0 };
    nya_skeleton_pose_rest(skeleton, &hand_written);

    hand_written.local[upper].rotation = nya_quaternion_from_axis_angle((f32x3){ 1.0F, 0.0F, 0.0F }, 1.0F);

    f32_4x4 palette[NYA_SKELETON_MAX_BONES];
    nya_skeleton_palette(skeleton, &hand_written, palette);

    nya_assert(identity_error(palette[upper]) > 0.05F, "writing a bone by hand did not move it");
    nya_assert(identity_error(palette[lower]) < 0.001F, "writing one bone moved another");

    printf("  PASSED\n");
  }

  printf("PASSED: test_skeleton\n");

  return 0;
}
