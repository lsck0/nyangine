/**
 * @file render_lut.c
 * */
#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Splits `line` on spaces and tabs into at most `capacity` tokens. Returns how many there were, uncapped. */
NYA_INTERNAL u32 _nya_lut_tokens(const u8* line, u64 length, OUT const u8** out_starts, OUT u64* out_lengths, u32 capacity);

/** Whether a token spells `keyword` exactly. */
NYA_INTERNAL b8 _nya_lut_token_is(const u8* token, u64 length, NYA_ConstCString keyword) __attr_no_discard;

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
