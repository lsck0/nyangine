/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_math_matrix_ops");

  // TEST: nya_matrix_times_vector - 2x2 matrix * 2D vector
  {
    f32_2x2 mat    = nya_matrix_create((f32x2){ 1.0F, 0.0F }, (f32x2){ 0.0F, 1.0F });
    f32x2   vec    = { 3.0F, 4.0F };
    f32x2   result = nya_matrix_times_vector(mat, vec);

    // Identity matrix * vector = vector
    nya_assert(result.x == 3.0F);
    nya_assert(result.y == 4.0F);
  }

  // TEST: nya_matrix_times_vector - 2x2 scaling matrix
  {
    f32_2x2 mat    = nya_matrix_create((f32x2){ 2.0F, 0.0F }, (f32x2){ 0.0F, 3.0F });
    f32x2   vec    = { 1.0F, 1.0F };
    f32x2   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 2.0F);
    nya_assert(result.y == 3.0F);
  }

  // TEST: nya_matrix_times_vector - 3x3 identity
  {
    f32_3x3 mat    = nya_matrix_create((f32x3){ 1.0F, 0.0F, 0.0F }, (f32x3){ 0.0F, 1.0F, 0.0F }, (f32x3){ 0.0F, 0.0F, 1.0F });
    f32x3   vec    = { 1.0F, 2.0F, 3.0F };
    f32x3   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 2.0F);
    nya_assert(result.z == 3.0F);
  }

  // TEST: nya_matrix_times_vector - 3x3 diagonal scaling
  {
    f32_3x3 mat    = nya_matrix_create((f32x3){ 2.0F, 0.0F, 0.0F }, (f32x3){ 0.0F, 3.0F, 0.0F }, (f32x3){ 0.0F, 0.0F, 4.0F });
    f32x3   vec    = { 1.0F, 1.0F, 1.0F };
    f32x3   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 2.0F);
    nya_assert(result.y == 3.0F);
    nya_assert(result.z == 4.0F);
  }

  // TEST: nya_matrix_times_vector - 4x4 identity
  {
    f32_4x4 mat = nya_matrix_create(
        (f32x4){ 1.0F, 0.0F, 0.0F, 0.0F },
        (f32x4){ 0.0F, 1.0F, 0.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 1.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
    f32x4 vec    = { 1.0F, 2.0F, 3.0F, 4.0F };
    f32x4 result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 2.0F);
    nya_assert(result.z == 3.0F);
    nya_assert(result.w == 4.0F);
  }

  // TEST: nya_matrix_times_vector - 4x4 translation (homogeneous coordinates)
  {
    // Translation matrix that adds (1, 2, 3) to (x, y, z)
    f32_4x4 mat = nya_matrix_create(
        (f32x4){ 1.0F, 0.0F, 0.0F, 1.0F },
        (f32x4){ 0.0F, 1.0F, 0.0F, 2.0F },
        (f32x4){ 0.0F, 0.0F, 1.0F, 3.0F },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
    f32x4 vec    = { 0.0F, 0.0F, 0.0F, 1.0F }; // Point at origin with w=1
    f32x4 result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 2.0F);
    nya_assert(result.z == 3.0F);
    nya_assert(result.w == 1.0F);
  }

  // TEST: nya_matrix_times_vector - f64 types
  {
    f64_3x3 mat    = nya_matrix_create((f64x3){ 2.0, 0.0, 0.0 }, (f64x3){ 0.0, 2.0, 0.0 }, (f64x3){ 0.0, 0.0, 2.0 });
    f64x3   vec    = { 1.0, 1.0, 1.0 };
    f64x3   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 2.0);
    nya_assert(result.y == 2.0);
    nya_assert(result.z == 2.0);
  }

  // TEST: nya_matrix_times_vector - zero matrix
  {
    f32_3x3 mat    = nya_matrix_create((f32x3){ 0.0F, 0.0F, 0.0F }, (f32x3){ 0.0F, 0.0F, 0.0F }, (f32x3){ 0.0F, 0.0F, 0.0F });
    f32x3   vec    = { 5.0F, 6.0F, 7.0F };
    f32x3   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 0.0F);
    nya_assert(result.y == 0.0F);
    nya_assert(result.z == 0.0F);
  }

  // TEST: nya_matrix_times_vector - rotation-like matrix
  {
    // 90 degree rotation around Z axis (approximately)
    f32_2x2 mat    = nya_matrix_create((f32x2){ 0.0F, -1.0F }, (f32x2){ 1.0F, 0.0F });
    f32x2   vec    = { 1.0F, 0.0F }; // Unit X vector
    f32x2   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 0.0F);
    nya_assert(result.y == 1.0F);
  }

  // TEST: nya_matrix_times_vector - general 3x3 matrix
  {
    f32_3x3 mat    = nya_matrix_create((f32x3){ 1.0F, 2.0F, 3.0F }, (f32x3){ 4.0F, 5.0F, 6.0F }, (f32x3){ 7.0F, 8.0F, 9.0F });
    f32x3   vec    = { 1.0F, 0.0F, 0.0F };
    f32x3   result = nya_matrix_times_vector(mat, vec);

    // Multiplying by {1, 0, 0} extracts the first column (first element of each row)
    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 4.0F);
    nya_assert(result.z == 7.0F);
  }

  // TEST: nya_matrix_create from 2D array
  {
    f32 entries[2][2] = {
      { 1.0F, 2.0F },
      { 3.0F, 4.0F }
    };
    f32_2x2 mat    = nya_matrix_create(entries);
    f32x2   vec    = { 1.0F, 0.0F };
    f32x2   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 3.0F);
  }

  // TEST: nya_matrix_create from 3D array
  {
    f32 entries[3][3] = {
      { 1.0F, 0.0F, 0.0F },
      { 0.0F, 2.0F, 0.0F },
      { 0.0F, 0.0F, 3.0F }
    };
    f32_3x3 mat    = nya_matrix_create(entries);
    f32x3   vec    = { 1.0F, 1.0F, 1.0F };
    f32x3   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 2.0F);
    nya_assert(result.z == 3.0F);
  }

  // TEST: nya_matrix_create from 4D array
  {
    f32 entries[4][4] = {
      { 1.0F, 0.0F, 0.0F, 0.0F },
      { 0.0F, 1.0F, 0.0F, 0.0F },
      { 0.0F, 0.0F, 1.0F, 0.0F },
      { 0.0F, 0.0F, 0.0F, 1.0F }
    };
    f32_4x4 mat    = nya_matrix_create(entries);
    f32x4   vec    = { 5.0F, 6.0F, 7.0F, 8.0F };
    f32x4   result = nya_matrix_times_vector(mat, vec);

    nya_assert(result.x == 5.0F);
    nya_assert(result.y == 6.0F);
    nya_assert(result.z == 7.0F);
    nya_assert(result.w == 8.0F);
  }

  // TEST: Predefined zero matrices
  {
    f32x3 vec    = { 1.0F, 2.0F, 3.0F };
    f32x3 result = nya_matrix_times_vector(f32_3x3_zero, vec);

    nya_assert(result.x == 0.0F);
    nya_assert(result.y == 0.0F);
    nya_assert(result.z == 0.0F);
  }

  // TEST: Predefined identity matrices
  {
    f32x4 vec    = { 1.0F, 2.0F, 3.0F, 4.0F };
    f32x4 result = nya_matrix_times_vector(f32_4x4_id, vec);

    nya_assert(result.x == 1.0F);
    nya_assert(result.y == 2.0F);
    nya_assert(result.z == 3.0F);
    nya_assert(result.w == 4.0F);
  }

  // CLEANUP
  nya_arena_destroy(arena);

  return 0;
}
