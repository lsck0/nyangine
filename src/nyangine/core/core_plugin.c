#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One slot of the host. The public NYA_Plugin is the first member, so a NYA_Plugin's `_state` can point
 * at its own slot and every private field is one dereference away with no arithmetic.
 * */
typedef struct {
    NYA_Plugin plugin;

    /** Whether this slot holds a loaded plugin. Slots never move, because a system entry names one by index. */
    b8 used;

    /** The registry entry's name, `<plugin>:hooks`. Held here because the registry keeps the pointer. */
    char system_name[NYA_PLUGIN_QUALIFIED_MAX];

    /** Everything the plugin's own code lives in. Freed whole at unload. */
    NYA_Arena* arena;

#ifdef NYA_PLUGIN_LUA
    NYA_LuaVM* vm;
#endif
} _NYA_PluginSlot;

typedef struct {
    _NYA_PluginSlot slots[NYA_PLUGIN_MAX];

    /** How many slots are in use, for the ceiling and for nya_plugin_count. */
    u32 count;

    /** Whose code is running, or null. See nya_plugin_current. */
    NYA_ConstCString current;
} _NYA_PluginHost;

/* No init: a zeroed host is already a valid empty one. */
NYA_INTERNAL _NYA_PluginHost _nya_plugin_host = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registers the ceiling, once per process however often the host is emptied and refilled. */
NYA_INTERNAL void _nya_plugin_ceiling_register(void);

/** The slot holding `name`, or null. */
NYA_INTERNAL _NYA_PluginSlot* _nya_plugin_slot_find(NYA_ConstCString name) __attr_no_discard;

/** The first free slot, or null when NYA_PLUGIN_MAX are in use. */
NYA_INTERNAL _NYA_PluginSlot* _nya_plugin_slot_free(void) __attr_no_discard;

/** The last path component of `path`, which for a plugin directory is its name. */
NYA_INTERNAL NYA_ConstCString _nya_plugin_basename(NYA_ConstCString path) __attr_no_discard;

/** Byte order, shorter first on a shared prefix. What puts `src/` in the order an author can predict. */
NYA_INTERNAL s32 _nya_plugin_name_compare(const NYA_String* a, const NYA_String* b) __attr_no_discard;

/**
 * Compares two `major.minor.patch` strings. Negative, zero or positive like every other comparator.
 * A missing component is zero, so "1.2" equals "1.2.0", and anything unparseable compares as zero.
 * */
NYA_INTERNAL s32 _nya_plugin_version_compare(NYA_ConstCString a, NYA_ConstCString b) __attr_no_discard;

/** What nya_reflect_check reports a bad manifest key through. `user_data` is the file path. */
NYA_INTERNAL void _nya_plugin_manifest_problem(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

/** Logs every permission in `missing` and returns how many there were. */
NYA_INTERNAL u32 _nya_plugin_permissions_report(NYA_ConstCString name, NYA_PluginPermission missing);

/** Everything that must be true before a VM is created: name, version, permissions, dependencies, conflicts. */
NYA_INTERNAL NYA_Error _nya_plugin_admit(NYA_ConstCString directory, const NYA_PluginManifest* manifest) __attr_no_discard;

/** Runs `src/` (every `.lua` in it) in name order, then `main.lua`. */
NYA_INTERNAL NYA_Error _nya_plugin_run_scripts(_NYA_PluginSlot* slot) __attr_no_discard;

/** Adds the slot's entry to the system registry, and removes it again. */
NYA_INTERNAL void _nya_plugin_system_register(_NYA_PluginSlot* slot);
NYA_INTERNAL void _nya_plugin_system_unregister(_NYA_PluginSlot* slot);

/** Frees everything a half-built slot took, and marks it free. Safe on a slot that got nowhere. */
NYA_INTERNAL void _nya_plugin_slot_release(_NYA_PluginSlot* slot);

/** One phase of one plugin, which is what the trampolines below call. */
NYA_INTERNAL void _nya_plugin_run_hook(u32 index, NYA_ConstCString hook, f32 delta_time_s);

/* ── the Lua side, and its absence ─────────────────────────────────────────────────────────────── */

/** Brings up the slot's VM with only the bindings its permissions allow. */
NYA_INTERNAL NYA_Error _nya_plugin_vm_create(_NYA_PluginSlot* slot) __attr_no_discard;

/** Closes it. Harmless on a slot that never had one. */
NYA_INTERNAL void _nya_plugin_vm_destroy(_NYA_PluginSlot* slot);

/** Compiles and runs one file into the slot's VM, reporting it as `chunk`. */
NYA_INTERNAL NYA_Error _nya_plugin_vm_run_file(_NYA_PluginSlot* slot, NYA_ConstCString path, NYA_ConstCString chunk) __attr_no_discard;

/** Whether the plugin defines a global function of this name. */
NYA_INTERNAL b8 _nya_plugin_vm_has(const _NYA_PluginSlot* slot, NYA_ConstCString function) __attr_no_discard;

/** Calls one. The caller owns the current-plugin bookkeeping and the error report. */
NYA_INTERNAL NYA_Error _nya_plugin_vm_call(
    _NYA_PluginSlot* slot,
    NYA_ConstCString function,
    NYA_Arena*       arena,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) __attr_no_discard;

/** What the plugin's VM is holding right now. */
NYA_INTERNAL u64 _nya_plugin_vm_memory_bytes(const _NYA_PluginSlot* slot) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * THE SLOT TRAMPOLINES
 * ─────────────────────────────────────────────────────────
 *
 * A system registry callback is `void(f32)` and carries no user data, so "which plugin is this" has to
 * be in the function's identity. One trio per slot, written once by a macro.
 *
 * The alternative was one `plugins` system iterating every loaded plugin, which costs one registry
 * entry instead of eight but gives up exactly what this is for: with one entry the registry's per-owner
 * accounting books every plugin's time against the same owner, and "which plugin is eating the frame"
 * goes back to being unanswerable.
 */

#define _NYA_PLUGIN_SLOT_FUNCTIONS(index)                                                                                                            \
    NYA_INTERNAL void _nya_plugin_frame_##index(f32 delta_time_s) {                                                                                  \
        _nya_plugin_run_hook(index, NYA_PLUGIN_HOOK_FRAME, delta_time_s);                                                                            \
    }                                                                                                                                                \
    NYA_INTERNAL void _nya_plugin_tick_##index(f32 delta_time_s) {                                                                                   \
        _nya_plugin_run_hook(index, NYA_PLUGIN_HOOK_TICK, delta_time_s);                                                                             \
    }                                                                                                                                                \
    NYA_INTERNAL void _nya_plugin_render_##index(f32 delta_time_s) {                                                                                 \
        _nya_plugin_run_hook(index, NYA_PLUGIN_HOOK_RENDER, delta_time_s);                                                                           \
    }                                                                                                                                                \
    NYA_INTERNAL u64 _nya_plugin_memory_##index(void) {                                                                                              \
        return _nya_plugin_vm_memory_bytes(&_nya_plugin_host.slots[index]);                                                                          \
    }

_NYA_PLUGIN_SLOT_FUNCTIONS(0)
_NYA_PLUGIN_SLOT_FUNCTIONS(1)
_NYA_PLUGIN_SLOT_FUNCTIONS(2)
_NYA_PLUGIN_SLOT_FUNCTIONS(3)
_NYA_PLUGIN_SLOT_FUNCTIONS(4)
_NYA_PLUGIN_SLOT_FUNCTIONS(5)
_NYA_PLUGIN_SLOT_FUNCTIONS(6)
_NYA_PLUGIN_SLOT_FUNCTIONS(7)

#define _NYA_PLUGIN_SLOT_ROW(index)                                                                                                                  \
    {                                                                                                                                                \
        .frame = _nya_plugin_frame_##index, .tick = _nya_plugin_tick_##index, .render = _nya_plugin_render_##index,                                   \
        .memory_bytes = _nya_plugin_memory_##index,                                                                                                  \
    }

/** The trampolines by slot. Parallel to `_nya_plugin_host.slots` and asserted to stay that way. */
NYA_INTERNAL const struct {
    NYA_SystemPhaseFn  frame;
    NYA_SystemPhaseFn  tick;
    NYA_SystemPhaseFn  render;
    NYA_SystemMemoryFn memory_bytes;
} _NYA_PLUGIN_SLOT_FNS[] = {
    _NYA_PLUGIN_SLOT_ROW(0), _NYA_PLUGIN_SLOT_ROW(1), _NYA_PLUGIN_SLOT_ROW(2), _NYA_PLUGIN_SLOT_ROW(3),
    _NYA_PLUGIN_SLOT_ROW(4), _NYA_PLUGIN_SLOT_ROW(5), _NYA_PLUGIN_SLOT_ROW(6), _NYA_PLUGIN_SLOT_ROW(7),
};

static_assert(nya_carray_length(_NYA_PLUGIN_SLOT_FNS) == NYA_PLUGIN_MAX,
              "one trampoline row per plugin slot: raise NYA_PLUGIN_MAX and the _NYA_PLUGIN_SLOT_FUNCTIONS list with it");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PERMISSIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_PluginPermission nya_plugin_permissions_granted(void) {
#if NYA_PLUGIN_PERMISSION_PROFILE == NYA_PLUGIN_PERMISSION_PROFILE_LOCKED
    return NYA_PLUGIN_PERMISSION_NONE;
#elif NYA_PLUGIN_PERMISSION_PROFILE == NYA_PLUGIN_PERMISSION_PROFILE_UI
    return NYA_PLUGIN_PERMISSION_UI | NYA_PLUGIN_PERMISSION_INPUT;
#elif NYA_PLUGIN_PERMISSION_PROFILE == NYA_PLUGIN_PERMISSION_PROFILE_GAMEPLAY
    return NYA_PLUGIN_PERMISSION_UI | NYA_PLUGIN_PERMISSION_INPUT | NYA_PLUGIN_PERMISSION_KEYBINDING | NYA_PLUGIN_PERMISSION_ENTITIES |
           NYA_PLUGIN_PERMISSION_AUDIO | NYA_PLUGIN_PERMISSION_ASSETS;
#elif NYA_PLUGIN_PERMISSION_PROFILE == NYA_PLUGIN_PERMISSION_PROFILE_ALL
    return NYA_PLUGIN_PERMISSION_UI | NYA_PLUGIN_PERMISSION_INPUT | NYA_PLUGIN_PERMISSION_KEYBINDING | NYA_PLUGIN_PERMISSION_ENTITIES |
           NYA_PLUGIN_PERMISSION_AUDIO | NYA_PLUGIN_PERMISSION_ASSETS | NYA_PLUGIN_PERMISSION_FILESYSTEM | NYA_PLUGIN_PERMISSION_NETWORK;
#else
#error "NYA_PLUGIN_PERMISSION_PROFILE is not one of the four profiles in core_plugin.h"
#endif
}

NYA_ConstCString nya_plugin_permission_name(NYA_PluginPermission permission) {
    // The reflection table is the same one a manifest is parsed through, so a name this returns is a
    // name a manifest may write and the two cannot drift apart.
    return nya_reflect_variant_name(nya_reflect_of(NYA_PluginPermission), (s64)permission);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NAMESPACING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_plugin_qualify(NYA_ConstCString plugin, NYA_ConstCString name, OUT char* out, u64 capacity) {
    nya_assert(name != nullptr, "a name to qualify");
    nya_assert(out != nullptr && capacity > 1, "somewhere to write the qualified name");

    if (plugin == nullptr) {
        (void)snprintf(out, capacity, "%s", name);
        return out;
    }

    (void)snprintf(out, capacity, "%s:%s", plugin, name);

    return out;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE MANIFEST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_plugin_manifest_load(NYA_ConstCString directory, OUT NYA_PluginManifest* out_manifest) {
    if (directory == nullptr || out_manifest == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_manifest_load needs a directory and somewhere to write");
    }

    *out_manifest = (NYA_PluginManifest){ 0 };

    char path[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", directory, NYA_PLUGIN_MANIFEST_FILE);

    if (!nya_filesystem_is_file(path)) return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no %s", directory, NYA_PLUGIN_MANIFEST_FILE);

    NYA_Arena* arena = nya_arena_create(.name = "plugin_manifest");
    defer      nya_arena_destroy(arena);

    NYA_String* contents = nya_string_create(arena);
    NYA_TRY(nya_file_read(path, contents));

    NYA_Object* document = nullptr;

    /*
     * The format is named, not detected. A manifest is `manifest.nya` and nothing else, so a plugin
     * cannot decide what its own manifest is parsed as, and a file that opens with a comment is not
     * mistaken for JSONC — which is what nya_serde_detect_format does with one, and why core_config.c
     * names the format too.
     *
     * NO_CHECKSUM: a manifest is written by a person in a text editor, and the native format's checksum
     * is over the contents, so an honest edit would otherwise refuse the file.
     */
    NYA_TRY(nya_deserialize(arena, contents->items, contents->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &document));

    /*
     * Checked before it is applied, and refused rather than partially applied. nya_reflect_from_object
     * skips what it cannot write, which is right for a save file growing a field and wrong here: a
     * misspelled permission would be skipped in silence and read as a permission the plugin never asked
     * for, which is the one mistake this boundary exists to catch.
     */
    u32 problems = nya_reflect_check(nya_reflect_of(NYA_PluginManifest), document, _nya_plugin_manifest_problem, (void*)path);

    if (problems > 0) {
        return nya_error(NYA_ERROR_PARSE, "%s has " FMTu32 " problem%s; see the log", path, problems, problems == 1 ? "" : "s");
    }

    NYA_TRY(nya_reflect_from_object(nya_reflect_of(NYA_PluginManifest), out_manifest, document));

    if (out_manifest->name[0] == '\0') return nya_error(NYA_ERROR_PARSE, "%s names no plugin", path);
    if (out_manifest->version[0] == '\0') return nya_error(NYA_ERROR_PARSE, "%s gives '%s' no version", path, out_manifest->name);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LOADING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_plugin_load(NYA_ConstCString directory) {
    if (directory == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_load needs a directory");

    _nya_plugin_ceiling_register();

    NYA_PluginManifest manifest = { 0 };
    NYA_TRY(nya_plugin_manifest_load(directory, &manifest));
    NYA_TRY(_nya_plugin_admit(directory, &manifest));

    _NYA_PluginSlot* slot = _nya_plugin_slot_free();

    if (slot == nullptr) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' cannot load: " FMTu32 " plugins are already loaded", manifest.name,
                         (u32)NYA_PLUGIN_MAX);
    }

    *slot = (_NYA_PluginSlot){
        .used   = true,
        .plugin = { .manifest = manifest, .permissions = manifest.permissions, .enabled = true },
    };

    (void)snprintf(slot->plugin.directory, sizeof(slot->plugin.directory), "%s", directory);
    (void)nya_plugin_qualify(slot->plugin.manifest.name, "hooks", slot->system_name, sizeof(slot->system_name));

    // Named after the plugin, so the arena registry and the memory report say which plugin is holding
    // what. The name outlives the arena: it points into this slot, which is static storage.
    slot->arena = nya_arena_create(.name = slot->plugin.manifest.name);

    _nya_plugin_host.count++;

    NYA_Error brought_up = _nya_plugin_vm_create(slot);
    if (!brought_up.ok) {
        _nya_plugin_slot_release(slot);
        return brought_up;
    }

    NYA_Error ran = _nya_plugin_run_scripts(slot);
    if (!ran.ok) {
        _nya_plugin_slot_release(slot);
        return ran;
    }

    /*
     * Asked once, here, rather than per frame: nya_lua_has_function is a global lookup and a plugin
     * cannot grow a hook after it has been loaded, since the chunk that could define one has run.
     */
    slot->plugin.has_frame  = _nya_plugin_vm_has(slot, NYA_PLUGIN_HOOK_FRAME);
    slot->plugin.has_tick   = _nya_plugin_vm_has(slot, NYA_PLUGIN_HOOK_TICK);
    slot->plugin.has_render = _nya_plugin_vm_has(slot, NYA_PLUGIN_HOOK_RENDER);

    _nya_plugin_system_register(slot);

    // After the registry entry, so anything on_load registers is already ordered against this plugin's
    // own hooks rather than landing in front of them.
    if (_nya_plugin_vm_has(slot, NYA_PLUGIN_HOOK_LOAD)) {
        NYA_Error loaded = nya_plugin_call(slot->plugin.manifest.name, NYA_PLUGIN_HOOK_LOAD, nullptr, nullptr, 0, nullptr);

        if (!loaded.ok) {
            _nya_plugin_system_unregister(slot);
            _nya_plugin_slot_release(slot);

            return loaded;
        }
    }

    nya_log_info("Loaded plugin '%s' %s by %s (%s).", slot->plugin.manifest.name, slot->plugin.manifest.version,
                 slot->plugin.manifest.author[0] != '\0' ? slot->plugin.manifest.author : "an unnamed author",
                 slot->plugin.manifest.license[0] != '\0' ? slot->plugin.manifest.license : "no licence stated");

    return NYA_OK;
}

NYA_Error nya_plugin_load_all(void) {
    if (!nya_filesystem_is_directory(NYA_PLUGIN_DIRECTORY)) {
        // Not an error: a game with no plugins directory is the normal case, and a missing one must not
        // be the difference between starting and not.
        nya_log_debug("No %s directory; no plugins to load.", NYA_PLUGIN_DIRECTORY);
        return NYA_OK;
    }

    NYA_Arena* arena = nya_arena_create(.name = "plugin_discovery");
    defer      nya_arena_destroy(arena);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_TRY(nya_filesystem_list(arena, NYA_PLUGIN_DIRECTORY, &entries));

    /*
     * Every candidate first, then loaded in dependency order. A directory with no manifest is not a
     * plugin at all and is skipped without a word: `plugins/.git` and an editor's scratch folder are
     * both normal.
     */
    char candidates[NYA_PLUGIN_CANDIDATE_MAX][NYA_PLUGIN_PATH_MAX] = { 0 };
    b8   settled[NYA_PLUGIN_CANDIDATE_MAX]                         = { 0 };
    u32  candidate_count                                           = 0;

    nya_array_foreach (entries, entry) {
        if (entry->type != NYA_FILE_TYPE_DIRECTORY) continue;

        char directory[NYA_PLUGIN_PATH_MAX];
        (void)snprintf(directory, sizeof(directory), "%s/%s", NYA_PLUGIN_DIRECTORY, nya_string_to_cstring(arena, entry->name));

        char manifest[NYA_PLUGIN_PATH_MAX];
        (void)snprintf(manifest, sizeof(manifest), "%s/%s", directory, NYA_PLUGIN_MANIFEST_FILE);

        if (!nya_filesystem_is_file(manifest)) continue;

        if (candidate_count >= NYA_PLUGIN_CANDIDATE_MAX) {
            nya_log_error("More than " FMTu32 " plugin directories in %s; '%s' and anything after it is ignored.", (u32)NYA_PLUGIN_CANDIDATE_MAX,
                          NYA_PLUGIN_DIRECTORY, directory);
            break;
        }

        (void)snprintf(candidates[candidate_count], sizeof(candidates[0]), "%s", directory);
        candidate_count++;
    }

    /*
     * Repeated passes rather than a topological sort: with at most eight candidates the worst case is
     * eight passes over eight entries, and a sort would need the dependency graph built, cycle checked
     * and reported, all to save sixty comparisons.
     */
    for (u32 pass = 0; pass < candidate_count; pass++) {
        u32 progressed = 0;

        for (u32 i = 0; i < candidate_count; i++) {
            if (settled[i]) continue;

            NYA_PluginManifest manifest = { 0 };
            NYA_Error          read     = nya_plugin_manifest_load(candidates[i], &manifest);

            if (!read.ok) {
                nya_log_error("Plugin '%s' was refused: %s", candidates[i], (NYA_ConstCString)read.message);
                settled[i] = true;
                continue;
            }

            b8 waiting = false;
            for (u32 d = 0; d < NYA_PLUGIN_DEPENDENCY_MAX; d++) {
                if (manifest.dependencies[d].name[0] == '\0') continue;
                if (nya_plugin_find(manifest.dependencies[d].name) != nullptr) continue;

                waiting = true;
                break;
            }

            if (waiting) continue;

            NYA_Error result = nya_plugin_load(candidates[i]);

            // Reported and stepped over. One plugin refusing to load is not a reason for the game not to
            // start, and the sentence below is the whole bug report its author needs.
            if (!result.ok) nya_log_error("Plugin '%s' was refused: %s", manifest.name, (NYA_ConstCString)result.message);

            settled[i] = true;
            progressed++;
        }

        if (progressed == 0) break;
    }

    // Whatever is left is waiting on something that never arrived, which is the one case the loop above
    // cannot report from the inside.
    for (u32 i = 0; i < candidate_count; i++) {
        if (settled[i]) continue;

        NYA_PluginManifest manifest = { 0 };
        if (!nya_plugin_manifest_load(candidates[i], &manifest).ok) continue;

        for (u32 d = 0; d < NYA_PLUGIN_DEPENDENCY_MAX; d++) {
            if (manifest.dependencies[d].name[0] == '\0') continue;
            if (nya_plugin_find(manifest.dependencies[d].name) != nullptr) continue;

            nya_log_error("Plugin '%s' needs '%s', which is not installed.", manifest.name, manifest.dependencies[d].name);
        }
    }

    if (_nya_plugin_host.count > 0) {
        nya_log_info("Loaded " FMTu32 " of " FMTu32 " plugins from %s.", _nya_plugin_host.count, candidate_count, NYA_PLUGIN_DIRECTORY);
    }

    return NYA_OK;
}

void nya_plugin_unload(NYA_ConstCString name) {
    if (name == nullptr) return;

    _NYA_PluginSlot* slot = _nya_plugin_slot_find(name);
    if (slot == nullptr) return;

    // Before anything is torn down, so the hook still has the VM and the bindings it was written
    // against. Its failure is reported and changes nothing: the plugin is going either way.
    if (_nya_plugin_vm_has(slot, NYA_PLUGIN_HOOK_UNLOAD)) {
        (void)nya_plugin_call(name, NYA_PLUGIN_HOOK_UNLOAD, nullptr, nullptr, 0, nullptr);
    }

    _nya_plugin_system_unregister(slot);

    nya_log_info("Unloaded plugin '%s'.", slot->plugin.manifest.name);

    _nya_plugin_slot_release(slot);
}

void nya_plugin_unload_all(void) {
    // Reverse load order, so a plugin others depend on is the last to go.
    for (u32 i = NYA_PLUGIN_MAX; i > 0; i--) {
        _NYA_PluginSlot* slot = &_nya_plugin_host.slots[i - 1];
        if (!slot->used) continue;

        nya_plugin_unload(slot->plugin.manifest.name);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENABLING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_plugin_enable(NYA_ConstCString name) {
    _NYA_PluginSlot* slot = name != nullptr ? _nya_plugin_slot_find(name) : nullptr;
    if (slot == nullptr) return;

    // The error count goes with it: enabling a plugin that was switched off for failing is a decision to
    // give it another run, and keeping the old count would switch it off again on its next mistake.
    slot->plugin.error_count = 0;
    slot->plugin.enabled     = true;

    nya_system_enable(slot->system_name);
}

void nya_plugin_disable(NYA_ConstCString name) {
    _NYA_PluginSlot* slot = name != nullptr ? _nya_plugin_slot_find(name) : nullptr;
    if (slot == nullptr) return;

    slot->plugin.enabled = false;

    nya_system_disable(slot->system_name);
}

b8 nya_plugin_is_enabled(NYA_ConstCString name) {
    const _NYA_PluginSlot* slot = name != nullptr ? _nya_plugin_slot_find(name) : nullptr;

    return slot != nullptr && slot->plugin.enabled;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 nya_plugin_count(void) {
    return _nya_plugin_host.count;
}

const NYA_Plugin* nya_plugin_at(u32 index) {
    nya_assert(index < _nya_plugin_host.count, "plugin index " FMTu32 " is out of range (" FMTu32 " loaded)", index, _nya_plugin_host.count);

    // Slots never move, so the index is over the used ones rather than over the array.
    u32 seen = 0;

    for (u32 i = 0; i < NYA_PLUGIN_MAX; i++) {
        if (!_nya_plugin_host.slots[i].used) continue;
        if (seen == index) return &_nya_plugin_host.slots[i].plugin;

        seen++;
    }

    nya_unreachable();
}

const NYA_Plugin* nya_plugin_find(NYA_ConstCString name) {
    const _NYA_PluginSlot* slot = name != nullptr ? _nya_plugin_slot_find(name) : nullptr;

    return slot != nullptr ? &slot->plugin : nullptr;
}

NYA_ConstCString nya_plugin_current(void) {
    return _nya_plugin_host.current;
}

NYA_SystemOwnerStats nya_plugin_stats(NYA_ConstCString name) {
    const _NYA_PluginSlot* slot = name != nullptr ? _nya_plugin_slot_find(name) : nullptr;
    if (slot == nullptr) return (NYA_SystemOwnerStats){ 0 };

    // Read out of the registry rather than counted here: the registry is where the time is measured and
    // the memory polled, and a second copy of those numbers would be a second copy to keep honest.
    for (u32 i = 0; i < nya_system_owner_count(); i++) {
        NYA_SystemOwnerStats stats = nya_system_owner_stats_at(i);
        if (!nya_string_equals(stats.name, slot->plugin.manifest.name)) continue;

        return stats;
    }

    return (NYA_SystemOwnerStats){ .name = slot->plugin.manifest.name, .kind = NYA_SYSTEM_OWNER_PLUGIN };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CALLING AND FAILING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_plugin_call(
    NYA_ConstCString name,
    NYA_ConstCString function,
    NYA_Arena*       arena,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) {
    if (name == nullptr || function == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_plugin_call needs a plugin and a function");

    _NYA_PluginSlot* slot = _nya_plugin_slot_find(name);
    if (slot == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no plugin called '%s' is loaded", name);

    // Not a failure and not counted against the plugin: every hook is optional, and asking for one a
    // plugin did not write is how the host finds that out.
    if (!_nya_plugin_vm_has(slot, function)) return nya_error(NYA_ERROR_NOT_FOUND, "plugin '%s' defines no '%s'", name, function);

    /*
     * Saved and restored rather than set and cleared, so a host binding that calls back into another
     * plugin leaves the right answer behind it. A script cannot nest these itself: one VM per plugin
     * means a plugin has no way to name another one's functions.
     */
    NYA_ConstCString previous = _nya_plugin_host.current;
    _nya_plugin_host.current  = slot->plugin.manifest.name;

    NYA_Error result = _nya_plugin_vm_call(slot, function, arena, arguments, argument_count, out_result);

    _nya_plugin_host.current = previous;

    if (!result.ok) {
        char context[NYA_PLUGIN_QUALIFIED_MAX];
        (void)snprintf(context, sizeof(context), "in %s", function);

        nya_plugin_error(name, context, (NYA_ConstCString)result.message);
    }

    return result;
}

void nya_plugin_error(NYA_ConstCString name, NYA_ConstCString what, NYA_ConstCString detail) {
    _NYA_PluginSlot* slot = name != nullptr ? _nya_plugin_slot_find(name) : nullptr;
    if (slot == nullptr) return;

    const NYA_PluginManifest* manifest = &slot->plugin.manifest;

    /*
     * "[plugin]" first and the engine's own voice nowhere in the line. A player reading a log, or an
     * author reading a bug report, has to be able to tell "the game broke" from "this plugin broke"
     * without knowing what any of the names mean.
     */
    nya_log_error("[plugin] %s %s by %s: %s%s%s", manifest->name, manifest->version,
                  manifest->author[0] != '\0' ? manifest->author : "an unnamed author", what != nullptr ? what : "failed",
                  detail != nullptr ? ": " : "", detail != nullptr ? detail : "");

    if (manifest->repository[0] != '\0') {
        nya_log_error("[plugin] report this to the author of '%s': %s", manifest->name, manifest->repository);
    }

    slot->plugin.error_count++;

    if (slot->plugin.error_count < NYA_PLUGIN_ERROR_MAX || !slot->plugin.enabled) return;

    // A hook that throws throws every frame. Switched off rather than left to fill the log, and said
    // once so the reason is in the same place as the failures that caused it.
    nya_log_error("[plugin] '%s' has failed " FMTu32 " times and is switched off. Re-enable it once its author has fixed it.", manifest->name,
                  (u32)NYA_PLUGIN_ERROR_MAX);

    nya_plugin_disable(manifest->name);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_plugin_ceiling_register(void) {
    // From the first load rather than at an init of its own: this host has none, a zeroed array
    // already being a valid empty one. Guarded so a test that resets and refills it many times over
    // one process does not add a copy of itself to the ceiling registry each time.
    static b8 registered = false;
    if (registered) return;

    nya_ceiling_register("plugins", NYA_PLUGIN_MAX, &_nya_plugin_host.count);

    registered = true;
}

_NYA_PluginSlot* _nya_plugin_slot_find(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    for (u32 i = 0; i < NYA_PLUGIN_MAX; i++) {
        if (!_nya_plugin_host.slots[i].used) continue;
        if (!nya_string_equals(_nya_plugin_host.slots[i].plugin.manifest.name, name)) continue;

        return &_nya_plugin_host.slots[i];
    }

    return nullptr;
}

_NYA_PluginSlot* _nya_plugin_slot_free(void) {
    for (u32 i = 0; i < NYA_PLUGIN_MAX; i++) {
        if (_nya_plugin_host.slots[i].used) continue;

        return &_nya_plugin_host.slots[i];
    }

    return nullptr;
}

NYA_ConstCString _nya_plugin_basename(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    NYA_ConstCString last = path;

    for (u64 i = 0; path[i] != '\0'; i++) {
        // Both separators, because a Windows caller may well hand this a backslash path and the answer
        // must not depend on which one they used.
        if (path[i] == '/' || path[i] == '\\') last = &path[i + 1];
    }

    return last;
}

s32 _nya_plugin_name_compare(const NYA_String* a, const NYA_String* b) {
    nya_assert(a != nullptr && b != nullptr);

    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);

    if (difference != 0) return difference < 0 ? -1 : 1;
    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

s32 _nya_plugin_version_compare(NYA_ConstCString a, NYA_ConstCString b) {
    nya_assert(a != nullptr && b != nullptr);

    NYA_ConstCString left  = a;
    NYA_ConstCString right = b;

    // Three components, because that is what a semantic version is. A fourth is ignored rather than
    // refused: a version is a plugin author's text and comparing it is not worth failing a load over.
    for (u32 component = 0; component < 3; component++) {
        u64 left_value  = 0;
        u64 right_value = 0;

        while (isdigit((unsigned char)*left)) {
            left_value = (left_value * 10) + (u64)(*left - '0');
            left++;
        }
        while (isdigit((unsigned char)*right)) {
            right_value = (right_value * 10) + (u64)(*right - '0');
            right++;
        }

        if (left_value != right_value) return left_value < right_value ? -1 : 1;

        if (*left == '.') left++;
        if (*right == '.') right++;
    }

    return 0;
}

void _nya_plugin_manifest_problem(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    nya_log_error("%s: '%s' is %s, expected %s.", (NYA_ConstCString)user_data, path, found, expected);
}

u32 _nya_plugin_permissions_report(NYA_ConstCString name, NYA_PluginPermission missing) {
    const NYA_TypeReflection* type  = nya_reflect_of(NYA_PluginPermission);
    u32                       count = 0;

    for (u32 i = 0; i < type->variant_count; i++) {
        const NYA_ReflectVariant* variant = &type->variants[i];

        if (variant->value == 0) continue;
        if (((s64)missing & variant->value) != variant->value) continue;

        // One line each rather than one line listing them: an NYA_Error message is 192 characters and
        // four permission names do not fit in it, and the list is what the author has to act on.
        nya_log_error("[plugin] '%s' asks for %s, which this build does not grant.", name, variant->name);
        count++;
    }

    return count;
}

NYA_Error _nya_plugin_admit(NYA_ConstCString directory, const NYA_PluginManifest* manifest) {
    nya_assert(directory != nullptr && manifest != nullptr);

    /*
     * The directory is the identity. A manifest that names itself something else could otherwise claim
     * a name another plugin answers to, and everything below — the qualified registrations, the owner
     * accounting, the error attribution — keys off that name.
     */
    NYA_ConstCString folder = _nya_plugin_basename(directory);

    if (!nya_string_equals(folder, manifest->name)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' calls itself '%s'; a plugin's name is its own directory", directory, manifest->name);
    }

    if (nya_plugin_find(manifest->name) != nullptr) {
        return nya_error(NYA_ERROR_ALREADY_EXISTS, "a plugin called '%s' is already loaded", manifest->name);
    }

    /*
     * The permission check, and the only place it happens. Everything after this point hands the plugin
     * bindings, and what it may have has to be settled before any of them exist.
     */
    NYA_PluginPermission granted = nya_plugin_permissions_granted();
    NYA_PluginPermission missing = (NYA_PluginPermission)((u64)manifest->permissions & ~(u64)granted);

    if (missing != NYA_PLUGIN_PERMISSION_NONE) {
        u32 count = _nya_plugin_permissions_report(manifest->name, missing);

        return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s' wants " FMTu32 " permission%s this build does not grant; see the log",
                         manifest->name, count, count == 1 ? "" : "s");
    }

    if (manifest->engine_version[0] != '\0') {
        NYA_ConstCString engine = nya_build_info().version;

        // "unknown" is what a build outside the build system reports, and comparing against it would
        // refuse every plugin in an editor's index build.
        if (!nya_string_equals(engine, "unknown") && _nya_plugin_version_compare(engine, manifest->engine_version) < 0) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' needs engine %s, this is %s", manifest->name, manifest->engine_version, engine);
        }
    }

    for (u32 i = 0; i < NYA_PLUGIN_DEPENDENCY_MAX; i++) {
        const NYA_PluginDependency* dependency = &manifest->dependencies[i];
        if (dependency->name[0] == '\0') continue;

        const NYA_Plugin* other = nya_plugin_find(dependency->name);
        if (other == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "'%s' needs '%s', which is not loaded", manifest->name, dependency->name);

        if (dependency->version[0] == '\0') continue;
        if (_nya_plugin_version_compare(other->manifest.version, dependency->version) >= 0) continue;

        return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' needs '%s' %s or newer, and %s is installed", manifest->name, dependency->name,
                         dependency->version, other->manifest.version);
    }

    /*
     * Both directions. A conflict is a statement about a pair, and whichever of the two is loaded first
     * must not decide whether the statement holds.
     */
    for (u32 i = 0; i < NYA_PLUGIN_DEPENDENCY_MAX; i++) {
        if (manifest->conflicts[i].name[0] == '\0') continue;
        if (nya_plugin_find(manifest->conflicts[i].name) == nullptr) continue;

        return nya_error(NYA_ERROR_ALREADY_EXISTS, "'%s' refuses to run beside '%s', which is loaded", manifest->name, manifest->conflicts[i].name);
    }

    for (u32 i = 0; i < _nya_plugin_host.count; i++) {
        const NYA_Plugin* other = nya_plugin_at(i);

        for (u32 c = 0; c < NYA_PLUGIN_DEPENDENCY_MAX; c++) {
            if (other->manifest.conflicts[c].name[0] == '\0') continue;
            if (!nya_string_equals(other->manifest.conflicts[c].name, manifest->name)) continue;

            return nya_error(NYA_ERROR_ALREADY_EXISTS, "'%s' is loaded and refuses to run beside '%s'", other->manifest.name, manifest->name);
        }
    }

    return NYA_OK;
}

NYA_Error _nya_plugin_run_scripts(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr && slot->arena != nullptr);

    char sources[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(sources, sizeof(sources), "%s/%s", slot->plugin.directory, NYA_PLUGIN_SOURCE_DIRECTORY);

    /*
     * `src/` is run before `main.lua`, in name order, and there is no `require`: `package` is one of the
     * libraries a restricted VM refuses, and a module loader is a path resolver, which is a way out of
     * the plugin's own directory. Ordering by name is what an author controls instead.
     */
    if (nya_filesystem_is_directory(sources)) {
        NYA_Arena* arena = nya_arena_create(.name = "plugin_sources");
        defer      nya_arena_destroy(arena);

        NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
        NYA_TRY(nya_filesystem_list(arena, sources, &entries));

        NYA_ArrayᐸNYA_Stringᐳ* names = nya_array_create(arena, NYA_String);

        nya_array_foreach (entries, entry) {
            if (entry->type != NYA_FILE_TYPE_FILE) continue;
            if (!nya_string_ends_with(entry->name, NYA_PLUGIN_SOURCE_EXTENSION)) continue;

            if (names->length >= NYA_PLUGIN_SOURCE_MAX) {
                nya_log_error("[plugin] '%s' has more than " FMTu32 " files under %s/; the rest are not run.", slot->plugin.manifest.name,
                              (u32)NYA_PLUGIN_SOURCE_MAX, NYA_PLUGIN_SOURCE_DIRECTORY);
                break;
            }

            nya_array_push_back(names, *nya_string_clone(arena, entry->name));
        }

        nya_array_sort(names, _nya_plugin_name_compare);

        nya_array_foreach (names, name) {
            NYA_CString file = nya_string_to_cstring(arena, name);

            char path[NYA_PLUGIN_PATH_MAX];
            (void)snprintf(path, sizeof(path), "%s/%s", sources, file);

            char chunk[NYA_PLUGIN_PATH_MAX];
            (void)snprintf(chunk, sizeof(chunk), "%s/%s/%s", slot->plugin.manifest.name, NYA_PLUGIN_SOURCE_DIRECTORY, file);

            NYA_TRY(_nya_plugin_vm_run_file(slot, path, chunk));
        }
    }

    char entry_path[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(entry_path, sizeof(entry_path), "%s/%s", slot->plugin.directory, NYA_PLUGIN_ENTRY_FILE);

    if (!nya_filesystem_is_file(entry_path)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "'%s' has no %s", slot->plugin.manifest.name, NYA_PLUGIN_ENTRY_FILE);
    }

    char entry_chunk[NYA_PLUGIN_PATH_MAX];
    (void)snprintf(entry_chunk, sizeof(entry_chunk), "%s/%s", slot->plugin.manifest.name, NYA_PLUGIN_ENTRY_FILE);

    return _nya_plugin_vm_run_file(slot, entry_path, entry_chunk);
}

void _nya_plugin_system_register(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr && slot->used);

    u32 index = (u32)(slot - _nya_plugin_host.slots);
    nya_assert(index < NYA_PLUGIN_MAX, "a slot outside the host's array");

    /*
     * No `after`: plugins are registered once the engine's and the game's systems are in, and the sort
     * keeps registration order where nothing constrains it, so a plugin runs last in every phase. That
     * is what a plugin wants — draw over the game, tick after it — and it is the only order the host
     * can promise without letting a plugin name an engine system and wedge itself into the middle.
     */
    nya_system_register((NYA_SystemEntry){
        .name         = slot->system_name,
        .frame        = slot->plugin.has_frame ? nya_callback(_NYA_PLUGIN_SLOT_FNS[index].frame) : NYA_CALLBACK_HANDLE_NONE,
        .tick         = slot->plugin.has_tick ? nya_callback(_NYA_PLUGIN_SLOT_FNS[index].tick) : NYA_CALLBACK_HANDLE_NONE,
        .render       = slot->plugin.has_render ? nya_callback(_NYA_PLUGIN_SLOT_FNS[index].render) : NYA_CALLBACK_HANDLE_NONE,
        .memory_bytes = nya_callback(_NYA_PLUGIN_SLOT_FNS[index].memory_bytes),
        .owner        = { .kind = NYA_SYSTEM_OWNER_PLUGIN, .plugin = slot->plugin.manifest.name },
    });
}

void _nya_plugin_system_unregister(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr);

    if (slot->system_name[0] == '\0') return;

    nya_system_unregister(slot->system_name);
}

void _nya_plugin_slot_release(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr);

    if (!slot->used) return;

    _nya_plugin_vm_destroy(slot);

    // After the VM: closing it runs LuaJIT's finalizers, and the wrapper it does that through lives in
    // this arena.
    if (slot->arena != nullptr) nya_arena_destroy(slot->arena);

    *slot = (_NYA_PluginSlot){ 0 };

    nya_assert(_nya_plugin_host.count > 0, "releasing a slot the host has no count for");
    _nya_plugin_host.count--;
}

void _nya_plugin_run_hook(u32 index, NYA_ConstCString hook, f32 delta_time_s) {
    nya_assert(index < NYA_PLUGIN_MAX, "a plugin trampoline for a slot that does not exist");

    _NYA_PluginSlot* slot = &_nya_plugin_host.slots[index];

    // Both checked: the registry's disable stops the call, and this stops one that a barrier has not
    // applied yet. Neither is the other's duplicate — one is the schedule, one is the plugin's state.
    if (!slot->used || !slot->plugin.enabled) return;

    // Built here rather than through nya_lua_number, which is behind the Lua plugin's flag: this
    // function compiles in a build that has none, and NYA_Value is base's, not Lua's.
    NYA_Value argument = { .type = NYA_TYPE_F64, .as_f64 = (f64)delta_time_s };

    // The result is dropped and the error already reported by nya_plugin_call. A hook returning
    // something is not wrong, it is just not asked a question.
    (void)nya_plugin_call(slot->plugin.manifest.name, hook, nullptr, &argument, 1, nullptr);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE LUA SIDE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Everything that names a VM, in one block, with a stub apiece for a build compiled without the Lua
 * plugin. Keeping it together is what keeps the six hundred lines above free of #ifdef.
 */

#ifdef NYA_PLUGIN_LUA

/**
 * `plugins/<name>/<relative>`, or null when `relative` would leave the plugin's own directory.
 *
 * The whole of the filesystem permission's safety is this function. A plugin names a path, and a path
 * is the classic way out of a sandbox: absolute, a drive letter, a `..`, or a symlink pointing
 * somewhere else entirely. The first three are refused here by construction.
 * */
NYA_INTERNAL NYA_ConstCString _nya_plugin_resolve(const _NYA_PluginSlot* slot, NYA_ConstCString relative, OUT char* out, u64 capacity) {
    nya_assert(slot != nullptr && out != nullptr && capacity > 1);

    if (relative == nullptr || relative[0] == '\0') return nullptr;

    // Absolute in either spelling, and a drive letter, all name somewhere that is not below the
    // plugin's directory however the rest of the path reads.
    if (relative[0] == '/' || relative[0] == '\\') return nullptr;
    if (relative[0] != '\0' && relative[1] == ':') return nullptr;

    for (u64 i = 0; relative[i] != '\0'; i++) {
        b8 at_segment_start = i == 0 || relative[i - 1] == '/' || relative[i - 1] == '\\';
        if (!at_segment_start) continue;

        if (relative[i] != '.' || relative[i + 1] != '.') continue;

        // `..` only counts as a segment of its own; a file honestly called `..hidden` is not one.
        if (relative[i + 2] == '\0' || relative[i + 2] == '/' || relative[i + 2] == '\\') return nullptr;
    }

    (void)snprintf(out, capacity, "%s/%s", slot->plugin.directory, relative);

    return out;
}

/** The slot whose code is running, or null. What a host binding asks instead of trusting an argument. */
NYA_INTERNAL _NYA_PluginSlot* _nya_plugin_calling_slot(void) {
    NYA_ConstCString current = _nya_plugin_host.current;

    return current != nullptr ? _nya_plugin_slot_find(current) : nullptr;
}

/**
 * The calling plugin's own name, as its directory spells it.
 *
 * @lua_manual(nya.plugin.name, NONE, -> string)
 * */
NYA_INTERNAL void _nya_plugin_binding_name(NYA_LuaCall* call) {
    const _NYA_PluginSlot* slot = _nya_plugin_calling_slot();
    if (slot == nullptr) return;

    call->results[0]   = nya_lua_string(slot->plugin.manifest.name);
    call->result_count = 1;
}

/**
 * Its version, from its manifest.
 *
 * @lua_manual(nya.plugin.version, NONE, -> string)
 * */
NYA_INTERNAL void _nya_plugin_binding_version(NYA_LuaCall* call) {
    const _NYA_PluginSlot* slot = _nya_plugin_calling_slot();
    if (slot == nullptr) return;

    call->results[0]   = nya_lua_string(slot->plugin.manifest.version);
    call->result_count = 1;
}

/**
 * Where it was loaded from. What every path in nya.file.* is relative to.
 *
 * @lua_manual(nya.plugin.directory, NONE, -> string)
 * */
NYA_INTERNAL void _nya_plugin_binding_directory(NYA_LuaCall* call) {
    const _NYA_PluginSlot* slot = _nya_plugin_calling_slot();
    if (slot == nullptr) return;

    call->results[0]   = nya_lua_string(slot->plugin.directory);
    call->result_count = 1;
}

/**
 * Reads a file inside the plugin's own directory, or nil when there is none.
 *
 * @lua_manual(nya.file.read, FILESYSTEM, path: string, -> string)
 * */
NYA_INTERNAL void _nya_plugin_binding_file_read(NYA_LuaCall* call) {
    const _NYA_PluginSlot* slot = _nya_plugin_calling_slot();
    if (slot == nullptr || call->argument_count < 1 || call->arguments[0].type != NYA_TYPE_STRING) return;

    char path[NYA_PLUGIN_PATH_MAX];
    if (_nya_plugin_resolve(slot, call->arguments[0].as_string, path, sizeof(path)) == nullptr) {
        nya_plugin_error(slot->plugin.manifest.name, "in nya.file.read", "a path outside the plugin's own directory was refused");
        return;
    }

    NYA_String* contents = nya_string_create(call->arena);

    NYA_Error read = nya_file_read(path, contents);
    if (!read.ok) return;

    call->results[0]   = nya_lua_string(nya_string_to_cstring(call->arena, contents));
    call->result_count = 1;
}

/**
 * Writes one, and answers whether it worked.
 *
 * @lua_manual(nya.file.write, FILESYSTEM, path: string, contents: string, -> boolean)
 * */
NYA_INTERNAL void _nya_plugin_binding_file_write(NYA_LuaCall* call) {
    const _NYA_PluginSlot* slot = _nya_plugin_calling_slot();
    if (slot == nullptr || call->argument_count < 2) return;
    if (call->arguments[0].type != NYA_TYPE_STRING || call->arguments[1].type != NYA_TYPE_STRING) return;

    char path[NYA_PLUGIN_PATH_MAX];
    if (_nya_plugin_resolve(slot, call->arguments[0].as_string, path, sizeof(path)) == nullptr) {
        nya_plugin_error(slot->plugin.manifest.name, "in nya.file.write", "a path outside the plugin's own directory was refused");
        return;
    }

    NYA_Error written = nya_file_write(path, (NYA_ConstCString)call->arguments[1].as_string);

    call->results[0]   = nya_lua_boolean(written.ok);
    call->result_count = 1;
}

/**
 * The bindings that need to know which plugin is calling, which is every one that is scoped to a
 * plugin's own identity. They cannot be generated from an engine header, because there is no engine
 * function behind them: the plugin is the argument.
 * */
NYA_INTERNAL void _nya_plugin_open_host_bindings(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr && slot->vm != nullptr);

    // Always: a plugin that cannot say its own name cannot write a useful log line, and none of these
    // three reaches anything outside the manifest the plugin shipped.
    nya_lua_register_path(slot->vm, "nya.plugin.name", _nya_plugin_binding_name, nullptr);
    nya_lua_register_path(slot->vm, "nya.plugin.version", _nya_plugin_binding_version, nullptr);
    nya_lua_register_path(slot->vm, "nya.plugin.directory", _nya_plugin_binding_directory, nullptr);

    if (((u64)slot->plugin.permissions & (u64)NYA_PLUGIN_PERMISSION_FILESYSTEM) == 0) return;

    nya_lua_register_path(slot->vm, "nya.file.read", _nya_plugin_binding_file_read, nullptr);
    nya_lua_register_path(slot->vm, "nya.file.write", _nya_plugin_binding_file_write, nullptr);
}

NYA_Error _nya_plugin_vm_create(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr && slot->arena != nullptr);

    // `restricted`, always, whatever the permissions say: io, os, package, ffi and debug are not
    // permissions a plugin can ask for, they are doors out of the process. See lua.h.
    NYA_TRY(nya_lua_create(slot->arena, (NYA_LuaOptions){ .restricted = true }, &slot->vm));

    nya_lua_open_engine_permitted(slot->vm, slot->plugin.permissions);
    _nya_plugin_open_host_bindings(slot);

    return NYA_OK;
}

void _nya_plugin_vm_destroy(_NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr);

    nya_lua_destroy(slot->vm);
    slot->vm = nullptr;
}

NYA_Error _nya_plugin_vm_run_file(_NYA_PluginSlot* slot, NYA_ConstCString path, NYA_ConstCString chunk) {
    nya_assert(slot != nullptr && slot->vm != nullptr);
    nya_assert(path != nullptr && chunk != nullptr);

    NYA_Arena* arena = nya_arena_create(.name = "plugin_script");
    defer      nya_arena_destroy(arena);

    NYA_String* source = nya_string_create(arena);
    NYA_TRY(nya_file_read(path, source));

    // Set for the run, so a binding called while the chunk is executing knows whose chunk it is. The
    // scripts run at load time are exactly when a plugin registers things, and an unattributed
    // registration is the one thing namespacing cannot survive.
    NYA_ConstCString previous = _nya_plugin_host.current;
    _nya_plugin_host.current  = slot->plugin.manifest.name;

    NYA_Error ran = nya_lua_run(slot->vm, nya_string_to_cstring(arena, source), chunk);

    _nya_plugin_host.current = previous;

    if (!ran.ok) nya_plugin_error(slot->plugin.manifest.name, chunk, (NYA_ConstCString)ran.message);

    return ran;
}

b8 _nya_plugin_vm_has(const _NYA_PluginSlot* slot, NYA_ConstCString function) {
    nya_assert(slot != nullptr && function != nullptr);

    // A const slot still hands out a mutable VM pointer, which is what nya_lua_has_function wants:
    // const here is about the slot's own fields, and the VM is somebody else's state either way.
    return nya_lua_has_function(slot->vm, function);
}

NYA_Error _nya_plugin_vm_call(
    _NYA_PluginSlot* slot,
    NYA_ConstCString function,
    NYA_Arena*       arena,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) {
    nya_assert(slot != nullptr && slot->vm != nullptr);

    return nya_lua_call(slot->vm, arena, function, arguments, argument_count, out_result);
}

u64 _nya_plugin_vm_memory_bytes(const _NYA_PluginSlot* slot) {
    nya_assert(slot != nullptr);

    return slot->used ? nya_lua_memory_bytes(slot->vm) : 0;
}

#else // NYA_PLUGIN_LUA

/*
 * A build without the Lua plugin still has the host: the manifest, the permission model and the
 * introspection are useful to a tool that lists what is installed, and refusing to compile would mean
 * every caller needs an #ifdef of its own. What it cannot do is run anything, and it says so.
 */

NYA_Error _nya_plugin_vm_create(_NYA_PluginSlot* slot) {
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "'%s' cannot run: this build has no Lua (see NYA_PLUGIN_LUA)", slot->plugin.manifest.name);
}

void _nya_plugin_vm_destroy(_NYA_PluginSlot* slot) {
    nya_unused(slot);
}

NYA_Error _nya_plugin_vm_run_file(_NYA_PluginSlot* slot, NYA_ConstCString path, NYA_ConstCString chunk) {
    nya_unused(slot, path, chunk);

    return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no Lua");
}

b8 _nya_plugin_vm_has(const _NYA_PluginSlot* slot, NYA_ConstCString function) {
    nya_unused(slot, function);

    return false;
}

NYA_Error _nya_plugin_vm_call(
    _NYA_PluginSlot* slot,
    NYA_ConstCString function,
    NYA_Arena*       arena,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) {
    nya_unused(slot, function, arena, arguments, argument_count, out_result);

    return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no Lua");
}

u64 _nya_plugin_vm_memory_bytes(const _NYA_PluginSlot* slot) {
    nya_unused(slot);

    return 0;
}

#endif // NYA_PLUGIN_LUA

#ifdef NYA_TESTING
__attr_maybe_unused void _nya_plugin_reset_for_test(void) {
    nya_plugin_unload_all();

    _nya_plugin_host = (_NYA_PluginHost){ 0 };
}
#endif
