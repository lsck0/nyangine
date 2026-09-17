/**
 * @file core_config.h
 *
 * ```c
 * NYA_EXPECT(nya_config_watch("assets/config/engine.nya", nya_reflect_of(NYA_ConfigEngine), &NYA_CONFIG.engine));
 * ```
 *
 * `type` and `instance` are not owned or copied. When they live in a hot reloadable game DLL, call
 * nya_config_watch again after every code reload; a watch on the same path replaces its pointers.
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/core/core_asset.h"
#include "nyangine/core/core_event.h"

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_ConfigWatch  NYA_ConfigWatch;
typedef struct NYA_ConfigSystem NYA_ConfigSystem;

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

struct NYA_ConfigSystem {
    /** Owns every watched path's copy. A game may pass a string living in its own hot-reloadable DLL,
     *  which a code reload can unmap; this arena's copy does not depend on that DLL staying mapped. */
    NYA_Arena* registry;

    NYA_ConfigWatch watches[NYA_CONFIG_WATCH_MAX];
    u32             watch_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENGINE CONFIG
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_ConfigEngineRenderer NYA_ConfigEngineRenderer;
typedef struct NYA_ConfigEnginePhysics  NYA_ConfigEnginePhysics;
typedef struct NYA_ConfigEngine         NYA_ConfigEngine;

/**
 * Renderer tuning a game may want to reach without a rebuild.
 * */
// @reflect
struct NYA_ConfigEngineRenderer {
    /** Samples per pixel, fed to nya_render_options_set. See NYA_RenderOptions.msaa_samples: 1 is off, 0 the default. */
    u32 msaa_samples;

    /** Depth slack the shadow comparison allows, in the shadow map's own depth range. See
     *  NYA_RENDER3D_SHADOW_BIAS: too little and flat surfaces self-shadow in stripes ("acne"); too
     *  much and a shadow visibly detaches from the object casting it ("peter-panning"). */
    f32 shadow_bias;

    /** How many cascades the directional shadow splits into. See NYA_Render3DShadowOptions: more
     *  is sharper shadows at a distance, at one extra scene pass each. */
    u32 shadow_cascades;

    /** Shadow map resolution per cascade, texels on a side. See NYA_Render3DShadowOptions. */
    u32 shadow_map_size;

    /* The cartoon post passes, handed to nya_post_ink_set and its siblings as they are. */

    NYA_PostInk              ink;
    NYA_PostAmbientOcclusion ambient_occlusion;
    NYA_PostAntialias        antialias;
    NYA_PostDepthOfField     depth_of_field;
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
    /** Downward acceleration, world units per second squared. See NYA_PHYSICS2D_GRAVITY_DEFAULT and
     *  NYA_PHYSICS3D_GRAVITY_DEFAULT, both 9.81 scaled into that world's own units. */
    f32 gravity;

    /** Solver iterations per step. See NYA_PHYSICS2D_SUB_STEPS / NYA_PHYSICS3D_SUB_STEPS: more gives
     *  stiffer stacks and less overlap, at a linear cost. */
    u32 sub_steps;
};

/**
 * The engine-owned half of a game's config, reached as `NYA_CONFIG.engine.renderer.shadow_bias` and
 * so on once a game embeds this in its own root config struct. See GNY_Config in gnyame/config.h for
 * the game-owned half, and the file header for how the whole is loaded and watched.
 * */
// @reflect
struct NYA_ConfigEngine {
    NYA_ConfigEngineRenderer renderer;
    NYA_ConfigEnginePhysics  physics;
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
