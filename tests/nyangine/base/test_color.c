/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include <math.h>

#define EPSILON 0.01F

static b8 color_equals(NYA_Color a, NYA_Color b, f32 eps) {
  return fabsf(a.r - b.r) < eps && fabsf(a.g - b.g) < eps && fabsf(a.b - b.b) < eps && fabsf(a.a - b.a) < eps;
}

static NYA_Color color_with_alpha(NYA_Color c, f32 a) {
  return (NYA_Color){ c.r, c.g, c.b, a };
}

s32 main(void) {
  // TEST: predefined colors have correct values
  {
    NYA_Color w = NYA_COLOR_WHITE;
    nya_assert(w.r == 1.0F && w.g == 1.0F && w.b == 1.0F && w.a == 1.0F);

    NYA_Color b = NYA_COLOR_BLACK;
    nya_assert(b.r == 0.0F && b.g == 0.0F && b.b == 0.0F && b.a == 1.0F);

    NYA_Color t = NYA_COLOR_TRANSPARENT;
    nya_assert(t.r == 0.0F && t.g == 0.0F && t.b == 0.0F && t.a == 0.0F);

    NYA_Color r = NYA_COLOR_RED;
    nya_assert(r.r == 1.0F && r.g == 0.0F && r.b == 0.0F && r.a == 1.0F);
  }

  // TEST: u32 packing roundtrip
  {
    NYA_Color orig = NYA_COLOR_RED;
    u32 packed = nya_color_to_u32(orig);
    nya_assert(packed == 0xFF0000FF);

    NYA_Color back = nya_color_from_u32(packed);
    nya_assert(color_equals(orig, back, EPSILON));

    nya_assert(nya_color_to_u32(NYA_COLOR_WHITE) == 0xFFFFFFFF);
    nya_assert(nya_color_to_u32(NYA_COLOR_BLACK) == 0x000000FF);
    nya_assert(nya_color_to_u32(NYA_COLOR_TRANSPARENT) == 0x00000000);
  }

  // TEST: from_u8
  {
    NYA_Color c = nya_color_from_u8(255, 128, 0, 255);
    nya_assert(fabsf(c.r - 1.0F) < EPSILON);
    nya_assert(fabsf(c.g - 0.502F) < EPSILON);
    nya_assert(fabsf(c.b - 0.0F) < EPSILON);
    nya_assert(fabsf(c.a - 1.0F) < EPSILON);
  }

  // TEST: hex parsing
  {
    NYA_Color c1 = nya_color_from_hex("#FF8000");
    nya_assert(fabsf(c1.r - 1.0F) < EPSILON);
    nya_assert(fabsf(c1.g - 0.502F) < EPSILON);
    nya_assert(fabsf(c1.b - 0.0F) < EPSILON);
    nya_assert(c1.a == 1.0F);

    NYA_Color c2 = nya_color_from_hex("FF000080");
    nya_assert(fabsf(c2.r - 1.0F) < EPSILON);
    nya_assert(fabsf(c2.a - 0.502F) < EPSILON);

    NYA_Color c3 = nya_color_from_hex("#00FF00FF");
    nya_assert(color_equals(c3, NYA_COLOR_GREEN, EPSILON));
  }

  // TEST: RGB → HSV → RGB roundtrip
  {
    NYA_Color colors[] = {
      NYA_COLOR_RED, NYA_COLOR_GREEN, NYA_COLOR_BLUE,
      NYA_COLOR_YELLOW, NYA_COLOR_CYAN, NYA_COLOR_MAGENTA,
      NYA_COLOR_WHITE, NYA_COLOR_ORANGE,
    };
    for (u32 i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
      NYA_ColorHSV hsv = nya_color_to_hsv(colors[i]);
      NYA_Color back = nya_color_from_hsv(hsv);
      nya_assert(color_equals(colors[i], back, EPSILON),
                 "HSV roundtrip failed for color %u", i);
    }
  }

  // TEST: RGB → HSL → RGB roundtrip
  {
    NYA_Color colors[] = {
      NYA_COLOR_RED, NYA_COLOR_GREEN, NYA_COLOR_BLUE,
      NYA_COLOR_YELLOW, NYA_COLOR_CYAN, NYA_COLOR_MAGENTA,
      NYA_COLOR_WHITE, NYA_COLOR_ORANGE,
    };
    for (u32 i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
      NYA_ColorHSL hsl = nya_color_to_hsl(colors[i]);
      NYA_Color back = nya_color_from_hsl(hsl);
      nya_assert(color_equals(colors[i], back, EPSILON),
                 "HSL roundtrip failed for color %u", i);
    }
  }

  // TEST: HSV known values (red = 0°, green = 120°, blue = 240°)
  {
    NYA_ColorHSV red_hsv = nya_color_to_hsv(NYA_COLOR_RED);
    nya_assert(fabsf(red_hsv.h - 0.0F) < EPSILON);
    nya_assert(fabsf(red_hsv.s - 1.0F) < EPSILON);
    nya_assert(fabsf(red_hsv.v - 1.0F) < EPSILON);

    NYA_ColorHSV green_hsv = nya_color_to_hsv(NYA_COLOR_GREEN);
    nya_assert(fabsf(green_hsv.h - 120.0F) < EPSILON);

    NYA_ColorHSV blue_hsv = nya_color_to_hsv(NYA_COLOR_BLUE);
    nya_assert(fabsf(blue_hsv.h - 240.0F) < EPSILON);
  }

  // TEST: HSL known values
  {
    NYA_ColorHSL white_hsl = nya_color_to_hsl(NYA_COLOR_WHITE);
    nya_assert(fabsf(white_hsl.l - 1.0F) < EPSILON);

    NYA_ColorHSL black_hsl = nya_color_to_hsl(NYA_COLOR_BLACK);
    nya_assert(fabsf(black_hsl.l - 0.0F) < EPSILON);

    NYA_ColorHSL red_hsl = nya_color_to_hsl(NYA_COLOR_RED);
    nya_assert(fabsf(red_hsl.h - 0.0F) < EPSILON);
    nya_assert(fabsf(red_hsl.l - 0.5F) < EPSILON);
    nya_assert(fabsf(red_hsl.s - 1.0F) < EPSILON);
  }

  // TEST: mix / lerp
  {
    NYA_Color mid = nya_color_mix(NYA_COLOR_BLACK, NYA_COLOR_WHITE, 0.5F);
    nya_assert(fabsf(mid.r - 0.5F) < EPSILON);
    nya_assert(fabsf(mid.g - 0.5F) < EPSILON);
    nya_assert(fabsf(mid.b - 0.5F) < EPSILON);

    NYA_Color start = nya_color_mix(NYA_COLOR_RED, NYA_COLOR_BLUE, 0.0F);
    nya_assert(color_equals(start, NYA_COLOR_RED, EPSILON));

    NYA_Color end = nya_color_mix(NYA_COLOR_RED, NYA_COLOR_BLUE, 1.0F);
    nya_assert(color_equals(end, NYA_COLOR_BLUE, EPSILON));
  }

  // TEST: darken
  {
    NYA_Color dark = nya_color_darken(NYA_COLOR_WHITE, 0.5F);
    nya_assert(fabsf(dark.r - 0.5F) < EPSILON);
    nya_assert(fabsf(dark.g - 0.5F) < EPSILON);
    nya_assert(fabsf(dark.b - 0.5F) < EPSILON);
    nya_assert(dark.a == 1.0F);

    NYA_Color full_dark = nya_color_darken(NYA_COLOR_WHITE, 1.0F);
    // full darken = multiply by 0 = black with alpha preserved
    nya_assert(full_dark.r == 0.0F && full_dark.g == 0.0F && full_dark.b == 0.0F);
    nya_assert(full_dark.a == 1.0F);
    nya_assert(color_equals(full_dark, NYA_COLOR_BLACK, EPSILON));
  }

  // TEST: lighten
  {
    NYA_Color light = nya_color_lighten(NYA_COLOR_BLACK, 0.5F);
    nya_assert(fabsf(light.r - 0.5F) < EPSILON);

    NYA_Color full_light = nya_color_lighten(NYA_COLOR_BLACK, 1.0F);
    nya_assert(color_equals(full_light, NYA_COLOR_WHITE, EPSILON));
  }

  // TEST: invert
  {
    NYA_Color inv = nya_color_invert(NYA_COLOR_RED);
    nya_assert(color_equals(inv, NYA_COLOR_CYAN, EPSILON));

    NYA_Color inv2 = nya_color_invert(NYA_COLOR_WHITE);
    NYA_Color expected_black = NYA_COLOR_BLACK;
    nya_assert(color_equals(inv2, expected_black, EPSILON));
  }

  // TEST: grayscale
  {
    NYA_Color gray = nya_color_grayscale(NYA_COLOR_WHITE);
    nya_assert(fabsf(gray.r - 1.0F) < EPSILON);
    nya_assert(gray.r == gray.g && gray.g == gray.b);

    NYA_Color gray2 = nya_color_grayscale(NYA_COLOR_RED);
    nya_assert(fabsf(gray2.r - 0.2126F) < EPSILON);
  }

  // TEST: luminance
  {
    nya_assert(fabsf(nya_color_luminance(NYA_COLOR_WHITE) - 1.0F) < EPSILON);
    nya_assert(fabsf(nya_color_luminance(NYA_COLOR_BLACK) - 0.0F) < EPSILON);
    nya_assert(nya_color_luminance(NYA_COLOR_GREEN) > nya_color_luminance(NYA_COLOR_RED));
    nya_assert(nya_color_luminance(NYA_COLOR_RED) > nya_color_luminance(NYA_COLOR_BLUE));
  }

  // TEST: alpha blend
  {
    // fully opaque fg replaces bg
    NYA_Color result = nya_color_blend(NYA_COLOR_WHITE, NYA_COLOR_RED);
    nya_assert(color_equals(result, NYA_COLOR_RED, EPSILON));

    // fully transparent fg leaves bg unchanged
    NYA_Color result2 = nya_color_blend(NYA_COLOR_WHITE, NYA_COLOR_TRANSPARENT);
    nya_assert(color_equals(result2, NYA_COLOR_WHITE, EPSILON));

    // 50% alpha red over white
    NYA_Color half_red = color_with_alpha(NYA_COLOR_RED, 0.5F);
    NYA_Color blended = nya_color_blend(NYA_COLOR_WHITE, half_red);
    nya_assert(blended.r > 0.9F);
    nya_assert(blended.g > 0.3F && blended.g < 0.6F);
  }

  // TEST: hue shift
  {
    // shift red by 120° → green
    NYA_Color shifted = nya_color_hue_shift(NYA_COLOR_RED, 120.0F);
    nya_assert(color_equals(shifted, NYA_COLOR_GREEN, EPSILON));

    // shift red by 240° → blue
    NYA_Color shifted2 = nya_color_hue_shift(NYA_COLOR_RED, 240.0F);
    nya_assert(color_equals(shifted2, NYA_COLOR_BLUE, EPSILON));

    // shift by 360° → same color
    NYA_Color shifted3 = nya_color_hue_shift(NYA_COLOR_RED, 360.0F);
    nya_assert(color_equals(shifted3, NYA_COLOR_RED, EPSILON));
  }

  // TEST: saturate / desaturate
  {
    // full desaturation → grayscale
    NYA_Color desat = nya_color_saturate(NYA_COLOR_RED, 0.0F);
    nya_assert(fabsf(desat.r - desat.g) < EPSILON);
    nya_assert(fabsf(desat.g - desat.b) < EPSILON);

    // factor 1.0 → unchanged
    NYA_Color same = nya_color_saturate(NYA_COLOR_RED, 1.0F);
    nya_assert(color_equals(same, NYA_COLOR_RED, EPSILON));
  }

  // TEST: with_alpha
  {
    NYA_Color c = color_with_alpha(NYA_COLOR_RED, 0.5F);
    nya_assert(c.r == 1.0F && c.g == 0.0F && c.b == 0.0F);
    nya_assert(fabsf(c.a - 0.5F) < EPSILON);
  }

  // TEST: equals with epsilon
  {
    NYA_Color a = { 0.5F, 0.5F, 0.5F, 1.0F };
    NYA_Color b = { 0.501F, 0.499F, 0.5F, 1.0F };
    nya_assert(color_equals(a, b, 0.01F));
    nya_assert(color_equals(a, b, 0.0001F) == false);
  }

  // TEST: luminance (declared in header, callable externally)
  {
    nya_assert(fabsf(nya_color_luminance(NYA_COLOR_WHITE) - 1.0F) < EPSILON);
    nya_assert(fabsf(nya_color_luminance(NYA_COLOR_BLACK) - 0.0F) < EPSILON);
    nya_assert(nya_color_luminance(NYA_COLOR_GREEN) > nya_color_luminance(NYA_COLOR_RED));
    nya_assert(nya_color_luminance(NYA_COLOR_RED) > nya_color_luminance(NYA_COLOR_BLUE));

    // Luminance of gray should equal the gray value (since R=G=B)
    NYA_Color gray = { 0.5F, 0.5F, 0.5F, 1.0F };
    f32       lum  = nya_color_luminance(gray);
    nya_assert(fabsf(lum - 0.5F) < EPSILON);
  }

  // TEST: HSV ↔ HSL consistency (via RGB roundtrip)
  {
    NYA_Color orig = NYA_COLOR_ORANGE;
    NYA_ColorHSV hsv = nya_color_to_hsv(orig);
    NYA_ColorHSL hsl = nya_color_to_hsl(orig);
    // hue should be the same in both representations
    nya_assert(fabsf(hsv.h - hsl.h) < EPSILON);

    NYA_Color from_hsv = nya_color_from_hsv(hsv);
    NYA_Color from_hsl = nya_color_from_hsl(hsl);
    nya_assert(color_equals(from_hsv, from_hsl, EPSILON));
  }

  printf("PASSED: test_color\n");
  return 0;
}
