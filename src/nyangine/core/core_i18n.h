/**
 * @file core_i18n.h
 *
 * Translated text, checked by the compiler.
 *
 * ```c
 * NYA_EXPECT(nya_i18n_load("de", NYA_STRING_KEYS, NYA_STRING_COUNT));
 *
 * nya_render2d_text(window, nya_string_menu_start(), x, y, colour);
 * nya_render2d_text(window, nya_string_hud_score("Ada", 4200), x, y, colour);
 * ```
 *
 * `nya_string_*` is generated into `assets/strings.h` by the build system (see src/build/i18n.h), one
 * accessor per key with parameters taken from that string's own format specifiers — passing anything
 * else does not compile, unlike `nya_i18n_get(id)` handed to printf with a runtime-chosen format
 * string. The build also refuses a translation whose arguments disagree with the base locale.
 *
 * Moved here from base, which could only read locale files once at startup with nya_file_read. Locales
 * are assets: indexed by the build, bakeable into the blob, and reloadable at runtime through
 * nya_asset_read, so a translator sees an edit without a restart.
 *
 * A locale is `assets/i18n/<code>.json`, also its asset handle. nya_i18n_load reads it **synchronously**
 * through nya_asset_read — not a queued nya_asset_load, which lands a frame late and would draw
 * `[string 4]` for the first frame — and registers it so the asset system watches it: one `stat` per
 * locale file per NYA_ASSET_HOT_RELOAD stat interval, none in a release build. Reading a string costs
 * nothing — nya_i18n_raw is an array index, touching neither the asset system nor the filesystem, which
 * is why the watch lives on a frame hook rather than in the accessors. A key the file lacks falls back
 * to the base locale, since the build already rejects a locale missing a key.
 *
 * An accessor with no arguments returns the stored string directly. One with arguments formats into a
 * ring of frame-lifetime buffers. **The result is valid until NYA_I18N_FORMAT_SLOTS more formatted
 * strings have been made** — usable inline in a draw call with no arena or free, but copy it if it has
 * to outlive the frame.
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
 *
 * Duplicated from src/build/i18n.h rather than shared: the build tool compiles with -DNYA_NO_SDL and
 * cannot include a core header. Must agree with what the asset indexer generated into assets.h, since
 * a locale's path *is* its asset handle — this plus a locale code rebuilds it at runtime.
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
     *
     * A second arena rather than a corner of the first, because `allocator` is emptied wholesale on
     * every commit. Anything a *reload* needs has to outlive that, and a reload has no caller to hand
     * it the keys again — which is the entire reason this arena exists.
     * */
    NYA_Arena* registry;

    /**
     * The strings, indexed by NYA_StringId. Null for a key this locale did not supply.
     *
     * Allocated for NYA_STRING_COUNT entries by the first load — which is why the count is passed in
     * rather than compiled in: core_i18n.c cannot include the generated header without the engine
     * depending on a file that only exists after the build system has run.
     * */
    NYA_CString* strings;

    /** The base locale's strings, kept so a missing key falls back to English rather than to nothing. */
    NYA_CString* fallback;

    u32 count;

    char locale[NYA_I18N_LOCALE_MAX];

    /**
     * The keys, copied out of whoever supplied them, in `registry`.
     *
     * Copied for the same reason nya_asset_load interns its handles: NYA_STRING_KEYS is an array of
     * string literals in the generated header, so a game passing it hands over pointers into its own
     * DLL's `.rodata`. Hot reloading dlcloses that DLL and unmaps them. Borrowing them was safe while
     * they were only read during the call that supplied them; keeping them so a reload can re-resolve
     * without a caller is exactly what makes copying mandatory.
     * */
    NYA_ConstCString* keys;

    /** The locale file's asset handle, and the base locale's. Null before the first successful load. */
    NYA_CString handle;
    NYA_CString fallback_handle;

#ifdef NYA_ASSET_HOT_RELOAD
    /**
     * The modification times the loaded strings were resolved from.
     *
     * Compared against what the asset system currently reports to decide that a file changed. Updated
     * only after a re-resolve *succeeds*, so a locale caught half written fails to parse, changes
     * nothing, and is tried again on the next frame rather than being recorded as handled.
     * */
    u64 modification_time;
    u64 fallback_modification_time;

    /**
     * Uptime at which a dead locale asset may next be re-armed. See _nya_i18n_rearm.
     *
     * Throttled because re-arming touches the filesystem, and the state it recovers from is one where
     * the file is missing — so without this a deleted locale would cost an unload, a load and a failed
     * open every frame, forever.
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
 *
 * Must come after nya_system_asset_init, since the watch is a frame hook and the loading path reads
 * through the asset system. Loads nothing: a game that never localises anything calls nya_i18n_load
 * never and pays one hook that returns immediately.
 * */
NYA_API void nya_system_i18n_init(void);

/** Releases every loaded string. Safe before anything has been loaded. */
NYA_API void nya_system_i18n_deinit(void);

/**
 * Loads a locale, replacing whatever was loaded before, and watches its file from then on.
 *
 * `keys` and `count` come from the generated header — pass `NYA_STRING_KEYS` and `NYA_STRING_COUNT`.
 * They are parameters rather than includes because the engine cannot include a file the build system
 * generates from the game's own assets without the two depending on each other in a circle. Both are
 * copied, so the caller may pass literals from a library that later unloads.
 *
 * The first call also loads the base locale as a fallback, so a key this locale is missing shows
 * English rather than nothing. Loading the base locale itself does not load it twice.
 *
 * NYA_ERROR_NOT_FOUND when there is no such locale file, which a caller should treat as "keep the
 * one that is loaded" rather than as fatal — a player's saved language preference may name a locale
 * that a later build dropped.
 * */
NYA_API NYA_Error nya_i18n_load(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count) __attr_no_discard;

/**
 * The same, from bytes the caller already has. Nothing is watched and no fallback is loaded.
 *
 * For text that never was a file in `assets/i18n`: a locale fetched over the network, one built in
 * memory, or a test's fixture. A locale that *is* a file wants nya_i18n_load, which reads the blob or
 * the disk as the build dictates and leaves the file watched — this entry point cannot watch anything,
 * because a buffer has no modification time.
 * */
NYA_API NYA_Error nya_i18n_load_bytes(NYA_ConstCString locale, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count) __attr_no_discard;

/** The locale currently loaded, or an empty string before the first load. */
NYA_API NYA_ConstCString nya_i18n_locale(void) __attr_no_discard;

/**
 * The raw string for an id, before any formatting. Falls back to the base locale, then to the key.
 *
 * For the rare caller that wants the format string itself — a debug listing, a length estimate. The
 * generated accessors are what game code uses, and they go through _nya_i18n_format.
 *
 * An array index and two comparisons. Nothing here consults the asset system, which is what makes it
 * safe on a path that runs per label per frame.
 *
 * Never null. An id nothing has loaded answers a placeholder naming the id, which is visible on
 * screen and therefore findable, rather than an empty string or a crash.
 * */
NYA_API NYA_ConstCString nya_i18n_raw(u32 id) __attr_no_discard;

/**
 * Formats a string into the ring and returns it. What the generated accessors call.
 *
 * Not for direct use: the whole point of the generated wrappers is that the argument types are
 * checked against the string, and calling this by hand throws that away. It is public because a
 * `static inline` in a generated header cannot call something hidden.
 * */
NYA_API NYA_ConstCString _nya_i18n_format(u32 id, ...);

#ifdef NYA_ASSET_HOT_RELOAD
/**
 * Asks the asset system whether either locale file changed, and re-resolves if one did.
 *
 * Registered on NYA_EVENT_FRAME_ENDED. Not NYA_INTERNAL and not static, for the same reason the asset
 * system's own hooks are not: a registered callback is re-resolved by name through dlsym after every
 * hot reload, and a symbol with internal linkage cannot be found again.
 * */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_i18n_watch(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD
