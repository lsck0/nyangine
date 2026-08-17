/**
 * @file render_color.h
 *
 * Color types and utilities for RGB, HSV, HSL with conversions, blending, and predefined colors.
 * */
#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PREDEFINED COLORS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_COLOR_AQUA        ((NYA_Color){ 0.0F, 1.0F, 1.0F, 1.0F })
#define NYA_COLOR_BEIGE       ((NYA_Color){ 0.961F, 0.961F, 0.863F, 1.0F })
#define NYA_COLOR_BLACK       ((NYA_Color){ 0.0F, 0.0F, 0.0F, 1.0F })
#define NYA_COLOR_BLUE        ((NYA_Color){ 0.0F, 0.0F, 1.0F, 1.0F })
#define NYA_COLOR_BROWN       ((NYA_Color){ 0.545F, 0.271F, 0.075F, 1.0F })
#define NYA_COLOR_CORAL       ((NYA_Color){ 1.0F, 0.498F, 0.314F, 1.0F })
#define NYA_COLOR_CRIMSON     ((NYA_Color){ 0.863F, 0.078F, 0.235F, 1.0F })
#define NYA_COLOR_CYAN        ((NYA_Color){ 0.0F, 1.0F, 1.0F, 1.0F })
#define NYA_COLOR_DARK_GRAY   ((NYA_Color){ 0.25F, 0.25F, 0.25F, 1.0F })
#define NYA_COLOR_GOLD        ((NYA_Color){ 1.0F, 0.843F, 0.0F, 1.0F })
#define NYA_COLOR_GRAY        ((NYA_Color){ 0.5F, 0.5F, 0.5F, 1.0F })
#define NYA_COLOR_GREEN       ((NYA_Color){ 0.0F, 1.0F, 0.0F, 1.0F })
#define NYA_COLOR_INDIGO      ((NYA_Color){ 0.294F, 0.0F, 0.510F, 1.0F })
#define NYA_COLOR_KHAKI       ((NYA_Color){ 0.941F, 0.902F, 0.549F, 1.0F })
#define NYA_COLOR_LAVENDER    ((NYA_Color){ 0.902F, 0.902F, 0.980F, 1.0F })
#define NYA_COLOR_LIGHT_GRAY  ((NYA_Color){ 0.75F, 0.75F, 0.75F, 1.0F })
#define NYA_COLOR_LIME        ((NYA_Color){ 0.75F, 1.0F, 0.0F, 1.0F })
#define NYA_COLOR_MAGENTA     ((NYA_Color){ 1.0F, 0.0F, 1.0F, 1.0F })
#define NYA_COLOR_MAROON      ((NYA_Color){ 0.502F, 0.0F, 0.0F, 1.0F })
#define NYA_COLOR_MINT        ((NYA_Color){ 0.596F, 1.0F, 0.596F, 1.0F })
#define NYA_COLOR_NAVY        ((NYA_Color){ 0.0F, 0.0F, 0.5F, 1.0F })
#define NYA_COLOR_OLIVE       ((NYA_Color){ 0.5F, 0.5F, 0.0F, 1.0F })
#define NYA_COLOR_ORANGE      ((NYA_Color){ 1.0F, 0.647F, 0.0F, 1.0F })
#define NYA_COLOR_PINK        ((NYA_Color){ 1.0F, 0.412F, 0.706F, 1.0F })
#define NYA_COLOR_PURPLE      ((NYA_Color){ 0.5F, 0.0F, 0.5F, 1.0F })
#define NYA_COLOR_RED         ((NYA_Color){ 1.0F, 0.0F, 0.0F, 1.0F })
#define NYA_COLOR_SALMON      ((NYA_Color){ 0.980F, 0.502F, 0.447F, 1.0F })
#define NYA_COLOR_SILVER      ((NYA_Color){ 0.75F, 0.75F, 0.75F, 1.0F })
#define NYA_COLOR_SKY_BLUE    ((NYA_Color){ 0.529F, 0.808F, 0.922F, 1.0F })
#define NYA_COLOR_TEAL        ((NYA_Color){ 0.0F, 0.5F, 0.5F, 1.0F })
#define NYA_COLOR_TRANSPARENT ((NYA_Color){ 0.0F, 0.0F, 0.0F, 0.0F })
#define NYA_COLOR_TURQUOISE   ((NYA_Color){ 0.251F, 0.878F, 0.816F, 1.0F })
#define NYA_COLOR_VIOLET      ((NYA_Color){ 0.933F, 0.510F, 0.933F, 1.0F })
#define NYA_COLOR_WHITE       ((NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F })
#define NYA_COLOR_YELLOW      ((NYA_Color){ 1.0F, 1.0F, 0.0F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Color    NYA_Color;
typedef struct NYA_ColorHSV NYA_ColorHSV;
typedef struct NYA_ColorHSL NYA_ColorHSL;

/**
 * NYA_Color
 *
 * RGBA float color, each component in [0, 1].
 * */
// @reflect
struct NYA_Color {
    f32 r, g, b, a;
};

/**
 * NYA_ColorHSV
 *
 * HSV color. h in [0, 360), s and v in [0, 1].
 * */
struct NYA_ColorHSV {
    f32 h, s, v, a;
};

/**
 * NYA_ColorHSL
 *
 * HSL color. h in [0, 360), s and l in [0, 1].
 * */
struct NYA_ColorHSL {
    f32 h, s, l, a;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Color    nya_color_from_hsl(NYA_ColorHSL hsl);
NYA_API NYA_Color    nya_color_from_hsv(NYA_ColorHSV hsv);
NYA_API NYA_Color    nya_color_from_hex(NYA_ConstCString hex);
NYA_API NYA_Color    nya_color_from_u32(u32 packed);
NYA_API NYA_Color    nya_color_from_u8(u8 r, u8 g, u8 b, u8 a);
NYA_API NYA_ColorHSL nya_color_to_hsl(NYA_Color color);
NYA_API NYA_ColorHSV nya_color_to_hsv(NYA_Color color);
NYA_API u32          nya_color_to_u32(NYA_Color color);

NYA_API NYA_Color nya_color_blend(NYA_Color bg, NYA_Color fg);
NYA_API NYA_Color nya_color_darken(NYA_Color color, f32 amount);
NYA_API f32       nya_color_luminance(NYA_Color color);
NYA_API NYA_Color nya_color_grayscale(NYA_Color color);
NYA_API NYA_Color nya_color_hue_shift(NYA_Color color, f32 degrees);
NYA_API NYA_Color nya_color_invert(NYA_Color color);
NYA_API NYA_Color nya_color_lighten(NYA_Color color, f32 amount);
NYA_API NYA_Color nya_color_mix(NYA_Color a, NYA_Color b, f32 t);
NYA_API NYA_Color nya_color_saturate(NYA_Color color, f32 factor);
