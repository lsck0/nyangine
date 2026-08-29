/**
 * @file lua.c
 *
 * The LuaJIT binding. See lua.h for the surface and for the two hot-reload rules.
 *
 * The whole file is a translation between Lua's stack and NYA_Value, plus the bookkeeping that keeps
 * the stack balanced across it. Nothing here is clever; the care is all in making sure that every
 * path — including every failure path — pops exactly what it pushed, because a leaked stack slot is
 * invisible until the hundredth call and then it is a stack overflow inside a script.
 * */
#include "nyangine/nyangine.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One function a game bound in. Held in the VM so the trampoline can find it by index. */
typedef struct {
    NYA_LuaFn fn;
    void*     user_data;
} _NYA_LuaBinding;

/** Bindings one VM may hold. Warns and refuses past this rather than growing. */
#ifndef NYA_LUA_MAX_BINDINGS
#define NYA_LUA_MAX_BINDINGS 128
#endif

struct NYA_LuaVM {
    lua_State* state;

    /** The arena the wrapper came from. Not Lua's heap — see the memory note in lua.h. */
    NYA_Arena* arena;

    _NYA_LuaBinding bindings[NYA_LUA_MAX_BINDINGS];
    u32             binding_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A NUL-terminated copy of `text` in `arena`.
 *
 * base_string works in NYA_String, and everything crossing this boundary is a C string — going
 * through NYA_String to copy one would allocate twice and convert twice.
 * */
NYA_INTERNAL NYA_CString _nya_lua_clone_cstring(NYA_Arena* arena, NYA_ConstCString text) {
    if (text == nullptr) return nullptr;

    u64         length = strlen(text) + 1;
    NYA_CString copy   = nya_arena_alloc(arena, length);

    nya_memcpy(copy, text, length);

    return copy;
}

/** Pushes one NYA_Value onto the stack. `depth` guards a table that contains itself. */
NYA_INTERNAL void _nya_lua_push(lua_State* state, const NYA_Value* value, u32 depth);

/** Reads the value at `index` into `out`, allocating from `arena`. */
NYA_INTERNAL void _nya_lua_read(lua_State* state, NYA_Arena* arena, s32 index, u32 depth, OUT NYA_Value* out);

/** The one C function every binding goes through. Finds its NYA_LuaFn from the upvalue. */
NYA_INTERNAL int _nya_lua_trampoline(lua_State* state);

/** Turns whatever Lua left on the stack into an NYA_Error of `kind`, and pops it. */
NYA_INTERNAL NYA_Error _nya_lua_take_error(lua_State* state, NYA_ErrorKind kind, NYA_ConstCString what) {
    NYA_ConstCString message = lua_tostring(state, -1);

    NYA_Error error = nya_error(kind, "%s: %s", what, message != nullptr ? message : "(no message)");

    lua_pop(state, 1);

    return error;
}

void _nya_lua_push(lua_State* state, const NYA_Value* value, u32 depth) {
    if (value == nullptr || depth > NYA_LUA_MAX_DEPTH) {
        lua_pushnil(state);
        return;
    }

    switch (value->type) {
        case NYA_TYPE_NULL:
        case NYA_TYPE_VOID: lua_pushnil(state); break;

        case NYA_TYPE_B8: lua_pushboolean(state, value->as_b8 != 0); break;
        case NYA_TYPE_B16: lua_pushboolean(state, value->as_b16 != 0); break;
        case NYA_TYPE_B32: lua_pushboolean(state, value->as_b32 != 0); break;
        case NYA_TYPE_B64: lua_pushboolean(state, value->as_b64 != 0); break;

        /*
         * Everything numeric becomes a Lua number, which is a double.
         *
         * ⚠ Integers past 2^53 lose their low bits doing so, and there is nowhere else for them to
         * go: Lua 5.1 — which is what LuaJIT is — has exactly one number type. An entity handle is
         * two u32s for this reason, passed as a table rather than packed into one number.
         */
        case NYA_TYPE_U8: lua_pushnumber(state, (lua_Number)value->as_u8); break;
        case NYA_TYPE_U16: lua_pushnumber(state, (lua_Number)value->as_u16); break;
        case NYA_TYPE_U32: lua_pushnumber(state, (lua_Number)value->as_u32); break;
        case NYA_TYPE_U64: lua_pushnumber(state, (lua_Number)value->as_u64); break;
        case NYA_TYPE_S8: lua_pushnumber(state, (lua_Number)value->as_s8); break;
        case NYA_TYPE_S16: lua_pushnumber(state, (lua_Number)value->as_s16); break;
        case NYA_TYPE_S32: lua_pushnumber(state, (lua_Number)value->as_s32); break;
        case NYA_TYPE_S64: lua_pushnumber(state, (lua_Number)value->as_s64); break;
        case NYA_TYPE_F16: lua_pushnumber(state, (lua_Number)value->as_f16); break;
        case NYA_TYPE_F32: lua_pushnumber(state, (lua_Number)value->as_f32); break;
        case NYA_TYPE_F64: lua_pushnumber(state, (lua_Number)value->as_f64); break;

        case NYA_TYPE_CHAR: lua_pushlstring(state, &value->as_char, 1); break;

        case NYA_TYPE_STRING: {
            if (value->as_string != nullptr) lua_pushstring(state, value->as_string);
            else lua_pushnil(state);
        } break;

        case NYA_TYPE_OBJECT: {
            lua_newtable(state);

            // Cast away const: nya_dict_foreach_key walks a mutable dict, and nothing in the loop
            // writes to it.
            NYA_Object* object = (NYA_Object*)&value->as_object;

            // The macro walks the key *slots*, so `key` is a pointer to one.
            nya_dict_foreach_key (object, key) {
                NYA_Value* entry = nya_object_get(object, *key);
                if (entry == nullptr) continue;

                _nya_lua_push(state, entry, depth + 1);
                lua_setfield(state, -2, *key);
            }
        } break;

        case NYA_TYPE_ARRAY: {
            lua_newtable(state);

            // Keyed from one, which is what makes it an array to `#` and to ipairs rather than a
            // table that happens to have numeric keys.
            for (u64 i = 0; i < value->as_array.length; i++) {
                _nya_lua_push(state, &value->as_array.items[i], depth + 1);
                lua_rawseti(state, -2, (int)(i + 1));
            }
        } break;

        default: lua_pushnil(state); break;
    }
}

/**
 * Whether a table is a dense 1..n sequence — which is what decides array against object.
 *
 * ⚠ **`lua_objlen` alone cannot answer this**, and trusting it silently drops values. It returns a
 * *border*: an `n` where `t[n]` is non-nil and `t[n+1]` is nil. For `{ [1]='a', [3]='c' }` that is
 * 1, so a check that only walks 1..length finds every key it looked for, calls the table a sequence,
 * and converts it to a one-element array — losing `[3]` with nothing to say about it.
 *
 * So the entries are counted as well. A table is a sequence when it has a positive border, every key
 * from 1 to it is present, **and it holds nothing else**.
 * */
NYA_INTERNAL b8 _nya_lua_is_sequence(lua_State* state, s32 index) {
    u64 length = (u64)lua_objlen(state, index);
    if (length == 0) return false;

    for (u64 i = 1; i <= length; i++) {
        lua_rawgeti(state, index, (int)i);

        b8 present = !lua_isnil(state, -1);
        lua_pop(state, 1);

        if (!present) return false;
    }

    // Everything the table actually holds, which is the half the border does not tell you. Stops as
    // soon as it is over, so a large keyed table is not walked in full to be rejected.
    u64 entries = 0;

    lua_pushnil(state);

    while (lua_next(state, index) != 0) {
        entries++;

        // The value; the key stays for the next lua_next.
        lua_pop(state, 1);

        if (entries > length) {
            // Popped explicitly: breaking out of lua_next leaves the key on the stack, and leaving it
            // there is precisely the imbalance this file's header is about.
            lua_pop(state, 1);
            return false;
        }
    }

    return entries == length;
}

void _nya_lua_read(lua_State* state, NYA_Arena* arena, s32 index, u32 depth, OUT NYA_Value* out) {
    *out = (NYA_Value){ .type = NYA_TYPE_NULL };

    // Absolute, because everything below pushes onto the stack and a negative index would then name
    // something different from what was asked for.
    s32 absolute = index < 0 ? lua_gettop(state) + index + 1 : index;

    switch (lua_type(state, absolute)) {
        case LUA_TNIL:
        case LUA_TNONE: *out = (NYA_Value){ .type = NYA_TYPE_NULL }; break;

        case LUA_TBOOLEAN: *out = (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = lua_toboolean(state, absolute) ? true : false }; break;

        case LUA_TNUMBER: *out = (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)lua_tonumber(state, absolute) }; break;

        case LUA_TSTRING: {
            // Copied into the arena: Lua owns its strings and collects them, so the pointer it hands
            // back is only valid while the value is still on the stack.
            NYA_ConstCString text = lua_tostring(state, absolute);

            *out = (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _nya_lua_clone_cstring(arena, text) };
        } break;

        case LUA_TTABLE: {
            // Refused rather than followed: a table that contains itself would otherwise recurse
            // until the stack ran out, and a script can build one in two lines.
            if (depth > NYA_LUA_MAX_DEPTH) {
                *out = (NYA_Value){ .type = NYA_TYPE_NULL };
                break;
            }

            if (_nya_lua_is_sequence(state, absolute)) {
                NYA_ArrayᐸNYA_Valueᐳ* items = nya_array_create(arena, NYA_Value);

                u64 length = (u64)lua_objlen(state, absolute);

                for (u64 i = 1; i <= length; i++) {
                    lua_rawgeti(state, absolute, (int)i);

                    NYA_Value item = { 0 };
                    _nya_lua_read(state, arena, -1, depth + 1, &item);
                    nya_array_push_back(items, item);

                    lua_pop(state, 1);
                }

                *out = (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *items };
                break;
            }

            NYA_Object object = nya_object_create_on_stack(arena);

            lua_pushnil(state);

            while (lua_next(state, absolute) != 0) {
                /*
                 * The key is read as a string, and it has to be copied before `lua_tostring` is
                 * allowed anywhere near it: converting a *number* key to a string in place would
                 * modify the key still sitting on the stack, and `lua_next` then loses its place and
                 * walks the table forever. Hence the type check rather than an unconditional convert.
                 */
                if (lua_type(state, -2) == LUA_TSTRING) {
                    NYA_CString key = _nya_lua_clone_cstring(arena, lua_tostring(state, -2));

                    NYA_Value entry = { 0 };
                    _nya_lua_read(state, arena, -1, depth + 1, &entry);

                    nya_object_set(&object, key, entry);
                }

                lua_pop(state, 1);
            }

            *out = (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = object };
        } break;

        default:
            // A function, a userdata, a thread, a cdata. None of these has an NYA_Value form and
            // handing one back as a pointer is exactly what this boundary exists to prevent.
            *out = (NYA_Value){ .type = NYA_TYPE_NULL };
            break;
    }
}

int _nya_lua_trampoline(lua_State* state) {
    NYA_LuaVM* vm    = lua_touserdata(state, lua_upvalueindex(1));
    u32        index = (u32)lua_tointeger(state, lua_upvalueindex(2));

    if (vm == nullptr || index >= vm->binding_count) return 0;

    _NYA_LuaBinding* binding = &vm->bindings[index];
    if (binding->fn == nullptr) return 0;

    /*
     * A stack arena for the call's values, destroyed the moment it returns.
     *
     * Not the VM's arena: a binding called sixty times a second for an hour would otherwise grow it
     * without bound, since an arena frees all at once or not at all. A stack arena is exactly the
     * right lifetime — everything a binding is handed dies with the call, which is what lua.h says
     * about NYA_LuaCall.arena.
     */
    NYA_Arena scratch_arena = nya_arena_create_on_stack(.name = "lua_call");
    NYA_Arena* scratch      = &scratch_arena;

    NYA_LuaCall call = { .arena = scratch, .user_data = binding->user_data };

    s32 argument_count = lua_gettop(state);
    if (argument_count > (s32)NYA_LUA_MAX_ARGUMENTS) argument_count = (s32)NYA_LUA_MAX_ARGUMENTS;

    NYA_Value arguments[NYA_LUA_MAX_ARGUMENTS] = { 0 };

    for (s32 i = 0; i < argument_count; i++) _nya_lua_read(state, scratch, i + 1, 0, &arguments[i]);

    call.arguments      = arguments;
    call.argument_count = (u32)argument_count;

    binding->fn(&call);

    u32 results = call.result_count < NYA_LUA_MAX_ARGUMENTS ? call.result_count : NYA_LUA_MAX_ARGUMENTS;

    for (u32 i = 0; i < results; i++) _nya_lua_push(state, &call.results[i], 0);

    // After the results are pushed, since they were built in it. Lua has copied everything it needs
    // onto its own stack by now.
    nya_arena_destroy_on_stack(scratch);

    return (int)results;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_lua_create(NYA_Arena* arena, NYA_LuaOptions options, OUT NYA_LuaVM** out_vm) {
    nya_assert(arena != nullptr && out_vm != nullptr);

    *out_vm = nullptr;

    /*
     * luaL_newstate, not lua_newstate with an arena allocator.
     *
     * On x64 LuaJIT's collector needs its heap in the low two gigabytes of the address space and
     * ships its own mmap-based allocator to guarantee that; a custom allocator there is documented
     * upstream as unsupported and fails at runtime rather than at build time. See the memory note in
     * lua.h — this is the one place in the engine that is not arena-backed, and it is not a choice.
     */
    lua_State* state = luaL_newstate();
    if (state == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "could not create a Lua state");

    if (!options.no_standard_library) {
        luaL_openlibs(state);

        if (options.restricted) {
            /*
             * Removed after opening rather than opened selectively, because LuaJIT's luaL_openlibs
             * is a single call and the alternative is naming every library that *should* stay.
             *
             * ⚠ Not a sandbox, and lua.h says so where a caller will read it. `ffi` alone can call
             * any function in the process, so leaving it reachable would make every other line here
             * decorative — but a script that has been *given* a way back in through a binding is
             * still inside whatever that binding allows.
             */
            NYA_ConstCString removed[] = { "io", "os", "package", "ffi", "debug" };

            for (u32 i = 0; i < nya_carray_length(removed); i++) {
                lua_pushnil(state);
                lua_setglobal(state, removed[i]);
            }
        }
    }

    NYA_LuaVM* vm = nya_arena_alloc(arena, sizeof(NYA_LuaVM));

    *vm = (NYA_LuaVM){ .state = state, .arena = arena };

    if (options.engine_api) nya_lua_open_engine(vm);

    *out_vm = vm;

    // Registered against whichever VM is created first. Binding counts are per-VM and a game can
    // create more than one, but in practice a VM lives as long as the world that owns it — the same
    // lifetime guarantee the sim and config ceilings lean on — so the common case of one long-lived
    // VM is exactly what this points at. Guarded so a test creating many short-lived VMs does not
    // add a copy of itself on every one.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("lua_bindings", NYA_LUA_MAX_BINDINGS, &vm->binding_count);
        ceiling_registered = true;
    }

    nya_log_info("Lua VM created (%s%s).", options.no_standard_library ? "no standard library" : "standard library",
                 options.restricted ? ", restricted" : "");

    return NYA_OK;
}

void nya_lua_destroy(NYA_LuaVM* vm) {
    if (vm == nullptr || vm->state == nullptr) return;

    lua_close(vm->state);

    // The wrapper itself belongs to the arena and is not freed here; zeroing the state is what makes
    // a second destroy a no-op rather than a double close.
    vm->state         = nullptr;
    vm->binding_count = 0;
}

NYA_Error nya_lua_run(NYA_LuaVM* vm, NYA_ConstCString code, NYA_ConstCString chunk_name) {
    if (vm == nullptr || vm->state == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no Lua VM");
    if (code == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no code to run");

    NYA_ConstCString name = chunk_name != nullptr ? chunk_name : "chunk";

    // Compiled first, so a syntax error is distinguishable from one raised while running. The two
    // want different reactions: one is a typo, the other is a bug.
    if (luaL_loadbuffer(vm->state, code, strlen(code), name) != 0) {
        return _nya_lua_take_error(vm->state, NYA_ERROR_PARSE, "Lua syntax error");
    }

    if (lua_pcall(vm->state, 0, 0, 0) != 0) {
        return _nya_lua_take_error(vm->state, NYA_ERROR_NOT_OK, "Lua error");
    }

    return NYA_OK;
}

NYA_Error nya_lua_run_asset(NYA_LuaVM* vm, NYA_ConstCString asset_handle) {
    if (vm == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no Lua VM");

    NYA_Asset* asset = nya_asset_get((NYA_CString)asset_handle);

    if (asset == nullptr) {
        // Queued rather than failed: this is the first ask, and the asset system resolves it over the
        // next frames like every other type. A caller re-running on reload asks again then.
        NYA_TRY(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = (NYA_CString)asset_handle }));

        return nya_error(NYA_ERROR_NOT_FOUND, "script '%s' is still loading", asset_handle);
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return nya_error(NYA_ERROR_NOT_FOUND, "script '%s' is not loaded", asset_handle);

    if (luaL_loadbuffer(vm->state, (const char*)asset->as_text.data, (size_t)asset->as_text.size, asset_handle) != 0) {
        return _nya_lua_take_error(vm->state, NYA_ERROR_PARSE, "Lua syntax error");
    }

    if (lua_pcall(vm->state, 0, 0, 0) != 0) return _nya_lua_take_error(vm->state, NYA_ERROR_NOT_OK, "Lua error");

    return NYA_OK;
}

b8 nya_lua_has_function(NYA_LuaVM* vm, NYA_ConstCString name) {
    if (vm == nullptr || vm->state == nullptr || name == nullptr) return false;

    lua_getglobal(vm->state, name);

    b8 is_function = lua_isfunction(vm->state, -1) ? true : false;
    lua_pop(vm->state, 1);

    return is_function;
}

NYA_Error nya_lua_call(
    NYA_LuaVM*       vm,
    NYA_Arena*       arena,
    NYA_ConstCString function,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) {
    if (vm == nullptr || vm->state == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no Lua VM");
    if (function == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no function named");

    if (out_result != nullptr) *out_result = (NYA_Value){ .type = NYA_TYPE_NULL };

    if (argument_count > NYA_LUA_MAX_ARGUMENTS) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Lua call takes at most %d arguments, got " FMTu32, NYA_LUA_MAX_ARGUMENTS,
                         argument_count);
    }

    lua_getglobal(vm->state, function);

    if (!lua_isfunction(vm->state, -1)) {
        lua_pop(vm->state, 1);

        // Distinguished from a call that failed: a name that is not a function is a typo in a script
        // or an optional hook that was not defined, and neither is an error worth a backtrace.
        return nya_error(NYA_ERROR_NOT_FOUND, "'%s' is not a Lua function", function);
    }

    for (u32 i = 0; i < argument_count; i++) _nya_lua_push(vm->state, &arguments[i], 0);

    // One result asked for regardless: Lua pads with nil, so a function returning nothing costs one
    // stack slot rather than a second code path.
    if (lua_pcall(vm->state, (int)argument_count, 1, 0) != 0) {
        return _nya_lua_take_error(vm->state, NYA_ERROR_NOT_OK, "Lua error");
    }

    if (out_result != nullptr && arena != nullptr) _nya_lua_read(vm->state, arena, -1, 0, out_result);

    lua_pop(vm->state, 1);

    return NYA_OK;
}

NYA_Error nya_lua_global_get(NYA_LuaVM* vm, NYA_Arena* arena, NYA_ConstCString name, OUT NYA_Value* out_value) {
    if (vm == nullptr || vm->state == nullptr || name == nullptr || out_value == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_lua_global_get needs a VM, a name and somewhere to write");
    }

    lua_getglobal(vm->state, name);
    _nya_lua_read(vm->state, arena, -1, 0, out_value);
    lua_pop(vm->state, 1);

    return NYA_OK;
}

NYA_Error nya_lua_global_set(NYA_LuaVM* vm, NYA_ConstCString name, const NYA_Value* value) {
    if (vm == nullptr || vm->state == nullptr || name == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "nya_lua_global_set needs a VM and a name");
    }

    _nya_lua_push(vm->state, value, 0);
    lua_setglobal(vm->state, name);

    return NYA_OK;
}

NYA_Value nya_lua_number(f64 value) {
    return (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = value };
}

NYA_Value nya_lua_integer(s64 value) {
    return (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = value };
}

NYA_Value nya_lua_boolean(b8 value) {
    return (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = value };
}

NYA_Value nya_lua_string(NYA_ConstCString value) {
    return (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)value };
}

NYA_Value nya_lua_nil(void) {
    return (NYA_Value){ .type = NYA_TYPE_NULL };
}

void nya_lua_register(NYA_LuaVM* vm, NYA_ConstCString name, NYA_LuaFn fn, void* user_data) {
    if (vm == nullptr || vm->state == nullptr || name == nullptr || fn == nullptr) return;

    if (vm->binding_count >= NYA_LUA_MAX_BINDINGS) {
        nya_log_warn("No free Lua binding slot for '%s'; %d are in use.", name, NYA_LUA_MAX_BINDINGS);
        return;
    }

    u32 index = vm->binding_count++;

    vm->bindings[index] = (_NYA_LuaBinding){ .fn = fn, .user_data = user_data };

    /*
     * Both upvalues rather than a pointer to the binding.
     *
     * An index into the VM's table survives the table being written to; a captured pointer into it
     * would not if the array ever moved. It does not today — it is fixed size — and the index costs
     * nothing, so this is the version that stays correct if that changes.
     */
    lua_pushlightuserdata(vm->state, vm);
    lua_pushinteger(vm->state, (lua_Integer)index);
    lua_pushcclosure(vm->state, _nya_lua_trampoline, 2);
    lua_setglobal(vm->state, name);
}

u64 nya_lua_memory_bytes(const NYA_LuaVM* vm) {
    if (vm == nullptr || vm->state == nullptr) return 0;

    // Kilobytes from Lua, bytes out: every other size in this engine is in bytes, and a unit that
    // changes at a module boundary is how a memory overlay ends up off by a thousand.
    return (u64)lua_gc((lua_State*)vm->state, LUA_GCCOUNT, 0) * 1024ULL;
}

void nya_lua_collect(NYA_LuaVM* vm) {
    if (vm == nullptr || vm->state == nullptr) return;

    (void)lua_gc(vm->state, LUA_GCCOLLECT, 0);
}
