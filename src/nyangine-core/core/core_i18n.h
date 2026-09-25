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

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_asset.h"
#include "nyangine-core/core/core_event.h"

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

/**
 * A CLDR plural category. Which categories a language actually uses is a property of the language:
 * English has `one` and `other`, Arabic has all six, Japanese has only `other`. The enum lists all
 * six so one type serves every language, and nya_i18n_plural_category maps a locale and a count onto
 * the one the language would pick. The order matches the order the CLDR spells the categories in.
 * */
typedef enum {
    NYA_I18N_PLURAL_ZERO,
    NYA_I18N_PLURAL_ONE,
    NYA_I18N_PLURAL_TWO,
    NYA_I18N_PLURAL_FEW,
    NYA_I18N_PLURAL_MANY,
    NYA_I18N_PLURAL_OTHER,

    NYA_I18N_PLURAL_COUNT,
} NYA_I18nPluralCategory;

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

    /**
     * The plural variants, row-major by id: `plurals[id * NYA_I18N_PLURAL_COUNT + category]`. Null for
     * every category of a key that is not plural, and for a category the loaded locale did not supply.
     * A plural key's `strings[id]` holds its `other` variant, so nya_i18n_raw and the fallback chain still
     * answer something for one.
     * */
    NYA_CString* plurals;

    /** The base locale's plural variants, laid out the same, so a plural key falls back category by category. */
    NYA_CString* fallback_plurals;

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

/**
 * The CLDR plural category a language picks for a count. `en` answers `one` for 1 and `other` for
 * everything else; `ru` runs its count through mod-10 and mod-100; `ar` uses all six. The rules are
 * hard-coded for a representative set of languages — en, de, fr, ru, pl, cs, ar, and the categoryless
 * ja/zh/ko — matched on the language subtag of the locale, so `en-US` resolves as `en`. Any language
 * not on that list answers `NYA_I18N_PLURAL_OTHER`, which is the safe default every language has.
 *
 * The count is taken as an integer: CLDR's fractional operands do not arise from a whole number, so a
 * category a language reaches only through a fraction (Czech's `many`, Russian's `other`) never comes
 * back from here.
 * */
NYA_API NYA_I18nPluralCategory nya_i18n_plural_category(NYA_ConstCString locale, s64 n) __attr_no_discard;

/** The CLDR name of a category — `"zero"`, `"one"`, … — which is also the JSON key a plural message uses. */
NYA_API NYA_ConstCString nya_i18n_plural_category_name(NYA_I18nPluralCategory category) __attr_no_discard;

/**
 * The decimal and grouping separators a locale writes numbers with: `en` groups with `,` and points with
 * `.`, `de` the other way around, `fr` groups with a space. Matched on the language subtag like the plural
 * rules, and a language not covered answers the `en` separators.
 * */
NYA_API NYA_ConstCString nya_i18n_decimal_separator(NYA_ConstCString locale) __attr_no_discard;
NYA_API NYA_ConstCString nya_i18n_group_separator(NYA_ConstCString locale) __attr_no_discard;

/**
 * Formats an integer into the ring with the locale's grouping separator, e.g. `1234567` as `1,234,567`
 * for `en` and `1.234.567` for `de`. Shares the ring with the formatted strings; see the note on lifetime.
 * */
NYA_API NYA_ConstCString nya_i18n_format_integer(NYA_ConstCString locale, s64 n) __attr_no_discard;

/**
 * Formats a plural message: selects the variant for `n` in the loaded locale, then formats it with the
 * arguments, exactly as _nya_i18n_format does. What a generated accessor for a plural key calls, passing
 * the count both as `n` and as the first format argument.
 * */
NYA_API NYA_ConstCString _nya_i18n_format_plural(u32 id, s64 n, ...);

#ifdef NYA_ASSET_HOT_RELOAD
/**
 * Asks the asset system whether either locale file changed, and re-resolves if one did.
 * */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_i18n_watch(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD
