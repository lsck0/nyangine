/**
 * Quaternions.
 *
 * Mostly written as algebraic identities rather than as literal expected components: a quaternion
 * and its negation are the same rotation, so comparing raw x/y/z/w pins an implementation detail
 * instead of the behaviour. Where a component is checked directly it is because the contract in
 * math_quaternion.h names it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define EPS 1.0e-4F

static void assert_vec_approx(f32x3 a, f32x3 b, NYA_ConstCString what) {
  nya_assert(
      fabsf(a.x - b.x) < EPS && fabsf(a.y - b.y) < EPS && fabsf(a.z - b.z) < EPS,
      "%s: got (%f, %f, %f), expected (%f, %f, %f)",
      what,
      (f64)a.x, (f64)a.y, (f64)a.z,
      (f64)b.x, (f64)b.y, (f64)b.z
  );
}

s32 main(void) {
  // π comes from <math.h>, which base_basic.h already includes.
  const f32 HALF_PI = (f32)M_PI_2;

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: identity
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: identity\n");
  {
    NYA_Quaternion id = nya_quaternion_identity;
    nya_assert(id.x == 0.0F && id.y == 0.0F && id.z == 0.0F && id.w == 1.0F);
    nya_assert(fabsf(nya_quaternion_length(id) - 1.0F) < EPS);

    // Rotating by identity leaves a vector alone.
    f32x3 v = { 1.0F, 2.0F, 3.0F };
    assert_vec_approx(nya_quaternion_rotate(id, v), v, "identity rotation");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: axis-angle round trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: axis-angle round trip\n");
  {
    f32x3          axis = { 0.0F, 1.0F, 0.0F };
    NYA_Quaternion q    = nya_quaternion_from_axis_angle(axis, HALF_PI);

    nya_assert(fabsf(nya_quaternion_length(q) - 1.0F) < EPS);

    f32x3 out_axis;
    f32   out_angle;
    nya_quaternion_to_axis_angle(q, &out_axis, &out_angle);

    nya_assert(fabsf(out_angle - HALF_PI) < EPS);
    assert_vec_approx(out_axis, axis, "recovered axis");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: rotation actually rotates
  //
  // A quarter turn about +Y takes +X to -Z under the right hand rule.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: rotation\n");
  {
    f32x3          y_axis = { 0.0F, 1.0F, 0.0F };
    NYA_Quaternion q      = nya_quaternion_from_axis_angle(y_axis, HALF_PI);

    f32x3 x        = { 1.0F, 0.0F, 0.0F };
    f32x3 rotated  = nya_quaternion_rotate(q, x);
    f32x3 expected = { 0.0F, 0.0F, -1.0F };
    assert_vec_approx(rotated, expected, "quarter turn about Y");

    // The axis itself is fixed by its own rotation.
    assert_vec_approx(nya_quaternion_rotate(q, y_axis), y_axis, "axis is fixed");

    // Rotation preserves length.
    f32x3 v       = { 3.0F, -4.0F, 12.0F };
    f32x3 v_rot   = nya_quaternion_rotate(q, v);
    f32   len_in  = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    f32   len_out = sqrtf(v_rot.x * v_rot.x + v_rot.y * v_rot.y + v_rot.z * v_rot.z);
    nya_assert(fabsf(len_in - len_out) < EPS);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: composition, conjugate and inverse
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: composition and inverse\n");
  {
    f32x3          axis = { 0.577F, 0.577F, 0.577F };
    NYA_Quaternion q    = nya_quaternion_normalize(nya_quaternion_from_axis_angle(axis, 1.1F));

    // q * q⁻¹ is the identity rotation.
    NYA_Quaternion round = nya_quaternion_multiply(q, nya_quaternion_inverse(q));
    nya_assert(nya_quaternion_approx_equals(round, nya_quaternion_identity, EPS));

    // For a unit quaternion the conjugate is the inverse.
    nya_assert(nya_quaternion_approx_equals(nya_quaternion_conjugate(q), nya_quaternion_inverse(q), EPS));

    // Composing two rotations equals applying them one after the other.
    f32x3          other_axis = { 1.0F, 0.0F, 0.0F };
    NYA_Quaternion p          = nya_quaternion_from_axis_angle(other_axis, 0.7F);
    f32x3          v          = { 0.3F, -1.4F, 2.0F };

    f32x3 sequential = nya_quaternion_rotate(p, nya_quaternion_rotate(q, v));
    f32x3 composed   = nya_quaternion_rotate(nya_quaternion_multiply(p, q), v);
    assert_vec_approx(sequential, composed, "p*q applies q then p");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: euler round trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: euler round trip\n");
  {
    // Away from gimbal lock, so the recovered angles are the ones that went in.
    f32 pitch = 0.3F, yaw = -0.6F, roll = 1.1F;

    NYA_Quaternion q = nya_quaternion_from_euler(pitch, yaw, roll);

    f32 out_pitch, out_yaw, out_roll;
    nya_quaternion_to_euler(q, &out_pitch, &out_yaw, &out_roll);

    nya_assert(fabsf(out_pitch - pitch) < EPS, "pitch: %f vs %f", (f64)out_pitch, (f64)pitch);
    nya_assert(fabsf(out_yaw - yaw) < EPS, "yaw: %f vs %f", (f64)out_yaw, (f64)yaw);
    nya_assert(fabsf(out_roll - roll) < EPS, "roll: %f vs %f", (f64)out_roll, (f64)roll);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_quaternion_from_to
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: from_to\n");
  {
    f32x3 from = { 1.0F, 0.0F, 0.0F };
    f32x3 to   = { 0.0F, 1.0F, 0.0F };

    NYA_Quaternion q = nya_quaternion_from_to(from, to);
    assert_vec_approx(nya_quaternion_rotate(q, from), to, "from_to maps from onto to");

    // The degenerate case: a vector onto itself is no rotation at all.
    NYA_Quaternion same = nya_quaternion_from_to(from, from);
    assert_vec_approx(nya_quaternion_rotate(same, from), from, "from_to with equal vectors");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: interpolation endpoints and midpoint
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nlerp and slerp\n");
  {
    f32x3          axis = { 0.0F, 0.0F, 1.0F };
    NYA_Quaternion a    = nya_quaternion_identity;
    NYA_Quaternion b    = nya_quaternion_from_axis_angle(axis, HALF_PI);

    // Endpoints are exact for both.
    nya_assert(nya_quaternion_approx_equals(nya_quaternion_slerp(a, b, 0.0F), a, EPS));
    nya_assert(nya_quaternion_approx_equals(nya_quaternion_slerp(a, b, 1.0F), b, EPS));
    nya_assert(nya_quaternion_approx_equals(nya_quaternion_nlerp(a, b, 0.0F), a, EPS));
    nya_assert(nya_quaternion_approx_equals(nya_quaternion_nlerp(a, b, 1.0F), b, EPS));

    // Both stay on the unit sphere, which is the whole reason nlerp normalises.
    for (u32 i = 0; i <= 10; i++) {
      f32 t = (f32)i / 10.0F;
      nya_assert(fabsf(nya_quaternion_length(nya_quaternion_slerp(a, b, t)) - 1.0F) < EPS);
      nya_assert(fabsf(nya_quaternion_length(nya_quaternion_nlerp(a, b, t)) - 1.0F) < EPS);
    }

    // Halfway along slerp is half the angle, which is what distinguishes it from nlerp.
    NYA_Quaternion mid = nya_quaternion_slerp(a, b, 0.5F);
    nya_assert(fabsf(nya_quaternion_angle_between(a, mid) - HALF_PI / 2.0F) < EPS);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: matrix conversion round trip
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: matrix conversion\n");
  {
    f32x3          axis = { 0.267F, 0.535F, 0.802F };
    NYA_Quaternion q    = nya_quaternion_normalize(nya_quaternion_from_axis_angle(axis, 0.9F));

    NYA_Quaternion back = nya_quaternion_from_matrix(nya_quaternion_to_matrix3(q));

    // q and -q are the same rotation, so either is a correct answer.
    NYA_Quaternion negated = { -back.x, -back.y, -back.z, -back.w };
    nya_assert(nya_quaternion_approx_equals(back, q, EPS) || nya_quaternion_approx_equals(negated, q, EPS));

    // Whichever it picked, it must rotate identically.
    f32x3 v = { 1.0F, -2.0F, 0.5F };
    assert_vec_approx(nya_quaternion_rotate(back, v), nya_quaternion_rotate(q, v), "matrix round trip rotates the same");

    // The 4x4 form is the 3x3 with a homogeneous row and column, so a point with w=1 comes back
    // rotated with its w untouched. Checked through the API rather than by indexing, since
    // f32_4x4 is a clang matrix type rather than a struct of rows.
    f32_4x4 m4        = nya_quaternion_to_matrix4(q);
    f32x4   point     = { v.x, v.y, v.z, 1.0F };
    f32x4   projected = nya_matrix_times_vector(m4, point);
    f32x3   rotated3  = nya_quaternion_rotate(q, v);

    nya_assert(fabsf(projected.w - 1.0F) < EPS);
    assert_vec_approx((f32x3){ projected.x, projected.y, projected.z }, rotated3, "matrix4 agrees with rotate");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: dot, length and normalize
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: dot, length, normalize\n");
  {
    NYA_Quaternion q = nya_quaternion_create(1.0F, 2.0F, 3.0F, 4.0F);

    nya_assert(fabsf(nya_quaternion_length_squared(q) - 30.0F) < EPS);
    nya_assert(fabsf(nya_quaternion_length(q) - sqrtf(30.0F)) < EPS);
    nya_assert(fabsf(nya_quaternion_dot(q, q) - 30.0F) < EPS);

    NYA_Quaternion n = nya_quaternion_normalize(q);
    nya_assert(fabsf(nya_quaternion_length(n) - 1.0F) < EPS);

    // A quaternion is orthogonal to nothing in particular, but dot with identity is just w.
    nya_assert(fabsf(nya_quaternion_dot(q, nya_quaternion_identity) - 4.0F) < EPS);

    // Scaling then normalising lands back on the same unit quaternion.
    NYA_Quaternion scaled = nya_quaternion_scale(q, 7.5F);
    nya_assert(nya_quaternion_approx_equals(nya_quaternion_normalize(scaled), n, EPS));

    // Addition is componentwise.
    NYA_Quaternion sum = nya_quaternion_add(q, q);
    nya_assert(fabsf(sum.x - 2.0F) < EPS && fabsf(sum.w - 8.0F) < EPS);
    printf("  PASSED\n");
  }

  printf("PASSED: test_math_quaternion\n");
  return 0;
}
