/**
 * @file render_lut.c
 * */
#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * What each viewer sees, linear RGB to linear RGB, by NYA_ColorVision. Machado, Oliveira and Fernandes, "A
 * Physiologically-based Model for Simulation of Color Vision Deficiency", IEEE TVCG 15(6), 2009: the severity 1.0
 * matrices of its table. Every row sums to one, so a grey loses nothing and the correction leaves it alone.
 * */
NYA_INTERNAL const f32 _NYA_LUT_VISION_SIMULATION[NYA_COLOR_VISION_COUNT][3][3] = {
    [NYA_COLOR_VISION_NONE]         = { { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
    [NYA_COLOR_VISION_PROTANOPIA]   = { { 0.152286F, 1.052583F, -0.204868F }, { 0.114503F, 0.786281F, 0.099216F }, { -0.003882F, -0.048116F, 1.051998F } },
    [NYA_COLOR_VISION_DEUTERANOPIA] = { { 0.367322F, 0.860646F, -0.227968F }, { 0.280085F, 0.672501F, 0.047413F }, { -0.011820F, 0.042940F, 0.968881F } },
    [NYA_COLOR_VISION_TRITANOPIA]   = { { 1.255528F, -0.076749F, -0.178779F }, { -0.078411F, 0.930809F, 0.147602F }, { 0.004733F, 0.691367F, 0.303900F } },
};

/**
 * Where the unseen part of a colour goes. Fidaner, Lin and Ozguven, "Analysis of Color Blindness" (2005), the
 * daltonize error shift: red's error into green and blue. Its one matrix is mirrored for tritanopia onto blue's error,
 * into red and green, since shifting into blue would add what a tritanope cannot see.
 * */
NYA_INTERNAL const f32 _NYA_LUT_VISION_SHIFT[NYA_COLOR_VISION_COUNT][3][3] = {
    [NYA_COLOR_VISION_PROTANOPIA]   = { { 0.0F, 0.0F, 0.0F }, { 0.7F, 1.0F, 0.0F }, { 0.7F, 0.0F, 1.0F } },
    [NYA_COLOR_VISION_DEUTERANOPIA] = { { 0.0F, 0.0F, 0.0F }, { 0.7F, 1.0F, 0.0F }, { 0.7F, 0.0F, 1.0F } },
    [NYA_COLOR_VISION_TRITANOPIA]   = { { 1.0F, 0.0F, 0.7F }, { 0.0F, 1.0F, 0.7F }, { 0.0F, 0.0F, 0.0F } },
};

/*
 * The sRGB transfer function, IEC 61966-2-1: a table holds what the swapchain shows, the matrices act on light.
 * */
#define _NYA_LUT_SRGB_LINEAR_BELOW  0.04045F
#define _NYA_LUT_SRGB_ENCODE_BELOW  0.0031308F
#define _NYA_LUT_SRGB_SLOPE         12.92F
#define _NYA_LUT_SRGB_OFFSET        0.055F
#define _NYA_LUT_SRGB_GAMMA         2.4F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Splits `line` on spaces and tabs into at most `capacity` tokens. Returns how many there were, uncapped. */
NYA_INTERNAL u32 _nya_lut_tokens(const u8* line, u64 length, OUT const u8** out_starts, OUT u64* out_lengths, u32 capacity);

/** Whether a token spells `keyword` exactly. */
NYA_INTERNAL b8 _nya_lut_token_is(const u8* token, u64 length, NYA_ConstCString keyword) __attr_no_discard;

/** What the GPU reads for `colour`: trilinear over the entries. */
NYA_INTERNAL f32x3 _nya_lut_sample(NYA_Lut lut, f32x3 colour) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_lut_parse(NYA_Arena* arena, const u8* text, u64 length, OUT NYA_Lut* out_lut) {
    nya_assert(arena != nullptr);
    nya_assert(text != nullptr || length == 0);
    nya_assert(out_lut != nullptr);

    *out_lut = (NYA_Lut){ 0 };

    u32 size    = 0;
    u32 entries = 0;
    u32 line_number = 0;
    u8* texels  = nullptr;

    u64 cursor = 0;

    while (cursor < length) {
        u64 line_start = cursor;
        while (cursor < length && text[cursor] != '\n') cursor++;

        u64 line_length = cursor - line_start;
        cursor++;
        line_number++;

        const u8* line = &text[line_start];

        // four slots, so a data row with a fourth value is seen as one rather than read as three.
        const u8* starts[4];
        u64       lengths[4];
        u32       count = _nya_lut_tokens(line, line_length, starts, lengths, nya_carray_length(starts));

        if (count == 0 || starts[0][0] == '#') continue;

        u8 first = starts[0][0];

        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) {
            // the title is free text and the only keyword that may carry spaces.
            if (_nya_lut_token_is(starts[0], lengths[0], "TITLE")) continue;

            if (entries > 0) return nya_error(NYA_ERROR_PARSE, "line %u: a keyword after the table data", line_number);

            if (_nya_lut_token_is(starts[0], lengths[0], "LUT_3D_SIZE")) {
                if (count != 2 || size != 0 || !nya_type_parse(NYA_TYPE_U32, starts[1], lengths[1], &size)) {
                    return nya_error(NYA_ERROR_PARSE, "line %u: LUT_3D_SIZE needs one size, once", line_number);
                }

                if (size < 2 || size > NYA_LUT_SIZE_MAX) {
                    return nya_error(NYA_ERROR_PARSE, "line %u: LUT_3D_SIZE %u is outside 2 to %d", line_number, size, NYA_LUT_SIZE_MAX);
                }

                texels = nya_arena_alloc(arena, (u64)size * size * size * 4);
                continue;
            }

            b8 is_min = _nya_lut_token_is(starts[0], lengths[0], "DOMAIN_MIN");
            b8 is_max = _nya_lut_token_is(starts[0], lengths[0], "DOMAIN_MAX");

            if (!is_min && !is_max) {
                return nya_error(NYA_ERROR_PARSE, "line %u: '%.*s' is not supported; only 3D tables are", line_number, (int)lengths[0], starts[0]);
            }

            // a domain other than the unit cube would need a remap in the shader, which no exporter in use writes.
            if (count != 4) return nya_error(NYA_ERROR_PARSE, "line %u: a domain needs three values", line_number);

            for (u32 i = 1; i < 4; i++) {
                f32 bound = 0.0F;

                if (!nya_type_parse(NYA_TYPE_F32, starts[i], lengths[i], &bound) || bound != (is_max ? 1.0F : 0.0F)) {
                    return nya_error(NYA_ERROR_PARSE, "line %u: only the unit domain is supported", line_number);
                }
            }

            continue;
        }

        if (size == 0) return nya_error(NYA_ERROR_PARSE, "line %u: table data before LUT_3D_SIZE", line_number);
        if (count != 3) return nya_error(NYA_ERROR_PARSE, "line %u: a table row needs three values, got %u", line_number, count);
        if (entries >= size * size * size) return nya_error(NYA_ERROR_PARSE, "line %u: more rows than LUT_3D_SIZE %u allows", line_number, size);

        u8* texel = &texels[(u64)entries * 4];

        for (u32 channel = 0; channel < 3; channel++) {
            f32 value = 0.0F;

            if (!nya_type_parse(NYA_TYPE_F32, starts[channel], lengths[channel], &value) || isnan(value)) {
                return nya_error(NYA_ERROR_PARSE, "line %u: '%.*s' is not a number", line_number, (int)lengths[channel], starts[channel]);
            }

            texel[channel] = (u8)lroundf(nya_clamp(value, 0.0F, 1.0F) * 255.0F);
        }

        texel[3] = 255;
        entries++;
    }

    if (size == 0) return nya_error(NYA_ERROR_PARSE, "no LUT_3D_SIZE");

    if (entries != size * size * size) {
        return nya_error(NYA_ERROR_PARSE, "%u rows where LUT_3D_SIZE %u needs %u", entries, size, size * size * size);
    }

    *out_lut = (NYA_Lut){ .size = size, .texels = texels };

    return NYA_OK;
}

NYA_Lut nya_lut_compose(NYA_Arena* arena, NYA_Lut lut, f32 fade, NYA_ColorVision vision) {
    nya_assert(arena != nullptr);
    nya_assert(lut.texels != nullptr);
    nya_assert_ge(lut.size, 2U);
    nya_assert_le(lut.size, (u32)NYA_LUT_SIZE_MAX);
    nya_assert(fade >= 0.0F && fade <= 1.0F, "fade %f is outside [0, 1]", (f64)fade);
    nya_assert_lt((u32)vision, (u32)NYA_COLOR_VISION_COUNT);

    if (fade == 0.0F && vision == NYA_COLOR_VISION_NONE) return lut;

    // I + shift * (I - simulation): the part of a colour the viewer loses, added back where they can see it.
    const f32(*simulation)[3] = _NYA_LUT_VISION_SIMULATION[vision];
    const f32(*shift)[3]      = _NYA_LUT_VISION_SHIFT[vision];
    f32 correction[3][3];

    for (u32 row = 0; row < 3; row++) {
        for (u32 column = 0; column < 3; column++) {
            correction[row][column] = row == column ? 1.0F : 0.0F;
            for (u32 k = 0; k < 3; k++) correction[row][column] += shift[row][k] * ((k == column ? 1.0F : 0.0F) - simulation[k][column]);
        }
    }

    u32 size   = nya_max(lut.size, (u32)NYA_LUT_COMPOSE_SIZE_MIN);
    u8* texels = nya_arena_alloc(arena, (u64)size * size * size * 4);

    for (u64 index = 0; index < (u64)size * size * size; index++) {
        f32x3 at     = (f32x3){ (f32)(index % size), (f32)((index / size) % size), (f32)(index / size / size) } / (f32)(size - 1);
        f32x3 graded = _nya_lut_sample(lut, at);
        f32x3 light  = graded + (at - graded) * fade;

        for (u32 channel = 0; channel < 3; channel++) {
            f32 value      = light[channel];
            light[channel] = value <= _NYA_LUT_SRGB_LINEAR_BELOW ? value / _NYA_LUT_SRGB_SLOPE
                                                                 : powf((value + _NYA_LUT_SRGB_OFFSET) / (1.0F + _NYA_LUT_SRGB_OFFSET), _NYA_LUT_SRGB_GAMMA);
        }

        for (u32 channel = 0; channel < 3; channel++) {
            f32 value = correction[channel][0] * light.x + correction[channel][1] * light.y + correction[channel][2] * light.z;
            value     = nya_clamp(value, 0.0F, 1.0F);
            value     = value <= _NYA_LUT_SRGB_ENCODE_BELOW ? value * _NYA_LUT_SRGB_SLOPE
                                                            : (1.0F + _NYA_LUT_SRGB_OFFSET) * powf(value, 1.0F / _NYA_LUT_SRGB_GAMMA) - _NYA_LUT_SRGB_OFFSET;

            texels[index * 4 + channel] = (u8)lroundf(nya_clamp(value, 0.0F, 1.0F) * 255.0F);
        }

        texels[index * 4 + 3] = 255;
    }

    return (NYA_Lut){ .size = size, .texels = texels };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _nya_lut_tokens(const u8* line, u64 length, OUT const u8** out_starts, OUT u64* out_lengths, u32 capacity) {
    u32 count = 0;
    u64 i     = 0;

    while (i < length) {
        // '\r' as whitespace, so a file saved on Windows parses the same.
        while (i < length && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
        if (i >= length) break;

        u64 start = i;
        while (i < length && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;

        if (count < capacity) {
            out_starts[count]  = &line[start];
            out_lengths[count] = i - start;
        }

        count++;
    }

    return count;
}

b8 _nya_lut_token_is(const u8* token, u64 length, NYA_ConstCString keyword) {
    u64 keyword_length = strlen(keyword);

    return length == keyword_length && nya_memcmp(token, keyword, length) == 0;
}

f32x3 _nya_lut_sample(NYA_Lut lut, f32x3 colour) {
    f32x3 scaled = colour * (f32)(lut.size - 1);
    u32   base[3];
    f32   weight[3];

    for (u32 axis = 0; axis < 3; axis++) {
        nya_assert(colour[axis] >= 0.0F && colour[axis] <= 1.0F);

        // the last cell is sampled at its top edge rather than past the table.
        base[axis]   = nya_min((u32)scaled[axis], lut.size - 2);
        weight[axis] = scaled[axis] - (f32)base[axis];
    }

    f32x3 result = { 0 };

    for (u32 corner = 0; corner < 8; corner++) {
        f32 w      = 1.0F;
        u64 offset = 0;

        // blue slowest, so the axes are walked from blue down to red.
        for (u32 i = 0; i < 3; i++) {
            u32 axis = 2 - i;
            u32 step = (corner >> axis) & 1U;
            w       *= step != 0 ? weight[axis] : 1.0F - weight[axis];
            offset   = offset * lut.size + base[axis] + step;
        }

        const u8* texel = &lut.texels[offset * 4];
        result         += (f32x3){ texel[0], texel[1], texel[2] } * (w / 255.0F);
    }

    return result;
}
