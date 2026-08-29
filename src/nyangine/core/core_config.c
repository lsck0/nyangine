#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#ifdef NYA_ASSET_HOT_RELOAD
/** The asset system's current modification time for `handle`, or zero when it has none. Same shape as
 *  the identically named helper in core_i18n.c; see the note there for why LOADED wins over a raw stat. */
NYA_INTERNAL u64 _nya_config_modification_time(NYA_CString handle);

/** Puts a config asset that has died back into a state where it can be watched again. Mirrors
 *  _nya_i18n_rearm, one watch at a time instead of for a single fixed pair of handles. */
NYA_INTERNAL void _nya_config_rearm(NYA_ConfigWatch* watch);
#endif // NYA_ASSET_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_config_init(void) {
    NYA_App* app = nya_app_get();

    app->config_system = (NYA_ConfigSystem){
        .registry = nya_arena_create(.name = "config_system_registry"),
    };

#ifdef NYA_ASSET_HOT_RELOAD
    /*
     * After the asset system's own frame-ended hooks, for the same reason core_i18n.c's watch is:
     * by the time this runs, a reload queued earlier this frame has already landed and the timestamp
     * it compares against is the settled one.
     */
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_config_watch_tick),
    });
#endif // NYA_ASSET_HOT_RELOAD

    nya_log_info("Config system initialized.");
}

void nya_system_config_deinit(void) {
    NYA_ConfigSystem* system = &nya_app_get()->config_system;

    if (system->registry != nullptr) nya_arena_destroy(system->registry);

    *system = (NYA_ConfigSystem){ 0 };

    nya_log_info("Config system deinitialized.");
}

NYA_Error nya_config_load(NYA_ConstCString path, const NYA_TypeReflection* type, void* instance) {
    nya_assert(path != nullptr);
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);

    NYA_Arena* scratch = nya_arena_create(.name = "config_load_scratch");
    defer      nya_arena_destroy(scratch);

    u8* data = nullptr;
    u64 size = 0;
    NYA_TRY(nya_asset_read(scratch, (NYA_CString)path, &data, &size));

    NYA_Object* object = nullptr;

    // NO_CHECKSUM: unlike a save file, a config file is meant to be hand edited while the game runs,
    // and a mismatch from an in-progress edit is expected rather than evidence of corruption.
    NYA_TRY(nya_deserialize(scratch, data, size, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &object));

    // `instance` is only reached once the file has fully parsed, so a missing file or a syntax error
    // above leaves it exactly as it was. nya_reflect_from_object cannot itself fail on a config
    // struct with no @on_apply, but returning its result keeps that true for one that grows one.
    return nya_reflect_from_object(type, instance, object);
}

NYA_Error nya_config_watch(NYA_ConstCString path, const NYA_TypeReflection* type, void* instance) {
    nya_assert(path != nullptr);
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);

    NYA_TRY(nya_config_load(path, type, instance));

#ifdef NYA_ASSET_HOT_RELOAD
    NYA_ConfigSystem* system = &nya_app_get()->config_system;

    // Guarded so bringing the app up and down within one process does not add a copy of itself.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("config_watches", NYA_CONFIG_WATCH_MAX, &system->watch_count);
        ceiling_registered = true;
    }

    if (system->watch_count >= NYA_CONFIG_WATCH_MAX) {
        // Refused rather than grown. See NYA_CONFIG_WATCH_MAX.
        nya_log_warn("Config watch table is full at " FMTu32 "; '%s' loaded once but will not be hot reloaded.", (u32)NYA_CONFIG_WATCH_MAX,
                     path);
        return NYA_OK;
    }

    // Copied, not borrowed: `path` may live in a game DLL's own .rodata, which a code reload can
    // unmap. See NYA_ConfigSystem.registry.
    NYA_CString handle = nya_string_to_cstring(system->registry, nya_string_from(system->registry, path));

    /*
     * Registered as a text asset so the file is watched from here on, mirroring _nya_i18n_remember.
     *
     * The load is redundant with the synchronous read nya_config_load just did — it re-reads the same
     * few bytes at the end of this frame — and it is what gives the asset a registry entry and a
     * modification time for nya_asset_get to compare against.
     */
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = handle });

    system->watches[system->watch_count] = (NYA_ConfigWatch){
        .handle            = handle,
        .type              = type,
        .instance          = instance,
        .modification_time = _nya_config_modification_time(handle),
    };
    system->watch_count++;
#endif // NYA_ASSET_HOT_RELOAD

    return NYA_OK;
}

#ifdef NYA_ASSET_HOT_RELOAD
void _nya_config_watch_tick(NYA_Event* event) {
    nya_unused(event);

    NYA_ConfigSystem* system = &nya_app_get()->config_system;

    for (u32 i = 0; i < system->watch_count; i++) {
        NYA_ConfigWatch* watch = &system->watches[i];

        // The point of this call, not the comparison below it: reload detection lives inside
        // nya_asset_get, which stats the file at most once per stat interval and queues the asset
        // when the timestamp moved. See the identical note on _nya_i18n_watch in core_i18n.c.
        (void)nya_asset_get(watch->handle);

        // Before the comparison, because a dead asset reports no timestamp at all and would
        // otherwise look like a file that simply had not changed.
        _nya_config_rearm(watch);

        u64 now = _nya_config_modification_time(watch->handle);
        if (now == watch->modification_time) continue;

        NYA_Error reloaded = nya_config_load(watch->handle, watch->type, watch->instance);

        // Not logged: a file caught mid-write fails to parse, nothing changes, and the unmoved
        // modification_time makes the next tick try again. See core_i18n.c's _nya_i18n_watch.
        if (!reloaded.ok) continue;

        watch->modification_time = now;

        nya_log_info("Reloaded config '%s'.", watch->handle);
    }
}
#endif // NYA_ASSET_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#ifdef NYA_ASSET_HOT_RELOAD
u64 _nya_config_modification_time(NYA_CString handle) {
    if (handle == nullptr) return 0;

    NYA_Asset* asset = nya_asset_get(handle);

    // Out of the blob: part of the executable, so there is no file and nothing that could differ.
    if (asset != nullptr && asset->from_blob) return 0;

    // The asset's own timestamp once it has one, and the file's until then. See the identical note on
    // _nya_i18n_modification_time in core_i18n.c for why the fallback stat exists.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) return asset->source_modification_time;

    u64 modified = 0;

    // A config that is genuinely missing answers zero, which compares equal to itself and so reads
    // as "nothing changed" rather than as a change that can never be resolved.
    if (!nya_filesystem_last_modified(handle, &modified).ok) return 0;

    return modified;
}

void _nya_config_rearm(NYA_ConfigWatch* watch) {
    NYA_Asset* asset = nya_asset_get(watch->handle);

    // Both terminal states, not just FAILED. See _nya_i18n_rearm in core_i18n.c: UNLOADED is what a
    // file briefly missing during an editor's save-and-rename actually produces, and it recovers no
    // more on its own than FAILED does.
    b8 stuck = asset == nullptr || asset->status == NYA_ASSET_STATUS_FAILED || asset->status == NYA_ASSET_STATUS_UNLOADED;
    if (!stuck) return;

    u64 now_ns = nya_app_get()->frame_stats.uptime_ns;
    if (now_ns < watch->next_recovery_ns) return;

    watch->next_recovery_ns = now_ns + _NYA_ASSET_STAT_INTERVAL_NS;

    nya_log_debug("Re-arming the config asset '%s' after a failed load.", watch->handle);

    (void)nya_asset_unload(watch->handle);
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = watch->handle });

    // Cleared so the file is re-resolved once it is back, rather than comparing against the last good
    // load's timestamp and finding nothing changed.
    watch->modification_time = 0;
}
#endif // NYA_ASSET_HOT_RELOAD
