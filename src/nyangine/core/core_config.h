/**
 * @file core_config.h
 *
 * Runtime, reflection-driven configuration: a struct on disk instead of a struct's worth of hand
 * written load/save code.
 *
 * ```c
 * NYA_EXPECT(nya_config_watch("assets/config/engine.nya", nya_reflect_of(NYA_ConfigEngine), &NYA_CONFIG.engine));
 * ```
 *
 * A config type is any `@reflect`ed struct — see base_reflection.h. Loading is nya_reflect_from_object
 * doing the work nya_settings_from_object does by hand for NYA_SettingsSystem: it walks the type's
 * fields and writes whatever the file has, leaving the rest of `instance` untouched. That tolerance is
 * what makes a config file forward and backward compatible for free — a field the struct doesn't have
 * yet is ignored, and a field the file doesn't mention keeps its default.
 *
 * The format is NYA_SERDE_FORMAT_NYA read with NYA_SERDE_NO_CHECKSUM: unlike a save file, a config
 * file is meant to be opened in an editor and changed while the game is running, and a checksum that
 * no longer matches after such an edit is not corruption.
 *
 * nya_config_watch reuses the asset system's own file watch — the same plumbing core_i18n.c's locale
 * reload runs on — rather than a second one: the file is registered as a text asset, and a frame hook
 * compares its modification time the same way _nya_i18n_watch does. Without NYA_ASSET_HOT_RELOAD it
 * degrades to nya_config_load once, exactly as a locale does.
 *
 * ⚠ **`instance` is not owned or copied.** A caller in a hot-reloadable game DLL (as gnyame is) that
 * registers a pointer into its own global and then triggers a *code* reload — not a config file
 * change, a rebuild of the DLL itself — leaves this system holding a pointer into memory that reload
 * may have unmapped, because nothing here re-registers watches after one; only core_app's own
 * subsystem table survives a code reload, not the game-side call that populated this table. In
 * practice this matches how GNY_LAUNCH already behaves (gnyame.h): state a game sets up once at
 * startup and does not need to survive being read again is not re-established by a code reload. A
 * config value living through a code reload as well as a file edit would need the engine to own the
 * storage instead of pointing into the game's, which is a larger change than this file makes.
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
 * Shadow tuning a game may want to reach without a rebuild.
 *
 * Named after the NYA_RENDER3D_SHADOW_* macros in render3d.h, whose values these mirror — a config
 * file that omits a field keeps whatever `instance` already held, which is normally one of those
 * defaults, so leaving a field out here is the same as never having added it.
 * */
// @reflect
struct NYA_ConfigEngineRenderer {
    /** Depth slack the shadow comparison allows, in the shadow map's own depth range. See
     *  NYA_RENDER3D_SHADOW_BIAS: too little and flat surfaces self-shadow in stripes ("acne"); too
     *  much and a shadow visibly detaches from the object casting it ("peter-panning"). */
    f32 shadow_bias;

    /** How many cascades the directional shadow splits into. See NYA_RENDER3D_SHADOW_CASCADES: more
     *  is sharper shadows at a distance, at one extra scene pass each. */
    u32 shadow_cascades;

    /** Shadow map resolution per cascade, texels on a side. See NYA_RENDER3D_SHADOW_MAP_SIZE. */
    u32 shadow_map_size;
};

/**
 * Solver tuning shared by both worlds.
 *
 * One set of numbers rather than a 2D and a 3D copy: physics2d.h and physics3d.h already keep the
 * same defaults for both (NYA_PHYSICS2D_SUB_STEPS and NYA_PHYSICS3D_SUB_STEPS are both 4), and a
 * config file editing them separately for no reason is a config file that can disagree with itself.
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
 *
 * Missing or extra fields are not an error — see nya_reflect_from_object, which this is a thin
 * wrapper over. `instance` is only touched once the file has fully parsed, so a malformed edit or a
 * missing file leaves it exactly as it was rather than partially overwritten.
 * */
NYA_API NYA_Error nya_config_load(NYA_ConstCString path, const NYA_TypeReflection* type, void* instance) __attr_no_discard;

/**
 * Loads `path` into `instance` once, then keeps it in sync with the file for as long as
 * NYA_ASSET_HOT_RELOAD is compiled in. Without it, this is exactly nya_config_load — a locale
 * degrades the same way; see core_i18n.h.
 *
 * Refused past NYA_CONFIG_WATCH_MAX watches: the file is still loaded once, it is simply not
 * followed afterward. See the ownership note on `instance` in the file header.
 * */
NYA_API NYA_Error nya_config_watch(NYA_ConstCString path, const NYA_TypeReflection* type, void* instance) __attr_no_discard;

#ifdef NYA_ASSET_HOT_RELOAD
/**
 * Asks the asset system whether any watched file changed, and reloads whichever did.
 *
 * Registered on NYA_EVENT_FRAME_ENDED. Not static, for the same reason core_i18n.c's own watch is
 * not: a registered callback is re-resolved by name through dlsym after a code hot reload, and a
 * symbol with internal linkage cannot be found again.
 * */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_config_watch_tick(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD
