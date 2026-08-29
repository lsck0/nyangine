/* THIS FILE IS GENERATED. DO NYAT TOUCH. */

#pragma once

#include "nyangine/core/core_i18n.h"

/*
 * Generated from ./assets/i18n/en.json by src/build/i18n.c. One entry and one accessor per key of the base
 * locale, with the accessor's parameters read off that string's format specifiers — so a call
 * with the wrong argument types is a compile error rather than a crash in one language.
 */

typedef enum {
    NYA_STRING_CUBE3D_HINT_CAMERA,
    NYA_STRING_CUBE3D_HINT_CLICK,
    NYA_STRING_CUBE3D_HINT_DRAG,
    NYA_STRING_CUBE3D_TITLE,
    NYA_STRING_HUD_BOXES,
    NYA_STRING_HUD_GREETING,
    NYA_STRING_HUD_HOSTING,
    NYA_STRING_HUD_HOVERING,
    NYA_STRING_HUD_OFFLINE,
    NYA_STRING_HUD_PAUSED,
    NYA_STRING_HUD_PLAYERS,
    NYA_STRING_HUD_SCORE,
    NYA_STRING_MENU_2D_SCENE,
    NYA_STRING_MENU_3D_SCENE,
    NYA_STRING_MENU_MAIN_MENU,
    NYA_STRING_MENU_MASTER_VOLUME,
    NYA_STRING_MENU_MUSIC_VOLUME,
    NYA_STRING_MENU_QUIT,
    NYA_STRING_MENU_RESTART,
    NYA_STRING_MENU_RESUME,
    NYA_STRING_MENU_START,

    NYA_STRING_COUNT,
} NYA_StringId;

/** The JSON key each id came from, in id order. Read by nya_i18n_load. */
static const NYA_ConstCString NYA_STRING_KEYS[NYA_STRING_COUNT] __attr_allow_unused = {
    "cube3d_hint_camera", "cube3d_hint_click",  "cube3d_hint_drag",  "cube3d_title", "hud_boxes",    "hud_greeting",  "hud_hosting",
    "hud_hovering",       "hud_offline",        "hud_paused",        "hud_players",  "hud_score",    "menu_2d_scene", "menu_3d_scene",
    "menu_main_menu",     "menu_master_volume", "menu_music_volume", "menu_quit",    "menu_restart", "menu_resume",   "menu_start",
};

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

/** `menu_main_menu` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_main_menu(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MAIN_MENU);
}

/** `menu_master_volume` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_master_volume(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MASTER_VOLUME);
}

/** `menu_music_volume` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_music_volume(void) {
    return _nya_i18n_format(NYA_STRING_MENU_MUSIC_VOLUME);
}

/** `menu_quit` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_quit(void) {
    return _nya_i18n_format(NYA_STRING_MENU_QUIT);
}

/** `menu_restart` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_restart(void) {
    return _nya_i18n_format(NYA_STRING_MENU_RESTART);
}

/** `menu_resume` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_resume(void) {
    return _nya_i18n_format(NYA_STRING_MENU_RESUME);
}

/** `menu_start` */
static inline __attr_allow_unused NYA_ConstCString nya_string_menu_start(void) {
    return _nya_i18n_format(NYA_STRING_MENU_START);
}
