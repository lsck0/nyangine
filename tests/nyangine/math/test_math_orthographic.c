/**
 * The orthographic projection the 2D renderer draws through.
 *
 * Worth its own file because this is the one piece of the shape batch that can be checked without a
 * GPU, and because getting it wrong is close to undebuggable by eye: a sign flip renders the whole
 * frame mirrored, which reads as broken geometry rather than a broken matrix, and an off-by-one in
 * the translate puts everything half a screen away with no error anywhere.
 *
 * Every assertion below is a corner of the screen mapped to the clip space corner it must land on.
 * Clip space here is the Direct3D style one SDL_GPU normalizes to: x and y run -1 to +1 with **y
 * pointing up**, so a y-down input has to come out inverted. That inversion is the point of the
 * matrix, and it is what the middle block pins down.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Projects a point and hands back clip space xy. w is always 1 here; there is no perspective. */
static f32x2 project(f32_4x4 projection, f32 x, f32 y) {
  f32x4 clip = nya_matrix_times_vector(projection, (f32x4){x, y, 0.0F, 1.0F});
  return (f32x2){clip[0], clip[1]};
}

static void assert_near(f32x2 got, f32 expected_x, f32 expected_y, NYA_ConstCString what) {
  nya_assert(fabsf(got[0] - expected_x) < 0.0001F, "%s: x expected %f, got %f", what, (f64)expected_x, (f64)got[0]);
  nya_assert(fabsf(got[1] - expected_y) < 0.0001F, "%s: y expected %f, got %f", what, (f64)expected_y, (f64)got[1]);
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a y-down screen projection maps the four corners where they belong
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The call the renderer makes: top is 0, bottom is the height, so y grows downward.
    f32_4x4 screen = nya_matrix_orthographic(0.0F, 1920.0F, 0.0F, 1080.0F);

    // Top left of the screen is the top left of clip space, which is y = +1 because clip space y
    // points up. This single assertion is what "y down" means in practice.
    assert_near(project(screen, 0.0F, 0.0F), -1.0F, 1.0F, "top left");
    assert_near(project(screen, 1920.0F, 0.0F), 1.0F, 1.0F, "top right");
    assert_near(project(screen, 0.0F, 1080.0F), -1.0F, -1.0F, "bottom left");
    assert_near(project(screen, 1920.0F, 1080.0F), 1.0F, -1.0F, "bottom right");

    // The centre of the screen is the origin. Falls out of the other four, but it is the value a
    // wrong translate term breaks first while leaving the corners looking plausible.
    assert_near(project(screen, 960.0F, 540.0F), 0.0F, 0.0F, "centre");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: y increases downward on screen, decreases upward in clip space
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32_4x4 screen = nya_matrix_orthographic(0.0F, 800.0F, 0.0F, 600.0F);

    f32x2 near_top    = project(screen, 400.0F, 100.0F);
    f32x2 near_bottom = project(screen, 400.0F, 500.0F);

    // The direction, not the values. A projection that got the magnitude right and the sign wrong
    // passes every "is this corner in the corner" check by symmetry, and fails this one.
    nya_assert(near_top[1] > near_bottom[1], "a smaller screen y must be higher on screen");
    nya_assert(near_top[1] > 0.0F, "the upper half of the screen is positive clip y");
    nya_assert(near_bottom[1] < 0.0F, "the lower half of the screen is negative clip y");

    // x is not flipped, and never was. Asserted so that a future change to the y term cannot quietly
    // take x with it.
    f32x2 left  = project(screen, 100.0F, 300.0F);
    f32x2 right = project(screen, 700.0F, 300.0F);
    nya_assert(left[0] < right[0], "a smaller screen x must be further left");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: swapping top and bottom gives the y-up convention a world camera wants
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Same function, top greater than bottom. No flag and no second function: the sign of the scale
    // falls out of the arguments, which is what makes one implementation serve both cameras.
    f32_4x4 world = nya_matrix_orthographic(0.0F, 100.0F, 100.0F, 0.0F);

    assert_near(project(world, 0.0F, 0.0F), -1.0F, -1.0F, "y-up origin is bottom left");
    assert_near(project(world, 100.0F, 100.0F), 1.0F, 1.0F, "y-up maximum is top right");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an off-centre viewport, which a split screen or a scrolled camera produces
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Nothing requires the rectangle to start at the origin. A camera scrolled to (200, 100) passes
    // its own bounds and everything else is unchanged.
    f32_4x4 scrolled = nya_matrix_orthographic(200.0F, 520.0F, 100.0F, 340.0F);

    assert_near(project(scrolled, 200.0F, 100.0F), -1.0F, 1.0F, "scrolled top left");
    assert_near(project(scrolled, 520.0F, 340.0F), 1.0F, -1.0F, "scrolled bottom right");
    assert_near(project(scrolled, 360.0F, 220.0F), 0.0F, 0.0F, "scrolled centre");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: z passes through into the depth range clip space accepts
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32_4x4 screen = nya_matrix_orthographic(0.0F, 640.0F, 0.0F, 480.0F);

    // Depth is 0..1 rather than -1..1 in this convention. The batch writes z = 0, so what matters is
    // that it survives unchanged and stays inside the range rather than landing outside it, where a
    // clipper is free to discard the whole vertex.
    f32x4 at_zero = nya_matrix_times_vector(screen, (f32x4){320.0F, 240.0F, 0.0F, 1.0F});
    nya_assert(fabsf(at_zero[2]) < 0.0001F, "z = 0 must stay at 0, got %f", (f64)at_zero[2]);
    nya_assert(fabsf(at_zero[3] - 1.0F) < 0.0001F, "w must stay 1, got %f", (f64)at_zero[3]);

    f32x4 at_half = nya_matrix_times_vector(screen, (f32x4){320.0F, 240.0F, 0.5F, 1.0F});
    nya_assert(fabsf(at_half[2] - 0.5F) < 0.0001F, "z must pass through unscaled, got %f", (f64)at_half[2]);
  }

  printf("PASSED: test_math_orthographic\n");
  return 0;
}
