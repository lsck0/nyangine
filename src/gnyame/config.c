#include "gnyame/gnyame.h"

GNY_Config NYA_CONFIG;

void gny_config_attach(void) {
    NYA_Error loaded = nya_config_watch(GNY_CONFIG_FILE, nya_reflect_of(GNY_Config), &NYA_CONFIG);

    // not fatal, like a missing settings file: NYA_CONFIG keeps its zeroed defaults.
    if (!loaded.ok) nya_log_warn("Could not load %s: %s", GNY_CONFIG_FILE, (NYA_ConstCString)loaded.message);
}

void gny_config_renderer_apply(NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_ConfigEngineRenderer* renderer = &NYA_CONFIG.engine.renderer;

    nya_render3d_shadow_options_set(
        window,
        (NYA_Render3DShadowOptions){
            .cascades = renderer->shadow_cascades,
            .map_size = renderer->shadow_map_size,
            .color    = renderer->shadow_color,
        }
    );

    // a 2D scene has no normal buffer, so the passes reading it run in 3D only and cost the 2D world nothing.
    nya_post_ink_set(window, renderer->ink);
    nya_post_ambient_occlusion_set(window, renderer->ambient_occlusion);
    nya_post_antialias_set(window, renderer->antialias);
    nya_post_debug_view_set(window, renderer->debug_view);
    nya_post_eye_adaptation_set(window, renderer->eye_adaptation);
    nya_post_light_shafts_set(window, renderer->light_shafts);
}

void gny_config_audio_apply(void) {
    const NYA_ConfigEngineAudio* audio = &NYA_CONFIG.engine.audio;

    nya_audio_bus_effects_set(NYA_AUDIO_BUS_SOUND, audio->sound);
    nya_audio_bus_effects_set(NYA_AUDIO_BUS_MUSIC, audio->music);
    nya_audio_bus_effects_set(NYA_AUDIO_BUS_MASTER, audio->master);
}

NYA_UI* gny_ui_begin(NYA_Window* window, NYA_UIPass pass) {
    nya_ui_style_set(window, NYA_CONFIG.engine.ui);

    return nya_ui_begin(window, pass);
}
