/**
 * The LuaJIT plugin: running scripts, and moving values across the boundary in both directions.
 *
 * Nearly all of the risk in a binding layer is stack discipline — a path that pops one fewer than it
 * pushed is invisible until the hundredth call and then it is a stack overflow inside a script. So
 * the marshalling cases here are run in a loop rather than once, and `lua_gettop` is checked at the
 * end: an imbalance shows up as a growing stack rather than as a wrong answer.
 *
 * The other thing worth testing is the boundary's refusals. A script can build a table containing
 * itself in two lines, and a converter that follows it recurses until the process dies.
 *
 * Headless: nothing here needs a window, and the engine `nya` table is not opened — that half needs
 * an app and a world, and is covered where those exist.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Counts calls and remembers what it was handed, so a binding can be proved to have really run. */
static u32 binding_calls    = 0;
static f64 binding_sum      = 0.0;
static b8  binding_saw_user = false;

/* Externally linked, not static: a binding in a game .so is resolved by name after a hot reload. */
void lua_test_add(NYA_LuaCall* call) {
    binding_calls++;

    for (u32 i = 0; i < call->argument_count; i++) {
        if (call->arguments[i].type == NYA_TYPE_F64) binding_sum += call->arguments[i].as_f64;
    }

    binding_saw_user = call->user_data != nullptr;

    call->results[0]   = nya_lua_number(binding_sum);
    call->result_count = 1;
}

/** Returns nothing, which is legal and is the case a naive trampoline gets wrong. */
void lua_test_silent(NYA_LuaCall* call) {
    nya_unused(call);
    binding_calls++;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    NYA_Arena* arena = nya_arena_create(.name = "test_lua");
    defer nya_arena_destroy(arena);

    NYA_LuaVM* vm = nullptr;
    NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ 0 }, &vm));
    defer nya_lua_destroy(vm);

    // ── Running code, and both kinds of failure.
    {
        NYA_EXPECT(nya_lua_run(vm, "answer = 6 * 7", "inline"));

        NYA_Value answer = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "answer", &answer));

        nya_check(answer.type == NYA_TYPE_F64, "a Lua number comes back as an f64, got %s", NYA_TYPE_NAME_MAP[answer.type]);
        nya_check(fabs(answer.as_f64 - 42.0) < 0.0001, "and should be 42, got %f", answer.as_f64);

        // A syntax error and a runtime error are different kinds, because they want different
        // reactions: one is a typo in a script, the other is a bug inside one.
        NYA_Error syntax = nya_lua_run(vm, "this is not lua", "bad");
        nya_check(!syntax.ok && syntax.kind == NYA_ERROR_PARSE, "a syntax error should be a parse error");

        NYA_Error runtime = nya_lua_run(vm, "error('deliberate')", "boom");
        nya_check(!runtime.ok && runtime.kind == NYA_ERROR_NOT_OK, "a raised error should not be");

        // Lua's own message names the line, which is why it is propagated rather than summarised.
        nya_check(strstr(runtime.message, "deliberate") != nullptr, "and should carry Lua's message, got '%s'", runtime.message);
    }

    // ── Calling a function, with arguments and a result.
    {
        NYA_EXPECT(nya_lua_run(vm, "function greet(who) return 'hello ' .. who end", "greet"));

        nya_check(nya_lua_has_function(vm, "greet"), "the function should be found");
        nya_check(!nya_lua_has_function(vm, "answer"), "a number is not a function");
        nya_check(!nya_lua_has_function(vm, "nothing_at_all"), "and neither is nothing");

        NYA_Value who    = nya_lua_string("world");
        NYA_Value result = { 0 };
        NYA_EXPECT(nya_lua_call(vm, arena, "greet", &who, 1, &result));

        nya_check(result.type == NYA_TYPE_STRING, "a string comes back as one, got %s", NYA_TYPE_NAME_MAP[result.type]);
        nya_check(nya_string_equals(result.as_string, "hello world"), "got '%s'", result.as_string);

        // Calling something that is not a function is NOT_FOUND rather than a failure, so an optional
        // hook that a script simply did not define is distinguishable from one that threw.
        NYA_Error missing = nya_lua_call(vm, arena, "no_such_function", nullptr, 0, nullptr);
        nya_check(!missing.ok && missing.kind == NYA_ERROR_NOT_FOUND, "a missing function is NOT_FOUND, got %s",
                  NYA_ERRORKIND_NAME_MAP[missing.kind]);
    }

    // ── A table becomes an object; an array-shaped table becomes an array.
    {
        NYA_EXPECT(nya_lua_run(vm,
                               "settings = { volume = 0.5, name = 'nyangine', fullscreen = true }"
                               "scores = { 10, 20, 30 }"
                               "sparse = { [1] = 'a', [3] = 'c' }",
                               "tables"));

        NYA_Value settings = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "settings", &settings));

        nya_check(settings.type == NYA_TYPE_OBJECT, "a keyed table is an object, got %s", NYA_TYPE_NAME_MAP[settings.type]);

        NYA_Value* volume = nya_object_get(&settings.as_object, "volume");
        nya_check(volume != nullptr && fabs(volume->as_f64 - 0.5) < 0.0001, "with its fields readable");

        NYA_Value* name = nya_object_get(&settings.as_object, "name");
        nya_check(name != nullptr && name->type == NYA_TYPE_STRING && nya_string_equals(name->as_string, "nyangine"),
                  "including the strings");

        NYA_Value* fullscreen = nya_object_get(&settings.as_object, "fullscreen");
        nya_check(fullscreen != nullptr && fullscreen->type == NYA_TYPE_B8 && fullscreen->as_b8, "and the booleans");

        NYA_Value scores = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "scores", &scores));

        nya_check(scores.type == NYA_TYPE_ARRAY, "a 1..n table is an array, got %s", NYA_TYPE_NAME_MAP[scores.type]);
        nya_check(scores.as_array.length == 3, "of three, got " FMTu64, scores.as_array.length);
        nya_check(fabs(scores.as_array.items[2].as_f64 - 30.0) < 0.0001, "in order");

        /*
         * A table with a hole is *not* an array, and this is the case the length operator alone gets
         * wrong: `#sparse` may answer either 1 or 3, so a converter trusting it would silently drop
         * a value or read a nil as one.
         */
        NYA_Value sparse = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "sparse", &sparse));
        nya_check(sparse.type == NYA_TYPE_OBJECT, "a table with a hole is not a sequence, got %s", NYA_TYPE_NAME_MAP[sparse.type]);
    }

    // ── The other direction: an object becomes a keyed table, an array a 1..n one.
    {
        NYA_Object config = nya_object_create_on_stack(arena);
        nya_object_set(&config, "level", nya_lua_number(7.0));
        nya_object_set(&config, "title", nya_lua_string("deep"));

        NYA_Value value = { .type = NYA_TYPE_OBJECT, .as_object = config };
        NYA_EXPECT(nya_lua_global_set(vm, "config", &value));

        NYA_EXPECT(nya_lua_run(vm, "config_ok = (config.level == 7 and config.title == 'deep')", "roundtrip"));

        NYA_Value ok = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "config_ok", &ok));
        nya_check(ok.type == NYA_TYPE_B8 && ok.as_b8, "an object should arrive in Lua as a keyed table");

        NYA_ArrayᐸNYA_Valueᐳ* items = nya_array_create(arena, NYA_Value);
        nya_array_push_back(items, nya_lua_number(1.0));
        nya_array_push_back(items, nya_lua_number(2.0));

        NYA_Value list = { .type = NYA_TYPE_ARRAY, .as_array = *items };
        NYA_EXPECT(nya_lua_global_set(vm, "list", &list));

        NYA_EXPECT(nya_lua_run(vm, "list_ok = (#list == 2 and list[1] == 1 and list[2] == 2)", "roundtrip"));

        NYA_Value list_ok = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "list_ok", &list_ok));
        nya_check(list_ok.as_b8, "an array should arrive keyed from one, which is what makes it a sequence in Lua");
    }

    // ── Bound C functions, including one that returns nothing.
    {
        u32 marker = 1;

        nya_lua_register(vm, "add", lua_test_add, &marker);
        nya_lua_register(vm, "silent", lua_test_silent, nullptr);

        binding_calls = 0;
        binding_sum   = 0.0;

        NYA_EXPECT(nya_lua_run(vm, "total = add(1, 2, 3)", "binding"));

        nya_check(binding_calls == 1, "the binding should have run once, got " FMTu32, binding_calls);
        nya_check(fabs(binding_sum - 6.0) < 0.0001, "with all three arguments, got %f", binding_sum);
        nya_check(binding_saw_user, "and its user data");

        NYA_Value total = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "total", &total));
        nya_check(fabs(total.as_f64 - 6.0) < 0.0001, "and its result should reach Lua, got %f", total.as_f64);

        // Returning nothing is legal, and is what a trampoline that always pushes one result breaks on.
        NYA_EXPECT(nya_lua_run(vm, "silent_ok = (silent() == nil)", "binding"));

        NYA_Value silent_ok = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "silent_ok", &silent_ok));
        nya_check(silent_ok.as_b8, "a binding returning nothing should be nil in Lua");
    }

    /*
     * ── A self-referencing table is refused rather than followed.
     *
     * Two lines of Lua, and a converter without a depth limit recurses on it until the process dies.
     * What is asserted is only that it returns — the shape of what comes back past the limit is not
     * interesting, and pinning it would be pinning the limit.
     */
    {
        NYA_EXPECT(nya_lua_run(vm, "loop = {} loop.self = loop", "cycle"));

        NYA_Value cycle = { 0 };
        NYA_EXPECT(nya_lua_global_get(vm, arena, "loop", &cycle));

        nya_check(cycle.type == NYA_TYPE_OBJECT, "it still converts as far as the limit");
    }

    /*
     * ── The stack stays balanced.
     *
     * The failure this catches is cumulative: a path that pops one fewer than it pushes is invisible
     * once and fatal after a few thousand calls. Run enough times that a leak of a single slot per
     * iteration would be unmistakable, then check what Lua thinks is on the stack.
     */
    {
        NYA_EXPECT(nya_lua_run(vm, "function churn(t) return { n = (t.n or 0) + 1, tag = 'x' } end", "churn"));

        NYA_Object seed = nya_object_create_on_stack(arena);
        nya_object_set(&seed, "n", nya_lua_number(0.0));

        NYA_Value argument = { .type = NYA_TYPE_OBJECT, .as_object = seed };

        for (u32 i = 0; i < 2000; i++) {
            NYA_Value result = { 0 };
            NYA_EXPECT(nya_lua_call(vm, arena, "churn", &argument, 1, &result));

            // And a failing call must balance too, which is the path that is easiest to get wrong.
            (void)nya_lua_call(vm, arena, "not_a_function", &argument, 1, &result);
            (void)nya_lua_run(vm, "error('again')", "boom");
        }

        // Reaching inside for this is the point: there is no public way to ask, and no other way to
        // tell an imbalance from a program that simply used more memory.
        nya_check(lua_gettop(vm->state) == 0, "the Lua stack should be empty between calls, got %d", lua_gettop(vm->state));
    }

    // ── Restricted mode removes what reaches outside the process.
    {
        NYA_LuaVM* limited = nullptr;
        NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ .restricted = true }, &limited));
        defer nya_lua_destroy(limited);

        NYA_EXPECT(nya_lua_run(limited, "gone = (io == nil and os == nil and package == nil and debug == nil)", "restricted"));

        NYA_Value gone = { 0 };
        NYA_EXPECT(nya_lua_global_get(limited, arena, "gone", &gone));
        nya_check(gone.as_b8, "restricted should remove io, os, package and debug");

        // But the language itself is still there, or it would not be worth restricting.
        NYA_EXPECT(nya_lua_run(limited, "kept = (string ~= nil and math ~= nil and table ~= nil)", "restricted"));

        NYA_Value kept = { 0 };
        NYA_EXPECT(nya_lua_global_get(limited, arena, "kept", &kept));
        nya_check(kept.as_b8, "and keep string, math and table");
    }

    // ── A VM with no standard library at all.
    {
        NYA_LuaVM* bare = nullptr;
        NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ .no_standard_library = true }, &bare));
        defer nya_lua_destroy(bare);

        NYA_EXPECT(nya_lua_run(bare, "empty = (string == nil and print == nil)", "bare"));

        NYA_Value empty = { 0 };
        NYA_EXPECT(nya_lua_global_get(bare, arena, "empty", &empty));
        nya_check(empty.as_b8, "no standard library means none of it");
    }

    // ── Introspection, and the degenerate cases every one of these has to survive.
    {
        nya_check(nya_lua_memory_bytes(vm) > 0, "a live VM has allocated something");
        nya_check(nya_lua_memory_bytes(nullptr) == 0, "and nothing has not");

        nya_lua_collect(vm);
        nya_lua_collect(nullptr);

        nya_check(!nya_lua_run(nullptr, "x = 1", "null").ok, "running on no VM fails rather than crashing");
        nya_check(!nya_lua_run(vm, nullptr, "null").ok, "and so does running nothing");
        nya_check(!nya_lua_has_function(nullptr, "x"), "no VM has no functions");
        nya_check(!nya_lua_call(vm, arena, nullptr, nullptr, 0, nullptr).ok, "a call needs a name");

        // Past the argument ceiling is refused rather than truncated: a call that silently drops its
        // last argument is worse than one that does not happen.
        NYA_Value many[NYA_LUA_MAX_ARGUMENTS + 1] = { 0 };
        nya_check(!nya_lua_call(vm, arena, "greet", many, NYA_LUA_MAX_ARGUMENTS + 1, nullptr).ok, "too many arguments is refused");

        nya_lua_register(vm, "nope", nullptr, nullptr);
        nya_lua_register(nullptr, "nope", lua_test_silent, nullptr);

        // Destroying twice must not close the state twice.
        NYA_LuaVM* doomed = nullptr;
        NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ 0 }, &doomed));
        nya_lua_destroy(doomed);
        nya_lua_destroy(doomed);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
