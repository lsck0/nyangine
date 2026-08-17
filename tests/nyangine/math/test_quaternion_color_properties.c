/**
 * Algebraic identities for quaternions, and round trips for the colour spaces.
 */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

static b8 near_enough(f32 a, f32 b, f32 tolerance) {
  return fabsf(a - b) <= tolerance;
}

static b8 vector_close(f32x3 a, f32x3 b, f32 tolerance) {
  return near_enough(a.x, b.x, tolerance) && near_enough(a.y, b.y, tolerance) && near_enough(a.z, b.z, tolerance);
}

static f32 vector_length(f32x3 v) {
  f32x3 s = v * v;
  return sqrtf(s.x + s.y + s.z);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  srand(20240607);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: euler round trip away from the poles
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: euler round trip\n");
  {
    for (u32 trial = 0; trial < 500; trial++) {
      // Pitch kept clear of +-90 degrees, where yaw and roll stop being separable and only their
      // sum survives — a documented property of euler angles rather than a defect.
      f32 pitch = ((f32)(rand() % 2000) / 1000.0F - 1.0F) * 1.3F;
      f32 yaw   = ((f32)(rand() % 2000) / 1000.0F - 1.0F) * 3.1F;
      f32 roll  = ((f32)(rand() % 2000) / 1000.0F - 1.0F) * 3.1F;

      NYA_Quaternion q = nya_quaternion_from_euler(pitch, yaw, roll);

      f32 back_pitch = 0.0F, back_yaw = 0.0F, back_roll = 0.0F;
      nya_quaternion_to_euler(q, &back_pitch, &back_yaw, &back_roll);

      NYA_Quaternion again = nya_quaternion_from_euler(back_pitch, back_yaw, back_roll);

      // Compared as rotations, not as angle triples: different triples can name the same rotation.
      nya_check(
        nya_quaternion_approx_equals(q, again, 1e-4F),
        "euler round trip changed the rotation for (%.3f, %.3f, %.3f)",
        (f64)pitch,
        (f64)yaw,
        (f64)roll
      );
    }
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: rotate agrees with the matrix, and preserves length
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: rotate vs matrix\n");
  {
    for (u32 trial = 0; trial < 500; trial++) {
      f32x3 axis = {
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
      };
      if (vector_length(axis) < 1e-3F) continue;

      f32            angle = ((f32)(rand() % 2000) / 1000.0F - 1.0F) * 3.14F;
      NYA_Quaternion q     = nya_quaternion_from_axis_angle(axis, angle);

      f32x3 v = {
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
      };

      f32x3   rotated = nya_quaternion_rotate(q, v);
      f32_3x3 m       = nya_quaternion_to_matrix3(q);
      f32x3   via_matrix = nya_matrix_times_vector(m, v);

      nya_check(vector_close(rotated, via_matrix, 1e-4F), "rotate and to_matrix3 disagree");
      nya_check(near_enough(vector_length(rotated), vector_length(v), 1e-4F), "rotation changed the vector's length");
    }
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: matrix round trip, inverse, and conjugate
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: quaternion algebra\n");
  {
    for (u32 trial = 0; trial < 500; trial++) {
      f32x3 axis = {
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
      };
      if (vector_length(axis) < 1e-3F) continue;

      f32            angle = ((f32)(rand() % 2000) / 1000.0F - 1.0F) * 3.14F;
      NYA_Quaternion q     = nya_quaternion_from_axis_angle(axis, angle);

      // Shepperd's method has four branches; a random axis exercises all of them.
      NYA_Quaternion from_matrix = nya_quaternion_from_matrix(nya_quaternion_to_matrix3(q));
      nya_check(nya_quaternion_approx_equals(q, from_matrix, 1e-4F), "to_matrix3 / from_matrix is not a round trip");

      // q * q⁻¹ = identity.
      NYA_Quaternion product = nya_quaternion_multiply(q, nya_quaternion_inverse(q));
      nya_check(nya_quaternion_approx_equals(product, nya_quaternion_identity, 1e-4F), "q * inverse(q) is not the identity");

      // Rotating by q then by its conjugate returns the vector.
      f32x3 v       = { 0.3F, -0.7F, 0.5F };
      f32x3 there   = nya_quaternion_rotate(q, v);
      f32x3 back    = nya_quaternion_rotate(nya_quaternion_conjugate(q), there);
      nya_check(vector_close(back, v, 1e-4F), "conjugate did not undo the rotation");

      // axis/angle round trip, as a rotation.
      f32x3 back_axis  = { 0 };
      f32   back_angle = 0.0F;
      nya_quaternion_to_axis_angle(q, &back_axis, &back_angle);
      NYA_Quaternion rebuilt = nya_quaternion_from_axis_angle(back_axis, back_angle);
      nya_check(nya_quaternion_approx_equals(q, rebuilt, 1e-4F), "axis/angle round trip changed the rotation");
    }
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: slerp and nlerp endpoints
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: interpolation endpoints\n");
  {
    NYA_Quaternion a = nya_quaternion_from_euler(0.2F, 0.4F, -0.3F);
    NYA_Quaternion b = nya_quaternion_from_euler(-0.5F, 1.1F, 0.8F);

    nya_check(nya_quaternion_approx_equals(nya_quaternion_slerp(a, b, 0.0F), a, 1e-5F), "slerp at t=0 is not the start");
    nya_check(nya_quaternion_approx_equals(nya_quaternion_slerp(a, b, 1.0F), b, 1e-5F), "slerp at t=1 is not the end");
    nya_check(nya_quaternion_approx_equals(nya_quaternion_nlerp(a, b, 0.0F), a, 1e-5F), "nlerp at t=0 is not the start");
    nya_check(nya_quaternion_approx_equals(nya_quaternion_nlerp(a, b, 1.0F), b, 1e-5F), "nlerp at t=1 is not the end");

    // Halfway is halfway: the angle to each end should match.
    NYA_Quaternion mid = nya_quaternion_slerp(a, b, 0.5F);
    f32            to_a = nya_quaternion_angle_between(mid, a);
    f32            to_b = nya_quaternion_angle_between(mid, b);
    nya_check(near_enough(to_a, to_b, 1e-3F), "slerp at t=0.5 is not equidistant: %.5f vs %.5f", (f64)to_a, (f64)to_b);

    // Every interpolated quaternion has to stay a unit rotation.
    for (u32 i = 0; i <= 10; i++) {
      f32 t = (f32)i / 10.0F;
      nya_check(near_enough(nya_quaternion_length(nya_quaternion_slerp(a, b, t)), 1.0F, 1e-4F), "slerp left the unit sphere at t=%.1f", (f64)t);
      nya_check(near_enough(nya_quaternion_length(nya_quaternion_nlerp(a, b, t)), 1.0F, 1e-4F), "nlerp left the unit sphere at t=%.1f", (f64)t);
    }
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: from_to and look
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: from_to and look\n");
  {
    for (u32 trial = 0; trial < 300; trial++) {
      f32x3 from = {
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
      };
      f32x3 to = {
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
        (f32)(rand() % 2000) / 1000.0F - 1.0F,
      };
      if (vector_length(from) < 1e-2F || vector_length(to) < 1e-2F) continue;

      f32x3 unit_to = to / vector_length(to);

      // The defining property: from_to(a, b) takes a to b.
      f32x3 carried = nya_quaternion_rotate(nya_quaternion_from_to(from, to), from / vector_length(from));
      nya_check(vector_close(carried, unit_to, 1e-3F), "from_to did not carry `from` onto `to`");

      // look points -Z along the direction, which is the convention the view matrix uses.
      f32x3 forward = nya_quaternion_rotate(nya_quaternion_look(to, f32x3_unit_y), -f32x3_unit_z);
      nya_check(vector_close(forward, unit_to, 1e-3F), "look did not aim -Z along the direction");
    }

    // The antipodal case, where the cross product vanishes and the fallback axis is used.
    f32x3 flipped = nya_quaternion_rotate(nya_quaternion_from_to(f32x3_unit_x, -f32x3_unit_x), f32x3_unit_x);
    nya_check(vector_close(flipped, -f32x3_unit_x, 1e-3F), "from_to failed on exactly opposite vectors");
  }
  printf("  done\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: colour space round trips
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: colour round trips\n");
  {
    for (u32 trial = 0; trial < 2000; trial++) {
      u8 r = (u8)(rand() % 256);
      u8 g = (u8)(rand() % 256);
      u8 b = (u8)(rand() % 256);
      u8 a = (u8)(rand() % 256);

      NYA_Color color = nya_color_from_u8(r, g, b, a);

      // u32 packing is exact for anything that came from bytes.
      nya_check(nya_color_to_u32(color) == (((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | (u32)a), "u8 -> u32 packing is wrong");
      NYA_Color unpacked = nya_color_from_u32(nya_color_to_u32(color));
      nya_check(near_enough(unpacked.r, color.r, 1e-6F) && near_enough(unpacked.g, color.g, 1e-6F), "u32 round trip lost a channel");

      // HSV and HSL both have to reconstruct the original RGB.
      NYA_Color via_hsv = nya_color_from_hsv(nya_color_to_hsv(color));
      nya_check(
        near_enough(via_hsv.r, color.r, 2e-3F) && near_enough(via_hsv.g, color.g, 2e-3F) && near_enough(via_hsv.b, color.b, 2e-3F),
        "HSV round trip changed rgb(%u, %u, %u) to (%.4f, %.4f, %.4f)",
        r,
        g,
        b,
        (f64)via_hsv.r,
        (f64)via_hsv.g,
        (f64)via_hsv.b
      );

      NYA_Color via_hsl = nya_color_from_hsl(nya_color_to_hsl(color));
      nya_check(
        near_enough(via_hsl.r, color.r, 2e-3F) && near_enough(via_hsl.g, color.g, 2e-3F) && near_enough(via_hsl.b, color.b, 2e-3F),
        "HSL round trip changed rgb(%u, %u, %u) to (%.4f, %.4f, %.4f)",
        r,
        g,
        b,
        (f64)via_hsl.r,
        (f64)via_hsl.g,
        (f64)via_hsl.b
      );
    }

    // Hex, with and without the alpha byte and the leading hash.
    nya_check(nya_color_to_u32(nya_color_from_hex("FF8000")) == 0xFF8000FF, "6 digit hex is wrong");
    nya_check(nya_color_to_u32(nya_color_from_hex("#FF8000")) == 0xFF8000FF, "a leading hash is not skipped");
    nya_check(nya_color_to_u32(nya_color_from_hex("FF800040")) == 0xFF800040, "8 digit hex is wrong");
    nya_check(nya_color_to_u32(nya_color_from_hex("ff8000")) == 0xFF8000FF, "lowercase hex is wrong");

    // A full hue turn is the identity, and blending onto an opaque background keeps it opaque.
    NYA_Color base = nya_color_from_u8(200, 100, 50, 255);
    NYA_Color turned = nya_color_hue_shift(base, 360.0F);
    nya_check(near_enough(turned.r, base.r, 2e-3F) && near_enough(turned.g, base.g, 2e-3F), "a 360 degree hue shift is not the identity");

    NYA_Color blended = nya_color_blend(base, nya_color_from_u8(0, 0, 0, 0));
    nya_check(near_enough(blended.r, base.r, 1e-5F) && near_enough(blended.a, 1.0F, 1e-5F), "blending a fully transparent layer changed the background");
  }
  printf("  done\n");

  printf("%s: test_quaternion_color_properties (" FMTu32 " failures)\n", nya_check_failures() == 0 ? "PASSED" : "FAILED", nya_check_failures());
  return nya_check_failures() == 0 ? 0 : 1;
}
