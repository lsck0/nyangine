#include "nyangine/nyangine.h"
#include "nyangine/serde/serde.h"

#include <stdarg.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_I18nSystem* _nya_i18n_system(void);

/** Resolves a parsed locale's keys into id order. */
NYA_INTERNAL NYA_Error _nya_i18n_resolve(NYA_Arena* arena, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out);

/** Reads a locale through the asset system — blob first, disk second — and resolves it. */
NYA_INTERNAL NYA_Error _nya_i18n_read(NYA_Arena* arena, NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out);

/** Commits already-resolved strings, replacing whatever was loaded. Takes ownership of nothing. */
NYA_INTERNAL void _nya_i18n_commit(NYA_ConstCString locale, NYA_CString* strings, NYA_CString* fallback, u32 count);

/** A locale's asset handle, which is its path. Allocated in `arena`. */
NYA_INTERNAL NYA_CString _nya_i18n_handle(NYA_Arena* arena, NYA_ConstCString locale);

/**
 * Copies the keys and the two handles into `registry`, and registers both files with the asset system.
 *
 * The registration is what makes the files watchable: reload detection lives in nya_asset_get, so a
 * file nothing has ever loaded as an asset is a file nothing will ever notice changing.
 * */
NYA_INTERNAL void _nya_i18n_remember(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count);

/** The load, without the logging, so a reload can reuse it without narrating itself as a first load. */
NYA_INTERNAL NYA_Error _nya_i18n_load_locale(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count);

#ifdef NYA_ASSET_HOT_RELOAD
/** The asset system's current modification time for a handle, or zero when it has none. */
NYA_INTERNAL u64 _nya_i18n_modification_time(NYA_CString handle);

/**
 * Puts a locale asset that has died back into a state where it can be watched again.
 *
 * The asset system only stats an asset while it is LOADED, so any other resting state means the file
 * has stopped being watched: a load that could not open its file leaves it FAILED, and a file that
 * vanished between the unload and the load of a reload leaves it UNLOADED. Neither recovers on its own,
 * and neither is distinguishable from "nothing has changed" by looking at timestamps.
 *
 * It is worth recovering from because the ordinary way an editor saves a file produces it: writing to a
 * temporary and renaming over the target means the path briefly does not exist, and a stat that lands
 * in that window fails. Without this, a translator using such an editor would get exactly one reload
 * and then silence until the game restarted.
 *
 * Unloading first is what makes the reload possible: the unload pass moves it to UNLOADED, which is the
 * one status nya_asset_load will accept for a handle it already knows.
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
     * Registered after the asset system's own frame-ended hooks, because init order puts this system
     * after it — so by the time this runs, a reload queued on an earlier frame has already been
     * unloaded and re-read, and the modification time it compares against is the settled one.
     */
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_i18n_watch),
    });
#endif // NYA_ASSET_HOT_RELOAD

    nya_info("Localization system initialized.");
}

void nya_system_i18n_deinit(void) {
    NYA_I18nSystem* system = &nya_app_get()->i18n_system;

    if (system->allocator != nullptr) nya_arena_destroy(system->allocator);
    if (system->registry != nullptr) nya_arena_destroy(system->registry);

    *system = (NYA_I18nSystem){ 0 };

    nya_info("Localization system deinitialized.");
}

NYA_Error nya_i18n_load(NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count) {
    nya_assert(locale != nullptr);
    nya_assert(keys != nullptr);

    NYA_TRY(_nya_i18n_load_locale(locale, keys, count));

    nya_info("Loaded locale '%s' (%u strings).", locale, count);

    return NYA_OK;
}

NYA_Error nya_i18n_load_bytes(NYA_ConstCString locale, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count) {
    nya_assert(locale != nullptr);
    nya_assert(keys != nullptr);

    if (count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a locale with no keys");

    NYA_Arena* scratch = nya_arena_create(.name = "i18n_load_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_CString* strings = nullptr;
    NYA_TRY(_nya_i18n_resolve(scratch, data, size, keys, count, &strings));

    /*
     * No fallback and nothing watched.
     *
     * These bytes were handed over rather than read, so there is no second file to fetch and no path
     * to stat. A caller wanting a fallback loads the base locale through here first — see the note in
     * core_i18n.h on why this entry point still exists now that the asset system can read the blob.
     */
    _nya_i18n_commit(locale, strings, nullptr, count);

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
     *
     * An empty string is indistinguishable from a label that is meant to be blank, so a missing
     * translation would show as a gap nobody investigates. A bracketed id shows up on screen, is
     * obviously wrong, and says which key to go and add.
     */
    static char missing[32];
    (void)snprintf(missing, sizeof(missing), "[string %u]", id);

    return missing;
}

NYA_ConstCString _nya_i18n_format(u32 id, ...) {
    NYA_I18nSystem* system = _nya_i18n_system();

    NYA_ConstCString format = nya_i18n_raw(id);

    // Round robin, so a caller may hold a handful at once — long enough to pass several to one draw
    // call, and deliberately not long enough to be mistaken for ownership.
    char* buffer = system->formatted[system->next_slot];

    system->next_slot = (system->next_slot + 1) % NYA_I18N_FORMAT_SLOTS;

    va_list arguments;
    va_start(arguments, id);

    /*
     * The one place a runtime format string is unavoidable, and the reason everything above exists to
     * constrain it.
     *
     * The string comes from a translator and the arguments come from a generated signature built off
     * the *base* locale — so the two agree only because src/build/i18n.c refuses to build a
     * translation whose specifiers differ. Without that check this call is a format string
     * vulnerability with a language selector attached.
     */
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
     *
     * Reload detection lives inside nya_asset_get: it stats the file at most once per stat interval
     * and queues the asset when the timestamp moved. Nothing else in the engine ever resolves a locale
     * as an asset — the strings are read out of this system's own table — so without these two calls
     * the files would be registered and never looked at again.
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
     *
     * The asset system moves an asset's timestamp forward before its new bytes have landed — that is
     * how it recognises a file still being written — so the bytes behind the handle at this instant
     * may still be the old ones. Reading the file again through nya_asset_read sidesteps the whole
     * question: a locale caught half written fails to parse, _nya_i18n_load_locale changes nothing,
     * and because the recorded timestamps are only advanced on success the next frame tries again.
     */
    /*
     * The locale code is copied out before the reload, because the reload writes it back.
     *
     * _nya_i18n_commit ends with an snprintf of the locale into system->locale, so handing it
     * system->locale directly makes source and destination the same buffer — which is undefined, and
     * in practice produced an empty locale: the reload worked, every string was correct, and
     * nya_i18n_locale() answered "". A test that only checked the strings would have missed it.
     */
    char locale[NYA_I18N_LOCALE_MAX];
    (void)snprintf(locale, sizeof(locale), "%s", system->locale);

    NYA_Error reloaded = _nya_i18n_load_locale(locale, system->keys, system->count);

    if (!reloaded.ok) return;

    nya_info("Reloaded locale '%s'.", locale);
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
     *
     * That is not hypothetical: nya_i18n_raw is reachable from a crash handler drawing a message, and
     * from a test that brings up nothing at all.
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
     *
     * A locale that fails halfway would otherwise leave the game with half of one language and half
     * of another, which is worse than either — and worse than simply keeping what was already
     * loaded, which is what a failure here does.
     */
    NYA_Arena* scratch = nya_arena_create(.name = "i18n_load_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_CString* strings = nullptr;
    NYA_TRY(_nya_i18n_read(scratch, locale, keys, count, &strings));

    // The base locale, once, so a key this one is missing shows English rather than nothing. Read
    // before the old arena is reset, into the same scratch, so a failure here changes nothing either.
    NYA_CString* fallback = nullptr;

    if (!nya_string_equals(locale, NYA_I18N_BASE_LOCALE)) {
        NYA_Error fallback_error = _nya_i18n_read(scratch, NYA_I18N_BASE_LOCALE, keys, count, &fallback);

        // Not fatal. A build without the base locale on disk is a broken install, and showing the
        // requested language with keys as placeholders beats refusing to start.
        if (!fallback_error.ok) {
            nya_warn("Could not read the base locale '%s' as a fallback; missing keys will show their names.", NYA_I18N_BASE_LOCALE);
            fallback = nullptr;
        }
    }

    /*
     * The keys are remembered *before* the commit, because the commit is what resets `allocator` — and
     * on a reload the `keys` being passed in are the previously remembered ones, which live in
     * `registry` precisely so that reset cannot take them.
     */
    _nya_i18n_remember(locale, keys, count);

    _nya_i18n_commit(locale, strings, fallback, count);

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
     *
     * Switching language changes the handles, and a game that switches back and forth would otherwise
     * grow this arena by two paths and a key table every time. The keys are re-copied along with them,
     * which costs a few hundred short strings on an operation a player performs from a menu.
     *
     * Copied out *before* the reset, because on a reload `keys` is the very array being freed here —
     * resetting first would hand _nya_i18n_read a table of dangling pointers.
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
     *
     * The queued load is redundant with the synchronous read that already happened — it re-reads a few
     * kilobytes of JSON at the end of this frame — and it is what gives the asset a registry entry and
     * a modification time for nya_asset_get to compare against. A failure is ignored: a locale that
     * came out of the blob has no file to watch, and one that failed to register is still loaded.
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
         *
         * FAILED is the obvious dead end. UNLOADED is the surprising one and is what a file that
         * disappears actually produces: the reload pass unloads the asset, the load that should have
         * followed finds no file, and what is left is an entry the asset system will neither stat (it
         * only stats a LOADED asset) nor reload. It looks idle rather than broken, which is exactly why
         * it went unnoticed until a test deleted a locale and watched reloading stop for good.
         *
         * LOADING and a queued unload are deliberately not included: those are in motion on their own,
         * and re-arming them would fight the queues instead of waiting a frame for them to finish.
         */
        b8 stuck = asset == nullptr || asset->status == NYA_ASSET_STATUS_FAILED || asset->status == NYA_ASSET_STATUS_UNLOADED;

        if (!stuck) continue;

        u64 now_ns = nya_app_get()->frame_stats.uptime_ns;
        if (now_ns < system->next_recovery_ns) continue;

        system->next_recovery_ns = now_ns + _NYA_ASSET_STAT_INTERVAL_NS;

        nya_debug("Re-arming the locale asset '%s' after a failed load.", handles[i]);

        (void)nya_asset_unload(handles[i]);
        (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = handles[i] });

        /*
         * The recorded timestamp is cleared so the strings are re-resolved once the file is back.
         *
         * Not doing this would leave the recovered asset reporting the same timestamp the last good
         * load recorded — nothing would look changed, and a locale deleted and restored with different
         * contents would keep showing the old strings.
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
     *
     * The gap is real and is not an edge case: nya_i18n_load reads the locale synchronously and only
     * *queues* the asset, so for the rest of that frame the asset is LOADING with a zero timestamp.
     * Recording that zero meant the first watch after the load saw a difference against nothing and
     * re-resolved a file that had not changed — every launch logged "Reloaded locale", which is
     * exactly the message that should mean a translator just saved something.
     *
     * Falling back to a direct stat closes it: the value recorded at load is the one the asset will
     * report a frame later, so the two agree and the first genuine edit is the first reload.
     */
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) return asset->source_modification_time;

    u64 modified = 0;

    // A locale that is genuinely missing answers zero, which compares equal to itself and so reads as
    // "nothing changed" rather than as a change that can never be resolved.
    if (!nya_filesystem_last_modified(handle, &modified).ok) return 0;

    return modified;
}
#endif // NYA_ASSET_HOT_RELOAD

NYA_Error _nya_i18n_read(NYA_Arena* arena, NYA_ConstCString locale, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out) {
    *out = nullptr;

    /*
     * Through the asset system rather than nya_file_read.
     *
     * That is the whole reason this moved out of base: nya_asset_read looks in the embedded blob first
     * and on disk second, so a shipped build and a development build read the same bytes through the
     * same call. The old base version could only read files, which is why it needed a second public
     * entry point for baked builds to hand it bytes it could not fetch itself.
     */
    u8* data = nullptr;
    u64 size = 0;

    NYA_CString handle = _nya_i18n_handle(arena, locale);

    NYA_TRY(nya_asset_read(arena, handle, &data, &size));

    return _nya_i18n_resolve(arena, data, size, keys, count, out);
}

NYA_Error _nya_i18n_resolve(NYA_Arena* arena, const u8* data, u64 size, const NYA_ConstCString* keys, u32 count, OUT NYA_CString** out) {
    *out = nullptr;

    NYA_Object* root = nullptr;

    // JSONC, so a locale file can carry comments for translators — which is the one kind of file
    // where a note beside a string is genuinely useful.
    NYA_TRY(nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &root));

    NYA_CString* strings = nya_arena_alloc(arena, count * sizeof(NYA_CString));

    for (u32 i = 0; i < count; i++) {
        NYA_Value* value = nya_object_get(root, (NYA_CString)keys[i]);

        // Left null rather than defaulted here. nya_i18n_raw is the one place that decides what a
        // missing string shows, so the fallback chain lives in exactly one function.
        strings[i] = value != nullptr && value->type == NYA_TYPE_STRING ? value->as_string : nullptr;
    }

    *out = strings;

    return NYA_OK;
}

void _nya_i18n_commit(NYA_ConstCString locale, NYA_CString* strings, NYA_CString* fallback, u32 count) {
    NYA_I18nSystem* system = _nya_i18n_system();

    // The previous locale's arena is emptied and the new strings copied into it. Freed as a whole
    // rather than destroyed, so switching language does not churn the allocator — and done only now,
    // after every parse has succeeded, so a failed load leaves the old language intact rather than
    // half replaced.
    nya_arena_free_all(system->allocator);

    system->strings  = nya_arena_alloc(system->allocator, count * sizeof(NYA_CString));
    system->fallback = fallback != nullptr ? nya_arena_alloc(system->allocator, count * sizeof(NYA_CString)) : nullptr;
    system->count    = count;

    for (u32 i = 0; i < count; i++) {
        system->strings[i] = strings[i] != nullptr ? nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, strings[i])) : nullptr;

        if (system->fallback != nullptr) {
            system->fallback[i] = fallback[i] != nullptr ? nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, fallback[i]))
                                                         : nullptr;
        }
    }

    (void)snprintf(system->locale, sizeof(system->locale), "%s", locale);
}
