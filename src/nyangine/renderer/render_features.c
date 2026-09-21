/**
 * @file render_features.c
 *
 * The switches, resolved once into two bit masks so the draw paths ask with a bit test. No GPU state, so both
 * builds include it and a headless test reaches the real thing.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The field names, in NYA_RenderFeature's order, for the overlay and the log. Spelled out rather than generated
 * from reflection: this file is below the reflection tables and a name here is read by a person.
 * */
NYA_INTERNAL NYA_ConstCString _NYA_RENDER_FEATURE_NAMES[NYA_RENDER_FEATURE_COUNT] = {
    [NYA_RENDER_FEATURE_FRUSTUM_CULLING]   = "frustum culling",
    [NYA_RENDER_FEATURE_OCCLUSION_CULLING] = "occlusion culling",
    [NYA_RENDER_FEATURE_BACKFACE_CULLING]  = "backface culling",
    [NYA_RENDER_FEATURE_DEPTH_TEST]        = "depth test",
    [NYA_RENDER_FEATURE_DRAW_SORTING]      = "draw sorting",
    [NYA_RENDER_FEATURE_TRANSPARENCY]      = "transparency",
    [NYA_RENDER_FEATURE_SHADOWS]           = "shadows",
    [NYA_RENDER_FEATURE_LIGHTING]          = "lighting",
    [NYA_RENDER_FEATURE_POINT_LIGHTS]      = "point lights",
    [NYA_RENDER_FEATURE_REFLECTIONS]       = "reflections",
    [NYA_RENDER_FEATURE_TEXTURES]          = "textures",
    [NYA_RENDER_FEATURE_FOG]               = "fog",
    [NYA_RENDER_FEATURE_SKY]               = "sky",
    [NYA_RENDER_FEATURE_DECALS]            = "decals",
    [NYA_RENDER_FEATURE_LOD]               = "lod",
    [NYA_RENDER_FEATURE_PARTICLES]         = "particles",
    [NYA_RENDER_FEATURE_HAZE]              = "haze",
    [NYA_RENDER_FEATURE_POST]              = "post",
    [NYA_RENDER_FEATURE_INK]               = "ink",
    [NYA_RENDER_FEATURE_AMBIENT_OCCLUSION] = "ambient occlusion",
    [NYA_RENDER_FEATURE_ANTIALIAS]         = "antialias",
    [NYA_RENDER_FEATURE_DEPTH_OF_FIELD]    = "depth of field",
    [NYA_RENDER_FEATURE_SPEED_LINES]       = "speed lines",
    [NYA_RENDER_FEATURE_BLOOM]             = "bloom",
    [NYA_RENDER_FEATURE_LIGHT_SHAFTS]      = "light shafts",
    [NYA_RENDER_FEATURE_MOTION_BLUR]       = "motion blur",
    [NYA_RENDER_FEATURE_EYE_ADAPTATION]    = "eye adaptation",
    [NYA_RENDER_FEATURE_GRADE]             = "grade",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render_features_set(NYA_Window* window, NYA_RenderFeatures features) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;

    // read as an array, which the static asserts in render_features.h make legal.
    const NYA_RenderToggle* switches = (const NYA_RenderToggle*)&features;

    u32 off = 0;
    u32 on  = 0;

    for (u32 feature = 0; feature < NYA_RENDER_FEATURE_COUNT; feature++) {
        nya_assert(switches[feature] < NYA_RENDER_TOGGLE_COUNT, "'%s' has a switch that is not one of the three",
                   _NYA_RENDER_FEATURE_NAMES[feature]);

        if (switches[feature] == NYA_RENDER_TOGGLE_OFF) off |= 1U << feature;
        if (switches[feature] == NYA_RENDER_TOGGLE_ON) on |= 1U << feature;
    }

    nya_assert((off & on) == 0, "a feature cannot be both on and off");

    render->features          = features;
    render->features_off_mask = off;
    render->features_on_mask  = on;
}

NYA_RenderFeatures nya_render_features(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.features;
}

b8 nya_render_feature_enabled(const NYA_Window* window, NYA_RenderFeature feature) {
    nya_assert(window != nullptr);
    nya_assert(feature < NYA_RENDER_FEATURE_COUNT);

    return (window->render_system.features_off_mask & (1U << feature)) == 0;
}

b8 nya_render_feature_on(const NYA_Window* window, NYA_RenderFeature feature, b8 asked) {
    nya_assert(window != nullptr);
    nya_assert(feature < NYA_RENDER_FEATURE_COUNT);

    const NYA_RenderSystemWindow* render = &window->render_system;

    u32 bit = 1U << feature;

    if ((render->features_off_mask & bit) != 0) return false;
    if ((render->features_on_mask & bit) != 0) return true;

    return asked;
}

NYA_ConstCString nya_render_feature_name(NYA_RenderFeature feature) {
    nya_assert(feature < NYA_RENDER_FEATURE_COUNT);

    return _NYA_RENDER_FEATURE_NAMES[feature];
}

u32 nya_render_features_disabled_text(const NYA_Window* window, char* out, u64 size) {
    nya_assert(window != nullptr);
    nya_assert(out != nullptr && size > 0);

    out[0] = '\0';

    u32 off  = 0;
    u64 at   = 0;
    b8  full = false;

    for (u32 feature = 0; feature < NYA_RENDER_FEATURE_COUNT; feature++) {
        if (nya_render_feature_enabled(window, (NYA_RenderFeature)feature)) continue;

        off++;

        // the count is of everything off, whether or not it fitted: a truncated row must not read as fewer.
        if (full) continue;

        // one overlay row, so a name that does not fit truncates rather than wrapping.
        s32 printed = snprintf(out + at, size - at, "%s%s", off > 1 ? ", " : "", _NYA_RENDER_FEATURE_NAMES[feature]);

        if (printed < 0 || (u64)printed >= size - at) {
            full = true;
            continue;
        }

        at += (u64)printed;
    }

    return off;
}
