/**
 * @file core_config.h
 *
 * ```c
 * const NYA_ConfigEngine* config = nya_config_engine();
 * f32                     bias   = config->renderer.shadow_bias;
 * ```
 *
 * The engine's own half of `engine.nya` — nya_config_engine, below — is loaded by nya_system_config_init
 * and lives for the process, so a module reads it through that accessor rather than watching the file
 * itself. A game's own section of the same file is its own struct, loaded and watched the ordinary way;
 * see GNY_Config in gnyame/config.h.
 *
 * ```c
 * NYA_EXPECT(nya_config_watch("assets/config/my_section.nya", nya_reflect_of(MyConfig), &my_instance));
 * ```
 *
 * `type` and `instance` are not owned or copied by nya_config_watch itself. When they live in a hot
 * reloadable game DLL, call nya_config_watch again after every code reload; a watch on the same path
 * replaces its pointers. The engine's own watch above needs none of this: `nya_system_config_init` runs
 * once, in the engine, which a code reload never unloads.
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/core/core_asset.h"
#include "nyangine/core/core_audio_effects.h"
#include "nyangine/core/core_audio_propagation.h"
#include "nyangine/core/core_event.h"
#include "nyangine/http/http_log.h"
#include "nyangine/ui/ui.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Distinct files nya_config_watch may be watching at once. Small: this is per file (an engine
 * section, a game section), not per field within one. See NYA_TWEEN_MAX for the same idiom.
 * */
#ifndef NYA_CONFIG_WATCH_MAX
#define NYA_CONFIG_WATCH_MAX 8
#endif

/** Longest asset path a config field can hold, terminator included. */
#define NYA_CONFIG_ASSET_PATH_MAX 128

/**
 * Where the engine's own half of the shared config file lives. A game's "game" object sits in the same
 * file; see GNY_CONFIG_FILE in gnyame/config.h, which names this same path rather than a copy of it.
 * */
#define NYA_CONFIG_ENGINE_FILE "./assets/config/engine.nya"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_ConfigWatch    NYA_ConfigWatch;
typedef struct NYA_ConfigDocument NYA_ConfigDocument;
typedef struct NYA_ConfigSystem   NYA_ConfigSystem;

/** One file nya_config_watch is following, and where the last load of it landed. */
struct NYA_ConfigWatch {
    /** The path, also the asset handle it was registered under. Owned by NYA_ConfigSystem.registry. */
    NYA_CString handle;

    const NYA_TypeReflection* type;

    /** Where nya_reflect_from_object writes on every reload. See the ownership note in the file header. */
    void* instance;

#ifdef NYA_ASSET_HOT_RELOAD
    /** The modification time the last successful load was resolved from. Advanced only on success,
     *  same reasoning as NYA_I18nSystem.modification_time: a file caught half written must be tried
     *  again next tick rather than recorded as handled. */
    u64 modification_time;

    /** Uptime at which a dead config asset may next be re-armed. See _nya_i18n_rearm, which this
     *  mirrors one watch at a time instead of for a single fixed pair of handles. */
    u64 next_recovery_ns;
#endif // NYA_ASSET_HOT_RELOAD
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENGINE CONFIG
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_ConfigEngineRenderer NYA_ConfigEngineRenderer;
typedef struct NYA_ConfigEnginePhysics  NYA_ConfigEnginePhysics;
typedef struct NYA_ConfigEngineAudio    NYA_ConfigEngineAudio;
typedef struct NYA_ConfigEngine         NYA_ConfigEngine;

/**
 * Renderer tuning a game may want to reach without a rebuild.
 * */
// @reflect
struct NYA_ConfigEngineRenderer {
    /**
     * One switch per renderer feature, laid over everything below. See NYA_RenderFeatures: a field left out is
     * NYA_RENDER_TOGGLE_DEFAULT and changes nothing, so this is where a feature is turned off to see what it was
     * doing, live, without a rebuild.
     * */
    NYA_RenderFeatures features;

    /** Depth slack the shadow comparison allows, in the shadow map's own depth range. See
     *  NYA_RENDER3D_SHADOW_BIAS: too little and flat surfaces self-shadow in stripes ("acne"); too
     *  much and a shadow visibly detaches from the object casting it ("peter-panning"). */
    f32 shadow_bias;

    /** How many cascades the directional shadow splits into. See NYA_Render3DShadowOptions: more
     *  is sharper shadows at a distance, at one extra scene pass each. */
    u32 shadow_cascades;

    /** Shadow map resolution per cascade, texels on a side. See NYA_Render3DShadowOptions. */
    u32 shadow_map_size;

    /* The scene post passes, handed to nya_post_ink_set and its siblings as they are. */

    NYA_PostInk              ink;
    NYA_PostAmbientOcclusion ambient_occlusion;
    NYA_PostAntialias        antialias;
    NYA_PostDepthOfField     depth_of_field;

    /** A game driving the lines by speed reads `amount` as their most. */
    NYA_PostSpeedLines speed_lines;

    NYA_PostBloom         bloom;
    NYA_PostEyeAdaptation eye_adaptation;
    NYA_PostLightShafts   light_shafts;
    NYA_PostMotionBlur    motion_blur;

    /** A zero field keeps the scene's own. See nya_render3d_fog_set and nya_render2d_haze_set. */
    NYA_Render3DFog  fog;
    NYA_Render2DHaze haze;

    /** See nya_render3d_decals_set. */
    NYA_Render3DDecals decals;

    /** See nya_render_output_set. */
    NYA_RenderOutput output;

    NYA_PostDebugView        debug_view;

    /** The hue shade leans toward, alpha as how far. See NYA_Render3DShadowOptions.color. */
    NYA_Color shadow_color;

    /** The `.cube` table the scene is graded through, an asset path. Empty turns grading off. */
    char grade_lut[NYA_CONFIG_ASSET_PATH_MAX];

    /** How much of the grade applies, in [0, 1]. Zero also turns it off, and then no pass runs. */
    f32 grade_strength;
};

/**
 * Solver tuning shared by both worlds.
 * */
// @reflect
struct NYA_ConfigEnginePhysics {
    /** Downward acceleration, world units per second squared. Read once, at nya_system_physics2d_init /
     *  nya_system_physics3d_init. Zero or negative falls back to NYA_PHYSICS2D_GRAVITY_DEFAULT and
     *  NYA_PHYSICS3D_GRAVITY_DEFAULT, both 9.81 scaled and signed into that world's own units. */
    f32 gravity;

    /** Solver iterations per step, read once at the same inits. Zero falls back to
     *  NYA_PHYSICS2D_SUB_STEPS / NYA_PHYSICS3D_SUB_STEPS: more gives stiffer stacks and less overlap, at
     *  a linear cost. */
    u32 sub_steps;
};

/**
 * Sound: how it travels through the world, and each bus's effects.
 * */
// @reflect
struct NYA_ConfigEngineAudio {
    /** See nya_audio_propagation_set. */
    NYA_AudioPropagation propagation;

    /* Handed to nya_audio_bus_effects_set as they are. */

    NYA_AudioEffects sound;
    NYA_AudioEffects music;
    NYA_AudioEffects master;
};

/**
 * The engine's own tunables, reached through nya_config_engine as `nya_config_engine()->renderer.shadow_bias`
 * and so on. See GNY_Config in gnyame/config.h for the game's own half of the same file, and the file
 * header above for why this half needs no watch or re-attach of its own.
 * */
// @reflect
struct NYA_ConfigEngine {
    NYA_ConfigEngineRenderer renderer;
    NYA_ConfigEnginePhysics  physics;
    NYA_ConfigEngineAudio    audio;

    /** See nya_ui_style_set. */
    NYA_UIStyle ui;

    /**
     * What this program's HTTP server writes about each request, and how much of a caller's address it
     * keeps. See http_log.h; the type carries `@on_apply`, so editing this section reaches a running
     * server on the next reload rather than on the next restart.
     * */
    NYA_HttpLogConfig http_log;
};

/**
 * The shape `engine.nya` really has at its top level: an "engine" object beside a "game" one the engine
 * does not reflect. One field rather than NYA_ConfigEngine itself, so nya_config_load can be pointed at
 * the file directly instead of at an "engine" subtree somebody has already cut out of it.
 * */
// @reflect
struct NYA_ConfigDocument {
    NYA_ConfigEngine engine;
};

struct NYA_ConfigSystem {
    /** Owns every watched path's copy. A game may pass a string living in its own hot-reloadable DLL,
     *  which a code reload can unmap; this arena's copy does not depend on that DLL staying mapped. */
    NYA_Arena* registry;

    NYA_ConfigWatch watches[NYA_CONFIG_WATCH_MAX];
    u32             watch_count;

    /**
     * The engine's own half of NYA_CONFIG_ENGINE_FILE, loaded by nya_system_config_init and, under
     * NYA_ASSET_HOT_RELOAD, kept in sync by the same watch tick as everything else nya_config_watch
     * follows. Written in place rather than swapped behind a pointer: every field below NYA_ConfigEngine
     * is a plain value with no `@on_apply` of its own (NYA_HttpLogConfig is the one exception and does
     * its own swap into a private copy, see http_log.h), so a reload can only ever finish a field's write
     * or not start it — nothing here reads a struct that is half one version and half another. See
     * nya_config_engine.
     * */
    NYA_ConfigDocument document;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/** Brings up the system and, under NYA_ASSET_HOT_RELOAD, registers the watch's frame hook. Cannot fail. */
NYA_API void nya_system_config_init(void);

/** Releases the watch registry. Safe before anything has been loaded. */
NYA_API void nya_system_config_deinit(void);

/**
 * The engine's own tunables, live for as long as the process: renderer, physics, audio, ui and
 * http_log, loaded from NYA_CONFIG_ENGINE_FILE by nya_system_config_init and kept current by the same
 * hot reload every other watch gets. Mutable rather than `const`: a few debug switchboards (the 3D
 * demo's render feature toggles, its look panel) write into it on purpose, as an in-memory override the
 * config file's own values still win back on the next edit, since a reload writes every field again
 * rather than only the ones that changed.
 *
 * Never null: before nya_system_config_init runs, or if NYA_CONFIG_ENGINE_FILE could not be read, this
 * still points at a real (zeroed, all-default) NYA_ConfigEngine rather than at nothing.
 * */
NYA_API NYA_ConfigEngine* nya_config_engine(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * CONFIG FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Reads `path`, deserializes it as NYA_SERDE_FORMAT_NYA, and writes every field it names into
 * `instance` through `type`'s reflection.
 * */
NYA_API NYA_Error nya_config_load(NYA_ConstCString path, const NYA_TypeReflection* type, void* instance) __attr_no_discard;

/**
 * Loads `path` into `instance`, then keeps it in sync with the file while NYA_ASSET_HOT_RELOAD is
 * compiled in. Without it this is nya_config_load, as core_i18n.h degrades.
 * */
NYA_API NYA_Error nya_config_watch(NYA_ConstCString path, const NYA_TypeReflection* type, void* instance) __attr_no_discard;

#ifdef NYA_ASSET_HOT_RELOAD
/**
 * Asks the asset system whether any watched file changed, and reloads whichever did.
 * */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_config_watch_tick(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD
