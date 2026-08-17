/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: f32x2 vector operations
  // ─────────────────────────────────────────────────────────────────────────────
  f32x2 vec2_a = {3.0F, 4.0F};
  f32x2 vec2_b = {1.0F, 2.0F};

  f32x2 vec2_sum = vec2_a + vec2_b;
  nya_assert(vec2_sum.x == 4.0F);
  nya_assert(vec2_sum.y == 6.0F);

  f32x2 vec2_diff = vec2_a - vec2_b;
  nya_assert(vec2_diff.x == 2.0F);
  nya_assert(vec2_diff.y == 2.0F);

  f32x2 vec2_prod = vec2_a * vec2_b;
  nya_assert(vec2_prod.x == 3.0F);
  nya_assert(vec2_prod.y == 8.0F);

  // Test unit vectors
  nya_assert(f32x2_unit_x.x == 1.0F && f32x2_unit_x.y == 0.0F);
  nya_assert(f32x2_unit_y.x == 0.0F && f32x2_unit_y.y == 1.0F);
  nya_assert(f32x2_zero.x == 0.0F && f32x2_zero.y == 0.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: f32x3 vector operations
  // ─────────────────────────────────────────────────────────────────────────────
  f32x3 vec_a = {1.0F, 2.0F, 3.0F};

  vec_a.xy = (f32x2){4.0F, 5.0F};
  nya_assert(vec_a.x == 4.0F);
  nya_assert(vec_a.y == 5.0F);
  nya_assert(vec_a.z == 3.0F);

  vec_a += f32x3_unit_z;
  nya_assert(vec_a.x == 4.0F);
  nya_assert(vec_a.y == 5.0F);
  nya_assert(vec_a.z == 4.0F);

  vec_a = nya_lerp(vec_a, f32x3_unit_x, 0.5F);
  nya_assert(vec_a.x == 2.5F);
  nya_assert(vec_a.y == 2.5F);
  nya_assert(vec_a.z == 2.0F);

  f32x3 vec_b = f32x3_unit_y;
  nya_assert(vec_b.x == 0.0F);
  nya_assert(vec_b.y == 1.0F);
  nya_assert(vec_b.z == 0.0F);

  f32x3 vec_c = vec_a * vec_b;
  nya_assert(vec_c.x == 0.0F);
  nya_assert(vec_c.y == 2.5F);
  nya_assert(vec_c.z == 0.0F);

  // Test subtraction
  f32x3 vec_sub = vec_a - vec_b;
  nya_assert(vec_sub.x == 2.5F);
  nya_assert(vec_sub.y == 1.5F);
  nya_assert(vec_sub.z == 2.0F);

  // Test scalar multiplication
  f32x3 vec_scaled = vec_a * 2.0F;
  nya_assert(vec_scaled.x == 5.0F);
  nya_assert(vec_scaled.y == 5.0F);
  nya_assert(vec_scaled.z == 4.0F);

  // Test unit vectors
  nya_assert(f32x3_unit_x.x == 1.0F && f32x3_unit_x.y == 0.0F && f32x3_unit_x.z == 0.0F);
  nya_assert(f32x3_unit_y.x == 0.0F && f32x3_unit_y.y == 1.0F && f32x3_unit_y.z == 0.0F);
  nya_assert(f32x3_unit_z.x == 0.0F && f32x3_unit_z.y == 0.0F && f32x3_unit_z.z == 1.0F);
  nya_assert(f32x3_zero.x == 0.0F && f32x3_zero.y == 0.0F && f32x3_zero.z == 0.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: f32x4 vector operations
  // ─────────────────────────────────────────────────────────────────────────────
  f32x4 vec4_a = {1.0F, 2.0F, 3.0F, 4.0F};
  f32x4 vec4_b = {4.0F, 3.0F, 2.0F, 1.0F};

  f32x4 vec4_sum = vec4_a + vec4_b;
  nya_assert(vec4_sum.x == 5.0F);
  nya_assert(vec4_sum.y == 5.0F);
  nya_assert(vec4_sum.z == 5.0F);
  nya_assert(vec4_sum.w == 5.0F);

  f32x4 vec4_prod = vec4_a * vec4_b;
  nya_assert(vec4_prod.x == 4.0F);
  nya_assert(vec4_prod.y == 6.0F);
  nya_assert(vec4_prod.z == 6.0F);
  nya_assert(vec4_prod.w == 4.0F);

  // Test unit vectors
  nya_assert(f32x4_unit_x.x == 1.0F && f32x4_unit_x.y == 0.0F && f32x4_unit_x.z == 0.0F && f32x4_unit_x.w == 0.0F);
  nya_assert(f32x4_unit_w.x == 0.0F && f32x4_unit_w.y == 0.0F && f32x4_unit_w.z == 0.0F && f32x4_unit_w.w == 1.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 2x2 matrix operations
  // ─────────────────────────────────────────────────────────────────────────────
  f32_2x2 mat2_id   = f32_2x2_id;
  f32x2   vec2_test = {3.0F, 4.0F};

  // Identity matrix should not change the vector
  f32x2 vec2_result = nya_matrix_times_vector(mat2_id, vec2_test);
  nya_assert(vec2_result.x == 3.0F);
  nya_assert(vec2_result.y == 4.0F);

  // Custom 2x2 matrix
  f32_2x2 mat2_custom        = nya_matrix_create((f32[2][2]){
      {2.0F, 0.0F},
      {0.0F, 3.0F},
  });
  f32x2   vec2_scaled_result = nya_matrix_times_vector(mat2_custom, vec2_test);
  nya_assert(vec2_scaled_result.x == 6.0F);  // 2 * 3
  nya_assert(vec2_scaled_result.y == 12.0F); // 3 * 4

  // Zero matrix
  f32_2x2 mat2_zero        = f32_2x2_zero;
  f32x2   vec2_zero_result = nya_matrix_times_vector(mat2_zero, vec2_test);
  nya_assert(vec2_zero_result.x == 0.0F);
  nya_assert(vec2_zero_result.y == 0.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 3x3 matrix operations
  // ─────────────────────────────────────────────────────────────────────────────
  f32_3x3 mat_a = f32_3x3_id;
  f32_3x3 mat_b = nya_matrix_create((f32[3][3]){
      {1.0F, 2.0F, 3.0F},
      {4.0F, 5.0F, 6.0F},
      {7.0F, 8.0F, 9.0F},
  });

  f32x3 vec_d = nya_matrix_times_vector(mat_b, f32x3_unit_z);
  nya_assert(vec_d.x == 3.0F);
  nya_assert(vec_d.y == 6.0F);
  nya_assert(vec_d.z == 9.0F);

  f32x3 vec_e = nya_matrix_times_vector(mat_a, vec_d);
  nya_assert(vec_e.x == 3.0F);
  nya_assert(vec_e.y == 6.0F);
  nya_assert(vec_e.z == 9.0F);

  f32x3 vec_f = nya_matrix_times_vector(mat_b, vec_a);
  nya_assert(vec_f.x == (1.0F * 2.5F + 2.0F * 2.5F + 3.0F * 2.0F));
  nya_assert(vec_f.y == (4.0F * 2.5F + 5.0F * 2.5F + 6.0F * 2.0F));
  nya_assert(vec_f.z == (7.0F * 2.5F + 8.0F * 2.5F + 9.0F * 2.0F));

  // Test unit vectors through identity matrix
  f32x3 unit_x_result = nya_matrix_times_vector(mat_a, f32x3_unit_x);
  nya_assert(unit_x_result.x == 1.0F && unit_x_result.y == 0.0F && unit_x_result.z == 0.0F);

  f32x3 unit_y_result = nya_matrix_times_vector(mat_a, f32x3_unit_y);
  nya_assert(unit_y_result.x == 0.0F && unit_y_result.y == 1.0F && unit_y_result.z == 0.0F);

  // Zero matrix
  f32_3x3 mat3_zero        = f32_3x3_zero;
  f32x3   vec3_zero_result = nya_matrix_times_vector(mat3_zero, vec_a);
  nya_assert(vec3_zero_result.x == 0.0F);
  nya_assert(vec3_zero_result.y == 0.0F);
  nya_assert(vec3_zero_result.z == 0.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 4x4 matrix operations
  // ─────────────────────────────────────────────────────────────────────────────
  f32_4x4 mat4_id   = f32_4x4_id;
  f32x4   vec4_test = {1.0F, 2.0F, 3.0F, 4.0F};

  // Identity matrix should not change the vector
  f32x4 vec4_id_result = nya_matrix_times_vector(mat4_id, vec4_test);
  nya_assert(vec4_id_result.x == 1.0F);
  nya_assert(vec4_id_result.y == 2.0F);
  nya_assert(vec4_id_result.z == 3.0F);
  nya_assert(vec4_id_result.w == 4.0F);

  // Custom 4x4 matrix (scaling)
  f32_4x4 mat4_scale  = nya_matrix_create((f32[4][4]){
      {2.0F, 0.0F, 0.0F, 0.0F},
      {0.0F, 3.0F, 0.0F, 0.0F},
      {0.0F, 0.0F, 4.0F, 0.0F},
      {0.0F, 0.0F, 0.0F, 1.0F},
  });
  f32x4   vec4_scaled = nya_matrix_times_vector(mat4_scale, vec4_test);
  nya_assert(vec4_scaled.x == 2.0F);  // 2 * 1
  nya_assert(vec4_scaled.y == 6.0F);  // 3 * 2
  nya_assert(vec4_scaled.z == 12.0F); // 4 * 3
  nya_assert(vec4_scaled.w == 4.0F);  // 1 * 4

  // Zero matrix
  f32_4x4 mat4_zero        = f32_4x4_zero;
  f32x4   vec4_zero_result = nya_matrix_times_vector(mat4_zero, vec4_test);
  nya_assert(vec4_zero_result.x == 0.0F);
  nya_assert(vec4_zero_result.y == 0.0F);
  nya_assert(vec4_zero_result.z == 0.0F);
  nya_assert(vec4_zero_result.w == 0.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: f64 vector and matrix types
  // ─────────────────────────────────────────────────────────────────────────────
  f64x3 vec64_a   = {1.0, 2.0, 3.0};
  f64x3 vec64_b   = {4.0, 5.0, 6.0};
  f64x3 vec64_sum = vec64_a + vec64_b;
  nya_assert(vec64_sum.x == 5.0);
  nya_assert(vec64_sum.y == 7.0);
  nya_assert(vec64_sum.z == 9.0);

  f64_3x3 mat64_id     = f64_3x3_id;
  f64x3   vec64_result = nya_matrix_times_vector(mat64_id, vec64_a);
  nya_assert(vec64_result.x == 1.0);
  nya_assert(vec64_result.y == 2.0);
  nya_assert(vec64_result.z == 3.0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Matrix creation from row vectors
  // ─────────────────────────────────────────────────────────────────────────────
  f32_3x3 mat_from_rows = nya_matrix_create(f32x3_unit_x, f32x3_unit_y, f32x3_unit_z);
  f32x3   row_test      = nya_matrix_times_vector(mat_from_rows, (f32x3){1.0F, 1.0F, 1.0F});
  nya_assert(row_test.x == 1.0F);
  nya_assert(row_test.y == 1.0F);
  nya_assert(row_test.z == 1.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Vector lerp
  // ─────────────────────────────────────────────────────────────────────────────
  f32x3 lerp_start  = {0.0F, 0.0F, 0.0F};
  f32x3 lerp_end    = {10.0F, 20.0F, 30.0F};
  f32x3 lerp_result = nya_lerp(lerp_start, lerp_end, 0.5F);
  nya_assert(lerp_result.x == 5.0F);
  nya_assert(lerp_result.y == 10.0F);
  nya_assert(lerp_result.z == 15.0F);

  lerp_result = nya_lerp(lerp_start, lerp_end, 0.0F);
  nya_assert(lerp_result.x == 0.0F);
  nya_assert(lerp_result.y == 0.0F);
  nya_assert(lerp_result.z == 0.0F);

  lerp_result = nya_lerp(lerp_start, lerp_end, 1.0F);
  nya_assert(lerp_result.x == 10.0F);
  nya_assert(lerp_result.y == 20.0F);
  nya_assert(lerp_result.z == 30.0F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the 3D projections land on SDL_GPU's 0..1 depth range
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The one thing about these matrices that is easy to get wrong and impossible to see.
     *
     * Every OpenGL-era reference derives them for a -1..1 depth range; SDL_GPU normalizes every
     * backend to Direct3D's 0..1. A matrix built for the wrong one does not look like a sign error —
     * it clips everything in the near half of the frustum, and renders as geometry with holes in it.
     */
    f32     near_plane = 0.5F;
    f32     far_plane  = 100.0F;
    f32_4x4 perspective = nya_matrix_perspective(1.0F, 1.6F, near_plane, far_plane);

    // View space looks down -z, so the near plane is at z = -near.
    f32x4 at_near = nya_matrix_times_vector(perspective, (f32x4){ 0.0F, 0.0F, -near_plane, 1.0F });
    f32x4 at_far  = nya_matrix_times_vector(perspective, (f32x4){ 0.0F, 0.0F, -far_plane, 1.0F });

    nya_assert(at_near.w > 0.0F, "anything in front of the camera has positive w, got %f", (f64)at_near.w);
    nya_assert(fabsf(at_near.z / at_near.w) < 0.001F, "the near_plane plane maps to depth 0, got %f", (f64)(at_near.z / at_near.w));
    nya_assert(fabsf((at_far.z / at_far.w) - 1.0F) < 0.001F, "the far_plane plane maps to depth 1, got %f", (f64)(at_far.z / at_far.w));

    f32_4x4 orthographic = nya_matrix_orthographic_3d(10.0F, 1.6F, near_plane, far_plane);

    f32x4 ortho_near = nya_matrix_times_vector(orthographic, (f32x4){ 0.0F, 0.0F, -near_plane, 1.0F });
    f32x4 ortho_far  = nya_matrix_times_vector(orthographic, (f32x4){ 0.0F, 0.0F, -far_plane, 1.0F });

    // No projective divide at all, which is the whole of what "no vanishing point" means.
    nya_assert(ortho_near.w == 1.0F, "an orthographic projection leaves w at one, got %f", (f64)ortho_near.w);
    nya_assert(fabsf(ortho_near.z) < 0.001F, "the near_plane plane maps to depth 0, got %f", (f64)ortho_near.z);
    nya_assert(fabsf(ortho_far.z - 1.0F) < 0.001F, "the far_plane plane maps to depth 1, got %f", (f64)ortho_far.z);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: look_at puts the world into the camera's frame
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Looking at the origin from four units along +z, which is the camera's own -z direction.
    f32_4x4 view = nya_matrix_look_at((f32x3){ 0.0F, 0.0F, 4.0F }, f32x3_zero, (f32x3){ 0.0F, 1.0F, 0.0F });

    f32x4 origin = nya_matrix_times_vector(view, (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F });

    // Four units in front of the camera means z = -4 in view space, not +4. The sign is the whole
    // handedness question, and getting it backwards renders the scene from behind itself.
    nya_assert(fabsf(origin.x) < 0.001F && fabsf(origin.y) < 0.001F, "the target sits on the view axis");
    nya_assert(fabsf(origin.z + 4.0F) < 0.001F, "the target is four units down -z, got %f", (f64)origin.z);

    // A point to the world's right is to the camera's right too, from this vantage.
    f32x4 right = nya_matrix_times_vector(view, (f32x4){ 1.0F, 0.0F, 0.0F, 1.0F });
    nya_assert(right.x > 0.0F, "world +x is camera +x from here, got %f", (f64)right.x);

    // Up parallel to the view direction names no roll, so the identity comes back rather than NaNs.
    f32_4x4 degenerate = nya_matrix_look_at((f32x3){ 0.0F, 4.0F, 0.0F }, f32x3_zero, (f32x3){ 0.0F, 1.0F, 0.0F });

    f32x4 unmoved = nya_matrix_times_vector(degenerate, (f32x4){ 3.0F, 0.0F, 0.0F, 1.0F });
    nya_assert(unmoved.x == 3.0F, "a degenerate look_at is the identity, not NaN");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the vector products the projections are built from
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_vector_dot((f32x3){ 1.0F, 2.0F, 3.0F }, (f32x3){ 4.0F, 5.0F, 6.0F }) == 32.0F);

    // Right-handed: x cross y is +z. The other handedness turns every look_at inside out.
    f32x3 cross = nya_vector_cross((f32x3){ 1.0F, 0.0F, 0.0F }, (f32x3){ 0.0F, 1.0F, 0.0F });
    nya_assert(cross.x == 0.0F && cross.y == 0.0F && cross.z == 1.0F, "x cross y is +z");

    nya_assert(nya_vector_length((f32x3){ 3.0F, 4.0F, 0.0F }) == 5.0F);

    f32x3 unit = nya_vector_normalize((f32x3){ 0.0F, 0.0F, -7.0F });
    nya_assert(unit.z == -1.0F, "normalize keeps the direction");

    // Zero rather than NaN, which is what keeps a camera that has not been aimed yet from blanking
    // the whole window.
    f32x3 zero = nya_vector_normalize(f32x3_zero);
    nya_assert(zero.x == 0.0F && zero.y == 0.0F && zero.z == 0.0F, "a zero vector normalizes to zero");
  }

  return 0;
}
