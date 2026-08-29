/**
 * The matrix overloads the existing tests never reach, and nya_matrix_transform, which nothing did.
 *
 * `nya_matrix_create` and `nya_matrix_times_vector` are overloaded across four element types and three
 * sizes — twenty-four functions — and coverage showed 26 of the module's 41 never called. Exercising the
 * non-f32 ones is not busywork: they are generated from the same macro, so a mistake in it is invisible
 * until something instantiates the type that has it, and the overload set is exactly what would break.
 *
 * `nya_matrix_transform` is the one with real logic here, and it had no test at all.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
    // ── nya_matrix_transform: translation, rotation and scale composed into one matrix.
    {
        // Identity rotation and unit scale: the translation should sit in the fourth column.
        f32_3x3 no_rotation = nya_matrix_create((f32x3){ 1, 0, 0 }, (f32x3){ 0, 1, 0 }, (f32x3){ 0, 0, 1 });
        f32_4x4 moved       = nya_matrix_transform((f32x3){ 5.0F, -3.0F, 2.0F }, no_rotation, (f32x3){ 1, 1, 1 });

        nya_check(fabsf(moved[0][3] - 5.0F) < 0.001F, "translation x should be in the fourth column, got %f", (f64)moved[0][3]);
        nya_check(fabsf(moved[1][3] + 3.0F) < 0.001F, "translation y likewise, got %f", (f64)moved[1][3]);
        nya_check(fabsf(moved[2][3] - 2.0F) < 0.001F, "translation z likewise, got %f", (f64)moved[2][3]);
        nya_check(fabsf(moved[3][3] - 1.0F) < 0.001F, "and the homogeneous corner should be one");

        // A point through it should land at the translation.
        f32x4 origin = nya_matrix_times_vector(moved, (f32x4){ 0, 0, 0, 1 });
        nya_check(fabsf(origin.x - 5.0F) < 0.001F && fabsf(origin.y + 3.0F) < 0.001F && fabsf(origin.z - 2.0F) < 0.001F,
                  "the origin should map to the translation, got (%f, %f, %f)", (f64)origin.x, (f64)origin.y, (f64)origin.z);

        // Scale must multiply the basis, not the translation.
        f32_4x4 scaled = nya_matrix_transform((f32x3){ 0, 0, 0 }, no_rotation, (f32x3){ 2.0F, 3.0F, 4.0F });
        f32x4   unit   = nya_matrix_times_vector(scaled, (f32x4){ 1, 1, 1, 1 });
        nya_check(fabsf(unit.x - 2.0F) < 0.001F && fabsf(unit.y - 3.0F) < 0.001F && fabsf(unit.z - 4.0F) < 0.001F,
                  "scale should multiply each axis, got (%f, %f, %f)", (f64)unit.x, (f64)unit.y, (f64)unit.z);

        // Rotation and translation together: rotate a quarter turn about z, then move.
        NYA_Quaternion quarter  = nya_quaternion_from_axis_angle((f32x3){ 0, 0, 1 }, 1.5707963F);
        f32_3x3        rotation = nya_quaternion_to_matrix3(quarter);
        f32_4x4        both     = nya_matrix_transform((f32x3){ 10.0F, 0.0F, 0.0F }, rotation, (f32x3){ 1, 1, 1 });

        // +x rotated a quarter turn about z is +y, then translated by +10x.
        f32x4 mapped = nya_matrix_times_vector(both, (f32x4){ 1, 0, 0, 1 });
        nya_check(fabsf(mapped.x - 10.0F) < 0.01F, "rotation then translation: x, got %f", (f64)mapped.x);
        nya_check(fabsf(mapped.y - 1.0F) < 0.01F, "rotation then translation: y, got %f", (f64)mapped.y);
    }

    // ── f32 create and times_vector at every size, as the reference the other types are checked against.
    {
        f32_2x2 m2 = nya_matrix_create((f32x2){ 1, 2 }, (f32x2){ 3, 4 });
        f32x2   v2 = nya_matrix_times_vector(m2, (f32x2){ 1, 1 });
        nya_check(fabsf(v2.x - 3.0F) < 0.001F && fabsf(v2.y - 7.0F) < 0.001F, "2x2 by (1,1) is row sums, got (%f, %f)",
                  (f64)v2.x, (f64)v2.y);

        f32_3x3 m3 = nya_matrix_create((f32x3){ 1, 0, 0 }, (f32x3){ 0, 2, 0 }, (f32x3){ 0, 0, 3 });
        f32x3   v3 = nya_matrix_times_vector(m3, (f32x3){ 1, 1, 1 });
        nya_check(fabsf(v3.x - 1.0F) < 0.001F && fabsf(v3.y - 2.0F) < 0.001F && fabsf(v3.z - 3.0F) < 0.001F,
                  "3x3 diagonal scales each axis, got (%f, %f, %f)", (f64)v3.x, (f64)v3.y, (f64)v3.z);

        f32_4x4 m4 = nya_matrix_create((f32x4){ 2, 0, 0, 0 }, (f32x4){ 0, 2, 0, 0 }, (f32x4){ 0, 0, 2, 0 }, (f32x4){ 0, 0, 0, 1 });
        f32x4   v4 = nya_matrix_times_vector(m4, (f32x4){ 1, 2, 3, 1 });
        nya_check(fabsf(v4.x - 2.0F) < 0.001F && fabsf(v4.y - 4.0F) < 0.001F && fabsf(v4.z - 6.0F) < 0.001F,
                  "4x4 doubles xyz, got (%f, %f, %f)", (f64)v4.x, (f64)v4.y, (f64)v4.z);
    }

    // ── f64: the same shapes, at double precision.
    {
        f64_2x2 m2 = nya_matrix_create((f64x2){ 1, 2 }, (f64x2){ 3, 4 });
        f64x2   v2 = nya_matrix_times_vector(m2, (f64x2){ 1, 1 });
        nya_check(fabs(v2.x - 3.0) < 1e-9 && fabs(v2.y - 7.0) < 1e-9, "f64 2x2, got (%f, %f)", v2.x, v2.y);

        f64_3x3 m3 = nya_matrix_create((f64x3){ 1, 0, 0 }, (f64x3){ 0, 2, 0 }, (f64x3){ 0, 0, 3 });
        f64x3   v3 = nya_matrix_times_vector(m3, (f64x3){ 1, 1, 1 });
        nya_check(fabs(v3.z - 3.0) < 1e-9, "f64 3x3, got z=%f", v3.z);

        f64_4x4 m4 = nya_matrix_create((f64x4){ 2, 0, 0, 0 }, (f64x4){ 0, 2, 0, 0 }, (f64x4){ 0, 0, 2, 0 }, (f64x4){ 0, 0, 0, 1 });
        f64x4   v4 = nya_matrix_times_vector(m4, (f64x4){ 1, 2, 3, 1 });
        nya_check(fabs(v4.y - 4.0) < 1e-9, "f64 4x4, got y=%f", v4.y);

        // Double precision must actually be double: a value f32 cannot hold must survive.
        f64_2x2 precise = nya_matrix_create((f64x2){ 1.0 + 1e-12, 0 }, (f64x2){ 0, 1 });
        f64x2   through = nya_matrix_times_vector(precise, (f64x2){ 1, 0 });
        nya_check(through.x != 1.0, "an f64 matrix must not round to f32 precision");
    }

    // ── f16: the same shapes, at half precision. Tolerances are wide because the type is.
    {
        f16_2x2 m2 = nya_matrix_create((f16x2){ 1, 2 }, (f16x2){ 3, 4 });
        f16x2   v2 = nya_matrix_times_vector(m2, (f16x2){ 1, 1 });
        nya_check(fabs((f64)v2.x - 3.0) < 0.05 && fabs((f64)v2.y - 7.0) < 0.05, "f16 2x2, got (%f, %f)", (f64)v2.x, (f64)v2.y);

        f16_3x3 m3 = nya_matrix_create((f16x3){ 1, 0, 0 }, (f16x3){ 0, 2, 0 }, (f16x3){ 0, 0, 3 });
        f16x3   v3 = nya_matrix_times_vector(m3, (f16x3){ 1, 1, 1 });
        nya_check(fabs((f64)v3.z - 3.0) < 0.05, "f16 3x3, got z=%f", (f64)v3.z);

        f16_4x4 m4 = nya_matrix_create((f16x4){ 2, 0, 0, 0 }, (f16x4){ 0, 2, 0, 0 }, (f16x4){ 0, 0, 2, 0 }, (f16x4){ 0, 0, 0, 1 });
        f16x4   v4 = nya_matrix_times_vector(m4, (f16x4){ 1, 2, 3, 1 });
        nya_check(fabs((f64)v4.z - 6.0) < 0.05, "f16 4x4, got z=%f", (f64)v4.z);
    }

    // ── f128: the widest, which nothing had ever instantiated.
    {
        f128_2x2 m2 = nya_matrix_create((f128x2){ 1, 2 }, (f128x2){ 3, 4 });
        f128x2   v2 = nya_matrix_times_vector(m2, (f128x2){ 1, 1 });
        nya_check(fabsl(v2.x - 3.0L) < 1e-12L && fabsl(v2.y - 7.0L) < 1e-12L, "f128 2x2, got (%Lf, %Lf)", v2.x, v2.y);

        /*
         * These two used to be absent: instantiating nya_matrix_create for f128x3 trips an
         * AddressSanitizer stack-buffer-overflow, a 40-byte write past a 32-byte stack object. The
         * cause turned out to be ext_vector_type(3) over x87 long double and not matrix_type — see
         * the note on the f128x3 typedef in math_vector.h. Sanitizers are on in this build, so these
         * cases are the regression test for that workaround: if the padding lane is ever removed,
         * the first line below aborts the process.
         */
        f128_3x3 m3 = nya_matrix_create((f128x3){ 1, 0, 0 }, (f128x3){ 0, 2, 0 }, (f128x3){ 0, 0, 3 });
        f128x3   v3 = nya_matrix_times_vector(m3, (f128x3){ 1, 1, 1 });
        nya_check(fabsl(v3.x - 1.0L) < 1e-12L && fabsl(v3.y - 2.0L) < 1e-12L && fabsl(v3.z - 3.0L) < 1e-12L,
                  "f128 3x3 diagonal scales each axis, got (%Lf, %Lf, %Lf)", v3.x, v3.y, v3.z);

        f128_4x4 m4 = nya_matrix_create((f128x4){ 2, 0, 0, 0 }, (f128x4){ 0, 2, 0, 0 }, (f128x4){ 0, 0, 2, 0 },
                                        (f128x4){ 0, 0, 0, 1 });
        f128x4   v4 = nya_matrix_times_vector(m4, (f128x4){ 1, 2, 3, 1 });
        nya_check(fabsl(v4.x - 2.0L) < 1e-12L && fabsl(v4.y - 4.0L) < 1e-12L && fabsl(v4.z - 6.0L) < 1e-12L
                      && fabsl(v4.w - 1.0L) < 1e-12L,
                  "f128 4x4, got (%Lf, %Lf, %Lf, %Lf)", v4.x, v4.y, v4.z, v4.w);

        // The zero and identity macros expand to nya_matrix_create, so they instantiate it too.
        f128_3x3 id3 = f128_3x3_id;
        nya_check(fabsl(id3[0][0] - 1.0L) < 1e-12L && fabsl(id3[0][1]) < 1e-12L, "f128_3x3_id should be the identity");
    }

    // ── The array-of-entries overloads, which take a C array rather than rows.
    {
        f32 entries2[2][2] = { { 1, 2 }, { 3, 4 } };
        f32_2x2 m2 = nya_matrix_create(entries2);
        nya_check(fabsf(m2[0][1] - 2.0F) < 0.001F, "entries should land where the rows say, got %f", (f64)m2[0][1]);

        f32 entries3[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 5 } };
        f32_3x3 m3 = nya_matrix_create(entries3);
        nya_check(fabsf(m3[2][2] - 5.0F) < 0.001F, "3x3 from entries, got %f", (f64)m3[2][2]);

        f32 entries4[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 7, 0, 0, 1 } };
        f32_4x4 m4 = nya_matrix_create(entries4);
        nya_check(fabsf(m4[3][0] - 7.0F) < 0.001F, "4x4 from entries, got %f", (f64)m4[3][0]);

        f64 entries_wide[2][2] = { { 9, 0 }, { 0, 9 } };
        f64_2x2 wide = nya_matrix_create(entries_wide);
        nya_check(fabs(wide[1][1] - 9.0) < 1e-9, "f64 from entries, got %f", wide[1][1]);
    }

    // ── Perspective and the two orthographics, checked as behaviour rather than as element values.
    {
        f32_4x4 projection = nya_matrix_perspective(1.0F, 16.0F / 9.0F, 0.1F, 100.0F);

        // A point at the near plane should land at depth 0, and at the far plane at w-divided depth 1 —
        // this projection maps depth onto [0, 1], which is why the frustum's near plane is row 2 alone.
        f32x4 near_point = nya_matrix_times_vector(projection, (f32x4){ 0, 0, -0.1F, 1 });
        f32x4 far_point  = nya_matrix_times_vector(projection, (f32x4){ 0, 0, -100.0F, 1 });

        nya_check(fabsf(near_point.z / near_point.w) < 0.01F, "the near plane should map to depth 0, got %f",
                  (f64)(near_point.z / near_point.w));
        nya_check(fabsf((far_point.z / far_point.w) - 1.0F) < 0.01F, "and the far plane to depth 1, got %f",
                  (f64)(far_point.z / far_point.w));

        f32_4x4 ortho = nya_matrix_orthographic_3d(10.0F, 1.0F, 0.1F, 50.0F);
        f32x4   flat  = nya_matrix_times_vector(ortho, (f32x4){ 1, 1, -10.0F, 1 });
        nya_check(fabsf(flat.w - 1.0F) < 0.001F, "an orthographic projection should not divide, w=%f", (f64)flat.w);
    }

    // ── look_at builds a view that puts the target ahead of the eye.
    {
        f32_4x4 view = nya_matrix_look_at((f32x3){ 0, 0, 10 }, (f32x3){ 0, 0, 0 }, (f32x3){ 0, 1, 0 });

        // The target, in view space, should sit down the negative z axis at the eye's distance.
        f32x4 target = nya_matrix_times_vector(view, (f32x4){ 0, 0, 0, 1 });
        nya_check(fabsf(target.z + 10.0F) < 0.01F, "the target should be 10 ahead, got z=%f", (f64)target.z);
        nya_check(fabsf(target.x) < 0.01F && fabsf(target.y) < 0.01F, "and centred");

        // The eye itself maps to the view-space origin.
        f32x4 eye = nya_matrix_times_vector(view, (f32x4){ 0, 0, 10, 1 });
        nya_check(nya_vector_length((f32x3){ eye.x, eye.y, eye.z }) < 0.01F, "the eye should map to the origin");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
