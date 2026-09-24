/**
 * The plugin host: what a manifest has to say, what the compile time permission grant refuses, how a
 * plugin's identity scopes what it registers, and how a plugin that keeps failing is switched off.
 *
 * Every plugin here is written to disk by the test itself, because that is what a plugin is: a
 * directory somebody else put there. The tree is removed again at the end.
 **/

/*
 * Before nyangine.h, so the host discovers the tree this test writes rather than the repository's real
 * `plugins/`. A #define above the include is also what makes this test compile its own copy of the
 * engine instead of linking the shared one; see _test_shares_engine in src/build/test.c.
 */
#define NYA_PLUGIN_DIRECTORY TEST_PLUGIN_ROOT

#define TEST_PLUGIN_ROOT "./.test_plugins"

/*
 * This suite is about the manifest, the permission grant and the namespacing, none of which is a
 * signature. The plugins it writes are unsigned on purpose, so it compiles the dev opt-out; the
 * signature policy itself is proved in test_plugin_signature.c.
 */
#define NYA_PLUGIN_REQUIRE_SIGNATURE false

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WRITING PLUGINS TO DISK
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Writes `contents` to `<root>/<plugin>/<relative>`, creating every directory on the way. */
static void plugin_file_write(NYA_ConstCString plugin, NYA_ConstCString relative, NYA_ConstCString contents) {
    char directory[512];
    (void)snprintf(directory, sizeof(directory), "%s/%s", TEST_PLUGIN_ROOT, plugin);

    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", directory, relative);

    // the file's own parent, which for "src/one.lua" is not the plugin's directory.
    char parent[512];
    (void)snprintf(parent, sizeof(parent), "%s", path);

    char* last = strrchr(parent, '/');
    if (last != nullptr) *last = '\0';

    NYA_EXPECT(nya_filesystem_create_directory(parent), "while creating %s", parent);
    NYA_EXPECT(nya_file_write(path, contents), "while writing %s", path);
}

/** A manifest with the given name, version and permission list. */
static void plugin_manifest_write(NYA_ConstCString directory, NYA_ConstCString name, NYA_ConstCString version, NYA_ConstCString body) {
    char manifest[2048];
    (void)snprintf(manifest, sizeof(manifest),
                   "nya 2 0\n"
                   "{\n"
                   "    name: string \"%s\";\n"
                   "    version: string \"%s\";\n"
                   "    author: string \"the test\";\n"
                   "    license: string \"MIT\";\n"
                   "    repository: string \"https://example.invalid/%s\";\n"
                   "%s"
                   "}\n",
                   name, version, name, body);

    plugin_file_write(directory, NYA_PLUGIN_MANIFEST_FILE, manifest);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TREE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static void plugins_write_all(void) {
    (void)nya_filesystem_delete_recursive(TEST_PLUGIN_ROOT);

    /* A plugin that asks for nothing, defines every hook, and has a module under src/. */
    plugin_manifest_write("good", "good", "1.2.0", "");
    plugin_file_write("good", "src/one.lua", "MESSAGE = 'from src'\n");
    plugin_file_write("good", NYA_PLUGIN_ENTRY_FILE,
                      "ticks = 0\n"
                      "function on_load() nya.log.info(MESSAGE .. ' in ' .. nya.plugin.name()) end\n"
                      "function on_tick(dt) ticks = ticks + 1 end\n"
                      "function double(x) return x * 2 end\n");

    /* Wants the network, which no profile below ALL grants and the test build does not compile. */
    plugin_manifest_write("greedy", "greedy", "0.1.0", "    permissions: string[] [\"NYA_PLUGIN_PERMISSION_NETWORK\"];\n");
    plugin_file_write("greedy", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");

    /* Calls itself something other than its own directory. */
    plugin_manifest_write("mismatch", "somebody_else", "0.1.0", "");
    plugin_file_write("mismatch", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");

    /* A permission that is not a permission, which must refuse the file rather than read as none. */
    plugin_manifest_write("typo", "typo", "0.1.0", "    permissions: string[] [\"NYA_PLUGIN_PERMISSION_EVERYTHING\"];\n");
    plugin_file_write("typo", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");

    /* Reaches for a binding its permissions never gave it. */
    plugin_manifest_write("nosy", "nosy", "0.1.0", "");
    plugin_file_write("nosy", NYA_PLUGIN_ENTRY_FILE, "nya.entity.spawn({ name = 'nope' })\n");

    /* Needs a plugin that is not installed. */
    plugin_manifest_write("needy", "needy", "0.1.0",
                          "    dependencies: array[] [object { name: string \"nowhere\"; version: string \"1.0.0\"; }];\n");
    plugin_file_write("needy", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");

    /* Refuses to run beside `good`. */
    plugin_manifest_write("jealous", "jealous", "0.1.0", "    conflicts: array[] [object { name: string \"good\"; }];\n");
    plugin_file_write("jealous", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");

    /* Throws every time it is ticked. */
    plugin_manifest_write("thrower", "thrower", "0.1.0", "");
    plugin_file_write("thrower", NYA_PLUGIN_ENTRY_FILE, "function on_tick(dt) error('on purpose') end\n");

    /* Needs an engine from the future. */
    plugin_manifest_write("futuristic", "futuristic", "0.1.0", "    engine_version: string \"9999.0.0\";\n");
    plugin_file_write("futuristic", NYA_PLUGIN_ENTRY_FILE, "function on_load() end\n");

    /* Defines a `spawn`, as does `twin` below. Neither may see the other's. */
    plugin_manifest_write("twin_a", "twin_a", "0.1.0", "");
    plugin_file_write("twin_a", NYA_PLUGIN_ENTRY_FILE, "function spawn() return 'a' end\n");

    plugin_manifest_write("twin_b", "twin_b", "0.1.0", "");
    plugin_file_write("twin_b", NYA_PLUGIN_ENTRY_FILE, "function spawn() return 'b' end\n");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TESTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
    plugins_write_all();

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the compile time grant is what the build says and nothing reads it from
    // anywhere else.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_PluginPermission granted = nya_plugin_permissions_granted();

        nya_assert(((u64)granted & (u64)NYA_PLUGIN_PERMISSION_NETWORK) == 0, "the test build compiles the gameplay profile, which has no network");
        nya_assert(((u64)granted & (u64)NYA_PLUGIN_PERMISSION_ENTITIES) != 0, "and does grant entities");

        nya_assert(nya_string_equals(nya_plugin_permission_name(NYA_PLUGIN_PERMISSION_UI), "NYA_PLUGIN_PERMISSION_UI"),
                   "a permission is named the way a manifest spells it");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a manifest is read through the reflection tables, every field of it.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_PluginManifest manifest = { 0 };
        NYA_Error          read     = nya_plugin_manifest_load(TEST_PLUGIN_ROOT "/good", &manifest);

        nya_assert(read.ok, "the manifest must parse: %s", (NYA_ConstCString)read.message);
        nya_assert(nya_string_equals(manifest.name, "good"));
        nya_assert(nya_string_equals(manifest.version, "1.2.0"));
        nya_assert(nya_string_equals(manifest.author, "the test"));
        nya_assert(nya_string_equals(manifest.license, "MIT"));
        nya_assert(manifest.permissions == NYA_PLUGIN_PERMISSION_NONE, "a manifest that asks for nothing gets nothing");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a misspelled permission refuses the manifest. Skipping it in silence
    // would read as a permission the plugin never asked for, which is the one
    // mistake this boundary exists to catch.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_PluginManifest manifest = { 0 };
        NYA_Error          read     = nya_plugin_manifest_load(TEST_PLUGIN_ROOT "/typo", &manifest);

        nya_assert(!read.ok, "a permission that is not a permission must refuse the file");
        nya_assert(read.kind == NYA_ERROR_PARSE);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a plugin loads, runs its src/ before its main.lua, and becomes one entry
    // in the system registry owned by its own name.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_plugin_reset_for_test();

        NYA_Error loaded = nya_plugin_load(TEST_PLUGIN_ROOT "/good");
        nya_assert(loaded.ok, "the plugin must load: %s", (NYA_ConstCString)loaded.message);

        nya_assert(nya_plugin_count() == 1);

        const NYA_Plugin* plugin = nya_plugin_find("good");
        nya_assert(plugin != nullptr, "and be findable by the name its directory gives it");
        nya_assert(plugin->has_tick, "its on_tick was noticed once, at load");
        nya_assert(!plugin->has_render, "and a hook it does not define is not looked for again");
        nya_assert(plugin->enabled);

        b8 registered = false;
        for (u32 i = 0; i < nya_system_registry_count(); i++) {
            const NYA_SystemEntry* entry = nya_system_registry_at(i);
            if (entry->owner.kind != NYA_SYSTEM_OWNER_PLUGIN) continue;
            if (!nya_string_equals(entry->owner.plugin, "good")) continue;

            registered = true;

            nya_assert(nya_string_equals(entry->name, "good:hooks"), "a plugin's registrations carry its name, got '%s'", entry->name);
            nya_assert(entry->memory_bytes != NYA_CALLBACK_HANDLE_NONE, "and report what the plugin's VM holds");
        }

        nya_assert(registered, "a loaded plugin is one system, owned by itself");

        NYA_SystemOwnerStats stats = nya_plugin_stats("good");
        nya_assert(nya_string_equals(stats.name, "good"), "and the registry accounts for it under that name");
        nya_assert(stats.memory_bytes > 0, "a live LuaJIT state is holding something");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: calling into a plugin, and what the host knows while it is running.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_plugin_call");
        defer      nya_arena_destroy(arena);

        nya_assert(nya_plugin_current() == nullptr, "nothing is running before the call");

        NYA_Value argument = nya_lua_number(21.0);
        NYA_Value result   = { 0 };

        NYA_Error called = nya_plugin_call("good", "double", arena, &argument, 1, &result);

        nya_assert(called.ok, "%s", (NYA_ConstCString)called.message);
        nya_assert(result.type == NYA_TYPE_F64 && (s64)result.as_f64 == 42, "the plugin answered");
        nya_assert(nya_plugin_current() == nullptr, "and the host is nobody's plugin again afterwards");

        NYA_Error missing = nya_plugin_call("good", "not_written", nullptr, nullptr, 0, nullptr);
        nya_assert(!missing.ok && missing.kind == NYA_ERROR_NOT_FOUND, "an optional hook nobody wrote is not a failure");
        nya_assert(nya_plugin_find("good")->error_count == 0, "and is not held against the plugin");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a plugin that wants more than the build grants is refused, and nothing
    // of it is left behind.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Error refused = nya_plugin_load(TEST_PLUGIN_ROOT "/greedy");

        nya_assert(!refused.ok, "the network is not in the gameplay profile");
        nya_assert(refused.kind == NYA_ERROR_PERMISSION_DENIED);
        nya_assert(nya_plugin_find("greedy") == nullptr, "a refused plugin is not loaded");
        nya_assert(nya_plugin_count() == 1, "and takes no slot");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a binding a plugin was not granted is not a refusal, it is a name that
    // is not there. `nosy` reaches for nya.entity and its chunk dies.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Error refused = nya_plugin_load(TEST_PLUGIN_ROOT "/nosy");

        nya_assert(!refused.ok, "a plugin with no entity permission cannot call nya.entity.spawn");
        nya_assert(nya_plugin_find("nosy") == nullptr);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the directory is the identity, and a name is claimed once.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Error mismatched = nya_plugin_load(TEST_PLUGIN_ROOT "/mismatch");
        nya_assert(!mismatched.ok, "a manifest may not name itself something other than its own folder");

        NYA_Error twice = nya_plugin_load(TEST_PLUGIN_ROOT "/good");
        nya_assert(!twice.ok && twice.kind == NYA_ERROR_ALREADY_EXISTS, "a name is answered by one plugin");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: dependencies, conflicts and the engine version, each refused by name.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Error needy = nya_plugin_load(TEST_PLUGIN_ROOT "/needy");
        nya_assert(!needy.ok && needy.kind == NYA_ERROR_NOT_FOUND, "a dependency that is not installed refuses the load");

        NYA_Error jealous = nya_plugin_load(TEST_PLUGIN_ROOT "/jealous");
        nya_assert(!jealous.ok && jealous.kind == NYA_ERROR_ALREADY_EXISTS, "and so does a conflict with something loaded");

        NYA_Error futuristic = nya_plugin_load(TEST_PLUGIN_ROOT "/futuristic");
        nya_assert(!futuristic.ok, "an engine version nobody has refuses the load");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: two plugins may both define `spawn`, because neither can see the other.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_plugin_twins");
        defer      nya_arena_destroy(arena);

        nya_assert(nya_plugin_load(TEST_PLUGIN_ROOT "/twin_a").ok);
        nya_assert(nya_plugin_load(TEST_PLUGIN_ROOT "/twin_b").ok);

        NYA_Value from_a = { 0 };
        NYA_Value from_b = { 0 };

        nya_assert(nya_plugin_call("twin_a", "spawn", arena, nullptr, 0, &from_a).ok);
        nya_assert(nya_plugin_call("twin_b", "spawn", arena, nullptr, 0, &from_b).ok);

        nya_assert(nya_string_equals(from_a.as_string, "a"), "twin_a's spawn is twin_a's, got '%s'", from_a.as_string);
        nya_assert(nya_string_equals(from_b.as_string, "b"), "and twin_b's is its own, got '%s'", from_b.as_string);

        char qualified[NYA_PLUGIN_QUALIFIED_MAX];
        nya_assert(nya_string_equals(nya_plugin_qualify("twin_a", "spawn", qualified, sizeof(qualified)), "twin_a:spawn"));
        nya_assert(nya_string_equals(nya_plugin_qualify(nullptr, "spawn", qualified, sizeof(qualified)), "spawn"),
                   "the engine's own registrations are in nobody's namespace");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a plugin that keeps throwing is switched off rather than left to fill
    // the log, and the error is the plugin's, never the engine's.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(nya_plugin_load(TEST_PLUGIN_ROOT "/thrower").ok);
        nya_assert(nya_plugin_is_enabled("thrower"));

        NYA_Value delta = nya_lua_number(0.016);

        for (u32 i = 0; i < NYA_PLUGIN_ERROR_MAX; i++) {
            NYA_Error threw = nya_plugin_call("thrower", "on_tick", nullptr, &delta, 1, nullptr);
            nya_assert(!threw.ok, "the hook throws every time");
        }

        nya_assert(nya_plugin_find("thrower")->error_count >= NYA_PLUGIN_ERROR_MAX, "every one of them counted");
        nya_assert(!nya_plugin_is_enabled("thrower"), "and past the limit it is switched off");
        nya_assert(!nya_system_is_enabled("thrower:hooks"), "which the registry agrees with");

        // Switching it back on is a decision to give it another run, so the count starts again.
        nya_plugin_enable("thrower");
        nya_assert(nya_plugin_is_enabled("thrower") && nya_plugin_find("thrower")->error_count == 0);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: unloading takes the plugin's systems with it, and is idempotent.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        u32 before = nya_system_registry_count();

        nya_plugin_unload("thrower");

        nya_assert(nya_plugin_find("thrower") == nullptr);
        nya_assert(nya_system_registry_count() == before - 1, "its registry entry went with it");

        nya_plugin_unload("thrower");
        nya_plugin_unload("never_existed");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: discovery. Every directory with a manifest is tried, the ones that are
    // refused are reported and stepped over, and the rest still load.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_plugin_reset_for_test();

        nya_assert(nya_plugin_load_all().ok, "one bad plugin is not a reason for the game not to start");

        nya_assert(nya_plugin_find("good") != nullptr, "the good one loaded");
        nya_assert(nya_plugin_find("greedy") == nullptr, "the greedy one did not");
        nya_assert(nya_plugin_find("needy") == nullptr, "nor did the one waiting on a plugin nobody has");

        for (u32 i = 0; i < nya_plugin_count(); i++) {
            const NYA_Plugin* plugin = nya_plugin_at(i);

            nya_assert(plugin != nullptr && plugin->manifest.name[0] != '\0', "every loaded plugin is at an index");
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a missing plugins directory is not a failure, it is the normal case.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_plugin_reset_for_test();

        NYA_EXPECT(nya_filesystem_delete_recursive(TEST_PLUGIN_ROOT), "while removing the test tree");

        nya_assert(nya_plugin_load_all().ok, "no plugins directory means no plugins");
        nya_assert(nya_plugin_count() == 0);
    }

    _nya_plugin_reset_for_test();

    printf("All tests passed.\n");
    return 0;
}
