/**
 * The `.cube` parser: the shipped tables, exactness of the identity, and malformed files refused.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define WHITE ((f32x3){ 1.0F, 1.0F, 1.0F })

/** Clamps each channel into [0, 1]. */
static f32x3 unit(f32x3 colour) {
    return (f32x3){ nya_clamp(colour.x, 0.0F, 1.0F), nya_clamp(colour.y, 0.0F, 1.0F), nya_clamp(colour.z, 0.0F, 1.0F) };
}

/** What the GPU does with a 3D texture: trilinear over the entries, coordinates mapped onto texel centres. */
static f32x3 sample(const NYA_Lut* lut, f32x3 colour) {
    f32x3 result = { 0 };

    f32 scaled[3] = { colour.x * (f32)(lut->size - 1), colour.y * (f32)(lut->size - 1), colour.z * (f32)(lut->size - 1) };
    u32 base[3];
    f32 weight[3];

    for (u32 axis = 0; axis < 3; axis++) {
        base[axis]   = nya_min((u32)scaled[axis], lut->size - 2);
        weight[axis] = scaled[axis] - (f32)base[axis];
    }

    for (u32 corner = 0; corner < 8; corner++) {
        u32 r = base[0] + (corner & 1U);
        u32 g = base[1] + ((corner >> 1) & 1U);
        u32 b = base[2] + ((corner >> 2) & 1U);

        f32 w = ((corner & 1U) ? weight[0] : 1.0F - weight[0]) * (((corner >> 1) & 1U) ? weight[1] : 1.0F - weight[1])
              * (((corner >> 2) & 1U) ? weight[2] : 1.0F - weight[2]);

        const u8* texel = &lut->texels[(((u64)b * lut->size + g) * lut->size + r) * 4];

        result += (f32x3){ texel[0], texel[1], texel[2] } * (w / 255.0F);
    }

    return result;
}

/** Parses `text`, which must fail, and checks it failed as a parse error rather than anything worse. */
static void expect_refused(NYA_Arena* arena, NYA_ConstCString text, NYA_ConstCString why) {
    NYA_Lut   lut    = { .size = 7 };
    NYA_Error result = nya_lut_parse(arena, (const u8*)text, strlen(text), &lut);

    nya_check(!result.ok && result.kind == NYA_ERROR_PARSE, "%s should be refused as a parse error", why);
    nya_check(lut.size == 0 && lut.texels == nullptr, "and leave the table empty: %s", why);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_render_lut");
    defer      nya_arena_destroy(arena);

    // The shipped identity maps every colour to itself exactly.
    {
        NYA_String file = *nya_string_create(arena);
        NYA_EXPECT(nya_file_read("./assets/grades/identity.cube", &file));

        NYA_Lut lut = { 0 };
        NYA_EXPECT(nya_lut_parse(arena, file.items, file.length, &lut));

        nya_check(lut.size == 2, "the identity is two entries a side, got " FMTu32, lut.size);

        for (u32 i = 0; i < 1000; i++) {
            f32x3 colour = { (f32)(i % 10) / 9.0F, (f32)((i / 10) % 10) / 9.0F, (f32)(i / 100) / 9.0F };
            colour      += (f32x3){ 0.013F, 0.029F, 0.047F } * (f32)(i % 3);
            colour       = unit(colour);

            f32x3 graded = sample(&lut, colour);

            nya_check(nya_vector_length(graded - colour) < 1e-5F, "identity moved (%f, %f, %f)", (f64)colour.x, (f64)colour.y, (f64)colour.z);
        }
    }

    // A finer identity is exact within the eight bit quantisation of its entries.
    {
        const u32 size = 17;

        NYA_String* text = nya_string_create(arena);
        nya_string_extend(text, "# comment\r\nTITLE \"generated identity\"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1.0 1.0 1.0\n");
        nya_string_extend_sprintf(text, "LUT_3D_SIZE " FMTu32 "\n\n", size);

        for (u32 b = 0; b < size; b++) {
            for (u32 g = 0; g < size; g++) {
                for (u32 r = 0; r < size; r++) {
                    nya_string_extend_sprintf(text, "%.6f\t%.6f %.6f\n", (f64)r / (size - 1), (f64)g / (size - 1), (f64)b / (size - 1));
                }
            }
        }

        NYA_Lut lut = { 0 };
        NYA_EXPECT(nya_lut_parse(arena, text->items, text->length, &lut));

        f32 worst = 0.0F;

        for (u32 i = 0; i < 4096; i++) {
            f32x3 colour = { (f32)(i & 15U) / 15.0F, (f32)((i >> 4) & 15U) / 15.0F, (f32)((i >> 8) & 15U) / 15.0F };
            colour       = unit((colour * 0.97F) + 0.011F);

            f32x3 error = sample(&lut, colour) - colour;

            worst = nya_max(worst, nya_max(fabsf(error.x), nya_max(fabsf(error.y), fabsf(error.z))));
        }

        nya_check(worst <= 0.5F / 255.0F + 1e-5F, "a 17 entry identity should be within half a step, worst %f", (f64)worst * 255.0);
    }

    // The shipped grade keeps black and white where they are.
    {
        NYA_String file = *nya_string_create(arena);
        NYA_EXPECT(nya_file_read("./assets/grades/vivid.cube", &file));

        NYA_Lut lut = { 0 };
        NYA_EXPECT(nya_lut_parse(arena, file.items, file.length, &lut));

        nya_check(nya_vector_length(sample(&lut, f32x3_zero)) < 1e-5F, "the grade should keep black");
        nya_check(nya_vector_length(sample(&lut, WHITE) - WHITE) < 1e-5F, "and white");
    }

    // Composing: a fade and a colour vision baked into the table.
    {
        NYA_String identity_file = *nya_string_create(arena);
        NYA_String vivid_file    = *nya_string_create(arena);
        NYA_EXPECT(nya_file_read("./assets/grades/identity.cube", &identity_file));
        NYA_EXPECT(nya_file_read("./assets/grades/vivid.cube", &vivid_file));

        NYA_Lut identity = { 0 };
        NYA_Lut vivid    = { 0 };
        NYA_EXPECT(nya_lut_parse(arena, identity_file.items, identity_file.length, &identity));
        NYA_EXPECT(nya_lut_parse(arena, vivid_file.items, vivid_file.length, &vivid));

        NYA_Lut same = nya_lut_compose(arena, vivid, 0.0F, NYA_COLOR_VISION_NONE);
        nya_check(same.texels == vivid.texels && same.size == vivid.size, "nothing to compose should hand the table back");

        NYA_Lut faded = nya_lut_compose(arena, vivid, 1.0F, NYA_COLOR_VISION_NONE);
        NYA_Lut half  = nya_lut_compose(arena, vivid, 0.5F, NYA_COLOR_VISION_NONE);
        nya_check(faded.size == NYA_LUT_COMPOSE_SIZE_MIN, "a composed table is at least NYA_LUT_COMPOSE_SIZE_MIN a side");

        for (u32 i = 0; i < 512; i++) {
            f32x3 colour = unit((f32x3){ (f32)(i & 7U) / 7.0F, (f32)((i >> 3) & 7U) / 7.0F, (f32)(i >> 6) / 7.0F } * 0.93F + 0.031F);
            f32x3 middle = (sample(&vivid, colour) + colour) * 0.5F;

            nya_check(nya_vector_length(sample(&faded, colour) - colour) < 1.5F / 255.0F, "a full fade should be the identity");
            nya_check(nya_vector_length(sample(&half, colour) - middle) < 1.5F / 255.0F, "half a fade should be halfway to the identity");
        }

        // expected values from I + shift * (I - simulation) in linear light, worked through the sRGB curve by hand.
        struct {
            NYA_ColorVision vision;
            f32x3           colour;
            f32x3           expected;
        } cases[] = {
            { NYA_COLOR_VISION_PROTANOPIA, { 1.0F, 0.0F, 0.0F }, { 255.0F, 183.9F, 203.0F } },
            { NYA_COLOR_VISION_DEUTERANOPIA, { 1.0F, 0.0F, 0.0F }, { 255.0F, 112.2F, 179.7F } },
            { NYA_COLOR_VISION_DEUTERANOPIA, { 0.0F, 1.0F, 0.0F }, { 0.0F, 221.3F, 0.0F } },
            { NYA_COLOR_VISION_TRITANOPIA, { 0.0F, 0.0F, 1.0F }, { 213.1F, 157.5F, 255.0F } },
        };

        for (u32 i = 0; i < nya_carray_length(cases); i++) {
            NYA_Lut corrected = nya_lut_compose(arena, identity, 0.0F, cases[i].vision);
            f32x3   got       = sample(&corrected, cases[i].colour) * 255.0F;

            nya_check(nya_vector_length(got - cases[i].expected) < 1.0F, "case " FMTu32 " got (%.1f, %.1f, %.1f)", i, (f64)got.x, (f64)got.y, (f64)got.z);
        }

        // every row of a simulation sums to one, so no viewer loses anything of a grey.
        for (u32 vision = 0; vision < NYA_COLOR_VISION_COUNT; vision++) {
            NYA_Lut corrected = nya_lut_compose(arena, identity, 0.0F, (NYA_ColorVision)vision);

            for (u32 step = 0; step <= 8; step++) {
                f32x3 grey = (f32x3){ 1.0F, 1.0F, 1.0F } * ((f32)step / 8.0F);
                nya_check(nya_vector_length(sample(&corrected, grey) - grey) < 1.0F / 255.0F, "vision " FMTu32 " moved a grey", vision);
            }
        }
    }

    // Malformed input is an operating error, never an assert.
    {
        expect_refused(arena, "", "an empty file");
        expect_refused(arena, "# only a comment\n", "a file with no size");
        expect_refused(arena, "0 0 0\nLUT_3D_SIZE 2\n", "data before the size");
        expect_refused(arena, "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n", "too few rows");
        expect_refused(arena, "LUT_3D_SIZE 1\n0 0 0\n", "a size of one");
        expect_refused(arena, "LUT_3D_SIZE 65\n", "a size past NYA_LUT_SIZE_MAX");
        expect_refused(arena, "LUT_3D_SIZE two\n", "a size that is not a number");
        expect_refused(arena, "LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n", "a repeated size");
        expect_refused(arena, "LUT_1D_SIZE 16\n", "a 1D table");
        expect_refused(arena, "LUT_3D_SIZE 2\nDOMAIN_MAX 2 2 2\n", "a domain past one");
        expect_refused(arena, "LUT_3D_SIZE 2\n0 0 0 0\n", "a row with four values");
        expect_refused(arena, "LUT_3D_SIZE 2\n0 0\n", "a row with two values");
        expect_refused(arena, "LUT_3D_SIZE 2\n0 zero 0\n", "a value that is not a number");

        NYA_String* long_table = nya_string_create(arena);
        nya_string_extend(long_table, "LUT_3D_SIZE 2\n");
        for (u32 i = 0; i < 9; i++) nya_string_extend(long_table, "0.5 0.5 0.5\n");

        NYA_Lut   lut    = { 0 };
        NYA_Error result = nya_lut_parse(arena, long_table->items, long_table->length, &lut);
        nya_check(!result.ok && result.kind == NYA_ERROR_PARSE, "a ninth row for a size of two should be refused");

        // values outside the unit range are clamped, not refused: grading tools overshoot by a rounding error.
        NYA_ConstCString overshoot = "LUT_3D_SIZE 2\n-0.01 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1.02 1 1\n";
        NYA_EXPECT(nya_lut_parse(arena, (const u8*)overshoot, strlen(overshoot), &lut));
        nya_check(lut.texels[0] == 0 && lut.texels[7 * 4] == 255, "overshoot should clamp into the byte range");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
