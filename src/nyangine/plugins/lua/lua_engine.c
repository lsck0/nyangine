/**
 * @file lua_engine.c
 *
 * The `nya` table a script sees. See `nya_lua_open_engine` in lua.h for the list.
 *
 * **What is in here and what is not is the whole design.** Every binding below is a function whose
 * worst outcome is that nothing happens: a bad handle answers nil, a missing argument is a default,
 * an unknown action is not held. None of them hands a script a pointer, and none of them can leave
 * the engine in a state C did not ask for.
 *
 * That rules out the obvious conveniences. There is no `entity(handle)` returning something a script
 * can write fields on, because that is a pointer with extra steps and it would outlive the entity.
 * A script reads a position and asks for a move; it does not hold an entity.
 *
 * These live in the **host** binary rather than in the game `.so`, which is why `nya.*` survives a
 * hot reload while a game's own bindings do not — see the note in lua.h.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** An argument as a number, or `fallback` when it is absent or not one. */
NYA_INTERNAL f64 _nya_lua_argument_number(const NYA_LuaCall* call, u32 index, f64 fallback) {
    if (index >= call->argument_count) return fallback;

    const NYA_Value* value = &call->arguments[index];

    // Only F64 and S64 are ever produced by the reader, but a value can also arrive from
    // nya_lua_global_set, and a caller building one by hand may reasonably use any width.
    switch (value->type) {
        case NYA_TYPE_F64: return value->as_f64;
        case NYA_TYPE_F32: return (f64)value->as_f32;
        case NYA_TYPE_S64: return (f64)value->as_s64;
        case NYA_TYPE_S32: return (f64)value->as_s32;
        case NYA_TYPE_U64: return (f64)value->as_u64;
        case NYA_TYPE_U32: return (f64)value->as_u32;
        default: return fallback;
    }
}

/** An argument as a string, or null. */
NYA_INTERNAL NYA_ConstCString _nya_lua_argument_string(const NYA_LuaCall* call, u32 index) {
    if (index >= call->argument_count) return nullptr;
    if (call->arguments[index].type != NYA_TYPE_STRING) return nullptr;

    return call->arguments[index].as_string;
}

/** A field of a table argument, as a number. */
NYA_INTERNAL f64 _nya_lua_field_number(const NYA_LuaCall* call, u32 index, NYA_ConstCString key, f64 fallback) {
    if (index >= call->argument_count || call->arguments[index].type != NYA_TYPE_OBJECT) return fallback;

    NYA_Value* field = nya_object_get(&call->arguments[index].as_object, (NYA_CString)key);
    if (field == nullptr) return fallback;

    NYA_LuaCall one = { .arguments = field, .argument_count = 1 };

    return _nya_lua_argument_number(&one, 0, fallback);
}

/** A field of a table argument, as a string. */
NYA_INTERNAL NYA_ConstCString _nya_lua_field_string(const NYA_LuaCall* call, u32 index, NYA_ConstCString key) {
    if (index >= call->argument_count || call->arguments[index].type != NYA_TYPE_OBJECT) return nullptr;

    NYA_Value* field = nya_object_get(&call->arguments[index].as_object, (NYA_CString)key);
    if (field == nullptr || field->type != NYA_TYPE_STRING) return nullptr;

    return field->as_string;
}

/**
 * An entity handle out of a `{ index =, generation = }` table.
 *
 * ⚠ **Two numbers rather than one**, deliberately. A handle is two u32s and Lua's only number is a
 * double: packing them into one would be exact today at 53 bits of mantissa and would stop being so
 * the moment either field widened, and the failure would be a handle that silently resolves to the
 * wrong entity rather than to none.
 * */
NYA_INTERNAL NYA_EntityHandle _nya_lua_argument_handle(const NYA_LuaCall* call, u32 index) {
    return (NYA_EntityHandle){
        .index      = (u32)_nya_lua_field_number(call, index, "index", 0.0),
        .generation = (u32)_nya_lua_field_number(call, index, "generation", 0.0),
    };
}

/** A handle as the table a script reads it back as. */
NYA_INTERNAL NYA_Value _nya_lua_handle_value(NYA_Arena* arena, NYA_EntityHandle handle) {
    NYA_Object table = nya_object_create_on_stack(arena);

    nya_object_set(&table, "index", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)handle.index });
    nya_object_set(&table, "generation", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)handle.generation });

    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = table };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BINDINGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Externally linked and named, not static — the hot reload path re-resolves callbacks by name, and a
 * static one has no name to resolve. See the conventions note in RESEARCH.md §17.
 */

void nya_lua_binding_log(NYA_LuaCall* call) {
    NYA_ConstCString text = _nya_lua_argument_string(call, 0);
    if (text != nullptr) nya_log_info("[lua] %s", text);
}

void nya_lua_binding_warn(NYA_LuaCall* call) {
    NYA_ConstCString text = _nya_lua_argument_string(call, 0);
    if (text != nullptr) nya_log_warn("[lua] %s", text);
}

void nya_lua_binding_error(NYA_LuaCall* call) {
    NYA_ConstCString text = _nya_lua_argument_string(call, 0);
    if (text != nullptr) nya_log_error("[lua] %s", text);
}

void nya_lua_binding_time(NYA_LuaCall* call) {
    call->results[0]   = nya_lua_number((f64)nya_app_get()->frame_stats.uptime_s);
    call->result_count = 1;
}

void nya_lua_binding_spawn(NYA_LuaCall* call) {
    /*
     * A table, not positional arguments.
     *
     * `nya.spawn{ name = "coin", x = 40, y = 12 }` reads at the call site the way
     * nya_entity_spawn's designated initialisers read in C, and it means adding a field here never
     * changes what an existing script means.
     */
    NYA_EntityHandle handle = nya_entity_spawn(
        .name     = _nya_lua_field_string(call, 0, "name"),
        .type     = (u32)_nya_lua_field_number(call, 0, "type", 0.0),
        .position = { (f32)_nya_lua_field_number(call, 0, "x", 0.0), (f32)_nya_lua_field_number(call, 0, "y", 0.0),
                      (f32)_nya_lua_field_number(call, 0, "z", 0.0) },
        .scale    = { 1.0F, 1.0F, 1.0F },
        .state    = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE
    );

    call->results[0]   = _nya_lua_handle_value(call->arena, handle);
    call->result_count = 1;
}

void nya_lua_binding_despawn(NYA_LuaCall* call) {
    // Deferred, which is what a script wants: it may well be running from inside an update, and the
    // barrier is what makes removing something mid-iteration safe.
    nya_entity_despawn_deferred(_nya_lua_argument_handle(call, 0));
}

void nya_lua_binding_position(NYA_LuaCall* call) {
    NYA_Entity* entity = nya_entity_get(_nya_lua_argument_handle(call, 0));

    // Nil for a handle that no longer resolves, which is the whole point of handing scripts handles
    // rather than anything they could dereference.
    if (entity == nullptr) {
        call->result_count = 0;
        return;
    }

    NYA_Object position = nya_object_create_on_stack(call->arena);

    nya_object_set(&position, "x", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)entity->position.x });
    nya_object_set(&position, "y", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)entity->position.y });
    nya_object_set(&position, "z", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)entity->position.z });

    call->results[0]   = (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = position };
    call->result_count = 1;
}

void nya_lua_binding_move_to(NYA_LuaCall* call) {
    NYA_Entity* entity = nya_entity_get(_nya_lua_argument_handle(call, 0));
    if (entity == nullptr) return;

    f32x3 target = {
        (f32)_nya_lua_argument_number(call, 1, (f64)entity->position.x),
        (f32)_nya_lua_argument_number(call, 2, (f64)entity->position.y),
        (f32)_nya_lua_argument_number(call, 3, (f64)entity->position.z),
    };

    // A missing duration is a teleport, which is what nya_entity_move_to already means by zero.
    nya_entity_move_to(entity, target, (f32)_nya_lua_argument_number(call, 4, 0.0), NYA_EASE_CUBIC_OUT);
}

/*
 * ⚠ **An action is a number, not a name.**
 *
 * NYA_InputAction is an enum the *game* defines — GNY_ACTION_JUMP and the rest — so there is no name
 * table to look one up in, and inventing one here would mean the engine holding a registry that
 * exists only for scripts. A game that wants names in its scripts exposes its own enum to Lua as a
 * table, which is one line of generated Lua and keeps the two definitions in one place.
 */
void nya_lua_binding_action(NYA_LuaCall* call) {
    NYA_InputAction action = (NYA_InputAction)(u32)_nya_lua_argument_number(call, 0, -1.0);

    call->results[0]   = nya_lua_boolean(call->argument_count > 0 && nya_input_action_pressed(action));
    call->result_count = 1;
}

void nya_lua_binding_action_pressed(NYA_LuaCall* call) {
    NYA_InputAction action = (NYA_InputAction)(u32)_nya_lua_argument_number(call, 0, -1.0);

    call->results[0]   = nya_lua_boolean(call->argument_count > 0 && nya_input_action_just_pressed(action));
    call->result_count = 1;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_lua_open_engine(NYA_LuaVM* vm) {
    if (vm == nullptr) return;

    /*
     * Registered as flat globals and then gathered into a table by a line of Lua.
     *
     * nya_lua_register sets a global, which is the only shape the binding trampoline supports — and
     * building the table in Lua rather than teaching the trampoline about tables keeps that one
     * mechanism instead of two. The temporary globals are cleared afterwards, so a script sees only
     * `nya`.
     */
    struct {
        NYA_ConstCString lua_name;
        NYA_ConstCString global;
        NYA_LuaFn        fn;
    } entries[] = {
        { "log", "_nya_log", nya_lua_binding_log },
        { "warn", "_nya_warn", nya_lua_binding_warn },
        { "error", "_nya_error", nya_lua_binding_error },
        { "time", "_nya_time", nya_lua_binding_time },
        { "spawn", "_nya_spawn", nya_lua_binding_spawn },
        { "despawn", "_nya_despawn", nya_lua_binding_despawn },
        { "position", "_nya_position", nya_lua_binding_position },
        { "move_to", "_nya_move_to", nya_lua_binding_move_to },
        { "action", "_nya_action", nya_lua_binding_action },
        { "action_pressed", "_nya_action_pressed", nya_lua_binding_action_pressed },
    };

    for (u32 i = 0; i < nya_carray_length(entries); i++) nya_lua_register(vm, entries[i].global, entries[i].fn, nullptr);

    NYA_ConstCString gather =
        "nya = {"
        "  log = _nya_log, warn = _nya_warn, error = _nya_error,"
        "  time = _nya_time,"
        "  spawn = _nya_spawn, despawn = _nya_despawn,"
        "  position = _nya_position, move_to = _nya_move_to,"
        "  action = _nya_action, action_pressed = _nya_action_pressed,"
        "}"
        "_nya_log, _nya_warn, _nya_error, _nya_time = nil, nil, nil, nil "
        "_nya_spawn, _nya_despawn, _nya_position, _nya_move_to = nil, nil, nil, nil "
        "_nya_action, _nya_action_pressed = nil, nil";

    NYA_Error result = nya_lua_run(vm, gather, "nya_lua_open_engine");

    // Not propagated: the string is a literal in this file, so a failure is a mistake here rather
    // than anything a caller can act on — but it must not be silent, or `nya` is simply missing.
    if (!result.ok) nya_log_error("Could not build the Lua `nya` table: %s", result.message);
}
