/**
 * @file core_i18n.h
 *
 * ```c
 * NYA_EXPECT(nya_i18n_load("de", NYA_STRING_KEYS, NYA_STRING_COUNT));
 *
 * nya_render2d_text(window, nya_string_menu_start(), x, y, colour);
 * nya_render2d_text(window, nya_string_hud_score("Ada", 4200), x, y, colour);
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_asset.h"
#include "nyangine/core/core_event.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Formatted strings alive at once before the oldest is overwritten. Sixteen covers a frame's worth of
 * draw calls; a caller holding more than this is building a list and wants an arena instead.
 * */
#ifndef NYA_I18N_FORMAT_SLOTS
#define NYA_I18N_FORMAT_SLOTS 16
#endif

/** Longest formatted string. Past this it is truncated, with the truncation visible rather than silent. */
#ifndef NYA_I18N_FORMAT_MAX
#define NYA_I18N_FORMAT_MAX 512
#endif

/** Longest locale code, so a language tag like `pt-BR` fits with room to spare. */
#define NYA_I18N_LOCALE_MAX 16

/**
 * Where locale files live, as the asset index spells it.
 * */
#define NYA_I18N_ASSET_DIRECTORY "./assets/i18n"

/** The locale a missing key falls back to. Must match NYA_I18N_BASE_LOCALE in src/build/i18n.h. */
#define NYA_I18N_BASE_LOCALE "en"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_I18nSystem NYA_I18nSystem;

struct NYA_I18nSystem {
    /** Owns every loaded string. Emptied rather than destroyed on a locale change, so switching is cheap. */
    NYA_Arena* allocator;

    /**
     * Owns what has to survive a locale change: the copied keys and the two asset handles.
     * */
    NYA_Arena* registry;

    /**
     * The strings, indexed by NYA_StringId. Null for a key this locale did not supply.
     * */
    NYA_CString* strings;

    /** The base locale's strings, kept so a missing key falls back to English rather than to nothing. */
    NYA_CString* fallback;

    u32 count;

    char locale[NYA_I18N_LOCALE_MAX];

    /**
     * The keys, copied out of whoever supplied them, in `registry`.
     * */
    NYA_ConstCString* keys;

    /** The locale file's asset handle, and the base locale's. Null before the first successful load. */
    NYA_CString handle;
    NYA_CString fallback_handle;

#ifdef NYA_ASSET_HOT_RELOAD
    /**
     * The modification times the loaded strings were resolved from.
     * */
    u64 modification_time;
    u64 fallback_modification_time;

    /**
     * Uptime at which a dead locale asset may next be re-armed. See _nya_i18n_rearm.
     * */
    u64 next_recovery_ns;
#endif // NYA_ASSET_HOT_RELOAD

    /** The ring formatted strings are written into. See the note on lifetime in the file header. */
    char formatted[NYA_I18N_FORMAT_SLOTS][NYA_I18N_FORMAT_MAX];
    u32  next_slot;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings up the system and registers the watch that makes a locale file hot reloadable.
 * */
NYA_API void nya_system_i18n_init(void);

/** Releases every loaded string. Safe before anything has been loaded. */
NYA_API void nya_system_i18n_deinit(void);

/**
 * Loads a locale, replacing whatever was loaded before, and watches its file from then on.
 * */
NYA_API NYA_Error nya_i18n_load(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count) __attr_no_discard;

/**
 * The same, from bytes the caller already has. Nothing is watched and no fallback is loaded.
 * */
NYA_API NYA_Error nya_i18n_load_bytes(NYA_ConstCString locale, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count) __attr_no_discard;

/** The locale currently loaded, or an empty string before the first load. */
NYA_API NYA_ConstCString nya_i18n_locale(void) __attr_no_discard;

/**
 * The raw string for an id, before any formatting. Falls back to the base locale, then to the key.
 * */
NYA_API NYA_ConstCString nya_i18n_raw(u32 id) __attr_no_discard;

/**
 * Formats a string into the ring and returns it. What the generated accessors call.
 * */
NYA_API NYA_ConstCString _nya_i18n_format(u32 id, ...);

#ifdef NYA_ASSET_HOT_RELOAD
/**
 * Asks the asset system whether either locale file changed, and re-resolves if one did.
 * */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_i18n_watch(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD
