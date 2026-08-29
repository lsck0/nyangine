/**
 * @file lua.h
 *
 * LuaJIT, in terms of NYA_Object — so a value crossing the boundary is the same type a JSON body or
 * a database row is, and any of the three can go through serde without a conversion step.
 *
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "scripts");
 * defer      nya_arena_destroy(arena);
 *
 * NYA_LuaVM* vm = nullptr;
 * NYA_EXPECT(nya_lua_create(arena, (NYA_LuaOptions){ 0 }, &vm));
 * defer nya_lua_destroy(vm);
 *
 * NYA_EXPECT(nya_lua_run(vm, "function greet(who) return 'hello ' .. who end", "inline"));
 *
 * NYA_Value  who    = nya_lua_string("world");
 * NYA_Value  result = { 0 };
 * NYA_EXPECT(nya_lua_call(vm, arena, "greet", &who, 1, &result));
 * // result.as_string is "hello world"
 * ```
 *
 * A plugin: nothing here is compiled unless `-DNYA_PLUGIN_LUA` is set. See plugins.h for why these
 * are not part of base.
 *
 * ## What the engine puts in front of a script
 *
 * A global table `nya`, holding the parts of the engine that make sense to drive from data — logging,
 * spawning and moving entities, reading an input action, asking the time. Deliberately small:
 * everything in it is a function whose failure mode is "nothing happens", and none of it can hand a
 * script a pointer. See `nya_lua_open_engine`, which `NYA_LuaOptions.engine_api` calls for you.
 *
 * A game adds its own with `nya_lua_register`.
 *
 * ## ⚠ Hot reload
 *
 * Two rules, both from the same fact: the game is a `.so` that is unloaded and replaced while the
 * host keeps running (see the hot reload notes in `core_entity.h` and `src/main.c`).
 *
 * 1. **Keep the `NYA_LuaVM*` in host-owned state** — the world, or an engine arena — not in a static
 *    inside the game. A pointer stored in the `.so`'s own data does not survive the reload.
 * 2. **Anything registered with `nya_lua_register` from the game must be registered again after a
 *    reload.** A `lua_CFunction` is an address inside the `.so`, and calling one after the library
 *    has been replaced jumps into an unmapped page. The engine's own bindings live in the host
 *    binary, which is not reloaded, so `nya.*` is unaffected.
 *
 * ## ⚠ Memory
 *
 * LuaJIT allocates through its own allocator rather than through an arena, which is a departure from
 * the rest of the engine and is not a choice: on x64 LuaJIT's garbage collector requires its heap in
 * the low two gigabytes of the address space and provides its own mmap-based allocator to guarantee
 * it. Handing it an arena is documented upstream as unsupported there. Everything on *this* side of
 * the boundary — every NYA_Value handed back — comes from the arena the caller passes.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Arguments one call may pass, and values one may return.
 *
 * A ceiling rather than a growable list because both directions are stack traffic: Lua's own stack
 * has to be grown to hold them, and a call wanting more than this is passing a table.
 * */
#ifndef NYA_LUA_MAX_ARGUMENTS
#define NYA_LUA_MAX_ARGUMENTS 16
#endif

/** How deep a table may nest when converted in either direction. Guards against a self-referencing one. */
#ifndef NYA_LUA_MAX_DEPTH
#define NYA_LUA_MAX_DEPTH 32
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_LuaVM      NYA_LuaVM;
typedef struct NYA_LuaOptions NYA_LuaOptions;
typedef struct NYA_LuaCall    NYA_LuaCall;

/**
 * What a bound C function receives and answers with.
 *
 * Values rather than a Lua stack, so a binding never has to know Lua's API and cannot leave the
 * stack unbalanced — which is the failure mode that makes hand-written bindings hard to trust.
 * */
struct NYA_LuaCall {
    /** Where to allocate anything handed back. Lives as long as the call, not longer. */
    NYA_Arena* arena;

    const NYA_Value* arguments;
    u32              argument_count;

    /** Whatever was passed to nya_lua_register alongside the function. */
    void* user_data;

    /** Fill in and set `result_count`. Leaving it zero returns nothing, which is legal. */
    NYA_Value results[NYA_LUA_MAX_ARGUMENTS];
    u32       result_count;
};

/**
 * A function a script may call.
 *
 * ⚠ **Must be externally linked and named** if it lives in the game rather than the engine — see the
 * hot reload note at the top of this file.
 * */
typedef void (*NYA_LuaFn)(NYA_LuaCall* call);

/** Everything optional about a VM. Every field's zero is its default. */
struct NYA_LuaOptions {
    /**
     * Open Lua's own standard libraries. Zero is **on**, since a script with no `string` or `math` is
     * barely a language; set `no_standard_library` to refuse them.
     * */
    b8 no_standard_library;

    /**
     * Refuse the libraries that reach outside the process: `io`, `os`, `package`, `ffi` and
     * `debug`.
     *
     * What to set for a script that came from somewhere other than the game's own assets — a mod, a
     * level shared between players. It is not a sandbox in the security sense and is not offered as
     * one; LuaJIT's `ffi` alone can call any function in the process, so leaving it reachable makes
     * every other restriction decorative. Removing them raises the floor from "trivially" to
     * "deliberately".
     * */
    b8 restricted;

    /** Put the engine's `nya` table in front of scripts. See nya_lua_open_engine. */
    b8 engine_api;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFECYCLE
 * ─────────────────────────────────────────────────────────
 */

/**
 * Creates a VM. `arena` owns the wrapper; LuaJIT owns its own heap — see the memory note above.
 *
 * ⚠ **There is a `_destroy` here, unlike most modules in this engine**, and it is not optional:
 * freeing the arena releases the wrapper and leaks the entire Lua heap, which is not the arena's to
 * free.
 * */
NYA_API NYA_Error nya_lua_create(NYA_Arena* arena, NYA_LuaOptions options, OUT NYA_LuaVM** out_vm) __attr_no_discard;

/** Closes the VM and everything in it. Harmless on null. */
NYA_API void nya_lua_destroy(NYA_LuaVM* vm);

/*
 * ─────────────────────────────────────────────────────────
 * RUNNING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Compiles and runs `code`. `chunk_name` is what appears in an error message; null becomes "chunk".
 *
 * Returns NYA_ERROR_PARSE for a syntax error and NYA_ERROR_NOT_OK for one raised while running, each
 * carrying Lua's own message — which names the line, so it is worth propagating rather than summarising.
 * */
NYA_API NYA_Error nya_lua_run(NYA_LuaVM* vm, NYA_ConstCString code, NYA_ConstCString chunk_name) __attr_no_discard;

/**
 * The same, for a script that came through the asset system.
 *
 * Which is how a script gets hot reload: the asset system already watches the file, so re-running
 * this when it changes is the whole of it.
 * */
NYA_API NYA_Error nya_lua_run_asset(NYA_LuaVM* vm, NYA_ConstCString asset_handle) __attr_no_discard;

/**
 * Calls a global function by name.
 *
 * Everything handed back is allocated from `arena`. `out_result` may be null for a call whose value
 * is not wanted; a function returning nothing leaves it a zeroed NYA_Value, which reads as
 * NYA_TYPE_NONE.
 *
 * NYA_ERROR_NOT_FOUND when the name is not a function, which is worth distinguishing from a call
 * that failed — a typo in a script and a bug inside one want different reactions.
 * */
NYA_API NYA_Error nya_lua_call(
    NYA_LuaVM*       vm,
    NYA_Arena*       arena,
    NYA_ConstCString function,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) __attr_no_discard;

/** Whether a global of this name exists and is a function. What to ask before calling an optional hook. */
NYA_API b8 nya_lua_has_function(NYA_LuaVM* vm, NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * VALUES
 * ─────────────────────────────────────────────────────────
 */

/** Reads a global into an NYA_Value allocated from `arena`. A table becomes an object or an array. */
NYA_API NYA_Error nya_lua_global_get(NYA_LuaVM* vm, NYA_Arena* arena, NYA_ConstCString name, OUT NYA_Value* out_value) __attr_no_discard;

/** Writes a global. An NYA_Object becomes a table keyed by string; an array becomes one keyed 1..n. */
NYA_API NYA_Error nya_lua_global_set(NYA_LuaVM* vm, NYA_ConstCString name, const NYA_Value* value) __attr_no_discard;

/*
 * ── Constructors, so a call site does not have to fill an NYA_Value by hand ──
 */

NYA_API NYA_Value nya_lua_number(f64 value) __attr_no_discard;
NYA_API NYA_Value nya_lua_integer(s64 value) __attr_no_discard;
NYA_API NYA_Value nya_lua_boolean(b8 value) __attr_no_discard;
NYA_API NYA_Value nya_lua_string(NYA_ConstCString value) __attr_no_discard;
NYA_API NYA_Value nya_lua_nil(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * BINDING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Makes `fn` callable from Lua as a global named `name`.
 *
 * ```c
 * void game_score_add(NYA_LuaCall* call) {
 *     if (call->argument_count < 1) return;
 *     score += (s64)call->arguments[0].as_real;
 * }
 *
 * nya_lua_register(vm, "score_add", game_score_add, nullptr);
 * ```
 *
 * ⚠ **Re-register after a hot reload** if `fn` lives in the game `.so`. See the note at the top.
 * */
NYA_API void nya_lua_register(NYA_LuaVM* vm, NYA_ConstCString name, NYA_LuaFn fn, void* user_data);

/**
 * Puts the engine's own `nya` table in front of scripts. Called for you by `NYA_LuaOptions.engine_api`.
 *
 * | Lua | Does |
 * | --- | --- |
 * | `nya.log(text)` · `nya.warn(text)` · `nya.error(text)` | writes through the engine's logger, so a script's output lands in `logs/` with everything else |
 * | `nya.time()` | seconds since the app started |
 * | `nya.spawn{ name=, x=, y=, z=, type= }` | spawns an entity, returning its handle as two numbers packed into a table `{ index=, generation= }` |
 * | `nya.despawn(handle)` | despawns one, deferred to the simulation barrier |
 * | `nya.position(handle)` | `{ x=, y=, z= }`, or nil for a handle that no longer resolves |
 * | `nya.move_to(handle, x, y, z, duration)` | an eased move, through core_tween |
 * | `nya.action(name)` | whether an input action is held |
 * | `nya.action_pressed(name)` | whether it went down this frame |
 *
 * ⚠ **A handle is a value, not a reference.** A script holding one across a despawn gets nil from
 * every call that takes it, exactly as C does — which is the property generational handles exist for
 * and the reason a script is never handed a pointer.
 * */
NYA_API void nya_lua_open_engine(NYA_LuaVM* vm);

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/** Bytes LuaJIT currently has allocated. For an overlay, and for noticing a script that leaks. */
NYA_API u64 nya_lua_memory_bytes(const NYA_LuaVM* vm) __attr_no_discard;

/** Runs a full garbage collection cycle. Rarely wanted; the collector is incremental on its own. */
NYA_API void nya_lua_collect(NYA_LuaVM* vm);
