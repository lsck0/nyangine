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

/*
 * ─────────────────────────────────────────────────────────
 * PROJECTIONS
 * ─────────────────────────────────────────────────────────
 */

f32_4x4 nya_matrix_orthographic(f32 left, f32 right, f32 top, f32 bottom) {
    nya_assert(right != left, "an orthographic projection needs a non-zero width");
    nya_assert(bottom != top, "an orthographic projection needs a non-zero height");

    /*
     * Clip space here is the Direct3D style one SDL_GPU normalizes every backend to: x and y run
     * -1 to +1 with y pointing **up**, and z runs 0 to 1 rather than -1 to 1.
     *
     * The y flip that turns a y-down input into that is not a separate step, it falls out of the
     * scale being negative whenever `bottom` is greater than `top`. So the same expression serves
     * both conventions and there is no branch deciding which one is in play.
     *
     * Getting this wrong renders the whole scene mirrored vertically, which reads as broken geometry
     * rather than a broken projection — hence spelling out which convention is assumed.
     */
    f32 x_scale = 2.0F / (right - left);
    f32 y_scale = 2.0F / (top - bottom);

    f32 x_translate = -(right + left) / (right - left);
    f32 y_translate = -(top + bottom) / (top - bottom);

    // Rows, matching nya_matrix_create's convention: row 0 is the top row as written down.
    return nya_matrix_create(
        (f32x4){ x_scale, 0.0F, 0.0F, x_translate },
        (f32x4){ 0.0F, y_scale, 0.0F, y_translate },
        // z passes through unscaled into the 0..1 depth range. Nothing drawn through this is depth
        // tested, so the only requirement is that a z of 0 lands inside the range rather than on its
        // boundary, where a clipper is free to reject it.
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

    /*
     * The z row maps view depth onto 0..1, not -1..1.
     *
     * At view z = -near it gives far * -near / (near - far) + near * far / (near - far), which is
     * zero, over a w of near. At view z = -far it gives far, over a w of far, which is one. The
     * fourth row is -1 rather than +1 because the view looks down -z, and that minus is what makes w
     * positive for anything in front of the camera.
     */
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

    // Half extents, because `height` is what the view covers top to bottom and the scale maps that
    // onto clip space's -1..+1 — a range of two, hence 2 / height rather than 1 / height.
    f32 y_scale = 2.0F / height;
    f32 x_scale = y_scale / aspect;

    // Linear in view depth, unlike the perspective one, and onto the same 0..1: -near maps to zero
    // and -far maps to one. w stays at one throughout, which is the whole of what "no vanishing
    // point" means.
    return nya_matrix_create(
        (f32x4){ x_scale, 0.0F, 0.0F, 0.0F },
        (f32x4){ 0.0F, y_scale, 0.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 1.0F / (near_plane - far_plane), near_plane / (near_plane - far_plane) },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

f32_4x4 nya_matrix_look_at(f32x3 eye, f32x3 target, f32x3 up) {
    f32x3 forward = nya_vector_normalize(target - eye);

    // A camera that has not been aimed anywhere. Identity rather than an assert, because this is
    // routinely called with a target that is still being computed on the first frame.
    if (nya_vector_dot(forward, forward) < 0.5F) return f32_4x4_id;

    f32x3 right = nya_vector_normalize(nya_vector_cross(forward, up));

    // Zero exactly when `up` is parallel to the view direction, which names no roll at all. Same
    // reasoning as above: a usable frame beats a matrix of NaNs.
    if (nya_vector_dot(right, right) < 0.5F) return f32_4x4_id;

    // Already unit and already perpendicular to both, being the cross of two orthogonal unit
    // vectors — so this one is not renormalized, and does not need to be.
    f32x3 above = nya_vector_cross(right, forward);

    /*
     * The rotation is the transpose of the camera's basis, because a view matrix moves the *world*
     * into the camera's frame rather than moving the camera. The translation is the negated
     * projection of the eye onto each axis, which is the same statement applied to the origin.
     *
     * The third row is negated: the basis points along the view direction and clip space wants
     * depth increasing away down -z.
     */
    return nya_matrix_create(
        (f32x4){ right.x, right.y, right.z, -nya_vector_dot(right, eye) },
        (f32x4){ above.x, above.y, above.z, -nya_vector_dot(above, eye) },
        (f32x4){ -forward.x, -forward.y, -forward.z, nya_vector_dot(forward, eye) },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

f32_4x4 nya_matrix_transform(f32x3 translation, f32_3x3 rotation, f32x3 scale) {
    /*
     * Built by rows, which is what nya_matrix_create takes, and is worth stating because the result is
     * *stored* by columns. The two are not in conflict — one is how the matrix is written down here and
     * the other is how clang lays a matrix type out in memory — but anything that later reads these
     * sixteen floats raw is reading columns.
     *
     * The rotation columns are scaled rather than the rows. Scaling a column scales the axis that basis
     * vector maps *from*, which is the object's own x, y and z — a scale in model space, applied before
     * the rotation. Scaling the rows would scale the world axes it maps *to*, which is a scale applied
     * after, and is the version that shears.
     */
    return nya_matrix_create(
        (f32x4){ rotation[0][0] * scale.x, rotation[0][1] * scale.y, rotation[0][2] * scale.z, translation.x },
        (f32x4){ rotation[1][0] * scale.x, rotation[1][1] * scale.y, rotation[1][2] * scale.z, translation.y },
        (f32x4){ rotation[2][0] * scale.x, rotation[2][1] * scale.y, rotation[2][2] * scale.z, translation.z },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}
