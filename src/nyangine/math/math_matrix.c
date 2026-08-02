#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * MATRIX CONSTRUCTORS
 * ─────────────────────────────────────────────────────────
 */

f16_2x2 nya_matrix_create(f16x2 row1, f16x2 row2) __attr_overloaded {
    f16_2x2 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[1][0] = row2[0];
    result[1][1] = row2[1];

    return result;
}

f16_3x3 nya_matrix_create(f16x3 row1, f16x3 row2, f16x3 row3) __attr_overloaded {
    f16_3x3 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];

    return result;
}

f16_4x4 nya_matrix_create(f16x4 row1, f16x4 row2, f16x4 row3, f16x4 row4) __attr_overloaded {
    f16_4x4 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[0][3] = row1[3];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[1][3] = row2[3];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];
    result[2][3] = row3[3];
    result[3][0] = row4[0];
    result[3][1] = row4[1];
    result[3][2] = row4[2];
    result[3][3] = row4[3];

    return result;
}

f32_2x2 nya_matrix_create(f32x2 row1, f32x2 row2) __attr_overloaded {
    f32_2x2 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[1][0] = row2[0];
    result[1][1] = row2[1];

    return result;
}

f32_3x3 nya_matrix_create(f32x3 row1, f32x3 row2, f32x3 row3) __attr_overloaded {
    f32_3x3 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];

    return result;
}

f32_4x4 nya_matrix_create(f32x4 row1, f32x4 row2, f32x4 row3, f32x4 row4) __attr_overloaded {
    f32_4x4 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[0][3] = row1[3];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[1][3] = row2[3];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];
    result[2][3] = row3[3];
    result[3][0] = row4[0];
    result[3][1] = row4[1];
    result[3][2] = row4[2];
    result[3][3] = row4[3];

    return result;
}

f64_2x2 nya_matrix_create(f64x2 row1, f64x2 row2) __attr_overloaded {
    f64_2x2 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[1][0] = row2[0];
    result[1][1] = row2[1];

    return result;
}

f64_3x3 nya_matrix_create(f64x3 row1, f64x3 row2, f64x3 row3) __attr_overloaded {
    f64_3x3 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];

    return result;
}

f64_4x4 nya_matrix_create(f64x4 row1, f64x4 row2, f64x4 row3, f64x4 row4) __attr_overloaded {
    f64_4x4 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[0][3] = row1[3];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[1][3] = row2[3];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];
    result[2][3] = row3[3];
    result[3][0] = row4[0];
    result[3][1] = row4[1];
    result[3][2] = row4[2];
    result[3][3] = row4[3];

    return result;
}

f128_2x2 nya_matrix_create(f128x2 row1, f128x2 row2) __attr_overloaded {
    f128_2x2 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[1][0] = row2[0];
    result[1][1] = row2[1];

    return result;
}

f128_3x3 nya_matrix_create(f128x3 row1, f128x3 row2, f128x3 row3) __attr_overloaded {
    f128_3x3 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];

    return result;
}

f128_4x4 nya_matrix_create(f128x4 row1, f128x4 row2, f128x4 row3, f128x4 row4) __attr_overloaded {
    f128_4x4 result;

    result[0][0] = row1[0];
    result[0][1] = row1[1];
    result[0][2] = row1[2];
    result[0][3] = row1[3];
    result[1][0] = row2[0];
    result[1][1] = row2[1];
    result[1][2] = row2[2];
    result[1][3] = row2[3];
    result[2][0] = row3[0];
    result[2][1] = row3[1];
    result[2][2] = row3[2];
    result[2][3] = row3[3];
    result[3][0] = row4[0];
    result[3][1] = row4[1];
    result[3][2] = row4[2];
    result[3][3] = row4[3];

    return result;
}

f16_2x2 nya_matrix_create(f16 entries[2][2]) __attr_overloaded {
    f16_2x2 result;

    for (s32 i = 0; i < 2; ++i) {
        for (s32 j = 0; j < 2; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f16_3x3 nya_matrix_create(f16 entries[3][3]) __attr_overloaded {
    f16_3x3 result;

    for (s32 i = 0; i < 3; ++i) {
        for (s32 j = 0; j < 3; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f16_4x4 nya_matrix_create(f16 entries[4][4]) __attr_overloaded {
    f16_4x4 result;

    for (s32 i = 0; i < 4; ++i) {
        for (s32 j = 0; j < 4; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f32_2x2 nya_matrix_create(f32 entries[2][2]) __attr_overloaded {
    f32_2x2 result;

    for (s32 i = 0; i < 2; ++i) {
        for (s32 j = 0; j < 2; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f32_3x3 nya_matrix_create(f32 entries[3][3]) __attr_overloaded {
    f32_3x3 result;

    for (s32 i = 0; i < 3; ++i) {
        for (s32 j = 0; j < 3; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f32_4x4 nya_matrix_create(f32 entries[4][4]) __attr_overloaded {
    f32_4x4 result;

    for (s32 i = 0; i < 4; ++i) {
        for (s32 j = 0; j < 4; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f64_2x2 nya_matrix_create(f64 entries[2][2]) __attr_overloaded {
    f64_2x2 result;

    for (s32 i = 0; i < 2; ++i) {
        for (s32 j = 0; j < 2; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f64_3x3 nya_matrix_create(f64 entries[3][3]) __attr_overloaded {
    f64_3x3 result;

    for (s32 i = 0; i < 3; ++i) {
        for (s32 j = 0; j < 3; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f64_4x4 nya_matrix_create(f64 entries[4][4]) __attr_overloaded {
    f64_4x4 result;

    for (s32 i = 0; i < 4; ++i) {
        for (s32 j = 0; j < 4; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f128_2x2 nya_matrix_create(f128 entries[2][2]) __attr_overloaded {
    f128_2x2 result;

    for (s32 i = 0; i < 2; ++i) {
        for (s32 j = 0; j < 2; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f128_3x3 nya_matrix_create(f128 entries[3][3]) __attr_overloaded {
    f128_3x3 result;

    for (s32 i = 0; i < 3; ++i) {
        for (s32 j = 0; j < 3; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

f128_4x4 nya_matrix_create(f128 entries[4][4]) __attr_overloaded {
    f128_4x4 result;

    for (s32 i = 0; i < 4; ++i) {
        for (s32 j = 0; j < 4; ++j) result[i][j] = entries[i][j];
    }

    return result;
}

/*
 * ─────────────────────────────────────────────────────────
 * MATRIX VECTOR MULTIPLICATION
 * ─────────────────────────────────────────────────────────
 */

f16x2 nya_matrix_times_vector(f16_2x2 mat, f16x2 vec) __attr_overloaded {
    return (f16x2){
        mat[0][0] * vec[0] + mat[0][1] * vec[1],
        mat[1][0] * vec[0] + mat[1][1] * vec[1],
    };
}

f16x3 nya_matrix_times_vector(f16_3x3 mat, f16x3 vec) __attr_overloaded {
    return (f16x3){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2],
    };
}

f16x4 nya_matrix_times_vector(f16_4x4 mat, f16x4 vec) __attr_overloaded {
    return (f16x4){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2] + mat[0][3] * vec[3],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2] + mat[1][3] * vec[3],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2] + mat[2][3] * vec[3],
        mat[3][0] * vec[0] + mat[3][1] * vec[1] + mat[3][2] * vec[2] + mat[3][3] * vec[3],
    };
}

f32x2 nya_matrix_times_vector(f32_2x2 mat, f32x2 vec) __attr_overloaded {
    return (f32x2){
        mat[0][0] * vec[0] + mat[0][1] * vec[1],
        mat[1][0] * vec[0] + mat[1][1] * vec[1],
    };
}

f32x3 nya_matrix_times_vector(f32_3x3 mat, f32x3 vec) __attr_overloaded {
    return (f32x3){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2],
    };
}

f32x4 nya_matrix_times_vector(f32_4x4 mat, f32x4 vec) __attr_overloaded {
    return (f32x4){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2] + mat[0][3] * vec[3],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2] + mat[1][3] * vec[3],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2] + mat[2][3] * vec[3],
        mat[3][0] * vec[0] + mat[3][1] * vec[1] + mat[3][2] * vec[2] + mat[3][3] * vec[3],
    };
}

f64x2 nya_matrix_times_vector(f64_2x2 mat, f64x2 vec) __attr_overloaded {
    return (f64x2){
        mat[0][0] * vec[0] + mat[0][1] * vec[1],
        mat[1][0] * vec[0] + mat[1][1] * vec[1],
    };
}

f64x3 nya_matrix_times_vector(f64_3x3 mat, f64x3 vec) __attr_overloaded {
    return (f64x3){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2],
    };
}

f64x4 nya_matrix_times_vector(f64_4x4 mat, f64x4 vec) __attr_overloaded {
    return (f64x4){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2] + mat[0][3] * vec[3],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2] + mat[1][3] * vec[3],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2] + mat[2][3] * vec[3],
        mat[3][0] * vec[0] + mat[3][1] * vec[1] + mat[3][2] * vec[2] + mat[3][3] * vec[3],
    };
}

f128x2 nya_matrix_times_vector(f128_2x2 mat, f128x2 vec) __attr_overloaded {
    return (f128x2){
        mat[0][0] * vec[0] + mat[0][1] * vec[1],
        mat[1][0] * vec[0] + mat[1][1] * vec[1],
    };
}

f128x3 nya_matrix_times_vector(f128_3x3 mat, f128x3 vec) __attr_overloaded {
    return (f128x3){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2],
    };
}

f128x4 nya_matrix_times_vector(f128_4x4 mat, f128x4 vec) __attr_overloaded {
    return (f128x4){
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2] + mat[0][3] * vec[3],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2] + mat[1][3] * vec[3],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2] + mat[2][3] * vec[3],
        mat[3][0] * vec[0] + mat[3][1] * vec[1] + mat[3][2] * vec[2] + mat[3][3] * vec[3],
    };
}
