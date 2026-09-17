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
}
