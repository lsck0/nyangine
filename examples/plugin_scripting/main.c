/**
 * @file examples/plugin_scripting/main.c
 *
 * The plugin surface, and nothing else: no window, no entities, no renderer. A LuaJIT VM is brought
 * up, a C function is registered into it, a script is run, and values are passed both ways as
 * NYA_Value.
 *
 * ```
 * ./build run example plugin_scripting
 * ```
 *
 * ## Two senses of the word, and this is the smaller one
 *
 * A *plugin* in `src/nyangine/plugins/` is an optional dependency compiled in behind a
 * `-DNYA_PLUGIN_*` flag and absent from the binary without it. The flags in `src/build/flags.h` are
 * the whole mechanism. Five exist:
 *
 * | Plugin  | Flag                 | State                                                      |
 * | :------ | :------------------- | :--------------------------------------------------------- |
 * | lua     | `NYA_PLUGIN_LUA`     | the VM below, and the generated `nya` table                 |
 * | curl    | `NYA_PLUGIN_CURL`    | blocking HTTP requests, client side only; nothing calls it  |
 * | discord | `NYA_PLUGIN_DISCORD` | built, never wired to a running client                      |
 * | steam   | `NYA_PLUGIN_STEAM`   | compiled and linked for the steam targets only              |
 *
 * A *plugin* in the sense a user means — a folder somebody else wrote, dropped into `plugins/` — is
 * `core_plugin.h`, and it is built on what this example shows. `plugins/hello/` in the repository
 * is a working one, loaded by gnyame at startup. Read core_plugin.h for the manifest, the compile
 * time permission grant and what it does and does not guarantee; this file is the layer underneath,
 * where a VM is a VM and nobody has decided yet who is allowed to call what.
 *
 * ## What is still missing
 *
 * The repository side of Dalamud's model: adding a repository by URL, an index served over HTTP,
 * per-plugin install and update from inside the game. TODO.md has it.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#ifndef NYA_PLUGIN_LUA
#error "This example needs -DNYA_PLUGIN_LUA, which ./build run example passes. See src/build/flags.h."
#endif

/* THE BINDING */

/** What the script has scored so far, summed by the binding below. */
typedef struct {
    s64 total;
} Score;

/**
 * `score_add(points)`, callable from Lua.
 *
 * Arguments arrive as NYA_Value and are whatever the script passed, so the count and the types are
 * checked rather than assumed: a script is untrusted input like any other.
 * */
NYA_INTERNAL void binding_score_add(NYA_LuaCall* call) {
    nya_assert(call != nullptr);

    Score* score = call->user_data;
    nya_assert(score != nullptr, "score_add was registered without its user data");

    if (call->argument_count < 1) {
        nya_log_warn("score_add() was called with no arguments; nothing added.");
        return;
    }

    // Lua has one number type, and the marshaller reports it as f64.
    if (call->arguments[0].type != NYA_TYPE_F64) {
        nya_log_warn("score_add() wants a number, got %s.", NYA_TYPE_NAME_MAP[call->arguments[0].type]);
        return;
    }

    score->total += (s64)call->arguments[0].as_f64;

    call->results[0]   = nya_lua_integer(score->total);
    call->result_count = 1;
}

/* THE SCRIPT */

/**
 * Inline rather than loaded from assets/: nya_lua_run_asset goes through the asset system, which
 * belongs to a running NYA_App, and this example has no app.
 * */
NYA_INTERNAL NYA_ConstCString SCRIPT =
    "settings = { name = 'plugin_scripting', rounds = 3, verbose = true }\n"
    "\n"
    "function play(rounds)\n"
    "  local total = 0\n"
    "  for i = 1, rounds do total = score_add(i * 10) end\n"
    "  return total\n"
    "end\n";

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_Arena* arena = nya_arena_create(.name = "plugin_scripting");
    defer      nya_arena_destroy(arena);

    Score score = { 0 };

    // `restricted` refuses io, os, package, ffi and debug, which is what running someone else's script should look like. The arena owns the wrapper; LuaJIT owns its own heap, so nya_lua_destroy is required and freeing the arena alone would leak all of it.
    NYA_LuaVM* vm = nullptr;
    NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ .restricted = true }, &vm), "while creating the VM");
    defer nya_lua_destroy(vm);

    nya_lua_register(vm, "score_add", binding_score_add, &score);

    // A script is input, not code: a syntax error in it is an operating error, so it is reported and the program carries on rather than crashing.
    NYA_Error ran = nya_lua_run(vm, SCRIPT, "plugin_scripting.lua");
    if (!ran.ok) {
        nya_log_error("The script did not load: %s", (NYA_ConstCString)ran.message);
        return EXIT_FAILURE;
    }

    // reading a table the script defined
    NYA_Value settings = { 0 };
    NYA_EXPECT(nya_lua_global_get(vm, arena, "settings", &settings), "while reading `settings`");

    nya_assert(settings.type == NYA_TYPE_OBJECT, "a Lua table keyed by string comes back as an object");

    NYA_Value* rounds = nya_object_get(&settings.as_object, "rounds");
    nya_assert(rounds != nullptr && rounds->type == NYA_TYPE_F64, "`settings.rounds` is a Lua number");

    // The same NYA_Object serde writes, so a script's table goes to disk with no conversion step.
    NYA_String* as_json = nya_serialize(arena, &settings.as_object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);
    nya_log_info("settings as json:\n" NYA_FMT_STRING, NYA_FMT_STRING_ARG(as_json));

    // calling into the script
    if (!nya_lua_has_function(vm, "play")) {
        nya_log_error("The script defines no `play`.");
        return EXIT_FAILURE;
    }

    NYA_Value argument = nya_lua_number(rounds->as_f64);
    NYA_Value result   = { 0 };
    NYA_EXPECT(nya_lua_call(vm, arena, "play", &argument, 1, &result), "while calling `play`");

    nya_log_info("play(%d) returned %.0f; the binding summed %lld.", (s32)rounds->as_f64, result.as_f64, (long long)score.total);

    // writing a global back
    NYA_Value verdict = nya_lua_string(score.total >= 60 ? "good round" : "poor round");
    NYA_EXPECT(nya_lua_global_set(vm, "verdict", &verdict), "while writing `verdict`");

    NYA_EXPECT(nya_lua_run(vm, "print('lua sees verdict = ' .. verdict)", "verdict"), "while printing the verdict");

    nya_log_info("LuaJIT holds %llu bytes.", (unsigned long long)nya_lua_memory_bytes(vm));

    // what a plugin's VM looks like — The same call the plugin host makes for every plugin it loads. A permission the plugin did not get is not a call that refuses: the name is never put in the VM, so `nya.entity` is nil and indexing it is an ordinary Lua error in the plugin's own chunk. That is the whole enforcement mechanism, and it is why there is nothing for a script to reach around.
    NYA_LuaVM* sandboxed = nullptr;
    NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ .restricted = true }, &sandboxed), "while creating the second VM");
    defer nya_lua_destroy(sandboxed);

    nya_lua_open_engine_permitted(sandboxed, NYA_PLUGIN_PERMISSION_INPUT);

    NYA_ConstCString probe =
        "nya.log.info('logging needs no permission, so it is here')\n"
        "nya.log.info('input was granted: nya.input.action_pressed is ' .. type(nya.input.action_pressed))\n"
        "nya.log.info('entities were not: nya.entity is ' .. type(nya.entity))\n";

    NYA_EXPECT(nya_lua_run(sandboxed, probe, "permissions.lua"), "while probing what the VM was given");

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
