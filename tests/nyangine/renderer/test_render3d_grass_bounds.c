/**
 * The bounding sphere an instanced grass patch is culled by: _nya_render3d_grass_bounds.
 *
 * nya_render3d_grass draws a whole field of blades in one instanced call, so the field is culled as one
 * sphere rather than blade by blade. That sphere is the one part of the path worth testing away from a GPU,
 * and the one part that can be quietly wrong: read the translation out of the wrong column and the sphere
 * sits at the origin, which culls nothing in the middle of a scene and clips the field at its edges; forget
 * the scale and a field of tall blades loses its tips to the frustum as the camera turns.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** An instance at `position` with a uniform `scale` and no rotation — the placement the bounds read. */
static NYA_Render3DInstance blade_at(f32x3 position, f32 scale) {
  return (NYA_Render3DInstance){
    .model = nya_matrix_transform(position, f32_3x3_id, (f32x3){ scale, scale, scale }),
    .tint  = NYA_COLOR_WHITE,
  };
}

/** Whether `point` is inside the sphere, the only promise the bounds make. */
static b8 covers(f32x3 center, f32 radius, f32x3 point) { return nya_vector_length(point - center) <= radius; }

s32 main(void) {
  // A blade half a metre across and a tip that the wind throws a quarter metre, the numbers nya_render3d_grass hands the helper (blade_radius, and amplitude * height for the sway reach).
  const f32 blade_radius = 0.5F;
  const f32 sway_reach   = 0.25F;

  // TEST: one blade, well away from the origin, so reading the translation from the wrong place cannot pass by luck. The sphere sits on it, padded by the blade and the sway.
  {
    const NYA_Render3DInstance one = blade_at((f32x3){ 10.0F, 4.0F, -6.0F }, 1.0F);

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    _nya_render3d_grass_bounds(&one, 1, blade_radius, sway_reach, &center, &radius);

    nya_check(fabsf(center.x - 10.0F) < 0.001F, "the centre is on the blade in x, got %f", (f64)center.x);
    nya_check(fabsf(center.y - 4.0F) < 0.001F, "and on y, got %f", (f64)center.y);
    nya_check(fabsf(center.z + 6.0F) < 0.001F, "and on z, got %f", (f64)center.z);

    // one placement has no box, so the radius is exactly the blade's reach plus the sway pad.
    nya_check(fabsf(radius - (blade_radius + sway_reach)) < 0.001F, "the radius is the pad alone, got %f", (f64)radius);

    printf("  PASSED\n");
  }

  // TEST: a row of blades: the centre is the midpoint of the box their bases make, and every base is inside the sphere.
  {
    const NYA_Render3DInstance blades[] = {
      blade_at((f32x3){ -4.0F, 0.0F, -4.0F }, 1.0F),
      blade_at((f32x3){ 4.0F, 0.0F, -4.0F }, 1.0F),
      blade_at((f32x3){ -4.0F, 0.0F, 4.0F }, 1.0F),
      blade_at((f32x3){ 4.0F, 0.0F, 4.0F }, 1.0F),
      blade_at((f32x3){ 0.0F, 0.0F, 0.0F }, 1.0F),
    };

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    _nya_render3d_grass_bounds(blades, nya_carray_length(blades), blade_radius, sway_reach, &center, &radius);

    nya_check(nya_vector_length(center) < 0.001F, "the centre is the middle of the field, got (%f, %f, %f)", (f64)center.x, (f64)center.y,
              (f64)center.z);

    for (u32 i = 0; i < nya_carray_length(blades); i++) {
      f32x3 base = { blades[i].model[0][3], blades[i].model[1][3], blades[i].model[2][3] };
      nya_check(covers(center, radius, base), "blade %u's base is inside the sphere", i);
    }

    // and a swaying tip a little past the corner is still covered, since the radius padded for it.
    nya_check(covers(center, radius, (f32x3){ 4.0F + sway_reach, 0.0F, 4.0F }), "a downwind tip at the corner is inside");

    printf("  PASSED\n");
  }

  // TEST: a scaled-up blade grows the padding, or a tall blade's tip would hang outside the sphere and be clipped as the camera turns.
  {
    const NYA_Render3DInstance plain  = blade_at(f32x3_zero, 1.0F);
    const NYA_Render3DInstance scaled = blade_at(f32x3_zero, 3.0F);

    f32x3 plain_center  = f32x3_zero;
    f32   plain_radius  = 0.0F;
    f32x3 scaled_center = f32x3_zero;
    f32   scaled_radius = 0.0F;

    _nya_render3d_grass_bounds(&plain, 1, blade_radius, sway_reach, &plain_center, &plain_radius);
    _nya_render3d_grass_bounds(&scaled, 1, blade_radius, sway_reach, &scaled_center, &scaled_radius);

    nya_check(fabsf(scaled_radius - (plain_radius * 3.0F)) < 0.01F, "three times the blade is three times the pad, got %f against %f",
              (f64)scaled_radius, (f64)(plain_radius * 3.0F));

    printf("  PASSED\n");
  }

  // TEST: the largest instance scale decides the margin, since one scale grows it for the whole field, and the sphere must cover that blade too.
  {
    const NYA_Render3DInstance mixed[] = {
      blade_at((f32x3){ -2.0F, 0.0F, 0.0F }, 1.0F),
      blade_at((f32x3){ 2.0F, 0.0F, 0.0F }, 4.0F),
    };

    f32x3 center = f32x3_zero;
    f32   radius = 0.0F;

    _nya_render3d_grass_bounds(mixed, 2, blade_radius, sway_reach, &center, &radius);

    // the box half-diagonal is 2 (bases at x = ±2), plus the big blade's pad, four times the base pad.
    f32 expected = 2.0F + ((blade_radius + sway_reach) * 4.0F);
    nya_check(fabsf(radius - expected) < 0.01F, "the tallest blade sets the pad, got %f against %f", (f64)radius, (f64)expected);

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_render3d_grass_bounds");

  return nya_check_failures() == 0 ? 0 : 1;
}
