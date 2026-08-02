#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL f32 _nya_color_clamp01(f32 v);
NYA_INTERNAL f32 _nya_color_fmod360(f32 h);
NYA_INTERNAL f32 _nya_hsl_to_rgb_component(f32 p, f32 q, f32 t);
NYA_INTERNAL u8  _nya_hex_nibble(char c);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Color nya_color_from_hsl(NYA_ColorHSL hsl) {
    if (hsl.s <= 0.0F) return (NYA_Color){ hsl.l, hsl.l, hsl.l, hsl.a };

    f32 q = hsl.l < 0.5F ? hsl.l * (1.0F + hsl.s) : hsl.l + hsl.s - hsl.l * hsl.s;
    f32 p = 2.0F * hsl.l - q;
    f32 h = _nya_color_fmod360(hsl.h) / 360.0F;

    return (NYA_Color){
        _nya_hsl_to_rgb_component(p, q, h + 1.0F / 3.0F),
        _nya_hsl_to_rgb_component(p, q, h),
        _nya_hsl_to_rgb_component(p, q, h - 1.0F / 3.0F),
        hsl.a,
    };
}

NYA_Color nya_color_from_hsv(NYA_ColorHSV hsv) {
    f32 h = _nya_color_fmod360(hsv.h);
    f32 c = hsv.v * hsv.s;
    f32 x = c * (1.0F - fabsf(fmodf(h / 60.0F, 2.0F) - 1.0F));
    f32 m = hsv.v - c;

    f32 r, g, b;
    if (h < 60.0F) {
        r = c;
        g = x;
        b = 0;
    } else if (h < 120.0F) {
        r = x;
        g = c;
        b = 0;
    } else if (h < 180.0F) {
        r = 0;
        g = c;
        b = x;
    } else if (h < 240.0F) {
        r = 0;
        g = x;
        b = c;
    } else if (h < 300.0F) {
        r = x;
        g = 0;
        b = c;
    } else {
        r = c;
        g = 0;
        b = x;
    }

    return (NYA_Color){ r + m, g + m, b + m, hsv.a };
}

NYA_Color nya_color_from_hex(NYA_ConstCString hex) {
    nya_assert(hex != nullptr);

    if (hex[0] == '#') hex++;

    u64 len = strlen(hex);
    nya_assert(len == 6 || len == 8, "Hex color must be 6 or 8 characters (got %zu).", len);

    u8 r = (_nya_hex_nibble(hex[0]) << 4) | _nya_hex_nibble(hex[1]);
    u8 g = (_nya_hex_nibble(hex[2]) << 4) | _nya_hex_nibble(hex[3]);
    u8 b = (_nya_hex_nibble(hex[4]) << 4) | _nya_hex_nibble(hex[5]);
    u8 a = len == 8 ? ((_nya_hex_nibble(hex[6]) << 4) | _nya_hex_nibble(hex[7])) : 255;

    return nya_color_from_u8(r, g, b, a);
}

NYA_Color nya_color_from_u32(u32 packed) {
    return (NYA_Color){
        .r = (f32)((packed >> 24) & 0xFF) / 255.0F,
        .g = (f32)((packed >> 16) & 0xFF) / 255.0F,
        .b = (f32)((packed >> 8) & 0xFF) / 255.0F,
        .a = (f32)(packed & 0xFF) / 255.0F,
    };
}

NYA_Color nya_color_from_u8(u8 r, u8 g, u8 b, u8 a) {
    return (NYA_Color){ (f32)r / 255.0F, (f32)g / 255.0F, (f32)b / 255.0F, (f32)a / 255.0F };
}

NYA_ColorHSL nya_color_to_hsl(NYA_Color c) {
    f32 max = fmaxf(fmaxf(c.r, c.g), c.b);
    f32 min = fminf(fminf(c.r, c.g), c.b);
    f32 d   = max - min;
    f32 l   = (max + min) * 0.5F;

    f32 h = 0.0F, s = 0.0F;
    if (d > 0.0F) {
        f32 denom = 1.0F - fabsf(2.0F * l - 1.0F);
        s         = denom > 0.0F ? d / denom : 1.0F;
        if (max == c.r)
            h = fmodf((c.g - c.b) / d, 6.0F);
        else if (max == c.g)
            h = (c.b - c.r) / d + 2.0F;
        else
            h = (c.r - c.g) / d + 4.0F;
        h *= 60.0F;
        if (h < 0.0F) h += 360.0F;
    }

    return (NYA_ColorHSL){ h, s, l, c.a };
}

NYA_ColorHSV nya_color_to_hsv(NYA_Color c) {
    f32 max = fmaxf(fmaxf(c.r, c.g), c.b);
    f32 min = fminf(fminf(c.r, c.g), c.b);
    f32 d   = max - min;

    f32 h = 0.0F;
    if (d > 0.0F) {
        if (max == c.r)
            h = fmodf((c.g - c.b) / d, 6.0F);
        else if (max == c.g)
            h = (c.b - c.r) / d + 2.0F;
        else
            h = (c.r - c.g) / d + 4.0F;
        h *= 60.0F;
        if (h < 0.0F) h += 360.0F;
    }

    f32 s = max > 0.0F ? d / max : 0.0F;
    return (NYA_ColorHSV){ h, s, max, c.a };
}

u32 nya_color_to_u32(NYA_Color c) {
    u8 r = (u8)(_nya_color_clamp01(c.r) * 255.0F + 0.5F);
    u8 g = (u8)(_nya_color_clamp01(c.g) * 255.0F + 0.5F);
    u8 b = (u8)(_nya_color_clamp01(c.b) * 255.0F + 0.5F);
    u8 a = (u8)(_nya_color_clamp01(c.a) * 255.0F + 0.5F);
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | (u32)a;
}

NYA_Color nya_color_blend(NYA_Color bg, NYA_Color fg) {
    f32 out_a = fg.a + bg.a * (1.0F - fg.a);
    if (out_a <= 0.0F) return (NYA_Color){ 0 };

    return (NYA_Color){
        (fg.r * fg.a + bg.r * bg.a * (1.0F - fg.a)) / out_a,
        (fg.g * fg.a + bg.g * bg.a * (1.0F - fg.a)) / out_a,
        (fg.b * fg.a + bg.b * bg.a * (1.0F - fg.a)) / out_a,
        out_a,
    };
}

NYA_Color nya_color_darken(NYA_Color c, f32 amount) {
    f32 f = 1.0F - _nya_color_clamp01(amount);
    return (NYA_Color){ c.r * f, c.g * f, c.b * f, c.a };
}

f32 nya_color_luminance(NYA_Color c) {
    return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
}

NYA_Color nya_color_grayscale(NYA_Color c) {
    f32 l = nya_color_luminance(c);
    return (NYA_Color){ l, l, l, c.a };
}

NYA_Color nya_color_hue_shift(NYA_Color c, f32 degrees) {
    NYA_ColorHSV hsv = nya_color_to_hsv(c);
    hsv.h            = _nya_color_fmod360(hsv.h + degrees);
    return nya_color_from_hsv(hsv);
}

NYA_Color nya_color_invert(NYA_Color c) {
    return (NYA_Color){ 1.0F - c.r, 1.0F - c.g, 1.0F - c.b, c.a };
}

NYA_Color nya_color_lighten(NYA_Color c, f32 amount) {
    f32 t = _nya_color_clamp01(amount);
    return (NYA_Color){
        c.r + (1.0F - c.r) * t,
        c.g + (1.0F - c.g) * t,
        c.b + (1.0F - c.b) * t,
        c.a,
    };
}

NYA_Color nya_color_mix(NYA_Color a, NYA_Color b, f32 t) {
    t       = _nya_color_clamp01(t);
    f32 inv = 1.0F - t;
    return (NYA_Color){
        a.r * inv + b.r * t,
        a.g * inv + b.g * t,
        a.b * inv + b.b * t,
        a.a * inv + b.a * t,
    };
}

NYA_Color nya_color_saturate(NYA_Color c, f32 factor) {
    NYA_ColorHSV hsv = nya_color_to_hsv(c);
    hsv.s            = _nya_color_clamp01(hsv.s * factor);
    return nya_color_from_hsv(hsv);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL f32 _nya_color_clamp01(f32 v) {
    return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
}

NYA_INTERNAL f32 _nya_color_fmod360(f32 h) {
    h = fmodf(h, 360.0F);
    return h < 0.0F ? h + 360.0F : h;
}

NYA_INTERNAL f32 _nya_hsl_to_rgb_component(f32 p, f32 q, f32 t) {
    if (t < 0.0F) t += 1.0F;
    if (t > 1.0F) t -= 1.0F;
    if (t < 1.0F / 6.0F) return p + (q - p) * 6.0F * t;
    if (t < 1.0F / 2.0F) return q;
    if (t < 2.0F / 3.0F) return p + (q - p) * (2.0F / 3.0F - t) * 6.0F;
    return p;
}

NYA_INTERNAL u8 _nya_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (u8)(c - '0');
    if (c >= 'a' && c <= 'f') return (u8)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (u8)(c - 'A' + 10);
    return 0;
}
