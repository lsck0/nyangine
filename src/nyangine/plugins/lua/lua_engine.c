/**
 * @file lua_engine.c
 * */
#include "nyangine/nyangine.h"

// ───────────────────────────────────── INTERNALS ─────────────────────────────────────

/** An argument as a number, or `fallback` when it is absent or not one. */
NYA_INTERNAL f64 _nya_lua_argument_number(const NYA_LuaCall* call, u32 index, f64 fallback) {
    if (index >= call->argument_count) return fallback;

    const NYA_Value* value = &call->arguments[index];

    // The reader only produces F64 and S64, but a value can arrive from nya_lua_global_set or be built by hand at any width.
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

/** An argument as a boolean, or `fallback` when it is absent. Lua's nil and false are both false. */
NYA_INTERNAL b8 _nya_lua_argument_boolean(const NYA_LuaCall* call, u32 index, b8 fallback) {
    if (index >= call->argument_count) return fallback;

    const NYA_Value* value = &call->arguments[index];

    switch (value->type) {
        case NYA_TYPE_B8: return value->as_b8;
        case NYA_TYPE_B16: return value->as_b16 != 0;
        case NYA_TYPE_B32: return value->as_b32 != 0;
        case NYA_TYPE_B64: return value->as_b64 != 0;

        // Lua's rule: everything but nil and false is true, so a number or string passed where a flag was wanted is true, not a silent false.
        case NYA_TYPE_NULL:
        case NYA_TYPE_VOID: return false;

        default: return true;
    }
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
 * An entity handle from a `{ index =, generation = }` table.
 *
 * Two numbers, because Lua numbers are doubles. Packing both u32s into one fits in 53 bits today but
 * would silently resolve the wrong entity if either field widened.
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

    nya_object_add(&table, "index", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)handle.index });
    nya_object_add(&table, "generation", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)handle.generation });

    return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = table };
}

/*
 * ───────────────────────────────────── BINDINGS ─────────────────────────────────────
 *
 * Almost all of them are generated. src/build/pp/luabind.c reads the `@lua` annotations in the engine
 * headers and writes src/genyarated/lua_bindings.c: one marshalling function per annotated declaration
 * and a table of them, included below.
 *
 * What is written by hand here is what a generator cannot see through — a variadic function, a macro
 * over an options struct, a call that needs an NYA_Entity* — and each one says which of those it is.
 * They carry `@lua_manual` so the definitions file describes the whole surface rather than the
 * generated half of it: an editor must see one API, not two.
 */

/** One binding: where it lives in the `nya` table, what runs, and what a plugin must have been granted to see it. */
typedef struct {
    NYA_ConstCString     path;
    NYA_LuaFn            fn;
    NYA_PluginPermission permission;
} _NYA_LuaBindingEntry;

/**
 * One line in the engine log, marked as the script's.
 *
 * Hand written because nya_log_info is variadic and a variadic C function has no signature to generate
 * a marshaller against. A script formats its own text and passes one string.
 *
 * @lua_manual(nya.log.info, NONE, text: string)
 * */
NYA_INTERNAL void nya_lua_binding_log(NYA_LuaCall* call) {
    NYA_ConstCString text = _nya_lua_argument_string(call, 0);
    if (text != nullptr) nya_log_info("[lua] %s", text);
}

/**
 * The same at warning level.
 *
 * @lua_manual(nya.log.warn, NONE, text: string)
 * */
NYA_INTERNAL void nya_lua_binding_warn(NYA_LuaCall* call) {
    NYA_ConstCString text = _nya_lua_argument_string(call, 0);
    if (text != nullptr) nya_log_warn("[lua] %s", text);
}

/**
 * The same at error level. A plugin's own failures reach the log through nya_plugin_error instead,
 * which names the plugin; this is for a script saying something went wrong on its own terms.
 *
 * @lua_manual(nya.log.error, NONE, text: string)
 * */
NYA_INTERNAL void nya_lua_binding_error(NYA_LuaCall* call) {
    NYA_ConstCString text = _nya_lua_argument_string(call, 0);
    if (text != nullptr) nya_log_error("[lua] %s", text);
}

/**
 * Seconds since the application started.
 *
 * Hand written because the value is a field of the struct nya_app_get returns, and a struct does not
 * cross the boundary.
 *
 * @lua_manual(nya.app.time, NONE, -> number)
 * */
NYA_INTERNAL void nya_lua_binding_time(NYA_LuaCall* call) {
    call->results[0]   = nya_lua_number((f64)nya_app_get()->frame_stats.uptime_s);
    call->result_count = 1;
}

/**
 * Spawns an entity from a table of `name`, `type`, `x`, `y` and `z`, and answers with its handle.
 *
 * Hand written because nya_entity_spawn is a macro over an options struct, which is exactly the shape
 * the generator cannot see through.
 *
 * @lua_manual(nya.entity.spawn, ENTITIES, options: table, -> table)
 * */
NYA_INTERNAL void nya_lua_binding_spawn(NYA_LuaCall* call) {
    // A table, not positional arguments.
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

/**
 * Where an entity is, as a table of `x`, `y` and `z`, or nil once its handle no longer resolves.
 *
 * Hand written because reading a position means holding an NYA_Entity*, and a pointer is the one thing
 * that must not cross into a script.
 *
 * @lua_manual(nya.entity.position, ENTITIES, entity: table, -> table)
 * */
NYA_INTERNAL void nya_lua_binding_position(NYA_LuaCall* call) {
    NYA_Entity* entity = nya_entity_get(_nya_lua_argument_handle(call, 0));

    // nil for a handle that no longer resolves, which is why scripts get handles rather than pointers.
    if (entity == nullptr) {
        call->result_count = 0;
        return;
    }

    NYA_Object position = nya_object_create_on_stack(call->arena);

    nya_object_add(&position, "x", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)entity->position.x });
    nya_object_add(&position, "y", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)entity->position.y });
    nya_object_add(&position, "z", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = (f64)entity->position.z });

    call->results[0]   = (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = position };
    call->result_count = 1;
}

/**
 * Tweens an entity to a point over `duration_s`. Zero is a teleport, which is what nya_entity_move_to
 * already means by it.
 *
 * Hand written for the same reason as the position above.
 *
 * @lua_manual(nya.entity.move_to, ENTITIES, entity: table, x: number, y: number, z: number, duration_s: number)
 * */
NYA_INTERNAL void nya_lua_binding_move_to(NYA_LuaCall* call) {
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

/** What the hand written ones above register as. The generated ones are in _NYA_LUA_GENERATED_BINDINGS. */
NYA_INTERNAL const _NYA_LuaBindingEntry _NYA_LUA_MANUAL_BINDINGS[] = {
    { "nya.log.info",        nya_lua_binding_log,      NYA_PLUGIN_PERMISSION_NONE     },
    { "nya.log.warn",        nya_lua_binding_warn,     NYA_PLUGIN_PERMISSION_NONE     },
    { "nya.log.error",       nya_lua_binding_error,    NYA_PLUGIN_PERMISSION_NONE     },
    { "nya.app.time",        nya_lua_binding_time,     NYA_PLUGIN_PERMISSION_NONE     },
    { "nya.entity.spawn",    nya_lua_binding_spawn,    NYA_PLUGIN_PERMISSION_ENTITIES },
    { "nya.entity.position", nya_lua_binding_position, NYA_PLUGIN_PERMISSION_ENTITIES },
    { "nya.entity.move_to",  nya_lua_binding_move_to,  NYA_PLUGIN_PERMISSION_ENTITIES },
};

// After the helpers it calls and the _NYA_LuaBindingEntry it fills in, and never edited by hand.
#include "genyarated/lua_bindings.c"

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

/** Registers one table's worth, skipping whatever `permissions` does not cover. */
NYA_INTERNAL void _nya_lua_open_table(NYA_LuaVM* vm, const _NYA_LuaBindingEntry* bindings, u32 count, NYA_PluginPermission permissions) {
    for (u32 i = 0; i < count; i++) {
        // The whole permission check: a binding that fails it is never registered, so its name does not exist in this VM. See nya_lua_open_engine_permitted.
        if (((u64)bindings[i].permission & ~(u64)permissions) != 0) continue;

        nya_lua_register_path(vm, bindings[i].path, bindings[i].fn, nullptr);
    }
}

void nya_lua_open_engine(NYA_LuaVM* vm) {
    // Everything, for a VM the host owns: the permission model is about plugins, and the game's own scripting is its own code.
    nya_lua_open_engine_permitted(vm, (NYA_PluginPermission)~0ULL);
}

void nya_lua_open_engine_permitted(NYA_LuaVM* vm, NYA_PluginPermission permissions) {
    if (vm == nullptr) return;

    _nya_lua_open_table(vm, _NYA_LUA_GENERATED_BINDINGS, nya_carray_length(_NYA_LUA_GENERATED_BINDINGS), permissions);

    // Second, so a hand-written binding wins where a path is in both tables; there is none today.
    _nya_lua_open_table(vm, _NYA_LUA_MANUAL_BINDINGS, nya_carray_length(_NYA_LUA_MANUAL_BINDINGS), permissions);
}
