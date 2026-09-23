#include "gnyame/gnyame.h"

GNY_Config NYA_CONFIG;

void gny_config_attach(void) {
    NYA_Error loaded = nya_config_watch(GNY_CONFIG_FILE, nya_reflect_of(GNY_Config), &NYA_CONFIG);

    // not fatal, like a missing settings file: NYA_CONFIG keeps its zeroed defaults.
    if (!loaded.ok) nya_log_warn("Could not load %s: %s", GNY_CONFIG_FILE, (NYA_ConstCString)loaded.message);
}

void gny_config_renderer_apply(NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_ConfigEngineRenderer* renderer = &nya_config_engine()->renderer;

    // first, because everything set below is laid over by the switches rather than the other way round.
    nya_render_features_set(window, renderer->features);

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
    nya_post_motion_blur_set(window, renderer->motion_blur);

    nya_settings_graphics_apply(window);
}

void gny_config_audio_apply(void) {
    const NYA_ConfigEngineAudio* audio = &nya_config_engine()->audio;

    nya_audio_bus_effects_set(NYA_AUDIO_BUS_SOUND, audio->sound);
    nya_audio_bus_effects_set(NYA_AUDIO_BUS_MUSIC, audio->music);
    nya_audio_bus_effects_set(NYA_AUDIO_BUS_MASTER, audio->master);
}

/* One pass is open at a time, so one recorder serves every window and every layer. */
NYA_INTERNAL NYA_UIRecorder _gny_ui_recorder;
NYA_INTERNAL b8             _gny_ui_recording;

NYA_UI* gny_ui_begin(NYA_Window* window, NYA_UIPass pass) {
    nya_ui_style_set(window, nya_config_engine()->ui);

    // an input pass reads the pointer against what the last draw pass measured, so both go through one presenter.
    nya_ui_presenter_set(window, _gny_ui_recording ? nya_ui_recorder_presenter(&_gny_ui_recorder) : nullptr);

    if (_gny_ui_recording) nya_ui_recorder_reset(&_gny_ui_recorder);

    return nya_ui_begin(window, pass);
}

void gny_ui_end(NYA_Window* window, NYA_UI* ui) {
    nya_assert(window != nullptr && ui != nullptr);

    nya_ui_end(ui);

    if (!_gny_ui_recording || nya_ui_recorder_count(&_gny_ui_recorder) == 0) return;

    // one line per widget, which is the whole pass: a menu read rather than looked at.
    char dump[GNY_UI_RECORD_DUMP_MAX];
    (void)nya_ui_recorder_write(&_gny_ui_recorder, dump, sizeof(dump));

    nya_log_info("ui pass, %u widgets through the \"%s\" presenter:\n%s", nya_ui_recorder_count(&_gny_ui_recorder),
                 nya_ui_presenter_get(window)->name, dump);
}

void gny_ui_record_toggle(void) {
    _gny_ui_recording = !_gny_ui_recording;

    if (_gny_ui_recording) {
        // measured in cells, so a dump lines up and reads like the terminal backend rather than like a font.
        nya_ui_recorder_init(&_gny_ui_recorder, NYA_UI_RECORD_CELL);
        nya_log_info("ui recording on: the menus draw nothing and are logged instead");
        return;
    }

    nya_ui_recorder_deinit(&_gny_ui_recorder);
    nya_log_info("ui recording off");
}
