#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The registry, or null when the world has none yet. Null is the only reason a lookup can be refused. */
NYA_INTERNAL NYA_PhysicsLayerSystem* _nya_physics_layer_system(void);

/** The index `name` is registered at, or NYA_PHYSICS_LAYER_MAX when it is not. */
NYA_INTERNAL u32 _nya_physics_layer_index_of(const NYA_PhysicsLayerSystem* system, NYA_ConstCString name);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_physics_layer_init(void) {
    NYA_PhysicsLayerSystem* system = &nya_world()->physics_layer_system;

    *system = (NYA_PhysicsLayerSystem){ .initialized = true };

    // bit zero is claimed before anything else can take it, so NYA_PHYSICS_LAYER_DEFAULT is a fact
    // rather than a convention every caller has to remember.
    (void)nya_physics_layer(NYA_PHYSICS_LAYER_DEFAULT_NAME);

    nya_assert(system->count == 1, "the default layer must be the only one a fresh registry holds");
    nya_assert(nya_physics_layer(NYA_PHYSICS_LAYER_DEFAULT_NAME) == NYA_PHYSICS_LAYER_DEFAULT);
}

void nya_system_physics_layer_deinit(void) {
    NYA_PhysicsLayerSystem* system = &nya_world()->physics_layer_system;
    if (!system->initialized) return;

    *system = (NYA_PhysicsLayerSystem){ 0 };
}

/*
 * ─────────────────────────────────────────────────────────
 * LAYERS
 * ─────────────────────────────────────────────────────────
 */

NYA_PhysicsLayerMask nya_physics_layer(NYA_ConstCString name) {
    nya_assert(name != nullptr, "a layer has to be named");
    nya_assert(name[0] != '\0', "a layer name cannot be empty");

    NYA_PhysicsLayerSystem* system = _nya_physics_layer_system();
    if (system == nullptr) return NYA_PHYSICS_LAYER_NONE;

    u32 existing = _nya_physics_layer_index_of(system, name);
    if (existing < NYA_PHYSICS_LAYER_MAX) return (NYA_PhysicsLayerMask)1 << existing;

    // operating error: a game with too many layers is a content problem, and crashing on it would make
    // a mod or a tilemap able to take the process down.
    if (system->count >= NYA_PHYSICS_LAYER_MAX) {
        nya_log_error("Cannot register collision layer '%s': all %d layers are taken.", name, NYA_PHYSICS_LAYER_MAX);
        return NYA_PHYSICS_LAYER_NONE;
    }

    u32 index = system->count;

    // truncation is reported rather than silent: two names sharing a prefix longer than the buffer would
    // otherwise collapse into one layer and the second body would quietly join the first's.
    s32 written = snprintf(system->names[index], NYA_PHYSICS_LAYER_NAME_MAX, "%s", name);
    if (written < 0 || written >= NYA_PHYSICS_LAYER_NAME_MAX) {
        nya_log_error("Collision layer name '%s' is longer than the %d bytes a name may take.", name, NYA_PHYSICS_LAYER_NAME_MAX);
        system->names[index][0] = '\0';
        return NYA_PHYSICS_LAYER_NONE;
    }

    system->count++;

    nya_assert(system->count <= NYA_PHYSICS_LAYER_MAX);
    nya_assert(_nya_physics_layer_index_of(system, name) == index, "a layer must be findable under the name it was registered with");

    return (NYA_PhysicsLayerMask)1 << index;
}

b8 nya_physics_layer_find(NYA_ConstCString name, OUT NYA_PhysicsLayerMask* out_layer) {
    nya_assert(out_layer != nullptr);

    *out_layer = NYA_PHYSICS_LAYER_NONE;

    if (name == nullptr || name[0] == '\0') return false;

    const NYA_PhysicsLayerSystem* system = _nya_physics_layer_system();
    if (system == nullptr) return false;

    u32 index = _nya_physics_layer_index_of(system, name);
    if (index >= NYA_PHYSICS_LAYER_MAX) return false;

    *out_layer = (NYA_PhysicsLayerMask)1 << index;
    return true;
}

NYA_ConstCString nya_physics_layer_name(u32 index) {
    const NYA_PhysicsLayerSystem* system = _nya_physics_layer_system();
    if (system == nullptr || index >= system->count) return nullptr;

    return system->names[index];
}

u32 nya_physics_layer_count(void) {
    const NYA_PhysicsLayerSystem* system = _nya_physics_layer_system();
    if (system == nullptr) return 0;

    return system->count;
}

b8 nya_physics_layer_mask_overlaps(NYA_PhysicsLayerMask layers_a, NYA_PhysicsLayerMask collides_with_a, NYA_PhysicsLayerMask layers_b,
                                   NYA_PhysicsLayerMask collides_with_b) {
    // both sides have to want it, which is what makes "this layer ignores that one" a single edit.
    return (layers_a & collides_with_b) != 0 && (layers_b & collides_with_a) != 0;
}

NYA_PhysicsLayerMask _nya_physics_layers(NYA_ConstCString const* names) {
    nya_assert(names != nullptr);

    NYA_PhysicsLayerMask mask = NYA_PHYSICS_LAYER_NONE;

    // bounded by the registry: a list longer than every layer there can be is a caller bug.
    for (u32 i = 0; i < NYA_PHYSICS_LAYER_MAX && names[i] != nullptr; i++) mask |= nya_physics_layer(names[i]);

    return mask;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_PhysicsLayerSystem* _nya_physics_layer_system(void) {
    // teardown paths and host tools reach physics calls with no world; answering "no layers" beats
    // asserting inside nya_world.
    if (!nya_world_exists()) return nullptr;

    NYA_PhysicsLayerSystem* system = &nya_world()->physics_layer_system;
    if (!system->initialized) return nullptr;

    return system;
}

u32 _nya_physics_layer_index_of(const NYA_PhysicsLayerSystem* system, NYA_ConstCString name) {
    nya_assert(system != nullptr);
    nya_assert(name != nullptr);

    // a linear scan over at most sixty-four short names, done once per body attach. a map would cost an
    // allocation and a hash to beat a loop that fits in two cache lines.
    for (u32 i = 0; i < system->count; i++) {
        if (nya_string_equals(system->names[i], name)) return i;
    }

    return NYA_PHYSICS_LAYER_MAX;
}
