#include "nyangine/nyangine.h"

// PUBLIC API IMPLEMENTATION

// MATRIX CONSTRUCTORS

#if !NYA_F16_IS_F32
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

#endif
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

#if !NYA_F16_IS_F32
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

#endif
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

// MATRIX VECTOR MULTIPLICATION

#if !NYA_F16_IS_F32
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

#endif
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

// PROJECTIONS

f32_4x4 nya_matrix_orthographic(f32 left, f32 right, f32 top, f32 bottom) {
    nya_assert(right != left, "an orthographic projection needs a non-zero width");
    nya_assert(bottom != top, "an orthographic projection needs a non-zero height");

    // Clip space is the Direct3D convention SDL_GPU normalizes to: x and y in -1..+1 with y up, z in 0..1.
    f32 x_scale = 2.0F / (right - left);
    f32 y_scale = 2.0F / (top - bottom);

    f32 x_translate = -(right + left) / (right - left);
    f32 y_translate = -(top + bottom) / (top - bottom);

    // Rows, matching nya_matrix_create's convention: row 0 is the top row as written down.
    return nya_matrix_create(
        (f32x4){ x_scale, 0.0F, 0.0F, x_translate },
        (f32x4){ 0.0F, y_scale, 0.0F, y_translate },
        // z passes through unscaled into the 0..1 depth range; the only requirement is that z=0 lands inside it, not on the boundary.
        (f32x4){ 0.0F, 0.0F, 1.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

f32_4x4 nya_matrix_perspective(f32 fov_y, f32 aspect, f32 near_plane, f32 far_plane) {
    nya_assert(aspect > 0.0F, "a perspective projection needs a positive aspect ratio, got %f", (f64)aspect);
    nya_assert(fov_y > 0.0F && fov_y < (f32)M_PI, "a field of view must be within (0, pi) radians, got %f", (f64)fov_y);
    nya_assert(near_plane > 0.0F, "a perspective near_plane plane must be positive, got %f", (f64)near_plane);
    nya_assert(far_plane > near_plane, "a perspective far_plane plane must be beyond the near_plane one, got %f and %f", (f64)far_plane, (f64)near_plane);

    // cot(fov_y / 2): how far the near plane is from the camera in units of half its height.
    f32 focal = 1.0F / tanf(fov_y * 0.5F);

    // The z row maps view depth onto 0..1, not -1..1.
    return nya_matrix_create(
        (f32x4){ focal / aspect, 0.0F, 0.0F, 0.0F },
        (f32x4){ 0.0F, focal, 0.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, far_plane / (near_plane - far_plane), (near_plane * far_plane) / (near_plane - far_plane) },
        (f32x4){ 0.0F, 0.0F, -1.0F, 0.0F }
    );
}

f32_4x4 nya_matrix_orthographic_3d(f32 height, f32 aspect, f32 near_plane, f32 far_plane) {
    nya_assert(height > 0.0F, "an orthographic projection needs a positive height, got %f", (f64)height);
    nya_assert(aspect > 0.0F, "an orthographic projection needs a positive aspect ratio, got %f", (f64)aspect);
    nya_assert(far_plane != near_plane, "an orthographic projection needs a non-zero depth range");

    // `height` covers the view top to bottom and maps onto clip space's range of two, hence 2 / height.
    f32 y_scale = 2.0F / height;
    f32 x_scale = y_scale / aspect;

    // Linear in view depth onto 0..1: -near maps to zero, -far to one, and w stays one (no vanishing point).
    return nya_matrix_create(
        (f32x4){ x_scale, 0.0F, 0.0F, 0.0F },
        (f32x4){ 0.0F, y_scale, 0.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 1.0F / (near_plane - far_plane), near_plane / (near_plane - far_plane) },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

f32_4x4 nya_matrix_look_at(f32x3 eye, f32x3 target, f32x3 up) {
    f32x3 forward = nya_vector_normalize(target - eye);

    // Unaimed camera: identity rather than an assert, since the target is often still being computed on the first frame.
    if (nya_vector_dot(forward, forward) < 0.5F) return f32_4x4_id;

    f32x3 right = nya_vector_normalize(nya_vector_cross(forward, up));

    // Zero when `up` is parallel to the view direction; a usable frame beats a matrix of NaNs.
    if (nya_vector_dot(right, right) < 0.5F) return f32_4x4_id;

    // the cross of two orthogonal unit vectors is already unit, so no renormalize.
    f32x3 above = nya_vector_cross(right, forward);

    // Rotation is the transpose of the camera basis (the view moves the world into the camera frame); translation is the negated eye projected onto each axis.
    return nya_matrix_create(
        (f32x4){ right.x, right.y, right.z, -nya_vector_dot(right, eye) },
        (f32x4){ above.x, above.y, above.z, -nya_vector_dot(above, eye) },
        (f32x4){ -forward.x, -forward.y, -forward.z, nya_vector_dot(forward, eye) },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

f32_4x4 nya_matrix_transform(f32x3 translation, f32_3x3 rotation, f32x3 scale) {
    // Written by rows as nya_matrix_create takes them, but stored by columns.
    return nya_matrix_create(
        (f32x4){ rotation[0][0] * scale.x, rotation[0][1] * scale.y, rotation[0][2] * scale.z, translation.x },
        (f32x4){ rotation[1][0] * scale.x, rotation[1][1] * scale.y, rotation[1][2] * scale.z, translation.y },
        (f32x4){ rotation[2][0] * scale.x, rotation[2][1] * scale.y, rotation[2][2] * scale.z, translation.z },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}
