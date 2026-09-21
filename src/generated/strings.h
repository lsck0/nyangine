/* THIS FILE IS GENERATED. DO NYAT TOUCH. */

#pragma once

#include "nyangine/core/core_i18n.h"

/*
 * Generated from ./assets/i18n/en.json by src/build/i18n.c. One entry and one accessor per key of the base
 * locale, with the accessor's parameters read off that string's format specifiers, so a call
 * with the wrong argument types is a compile error rather than a crash in one language.
 */

typedef enum {
    NYA_STRING_CUBE3D_FEATURES,
    NYA_STRING_CUBE3D_HINT_ANIMATION,
    NYA_STRING_CUBE3D_HINT_CAMERA,
    NYA_STRING_CUBE3D_HINT_CLICK,
    NYA_STRING_CUBE3D_HINT_DRAG,
    NYA_STRING_CUBE3D_KEYS,
    NYA_STRING_CUBE3D_RENDER_KEYS,
    NYA_STRING_CUBE3D_TITLE,
    NYA_STRING_HUD_BOXES,
    NYA_STRING_HUD_GREETING,
    NYA_STRING_HUD_HOSTING,
    NYA_STRING_HUD_HOVERING,
    NYA_STRING_HUD_KEYS,
    NYA_STRING_HUD_OFFLINE,
    NYA_STRING_HUD_PAUSED,
    NYA_STRING_HUD_PLAYERS,
    NYA_STRING_HUD_ROBOTS_DQN,
    NYA_STRING_HUD_ROBOTS_NEAT,
    NYA_STRING_HUD_ROBOTS_RUN,
    NYA_STRING_HUD_SCORE,
    NYA_STRING_MENU_2D_SCENE,
    NYA_STRING_MENU_3D_SCENE,
    NYA_STRING_MENU_ACCENT,
    NYA_STRING_MENU_ANIMATE,
    NYA_STRING_MENU_ANTIALIASING,
    NYA_STRING_MENU_AUTO,
    NYA_STRING_MENU_BARS,
    NYA_STRING_MENU_BLOOM,
    NYA_STRING_MENU_CHART,
    NYA_STRING_MENU_DEPTH_OF_FIELD,
    NYA_STRING_MENU_DRAWS,
    NYA_STRING_MENU_EYE_ADAPTATION,
    NYA_STRING_MENU_FADE,
    NYA_STRING_MENU_FIELD_OF_VIEW,
    NYA_STRING_MENU_FLAT,
    NYA_STRING_MENU_FRAME_MS,
    NYA_STRING_MENU_FXAA,
    NYA_STRING_MENU_GRAPHICS,
    NYA_STRING_MENU_HIGH,
    NYA_STRING_MENU_INVITE,
    NYA_STRING_MENU_LANGUAGE,
    NYA_STRING_MENU_LIGHT_SHAFTS,
    NYA_STRING_MENU_LINE,
    NYA_STRING_MENU_LOOK,
    NYA_STRING_MENU_LOW,
    NYA_STRING_MENU_MAIN_MENU,
    NYA_STRING_MENU_MASTER_VOLUME,
    NYA_STRING_MENU_MEDIUM,
    NYA_STRING_MENU_METRIC,
    NYA_STRING_MENU_MOTION_BLUR,
    NYA_STRING_MENU_MUSIC_VOLUME,
    NYA_STRING_MENU_NAME,
    NYA_STRING_MENU_OCCLUSION,
    NYA_STRING_MENU_OFF,
    NYA_STRING_MENU_ON,
    NYA_STRING_MENU_PAUSED,
    NYA_STRING_MENU_QUIT,
    NYA_STRING_MENU_RENDER_SCALE,
    NYA_STRING_MENU_RESET,
    NYA_STRING_MENU_RESTART,
    NYA_STRING_MENU_RESUME,
    NYA_STRING_MENU_SCALE,
    NYA_STRING_MENU_SHADOWS,
    NYA_STRING_MENU_SHEET,
    NYA_STRING_MENU_START,
    NYA_STRING_MENU_STATS,
    NYA_STRING_MENU_SUBTITLE,
    NYA_STRING_MENU_TABLE,
    NYA_STRING_MENU_VALUE,
    NYA_STRING_MENU_VERTICES,
    NYA_STRING_MENU_WIDGETS,
    NYA_STRING_PRESENCE_3D,
    NYA_STRING_PRESENCE_ALONE,
    NYA_STRING_PRESENCE_HOSTING,
    NYA_STRING_PRESENCE_JOINED,
    NYA_STRING_PRESENCE_MENU,
    NYA_STRING_PRESENCE_SANDBOX,
    NYA_STRING_SOCIAL_ACCEPT,
    NYA_STRING_SOCIAL_DECLINE,
    NYA_STRING_SOCIAL_JOIN_REQUEST,
    NYA_STRING_SOCIAL_WANTS_TO_JOIN,

    NYA_STRING_COUNT,
} NYA_StringId;

/** The JSON key each id came from, in id order. Read by nya_i18n_load. */
static const NYA_ConstCString NYA_STRING_KEYS[NYA_STRING_COUNT] __attr_allow_unused = {
    "cube3d_features",
    "cube3d_hint_animation",
    "cube3d_hint_camera",
    "cube3d_hint_click",
    "cube3d_hint_drag",
    "cube3d_keys",
    "cube3d_render_keys",
    "cube3d_title",
    "hud_boxes",
    "hud_greeting",
    "hud_hosting",
    "hud_hovering",
    "hud_keys",
    "hud_offline",
    "hud_paused",
    "hud_players",
    "hud_robots_dqn",
    "hud_robots_neat",
    "hud_robots_run",
    "hud_score",
    "menu_2d_scene",
    "menu_3d_scene",
    "menu_accent",
    "menu_animate",
    "menu_antialiasing",
    "menu_auto",
    "menu_bars",
    "menu_bloom",
    "menu_chart",
    "menu_depth_of_field",
    "menu_draws",
    "menu_eye_adaptation",
    "menu_fade",
    "menu_field_of_view",
    "menu_flat",
    "menu_frame_ms",
    "menu_fxaa",
    "menu_graphics",
    "menu_high",
    "menu_invite",
    "menu_language",
    "menu_light_shafts",
    "menu_line",
    "menu_look",
    "menu_low",
    "menu_main_menu",
    "menu_master_volume",
    "menu_medium",
    "menu_metric",
    "menu_motion_blur",
    "menu_music_volume",
    "menu_name",
    "menu_occlusion",
    "menu_off",
    "menu_on",
    "menu_paused",
    "menu_quit",
    "menu_render_scale",
    "menu_reset",
    "menu_restart",
    "menu_resume",
    "menu_scale",
    "menu_shadows",
    "menu_sheet",
    "menu_start",
    "menu_stats",
    "menu_subtitle",
    "menu_table",
    "menu_value",
    "menu_vertices",
    "menu_widgets",
    "presence_3d",
    "presence_alone",
    "presence_hosting",
    "presence_joined",
    "presence_menu",
    "presence_sandbox",
    "social_accept",
    "social_decline",
    "social_join_request",
    "social_wants_to_join",
};

/** `cube3d_features` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_features(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_FEATURES);
}

/** `cube3d_hint_animation` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_hint_animation(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_HINT_ANIMATION);
}

/** `cube3d_hint_camera` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_hint_camera(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_HINT_CAMERA);
}

/** `cube3d_hint_click` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_hint_click(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_HINT_CLICK);
}

/** `cube3d_hint_drag` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_hint_drag(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_HINT_DRAG);
}

/** `cube3d_keys` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_keys(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_KEYS);
}

/** `cube3d_render_keys` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_render_keys(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_RENDER_KEYS);
}

/** `cube3d_title` */
static inline __attr_allow_unused NYA_ConstCString nya_string_cube3d_title(void) {
    return _nya_i18n_format(NYA_STRING_CUBE3D_TITLE);
}

/** `hud_boxes` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_boxes(u32 a0, u32 a1) {
    return _nya_i18n_format(NYA_STRING_HUD_BOXES, a0, a1);
}

/** `hud_greeting` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_greeting(NYA_ConstCString a0) {
    return _nya_i18n_format(NYA_STRING_HUD_GREETING, a0);
}

/** `hud_hosting` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_hosting(u32 a0) {
    return _nya_i18n_format(NYA_STRING_HUD_HOSTING, a0);
}

/** `hud_hovering` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_hovering(NYA_ConstCString a0) {
    return _nya_i18n_format(NYA_STRING_HUD_HOVERING, a0);
}

/** `hud_keys` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_keys(void) {
    return _nya_i18n_format(NYA_STRING_HUD_KEYS);
}

/** `hud_offline` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_offline(void) {
    return _nya_i18n_format(NYA_STRING_HUD_OFFLINE);
}

/** `hud_paused` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_paused(void) {
    return _nya_i18n_format(NYA_STRING_HUD_PAUSED);
}

/** `hud_players` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_players(u32 a0) {
    return _nya_i18n_format(NYA_STRING_HUD_PLAYERS, a0);
}

/** `hud_robots_dqn` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_robots_dqn(u32 a0, f64 a1, f64 a2) {
    return _nya_i18n_format(NYA_STRING_HUD_ROBOTS_DQN, a0, a1, a2);
}

/** `hud_robots_neat` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_robots_neat(u32 a0, u32 a1, f64 a2) {
    return _nya_i18n_format(NYA_STRING_HUD_ROBOTS_NEAT, a0, a1, a2);
}

/** `hud_robots_run` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_robots_run(u32 a0, f64 a1, f64 a2) {
    return _nya_i18n_format(NYA_STRING_HUD_ROBOTS_RUN, a0, a1, a2);
}

/** `hud_score` */
static inline __attr_allow_unused NYA_ConstCString nya_string_hud_score(NYA_ConstCString a0, s32 a1) {
    return _nya_i18n_format(NYA_STRING_HUD_SCORE, a0, a1);
}

/** `menu_2d_scene` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_2d_scene(void) {
    return _nya_i18n_format(NYA_STRING_MENU_2D_SCENE);
}

/** `menu_3d_scene` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_3d_scene(void) {
    return _nya_i18n_format(NYA_STRING_MENU_3D_SCENE);
}

/** `menu_accent` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_accent(void) {
    return _nya_i18n_format(NYA_STRING_MENU_ACCENT);
}

/** `menu_animate` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_animate(void) {
    return _nya_i18n_format(NYA_STRING_MENU_ANIMATE);
}

/** `menu_antialiasing` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_antialiasing(void) {
    return _nya_i18n_format(NYA_STRING_MENU_ANTIALIASING);
}

/** `menu_auto` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_auto(void) {
    return _nya_i18n_format(NYA_STRING_MENU_AUTO);
}

/** `menu_bars` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_bars(void) {
    return _nya_i18n_format(NYA_STRING_MENU_BARS);
}

/** `menu_bloom` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_bloom(void) {
    return _nya_i18n_format(NYA_STRING_MENU_BLOOM);
}

/** `menu_chart` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_chart(void) {
    return _nya_i18n_format(NYA_STRING_MENU_CHART);
}

/** `menu_depth_of_field` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_depth_of_field(void) {
    return _nya_i18n_format(NYA_STRING_MENU_DEPTH_OF_FIELD);
}

/** `menu_draws` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_draws(void) {
    return _nya_i18n_format(NYA_STRING_MENU_DRAWS);
}

/** `menu_eye_adaptation` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_eye_adaptation(void) {
    return _nya_i18n_format(NYA_STRING_MENU_EYE_ADAPTATION);
}

/** `menu_fade` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_fade(void) {
    return _nya_i18n_format(NYA_STRING_MENU_FADE);
}

/** `menu_field_of_view` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_field_of_view(void) {
    return _nya_i18n_format(NYA_STRING_MENU_FIELD_OF_VIEW);
}

/** `menu_flat` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_flat(void) {
    return _nya_i18n_format(NYA_STRING_MENU_FLAT);
}

/** `menu_frame_ms` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_frame_ms(void) {
    return _nya_i18n_format(NYA_STRING_MENU_FRAME_MS);
}

/** `menu_fxaa` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_fxaa(void) {
    return _nya_i18n_format(NYA_STRING_MENU_FXAA);
}

/** `menu_graphics` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_graphics(void) {
    return _nya_i18n_format(NYA_STRING_MENU_GRAPHICS);
}

/** `menu_high` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_high(void) {
    return _nya_i18n_format(NYA_STRING_MENU_HIGH);
}

/** `menu_invite` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_invite(void) {
    return _nya_i18n_format(NYA_STRING_MENU_INVITE);
}

/** `menu_language` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_language(void) {
    return _nya_i18n_format(NYA_STRING_MENU_LANGUAGE);
}

/** `menu_light_shafts` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_light_shafts(void) {
    return _nya_i18n_format(NYA_STRING_MENU_LIGHT_SHAFTS);
}

/** `menu_line` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_line(void) {
    return _nya_i18n_format(NYA_STRING_MENU_LINE);
}

/** `menu_look` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_look(void) {
    return _nya_i18n_format(NYA_STRING_MENU_LOOK);
}

/** `menu_low` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_low(void) {
    return _nya_i18n_format(NYA_STRING_MENU_LOW);
}

/** `menu_main_menu` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_main_menu(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MAIN_MENU);
}

/** `menu_master_volume` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_master_volume(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MASTER_VOLUME);
}

/** `menu_medium` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_medium(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MEDIUM);
}

/** `menu_metric` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_metric(void) {
    return _nya_i18n_format(NYA_STRING_MENU_METRIC);
}

/** `menu_motion_blur` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_motion_blur(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MOTION_BLUR);
}

/** `menu_music_volume` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_music_volume(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MUSIC_VOLUME);
}

/** `menu_name` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_name(void) {
    return _nya_i18n_format(NYA_STRING_MENU_NAME);
}

/** `menu_occlusion` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_occlusion(void) {
    return _nya_i18n_format(NYA_STRING_MENU_OCCLUSION);
}

/** `menu_off` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_off(void) {
    return _nya_i18n_format(NYA_STRING_MENU_OFF);
}

/** `menu_on` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_on(void) {
    return _nya_i18n_format(NYA_STRING_MENU_ON);
}

/** `menu_paused` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_paused(void) {
    return _nya_i18n_format(NYA_STRING_MENU_PAUSED);
}

/** `menu_quit` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_quit(void) {
    return _nya_i18n_format(NYA_STRING_MENU_QUIT);
}

/** `menu_render_scale` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_render_scale(void) {
    return _nya_i18n_format(NYA_STRING_MENU_RENDER_SCALE);
}

/** `menu_reset` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_reset(void) {
    return _nya_i18n_format(NYA_STRING_MENU_RESET);
}

/** `menu_restart` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_restart(void) {
    return _nya_i18n_format(NYA_STRING_MENU_RESTART);
}

/** `menu_resume` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_resume(void) {
    return _nya_i18n_format(NYA_STRING_MENU_RESUME);
}

/** `menu_scale` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_scale(void) {
    return _nya_i18n_format(NYA_STRING_MENU_SCALE);
}

/** `menu_shadows` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_shadows(void) {
    return _nya_i18n_format(NYA_STRING_MENU_SHADOWS);
}

/** `menu_sheet` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_sheet(void) {
    return _nya_i18n_format(NYA_STRING_MENU_SHEET);
}

/** `menu_start` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_start(void) {
    return _nya_i18n_format(NYA_STRING_MENU_START);
}

/** `menu_stats` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_stats(void) {
    return _nya_i18n_format(NYA_STRING_MENU_STATS);
}

/** `menu_subtitle` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_subtitle(void) {
    return _nya_i18n_format(NYA_STRING_MENU_SUBTITLE);
}

/** `menu_table` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_table(void) {
    return _nya_i18n_format(NYA_STRING_MENU_TABLE);
}

/** `menu_value` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_value(void) {
    return _nya_i18n_format(NYA_STRING_MENU_VALUE);
}

/** `menu_vertices` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_vertices(void) {
    return _nya_i18n_format(NYA_STRING_MENU_VERTICES);
}

/** `menu_widgets` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_widgets(void) {
    return _nya_i18n_format(NYA_STRING_MENU_WIDGETS);
}

/** `presence_3d` */
static inline __attr_allow_unused NYA_ConstCString nya_string_presence_3d(void) {
    return _nya_i18n_format(NYA_STRING_PRESENCE_3D);
}

/** `presence_alone` */
static inline __attr_allow_unused NYA_ConstCString nya_string_presence_alone(void) {
    return _nya_i18n_format(NYA_STRING_PRESENCE_ALONE);
}

/** `presence_hosting` */
static inline __attr_allow_unused NYA_ConstCString nya_string_presence_hosting(void) {
    return _nya_i18n_format(NYA_STRING_PRESENCE_HOSTING);
}

/** `presence_joined` */
static inline __attr_allow_unused NYA_ConstCString nya_string_presence_joined(void) {
    return _nya_i18n_format(NYA_STRING_PRESENCE_JOINED);
}

/** `presence_menu` */
static inline __attr_allow_unused NYA_ConstCString nya_string_presence_menu(void) {
    return _nya_i18n_format(NYA_STRING_PRESENCE_MENU);
}

/** `presence_sandbox` */
static inline __attr_allow_unused NYA_ConstCString nya_string_presence_sandbox(void) {
    return _nya_i18n_format(NYA_STRING_PRESENCE_SANDBOX);
}

/** `social_accept` */
static inline __attr_allow_unused NYA_ConstCString nya_string_social_accept(void) {
    return _nya_i18n_format(NYA_STRING_SOCIAL_ACCEPT);
}

/** `social_decline` */
static inline __attr_allow_unused NYA_ConstCString nya_string_social_decline(void) {
    return _nya_i18n_format(NYA_STRING_SOCIAL_DECLINE);
}

/** `social_join_request` */
static inline __attr_allow_unused NYA_ConstCString nya_string_social_join_request(void) {
    return _nya_i18n_format(NYA_STRING_SOCIAL_JOIN_REQUEST);
}

/** `social_wants_to_join` */
static inline __attr_allow_unused NYA_ConstCString nya_string_social_wants_to_join(void) {
    return _nya_i18n_format(NYA_STRING_SOCIAL_WANTS_TO_JOIN);
}
