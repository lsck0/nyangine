#include "nyangine-core/nyangine.h"

// TYPES

typedef struct {
    NYA_ConstCString name;
    u32              capacity;

    /** Points at the subsystem's own counter. See the file comment: never owned, never copied. */
    const u32* live;
} _NYA_CeilingEntry;

typedef struct {
    NYA_ConstCString name;

    /** Points at the subsystem's own byte count, never owned, like a ceiling's counter. */
    const u64* bytes;
} _NYA_GaugeEntry;

typedef struct {
    _NYA_CeilingEntry entries[NYA_CEILING_REGISTRY_MAX];
    u32               count;

    _NYA_GaugeEntry gauges[NYA_GAUGE_REGISTRY_MAX];
    u32             gauge_count;
} _NYA_CeilingRegistry;

/* No init: a zeroed registry is already a valid empty one. */
NYA_INTERNAL _NYA_CeilingRegistry _nya_ceiling_registry = { 0 };

// PRIVATE API DECLARATION

/**
 * live/capacity for `index` into `entries`, not the sorted order. Zero for a zero capacity rather than
 * dividing; a HUD row is not the place to assert.
 * */
NYA_INTERNAL f32 _nya_ceiling_fullness(u32 index);

/**
 * Fills `order[0..count)` with indices into `_nya_ceiling_registry.entries`, fullest fullness first.
 * */
NYA_INTERNAL void _nya_ceiling_order(OUT u32 order[NYA_CEILING_REGISTRY_MAX]);

// PUBLIC API IMPLEMENTATION

// Registration is lazy-on-first-use from arbitrary threads (a first log, the mixer thread, the
// renderer), so the registry is written concurrently and read by the /metrics scrape. A spinlock — held
// for a couple of stores at a time, like base_logging's — serialises the writers so a reader never sees
// a half-written entry or a torn count.
NYA_INTERNAL atomic_flag _nya_ceiling_lock = ATOMIC_FLAG_INIT;

void nya_ceiling_register(NYA_ConstCString name, u32 capacity, const u32* live) {
    nya_assert(name != nullptr, "a ceiling must be registered with a name");
    nya_assert(live != nullptr, "a ceiling must be registered with a live counter to point at");

    while (atomic_flag_test_and_set(&_nya_ceiling_lock)) {}

    if (_nya_ceiling_registry.count >= NYA_CEILING_REGISTRY_MAX) {
        // Refused rather than grown. See NYA_CEILING_REGISTRY_MAX. Unlock before the warn: a first warn
        // may lazily register the log-ring ceiling, which would re-enter this and spin on the held lock.
        atomic_flag_clear(&_nya_ceiling_lock);
        nya_log_warn("Ceiling registry is full at " FMTu32 "; '%s' was not registered.", (u32)NYA_CEILING_REGISTRY_MAX, name);
        return;
    }

    _nya_ceiling_registry.entries[_nya_ceiling_registry.count] = (_NYA_CeilingEntry){
        .name     = name,
        .capacity = capacity,
        .live     = live,
    };
    _nya_ceiling_registry.count++;
    atomic_flag_clear(&_nya_ceiling_lock);
}

void nya_ceiling_unregister(NYA_ConstCString name) {
    nya_assert(name != nullptr, "a ceiling must be unregistered by name");

    while (atomic_flag_test_and_set(&_nya_ceiling_lock)) {}
    for (u32 index = 0; index < _nya_ceiling_registry.count; index++) {
        if (strcmp(_nya_ceiling_registry.entries[index].name, name) != 0) continue;

        // The last entry fills the hole and the count shrinks; the sorted order is rebuilt on every query, so entries need not stay ordered here.
        _nya_ceiling_registry.count--;
        _nya_ceiling_registry.entries[index]                       = _nya_ceiling_registry.entries[_nya_ceiling_registry.count];
        _nya_ceiling_registry.entries[_nya_ceiling_registry.count] = (_NYA_CeilingEntry){ 0 };
        break;
    }
    atomic_flag_clear(&_nya_ceiling_lock);
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

void nya_gauge_register(NYA_ConstCString name, const u64* bytes) {
    nya_assert(name != nullptr, "a gauge must be registered with a name");
    nya_assert(bytes != nullptr, "a gauge must be registered with a byte count to point at");

    while (atomic_flag_test_and_set(&_nya_ceiling_lock)) {}
    if (_nya_ceiling_registry.gauge_count >= NYA_GAUGE_REGISTRY_MAX) {
        // refused rather than grown, for the ceiling registry's reason; unlock before the warn (see register).
        atomic_flag_clear(&_nya_ceiling_lock);
        nya_log_warn("Gauge registry is full at " FMTu32 "; '%s' was not registered.", (u32)NYA_GAUGE_REGISTRY_MAX, name);
        return;
    }

    _nya_ceiling_registry.gauges[_nya_ceiling_registry.gauge_count] = (_NYA_GaugeEntry){
        .name  = name,
        .bytes = bytes,
    };
    _nya_ceiling_registry.gauge_count++;
    atomic_flag_clear(&_nya_ceiling_lock);
}

u32 nya_gauge_count(void) {
    return _nya_ceiling_registry.gauge_count;
}

NYA_ConstCString nya_gauge_name_at(u32 index) {
    nya_assert(index < _nya_ceiling_registry.gauge_count, "gauge index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_ceiling_registry.gauge_count);

    return _nya_ceiling_registry.gauges[index].name;
}

u64 nya_gauge_bytes_at(u32 index) {
    nya_assert(index < _nya_ceiling_registry.gauge_count, "gauge index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_ceiling_registry.gauge_count);

    return *_nya_ceiling_registry.gauges[index].bytes;
}

#ifdef NYA_TESTING
__attr_maybe_unused void _nya_ceiling_registry_reset_for_test(void) {
    _nya_ceiling_registry = (_NYA_CeilingRegistry){ 0 };
}
#endif

// PRIVATE API IMPLEMENTATION

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
