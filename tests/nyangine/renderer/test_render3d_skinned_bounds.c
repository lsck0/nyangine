/**
 * The sphere a posed mesh is culled by: nya_render3d_skinned_bounds.
 *
 * Until this existed the skinned draw had no bounds at all, so it was recorded for the camera and for
 * every shadow cascade whatever the pose was, and drawn whether or not it was near any of them.
 *
 * Bounds are the one part of that worth testing away from a GPU, and the one part that can be quietly
 * wrong: read the translation out of the wrong place in the matrix and the sphere sits at the origin,
 * which culls nothing near the middle of a scene and everything at its edges.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A bone at `position` with no rotation and no scale. */
static f32_4x4 bone_at(f32x3 position) {
  return nya_matrix_transform(position, f32_3x3_id, (f32x3){ 1.0F, 1.0F, 1.0F });
}

/** Whether `point` is inside the sphere, which is the only promise these bounds make. */
static b8 covers(f32x3 center, f32 radius, f32x3 point) { return nya_vector_length(point - center) <= radius; }

s32 main(void) {
  const f32x3 rest_min = { -0.5F, 0.0F, -0.5F };
  const f32x3 rest_max = { 0.5F, 2.0F, 0.5F };

  // TEST: nothing to bound is refused rather than answered with a sphere at the
  //       origin, which would be culled against and would be wrong
  {
    f32x3 center = { 9.0F, 9.0F, 9.0F };
    f32   radius = 9.0F;

    const f32_4x4 one = bone_at(f32x3_zero);

    nya_check(!nya_render3d_skinned_bounds(nullptr, 1, f32_4x4_id, rest_min, rest_max, &center, &radius), "no palette has no bounds");
    nya_check(!nya_render3d_skinned_bounds(&one, 0, f32_4x4_id, rest_min, rest_max, &center, &radius), "and neither does no bones");

    printf("  PASSED\n");
  }

  // TEST: the sphere sits on the bones, not on the origin
  {
    // Well away from the origin, so reading the translation from the wrong place cannot pass by luck.
    const f32_4x4 palette[2] = { bone_at((f32x3){ 10.0F, 4.0F, -6.0F }), bone_at((f32x3){ 12.0F, 4.0F, -6.0F }) };

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    nya_check(nya_render3d_skinned_bounds(palette, 2, f32_4x4_id, rest_min, rest_max, &center, &radius), "two bones have bounds");

    nya_check(fabsf(center.x - 11.0F) < 0.001F, "the centre is between the bones on x, got %f", (f64)center.x);
    nya_check(fabsf(center.y - 4.0F) < 0.001F, "and on y, got %f", (f64)center.y);
    nya_check(fabsf(center.z + 6.0F) < 0.001F, "and on z, got %f", (f64)center.z);

    // Both bones inside, and the skin that hangs off them with it.
    nya_check(covers(center, radius, (f32x3){ 10.0F, 4.0F, -6.0F }), "the first bone is inside");
    nya_check(covers(center, radius, (f32x3){ 12.0F, 4.0F, -6.0F }), "and so is the second");
    nya_check(covers(center, radius, (f32x3){ 10.0F, 5.0F, -6.0F }), "and so is a vertex a metre above one of them");

    printf("  PASSED\n");
  }

  // TEST: the model transform moves the bones, since the palette is posed
  //       through it
  {
    const f32_4x4 palette[1] = { bone_at(f32x3_zero) };
    const f32_4x4 model      = nya_matrix_transform((f32x3){ 100.0F, 0.0F, 0.0F }, f32_3x3_id, (f32x3){ 1.0F, 1.0F, 1.0F });

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    nya_check(nya_render3d_skinned_bounds(palette, 1, model, rest_min, rest_max, &center, &radius), "one bone still has bounds");
    nya_check(fabsf(center.x - 100.0F) < 0.001F, "a moved model moves the sphere with it, got %f", (f64)center.x);

    printf("  PASSED\n");
  }

  // TEST: a scaled model grows the padding, or the skin on a scaled up mesh
  //       would hang outside its own bounds and lose a limb to a cascade
  {
    const f32_4x4 palette[1] = { bone_at(f32x3_zero) };

    f32x3 plain_center  = f32x3_zero;
    f32   plain_radius  = 0.0F;
    f32x3 scaled_center = f32x3_zero;
    f32   scaled_radius = 0.0F;

    nya_check(nya_render3d_skinned_bounds(palette, 1, f32_4x4_id, rest_min, rest_max, &plain_center, &plain_radius), "unscaled");

    const f32_4x4 scaled = nya_matrix_transform(f32x3_zero, f32_3x3_id, (f32x3){ 3.0F, 3.0F, 3.0F });
    nya_check(nya_render3d_skinned_bounds(palette, 1, scaled, rest_min, rest_max, &scaled_center, &scaled_radius), "scaled");

    nya_check(fabsf(scaled_radius - (plain_radius * 3.0F)) < 0.01F, "three times the model is three times the radius, got %f against %f",
              (f64)scaled_radius, (f64)(plain_radius * 3.0F));

    // A non-uniform scale takes its longest axis, since the sphere has to cover that one too.
    const f32_4x4 stretched = nya_matrix_transform(f32x3_zero, f32_3x3_id, (f32x3){ 1.0F, 5.0F, 1.0F });

    f32x3 stretched_center = f32x3_zero;
    f32   stretched_radius = 0.0F;
    nya_check(nya_render3d_skinned_bounds(palette, 1, stretched, rest_min, rest_max, &stretched_center, &stretched_radius), "stretched");

    nya_check(fabsf(stretched_radius - (plain_radius * 5.0F)) < 0.01F, "the longest axis decides, got %f against %f", (f64)stretched_radius,
              (f64)(plain_radius * 5.0F));

    printf("  PASSED\n");
  }

  // TEST: a pose that walks away from the rest bounds is still covered, which is
  //       the whole reason the bones are read instead of the rest box
  {
    // An arm thrown ten metres out, as a long animation can.
    const f32_4x4 palette[2] = { bone_at(f32x3_zero), bone_at((f32x3){ 10.0F, 0.0F, 0.0F }) };

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    nya_check(nya_render3d_skinned_bounds(palette, 2, f32_4x4_id, rest_min, rest_max, &center, &radius), "a spread pose has bounds");

    nya_check(covers(center, radius, f32x3_zero), "the bone at the origin is inside");
    nya_check(covers(center, radius, (f32x3){ 10.0F, 0.0F, 0.0F }), "and the one ten metres away is too");

    // The rest box alone would have been about a metre across, and would have cut the far bone off.
    nya_check(radius > 5.0F, "the sphere follows the pose rather than the rest bounds, got %f", (f64)radius);

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_render3d_skinned_bounds");

  return nya_check_failures() == 0 ? 0 : 1;
}
