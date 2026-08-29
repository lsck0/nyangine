#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_ConstCString name;
    u32              capacity;

    /** Points at the subsystem's own counter. See the file comment: never owned, never copied. */
    const u32* live;
} _NYA_CeilingEntry;

typedef struct {
    _NYA_CeilingEntry entries[NYA_CEILING_REGISTRY_MAX];
    u32               count;
} _NYA_CeilingRegistry;

/* No init: a zeroed registry is already a valid empty one. */
NYA_INTERNAL _NYA_CeilingRegistry _nya_ceiling_registry = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** live/capacity for `index` into `entries`, not into the sorted order. Zero for a zero capacity
 *  rather than dividing by it — a ceiling of zero should never be registered, but a HUD row is not
 *  the place to assert that. */
NYA_INTERNAL f32 _nya_ceiling_fullness(u32 index);

/**
 * Fills `order[0..count)` with indices into `_nya_ceiling_registry.entries`, fullest fullness first.
 *
 * Recomputed on every call rather than cached — see the file comment. A stable insertion sort: with
 * at most NYA_CEILING_REGISTRY_MAX entries, quadratic is not a concern, and stability keeps equally
 * full ceilings in registration order instead of shuffling on every call for no reason.
 * */
NYA_INTERNAL void _nya_ceiling_order(OUT u32 order[NYA_CEILING_REGISTRY_MAX]);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ceiling_register(NYA_ConstCString name, u32 capacity, const u32* live) {
    nya_assert(name != nullptr, "a ceiling must be registered with a name");
    nya_assert(live != nullptr, "a ceiling must be registered with a live counter to point at");

    if (_nya_ceiling_registry.count >= NYA_CEILING_REGISTRY_MAX) {
        // Refused rather than grown. See NYA_CEILING_REGISTRY_MAX.
        nya_log_warn("Ceiling registry is full at " FMTu32 "; '%s' was not registered.", (u32)NYA_CEILING_REGISTRY_MAX, name);
        return;
    }

    _nya_ceiling_registry.entries[_nya_ceiling_registry.count] = (_NYA_CeilingEntry){
        .name     = name,
        .capacity = capacity,
        .live     = live,
    };
    _nya_ceiling_registry.count++;
}

u32 nya_ceiling_count(void) {
    return _nya_ceiling_registry.count;
}

NYA_ConstCString nya_ceiling_name_at(u32 index) {
    nya_assert(index < _nya_ceiling_registry.count, "ceiling index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_ceiling_registry.count);

    u32 order[NYA_CEILING_REGISTRY_MAX];
    _nya_ceiling_order(order);

    return _nya_ceiling_registry.entries[order[index]].name;
}

u32 nya_ceiling_capacity_at(u32 index) {
    nya_assert(index < _nya_ceiling_registry.count, "ceiling index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_ceiling_registry.count);

    u32 order[NYA_CEILING_REGISTRY_MAX];
    _nya_ceiling_order(order);

    return _nya_ceiling_registry.entries[order[index]].capacity;
}

u32 nya_ceiling_live_at(u32 index) {
    nya_assert(index < _nya_ceiling_registry.count, "ceiling index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_ceiling_registry.count);

    u32 order[NYA_CEILING_REGISTRY_MAX];
    _nya_ceiling_order(order);

    return *_nya_ceiling_registry.entries[order[index]].live;
}

#ifdef NYA_TESTING
void _nya_ceiling_registry_reset_for_test(void) {
    _nya_ceiling_registry = (_NYA_CeilingRegistry){ 0 };
}
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _nya_ceiling_fullness(u32 index) {
    const _NYA_CeilingEntry* entry = &_nya_ceiling_registry.entries[index];
    if (entry->capacity == 0) return 0.0F;

    return (f32)*entry->live / (f32)entry->capacity;
}

void _nya_ceiling_order(OUT u32 order[NYA_CEILING_REGISTRY_MAX]) {
    for (u32 i = 0; i < _nya_ceiling_registry.count; i++) order[i] = i;

    for (u32 i = 1; i < _nya_ceiling_registry.count; i++) {
        u32 key           = order[i];
        f32 key_fullness  = _nya_ceiling_fullness(key);
        u32 j             = i;

        while (j > 0 && _nya_ceiling_fullness(order[j - 1]) < key_fullness) {
            order[j] = order[j - 1];
            j--;
        }

        order[j] = key;
    }
}
