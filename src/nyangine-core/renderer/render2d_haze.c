#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render2d_haze_draw(NYA_Window* window, f32 distance) {
    nya_assert(window != nullptr);
    nya_assert(distance >= 0.0F, "a distance between planes, not a signed value: %f", (f64)distance);

    const NYA_Render2DHaze* haze = &window->render_system.haze;

    if (!nya_render_feature_on(window, NYA_RENDER_FEATURE_HAZE, haze->density > 0.0F) || distance <= 0.0F) return;

    NYA_Color color = haze->color.a > 0.0F ? haze->color : NYA_RENDER3D_FOG_COLOR;

    // what lets light through multiplies from veil to veil, as it does along a ray, so the planes add up to fog.
    NYA_Color bottom = color;
    bottom.a         = color.a * (1.0F - expf(-haze->density * distance));

    NYA_Color top = bottom;
    top.a        *= 1.0F - haze->falloff;

    u32 width, height;
    nya_render2d_target_size(window, &width, &height);

    nya_render2d_rect_gradient(window, 0.0F, 0.0F, (f32)width, (f32)height, (NYA_Color[4]){ top, top, bottom, bottom });
}

void nya_render2d_haze_set(NYA_Window* window, NYA_Render2DHaze haze) {
    nya_assert(window != nullptr);

    haze.color   = (NYA_Color){ nya_clamp(haze.color.r, 0.0F, 1.0F), nya_clamp(haze.color.g, 0.0F, 1.0F), nya_clamp(haze.color.b, 0.0F, 1.0F),
                                nya_clamp(haze.color.a, 0.0F, 1.0F) };
    haze.density = nya_clamp(haze.density, 0.0F, 64.0F);
    haze.falloff = nya_clamp(haze.falloff, 0.0F, 1.0F);

    window->render_system.haze = haze;
}

NYA_Render2DHaze nya_render2d_haze(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.haze;
}
