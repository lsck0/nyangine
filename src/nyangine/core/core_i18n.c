#include "nyangine/nyangine.h"
#include "nyangine/serde/serde.h"

#include <stdarg.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_I18nSystem* _nya_i18n_system(void);

/**
 * Resolves a parsed locale's keys into id order. A key whose value is a string resolves to that string
 * and no plural variants; a key whose value is an object is a plural message, and its category variants
 * are written into `out_plurals` while `out` gets the `other` variant so the fallback chain still answers.
 * */
NYA_INTERNAL NYA_Error
_nya_i18n_resolve(NYA_Arena* arena, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out, OUT NYA_CString** out_plurals);

/** Reads a locale through the asset system, blob first then disk, and resolves it. */
NYA_INTERNAL NYA_Error
_nya_i18n_read(NYA_Arena* arena, NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out, OUT NYA_CString** out_plurals);

/** Commits already-resolved strings, replacing whatever was loaded. Takes ownership of nothing. */
NYA_INTERNAL void _nya_i18n_commit(NYA_ConstCString locale, NYA_CString* strings, NYA_CString* plurals, NYA_CString* fallback, NYA_CString* fallback_plurals, u32 count);

/** The language subtag of a locale, lower-cased: `en` from `en-US`, `pt` from `pt_BR`. */
NYA_INTERNAL void _nya_i18n_language(NYA_ConstCString locale, OUT char* out, u64 capacity);

/**
 * The variant a plural id resolves to for a category, following the same fallback chain nya_i18n_raw does:
 * the loaded locale's variant, then its `other`, then the base locale's variant, then its `other`, then
 * whatever nya_i18n_raw answers.
 * */
NYA_INTERNAL NYA_ConstCString _nya_i18n_plural_variant(u32 id, NYA_I18nPluralCategory category);

/** A locale's asset handle, which is its path. Allocated in `arena`. */
NYA_INTERNAL NYA_CString _nya_i18n_handle(NYA_Arena* arena, NYA_ConstCString locale);

/**
 * Copies the keys and the two handles into `registry`, and registers both files with the asset system.
 * */
NYA_INTERNAL void _nya_i18n_remember(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count);

/** The load, without the logging, so a reload can reuse it without narrating itself as a first load. */
NYA_INTERNAL NYA_Error _nya_i18n_load_locale(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count);

#ifdef NYA_ASSET_HOT_RELOAD
/** The asset system's current modification time for a handle, or zero when it has none. */
NYA_INTERNAL u64 _nya_i18n_modification_time(NYA_CString handle);

/**
 * Puts a locale asset that has died back into a state where it can be watched again.
 * */
NYA_INTERNAL void _nya_i18n_rearm(void);
#endif // NYA_ASSET_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_i18n_init(void) {
    NYA_App* app = nya_app_get();

    app->i18n_system = (NYA_I18nSystem){
        .allocator = nya_arena_create(.name = "i18n_system_allocator"),
        .registry  = nya_arena_create(.name = "i18n_system_registry"),
    };

#ifdef NYA_ASSET_HOT_RELOAD
    /*
     * Registered after the asset system's frame-ended hooks, so a reload queued earlier has already been
     * re-read and the modification time compared against is settled.
     */
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_i18n_watch),
    });
#endif // NYA_ASSET_HOT_RELOAD

    nya_log_info("Localization system initialized.");
}

void nya_system_i18n_deinit(void) {
    NYA_I18nSystem* system = &nya_app_get()->i18n_system;

    if (system->allocator != nullptr) nya_arena_destroy(system->allocator);
    if (system->registry != nullptr) nya_arena_destroy(system->registry);

    *system = (NYA_I18nSystem){ 0 };

    nya_log_info("Localization system deinitialized.");
}

NYA_Error nya_i18n_load(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count) {
    nya_assert(locale != nullptr);
    nya_assert(keys != nullptr);

    NYA_TRY(_nya_i18n_load_locale(locale, keys, count));

    nya_log_info("Loaded locale '%s' (%u strings).", locale, count);

    return NYA_OK;
}

NYA_Error nya_i18n_load_bytes(NYA_ConstCString locale, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count) {
    nya_assert(locale != nullptr);
    nya_assert(keys != nullptr);

    if (count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a locale with no keys");

    NYA_Arena* scratch = nya_arena_create(.name = "i18n_load_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_CString* strings = nullptr;
    NYA_CString* plurals = nullptr;
    NYA_TRY(_nya_i18n_resolve(scratch, data, size, keys, count, &strings, &plurals));

    /*
     * No fallback and nothing watched.
     */
    _nya_i18n_commit(locale, strings, plurals, nullptr, nullptr, count);

    // forget the file an earlier nya_i18n_load watched, or editing it would reload that file over these
    // strings under this locale's name.
    NYA_I18nSystem* system  = _nya_i18n_system();
    system->handle          = nullptr;
    system->fallback_handle = nullptr;

    return NYA_OK;
}

NYA_ConstCString nya_i18n_locale(void) {
    return _nya_i18n_system()->locale;
}

NYA_ConstCString nya_i18n_raw(u32 id) {
    NYA_I18nSystem* system = _nya_i18n_system();

    if (id < system->count && system->strings != nullptr && system->strings[id] != nullptr) return system->strings[id];
    if (id < system->count && system->fallback != nullptr && system->fallback[id] != nullptr) return system->fallback[id];

    /*
     * A visible placeholder rather than an empty string.
     */
    static char missing[32];
    (void)snprintf(missing, sizeof(missing), "[string %u]", id);

    return missing;
}

NYA_ConstCString _nya_i18n_format(u32 id, ...) {
    NYA_I18nSystem* system = _nya_i18n_system();

    NYA_ConstCString format = nya_i18n_raw(id);

    // round robin, so a caller can hold a few at once, enough for one draw call and not enough to look
    // like ownership.
    char* buffer = system->formatted[system->next_slot];

    system->next_slot = (system->next_slot + 1) % NYA_I18N_FORMAT_SLOTS;

    va_list arguments;
    va_start(arguments, id);

    /*
     * The one place a runtime format string is unavoidable, and the reason everything above exists to
     * constrain it.
     */
    (void)vsnprintf(buffer, NYA_I18N_FORMAT_MAX, format, arguments);

    va_end(arguments);

    return buffer;
}

NYA_I18nPluralCategory nya_i18n_plural_category(NYA_ConstCString locale, s64 n) {
    char language[NYA_I18N_LOCALE_MAX];
    _nya_i18n_language(locale != nullptr ? locale : "", language, sizeof(language));

    // The CLDR operands for a whole number: n = i = |value|, and the visible fraction v = 0. The
    // absolute value is taken without negating, so the most-negative s64 does not overflow — a signed
    // overflow the build's sanitizer would trap.
    u64 i      = n < 0 ? (u64)(-(n + 1)) + 1U : (u64)n;
    u64 mod10  = i % 10;
    u64 mod100 = i % 100;

    // one/other, the shape most languages share. `one` is exactly one.
    if (nya_string_equals(language, "en") || nya_string_equals(language, "de")) {
        return i == 1 ? NYA_I18N_PLURAL_ONE : NYA_I18N_PLURAL_OTHER;
    }

    // French counts zero and one alike as `one`, and a nonzero whole million as `many`.
    if (nya_string_equals(language, "fr")) {
        if (i == 0 || i == 1) return NYA_I18N_PLURAL_ONE;
        if (i != 0 && i % 1000000 == 0) return NYA_I18N_PLURAL_MANY;
        return NYA_I18N_PLURAL_OTHER;
    }

    // Russian, by the last digit unless the last two are the teens. No whole number is `other` here.
    if (nya_string_equals(language, "ru")) {
        if (mod10 == 1 && mod100 != 11) return NYA_I18N_PLURAL_ONE;
        if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14)) return NYA_I18N_PLURAL_FEW;
        return NYA_I18N_PLURAL_MANY;
    }

    // Polish, the same shape as Russian but `one` is strictly one, not every number ending in one.
    if (nya_string_equals(language, "pl")) {
        if (i == 1) return NYA_I18N_PLURAL_ONE;
        if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14)) return NYA_I18N_PLURAL_FEW;
        return NYA_I18N_PLURAL_MANY;
    }

    // Czech: one, a few (two to four), and otherwise. Its `many` is a fraction-only category.
    if (nya_string_equals(language, "cs")) {
        if (i == 1) return NYA_I18N_PLURAL_ONE;
        if (i >= 2 && i <= 4) return NYA_I18N_PLURAL_FEW;
        return NYA_I18N_PLURAL_OTHER;
    }

    // Arabic uses every category, and the last two digits decide `few` and `many`.
    if (nya_string_equals(language, "ar")) {
        if (i == 0) return NYA_I18N_PLURAL_ZERO;
        if (i == 1) return NYA_I18N_PLURAL_ONE;
        if (i == 2) return NYA_I18N_PLURAL_TWO;
        if (mod100 >= 3 && mod100 <= 10) return NYA_I18N_PLURAL_FEW;
        if (mod100 >= 11 && mod100 <= 99) return NYA_I18N_PLURAL_MANY;
        return NYA_I18N_PLURAL_OTHER;
    }

    // Languages with no count distinction at all: one form for every number.
    if (nya_string_equals(language, "ja") || nya_string_equals(language, "zh") || nya_string_equals(language, "ko")) {
        return NYA_I18N_PLURAL_OTHER;
    }

    // Any language whose rules are not spelled out here. `other` is the one category every language has,
    // so a message with only an `other` variant reads correctly whatever the count.
    return NYA_I18N_PLURAL_OTHER;
}

NYA_ConstCString nya_i18n_plural_category_name(NYA_I18nPluralCategory category) {
    switch (category) {
        case NYA_I18N_PLURAL_ZERO: return "zero";
        case NYA_I18N_PLURAL_ONE:  return "one";
        case NYA_I18N_PLURAL_TWO:  return "two";
        case NYA_I18N_PLURAL_FEW:  return "few";
        case NYA_I18N_PLURAL_MANY: return "many";
        // `other` is the safe answer for the real category and for the count sentinel alike.
        case NYA_I18N_PLURAL_OTHER:
        case NYA_I18N_PLURAL_COUNT:
        default: return "other";
    }
}

NYA_ConstCString nya_i18n_decimal_separator(NYA_ConstCString locale) {
    char language[NYA_I18N_LOCALE_MAX];
    _nya_i18n_language(locale != nullptr ? locale : "", language, sizeof(language));

    // A comma in the languages that write 3,14, a point everywhere else.
    if (nya_string_equals(language, "de") || nya_string_equals(language, "fr") || nya_string_equals(language, "ru") ||
        nya_string_equals(language, "pl") || nya_string_equals(language, "cs") || nya_string_equals(language, "es") ||
        nya_string_equals(language, "it")) {
        return ",";
    }

    return ".";
}

NYA_ConstCString nya_i18n_group_separator(NYA_ConstCString locale) {
    char language[NYA_I18N_LOCALE_MAX];
    _nya_i18n_language(locale != nullptr ? locale : "", language, sizeof(language));

    // A space where the decimal is a comma and the thousands are spaced, a point where the decimal is a
    // comma and the thousands are pointed, a comma otherwise. A plain space, not the narrow no-break space
    // the CLDR names, so the basics stay one ASCII byte; this is grouping, not typesetting.
    if (nya_string_equals(language, "fr") || nya_string_equals(language, "ru") || nya_string_equals(language, "pl") ||
        nya_string_equals(language, "cs")) {
        return " ";
    }

    if (nya_string_equals(language, "de") || nya_string_equals(language, "es") || nya_string_equals(language, "it")) {
        return ".";
    }

    return ",";
}

NYA_ConstCString nya_i18n_format_integer(NYA_ConstCString locale, s64 n) {
    NYA_I18nSystem* system = _nya_i18n_system();

    NYA_ConstCString group = nya_i18n_group_separator(locale);

    // The digits, written out most-significant last, then reversed with a separator every three.
    char digits[24];
    u32  digit_count = 0;

    u64 magnitude = n < 0 ? (u64)(-(n + 1)) + 1U : (u64)n;

    do {
        digits[digit_count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0 && digit_count < sizeof(digits));

    char* buffer = system->formatted[system->next_slot];
    system->next_slot = (system->next_slot + 1) % NYA_I18N_FORMAT_SLOTS;

    u32 length = 0;

    if (n < 0 && length + 1 < NYA_I18N_FORMAT_MAX) buffer[length++] = '-';

    for (u32 emitted = 0; emitted < digit_count; emitted++) {
        // A separator before every third digit from the right, except leading the number.
        if (emitted > 0 && (digit_count - emitted) % 3 == 0) {
            for (const char* g = group; *g != '\0' && length + 1 < NYA_I18N_FORMAT_MAX; g++) buffer[length++] = *g;
        }

        if (length + 1 < NYA_I18N_FORMAT_MAX) buffer[length++] = digits[digit_count - 1 - emitted];
    }

    buffer[length] = '\0';

    return buffer;
}

NYA_ConstCString _nya_i18n_format_plural(u32 id, s64 n, ...) {
    NYA_I18nSystem* system = _nya_i18n_system();

    NYA_I18nPluralCategory category = nya_i18n_plural_category(system->locale, n);
    NYA_ConstCString       format   = _nya_i18n_plural_variant(id, category);

    char* buffer = system->formatted[system->next_slot];

    system->next_slot = (system->next_slot + 1) % NYA_I18N_FORMAT_SLOTS;

    va_list arguments;
    va_start(arguments, n);

    (void)vsnprintf(buffer, NYA_I18N_FORMAT_MAX, format, arguments);

    va_end(arguments);

    return buffer;
}

#ifdef NYA_ASSET_HOT_RELOAD
void _nya_i18n_watch(NYA_Event* event) {
    nya_unused(event);

    NYA_I18nSystem* system = &nya_app_get()->i18n_system;

    // Nothing has been loaded from a file, so there is nothing to watch. A game that never localises
    // anything, and a locale supplied through nya_i18n_load_bytes, both land here and stop.
    if (system->handle == nullptr) return;

    /*
     * The two nya_asset_get calls are the point of this function, not the comparison below them.
     */
    (void)nya_asset_get(system->handle);
    if (system->fallback_handle != nullptr) (void)nya_asset_get(system->fallback_handle);

    // Before the comparison, because a dead asset reports no timestamp at all and would otherwise look
    // like a file that simply had not changed.
    _nya_i18n_rearm();

    u64 now          = _nya_i18n_modification_time(system->handle);
    u64 fallback_now = system->fallback_handle != nullptr ? _nya_i18n_modification_time(system->fallback_handle) : 0;

    if (now == system->modification_time && fallback_now == system->fallback_modification_time) return;

    /*
     * Re-read from scratch rather than from the reloaded asset's bytes.
     */
    /*
     * The locale code is copied out before the reload, because the reload writes it back.
     */
    char locale[NYA_I18N_LOCALE_MAX];
    (void)snprintf(locale, sizeof(locale), "%s", system->locale);

    NYA_Error reloaded = _nya_i18n_load_locale(locale, system->keys, system->count);

    if (!reloaded.ok) return;

    nya_log_info("Reloaded locale '%s'.", locale);
}
#endif // NYA_ASSET_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_I18nSystem* _nya_i18n_system(void) {
    NYA_I18nSystem* system = &nya_app_get()->i18n_system;

    /*
     * Created on first use as well as by the init, so reading a string before the system is up answers
     * a placeholder instead of faulting.
     */
    if (system->allocator == nullptr) system->allocator = nya_arena_create(.name = "i18n_system_allocator");
    if (system->registry == nullptr) system->registry = nya_arena_create(.name = "i18n_system_registry");

    return system;
}

NYA_Error _nya_i18n_load_locale(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count) {
    if (count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a locale with no keys");
    if (keys == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a locale with no keys");

    /*
     * Read into a scratch arena first, and only commit once it has parsed.
     */
    NYA_Arena* scratch = nya_arena_create(.name = "i18n_load_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_CString* strings = nullptr;
    NYA_CString* plurals = nullptr;
    NYA_TRY(_nya_i18n_read(scratch, locale, keys, count, &strings, &plurals));

    // The base locale, once, so a key this one is missing shows English rather than nothing. Read
    // before the old arena is reset, into the same scratch, so a failure here changes nothing either.
    NYA_CString* fallback         = nullptr;
    NYA_CString* fallback_plurals = nullptr;

    if (!nya_string_equals(locale, NYA_I18N_BASE_LOCALE)) {
        NYA_Error fallback_error = _nya_i18n_read(scratch, NYA_I18N_BASE_LOCALE, keys, count, &fallback, &fallback_plurals);

        // Not fatal. A build without the base locale on disk is a broken install, and showing the
        // requested language with keys as placeholders beats refusing to start.
        if (!fallback_error.ok) {
            nya_log_warn("Could not read the base locale '%s' as a fallback; missing keys will show their names.", NYA_I18N_BASE_LOCALE);
            fallback         = nullptr;
            fallback_plurals = nullptr;
        }
    }

    /*
     * Keys are remembered before the commit resets `allocator`. On reload the `keys` passed in are the
     * remembered ones, which live in `registry` so that reset cannot take them.
     */
    _nya_i18n_remember(locale, keys, count);

    _nya_i18n_commit(locale, strings, plurals, fallback, fallback_plurals, count);

#ifdef NYA_ASSET_HOT_RELOAD
    // Recorded only now, after everything parsed. See the note in _nya_i18n_watch on why a failed
    // reload must leave these alone.
    NYA_I18nSystem* system = _nya_i18n_system();

    system->modification_time          = _nya_i18n_modification_time(system->handle);
    system->fallback_modification_time = system->fallback_handle != nullptr ? _nya_i18n_modification_time(system->fallback_handle) : 0;
#endif // NYA_ASSET_HOT_RELOAD

    return NYA_OK;
}

NYA_CString _nya_i18n_handle(NYA_Arena* arena, NYA_ConstCString locale) {
    NYA_String* path = nya_string_sprintf(arena, "%s/%s.json", NYA_I18N_ASSET_DIRECTORY, locale);

    return nya_string_to_cstring(arena, path);
}

void _nya_i18n_remember(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count) {
    NYA_I18nSystem* system = _nya_i18n_system();

    /*
     * Emptied and rebuilt rather than appended to.
     */
    NYA_Arena* staging = nya_arena_create(.name = "i18n_remember_staging");
    defer      nya_arena_destroy(staging);

    NYA_ConstCString* copied = nya_arena_alloc(staging, count * sizeof(NYA_ConstCString));

    for (u32 i = 0; i < count; i++) {
        copied[i] = keys[i] != nullptr ? nya_string_to_cstring(staging, nya_string_from(staging, keys[i])) : nullptr;
    }

    nya_arena_free_all(system->registry);

    system->keys = nya_arena_alloc(system->registry, count * sizeof(NYA_ConstCString));

    for (u32 i = 0; i < count; i++) {
        system->keys[i] = copied[i] != nullptr ? nya_string_to_cstring(system->registry, nya_string_from(system->registry, copied[i])) : nullptr;
    }

    system->handle          = _nya_i18n_handle(system->registry, locale);
    system->fallback_handle = nya_string_equals(locale, NYA_I18N_BASE_LOCALE) ? nullptr : _nya_i18n_handle(system->registry, NYA_I18N_BASE_LOCALE);

    /*
     * Registered as text assets so the file is watched from here on.
     */
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = system->handle });

    if (system->fallback_handle != nullptr) {
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = system->fallback_handle });
    }
}

#ifdef NYA_ASSET_HOT_RELOAD
void _nya_i18n_rearm(void) {
    NYA_I18nSystem* system = &nya_app_get()->i18n_system;

    NYA_CString handles[] = { system->handle, system->fallback_handle };

    for (u64 i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        if (handles[i] == nullptr) continue;

        NYA_Asset* asset = nya_asset_get(handles[i]);

        /*
         * Both terminal states, not just FAILED.
         */
        b8 stuck = asset == nullptr || asset->status == NYA_ASSET_STATUS_FAILED || asset->status == NYA_ASSET_STATUS_UNLOADED;

        if (!stuck) continue;

        u64 now_ns = nya_app_get()->frame_stats.uptime_ns;
        if (now_ns < system->next_recovery_ns) continue;

        system->next_recovery_ns = now_ns + _NYA_ASSET_STAT_INTERVAL_NS;

        nya_log_debug("Re-arming the locale asset '%s' after a failed load.", handles[i]);

        (void)nya_asset_unload(handles[i]);
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = handles[i] });

        /*
         * The recorded timestamp is cleared so the strings are re-resolved once the file is back.
         */
        if (i == 0) system->modification_time = 0;
        else system->fallback_modification_time = 0;
    }
}

u64 _nya_i18n_modification_time(NYA_CString handle) {
    if (handle == nullptr) return 0;

    NYA_Asset* asset = nya_asset_get(handle);

    // Out of the blob: part of the executable, so there is no file and nothing that could differ.
    if (asset != nullptr && asset->from_blob) return 0;

    /*
     * The asset's own timestamp once it has one, and the file's until then.
     */
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) return asset->source_modification_time;

    u64 modified = 0;

    // A locale that is missing answers zero, which compares equal to itself and so reads as
    // "nothing changed" rather than as a change that can never be resolved.
    if (!nya_filesystem_last_modified(handle, &modified).ok) return 0;

    return modified;
}
#endif // NYA_ASSET_HOT_RELOAD

NYA_Error _nya_i18n_read(NYA_Arena* arena, NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out, OUT NYA_CString** out_plurals) {
    *out         = nullptr;
    *out_plurals = nullptr;

    /*
     * Through the asset system rather than nya_file_read.
     */
    u8* data = nullptr;
    u64 size = 0;

    NYA_CString handle = _nya_i18n_handle(arena, locale);

    NYA_TRY(nya_asset_read(arena, handle, &data, &size));

    return _nya_i18n_resolve(arena, data, size, keys, count, out, out_plurals);
}

NYA_Error
_nya_i18n_resolve(NYA_Arena* arena, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out, OUT NYA_CString** out_plurals) {
    *out         = nullptr;
    *out_plurals = nullptr;

    NYA_Object* root = nullptr;

    // JSONC, so translators can leave notes beside strings.
    NYA_TRY(nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &root));

    NYA_CString* strings = nya_arena_alloc(arena, count * sizeof(NYA_CString));
    NYA_CString* plurals = nya_arena_alloc(arena, (u64)count * NYA_I18N_PLURAL_COUNT * sizeof(NYA_CString));

    // Zeroed up front: the arena does not zero, and every category of a non-plural key stays null.
    for (u32 slot = 0; slot < count * NYA_I18N_PLURAL_COUNT; slot++) plurals[slot] = nullptr;

    for (u32 i = 0; i < count; i++) {
        NYA_Value* value = nya_object_get(root, (NYA_CString)keys[i]);

        // A plural key is an object of category variants. Each present variant is stored, and the
        // `other` variant doubles as the key's plain string so nya_i18n_raw and the fallback chain
        // still answer for it. Left null otherwise: nya_i18n_raw is the one place that decides what a
        // missing string shows, so the fallback chain lives in exactly one function.
        if (value != nullptr && value->type == NYA_TYPE_OBJECT) {
            for (u32 category = 0; category < NYA_I18N_PLURAL_COUNT; category++) {
                NYA_Value* variant                            = nya_object_get(&value->as_object, (NYA_CString)nya_i18n_plural_category_name((NYA_I18nPluralCategory)category));
                plurals[i * NYA_I18N_PLURAL_COUNT + category] = variant != nullptr && variant->type == NYA_TYPE_STRING ? variant->as_string : nullptr;
            }

            strings[i] = plurals[i * NYA_I18N_PLURAL_COUNT + NYA_I18N_PLURAL_OTHER];
        } else {
            strings[i] = value != nullptr && value->type == NYA_TYPE_STRING ? value->as_string : nullptr;
        }
    }

    *out         = strings;
    *out_plurals = plurals;

    return NYA_OK;
}

void _nya_i18n_commit(NYA_ConstCString locale, NYA_CString* strings, NYA_CString* plurals, NYA_CString* fallback, NYA_CString* fallback_plurals, u32 count) {
    NYA_I18nSystem* system = _nya_i18n_system();

    // the previous locale's arena is emptied and reused rather than destroyed, so switching language
    // does not churn the allocator. Only after every parse succeeded, so a failed load keeps the old
    // language intact.
    nya_arena_free_all(system->allocator);

    system->strings          = nya_arena_alloc(system->allocator, count * sizeof(NYA_CString));
    system->fallback         = fallback != nullptr ? nya_arena_alloc(system->allocator, count * sizeof(NYA_CString)) : nullptr;
    system->plurals          = nya_arena_alloc(system->allocator, (u64)count * NYA_I18N_PLURAL_COUNT * sizeof(NYA_CString));
    system->fallback_plurals = fallback_plurals != nullptr ? nya_arena_alloc(system->allocator, (u64)count * NYA_I18N_PLURAL_COUNT * sizeof(NYA_CString)) : nullptr;
    system->count            = count;

    for (u32 i = 0; i < count; i++) {
        system->strings[i] = strings[i] != nullptr ? nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, strings[i])) : nullptr;

        if (system->fallback != nullptr) {
            system->fallback[i] = fallback[i] != nullptr ? nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, fallback[i]))
                                                         : nullptr;
        }

        for (u32 category = 0; category < NYA_I18N_PLURAL_COUNT; category++) {
            u32 slot = i * NYA_I18N_PLURAL_COUNT + category;

            system->plurals[slot] = plurals[slot] != nullptr ? nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, plurals[slot]))
                                                              : nullptr;

            if (system->fallback_plurals != nullptr) {
                system->fallback_plurals[slot] =
                    fallback_plurals[slot] != nullptr ? nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, fallback_plurals[slot]))
                                                      : nullptr;
            }
        }
    }

    (void)snprintf(system->locale, sizeof(system->locale), "%s", locale);
}

void _nya_i18n_language(NYA_ConstCString locale, OUT char* out, u64 capacity) {
    u64 length = 0;

    for (const char* cursor = locale; *cursor != '\0' && *cursor != '-' && *cursor != '_' && length + 1 < capacity; cursor++) {
        char character = *cursor;

        if (character >= 'A' && character <= 'Z') character = (char)(character - 'A' + 'a');

        out[length++] = character;
    }

    out[length] = '\0';
}

NYA_ConstCString _nya_i18n_plural_variant(u32 id, NYA_I18nPluralCategory category) {
    NYA_I18nSystem* system = _nya_i18n_system();

    if (id < system->count && system->plurals != nullptr) {
        NYA_ConstCString exact = system->plurals[id * NYA_I18N_PLURAL_COUNT + category];
        if (exact != nullptr) return exact;

        NYA_ConstCString other = system->plurals[id * NYA_I18N_PLURAL_COUNT + NYA_I18N_PLURAL_OTHER];
        if (other != nullptr) return other;
    }

    if (id < system->count && system->fallback_plurals != nullptr) {
        NYA_ConstCString exact = system->fallback_plurals[id * NYA_I18N_PLURAL_COUNT + category];
        if (exact != nullptr) return exact;

        NYA_ConstCString other = system->fallback_plurals[id * NYA_I18N_PLURAL_COUNT + NYA_I18N_PLURAL_OTHER];
        if (other != nullptr) return other;
    }

    // Not a plural key, or a plural key with no variant loaded at all: nya_i18n_raw decides what shows.
    return nya_i18n_raw(id);
}
