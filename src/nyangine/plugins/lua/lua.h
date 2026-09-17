/**
 * @file lua.h
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
 * ## Hot reload
 *
 * Lua keeps the raw C function pointer of every registered binding. A binding that lives in the game library
 * points into code a reload unmaps, so register game bindings again after every reload. Engine bindings live in
 * the executable and survive.
 *
 * ## Memory
 *
 * The VM's heap is LuaJIT's, not an arena. On x64 LuaJIT's collector needs its heap in the low 2 GB and ships its
 * own allocator to guarantee that; a custom allocator is unsupported there. The arena only owns the wrapper.
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
 * A function a script may call. If it lives in the game library, re-register it after a hot reload; see the
 * file header.
 * */
typedef void (*NYA_LuaFn)(NYA_LuaCall* call);

/** Everything optional about a VM. Every field's zero is its default. */
struct NYA_LuaOptions {
    /**
     * Open Lua's standard libraries. Zero means on, since a script without `string` or `math` is barely a
     * language; set `no_standard_library` to refuse them.
     * */
    b8 no_standard_library;

    /**
     * Refuse the libraries that reach outside the process: `io`, `os`, `package`, `ffi` and
     * `debug`.
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
 * Creates a VM. `arena` owns the wrapper; LuaJIT owns its heap (see the memory note above). nya_lua_destroy is
 * required: freeing the arena alone leaks the whole Lua heap.
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
 * */
NYA_API NYA_Error nya_lua_run(NYA_LuaVM* vm, NYA_ConstCString code, NYA_ConstCString chunk_name) __attr_no_discard;

/**
 * The same, for a script that came through the asset system.
 * */
NYA_API NYA_Error nya_lua_run_asset(NYA_LuaVM* vm, NYA_ConstCString asset_handle) __attr_no_discard;

/**
 * Calls a global function by name.
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

/* Constructors, so a call site does not fill an NYA_Value by hand. */

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
 * Re-register after a hot reload if `fn` lives in the game library.
 * */
NYA_API void nya_lua_register(NYA_LuaVM* vm, NYA_ConstCString name, NYA_LuaFn fn, void* user_data);

/**
 * Puts the engine's `nya` table in front of scripts. Called for you by `NYA_LuaOptions.engine_api`. Handles are
 * values: after a despawn every call taking one returns nil, as in C.
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

#ifdef NYA_TESTING
/** Values left on the Lua stack. Zero between calls; test-only, since nothing else can tell an imbalance from use. */
NYA_INTERNAL s32 _nya_lua_stack_depth_for_test(const NYA_LuaVM* vm);
#endif
