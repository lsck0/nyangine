#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_SystemEntry entries[NYA_SYSTEM_REGISTRY_MAX];
    u32             count;

    /**
     * Whether `entries` is in run order yet.
     *
     * Sorted in place rather than into a second array: run_init/update/deinit want a plain forward
     * (or reverse) walk with no indirection, and there is no case where both the registration order
     * and the run order are needed at once.
     * */
    b8 finalized;
} _NYA_SystemRegistry;

/* No init: a zeroed registry is already a valid empty one, so there is nothing to bring up. */
NYA_INTERNAL _NYA_SystemRegistry _nya_system_registry = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_register(NYA_SystemEntry entry) {
    // Registered here rather than at some dedicated init: this registry has none, a zeroed array
    // already being a valid empty one. Guarded so a test that resets and refills the registry many
    // times over one process does not add a copy of itself to the ceiling registry each time.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("systems", NYA_SYSTEM_REGISTRY_MAX, &_nya_system_registry.count);
        ceiling_registered = true;
    }

    nya_assert(entry.name != nullptr, "a system must be registered with a name");

    if (_nya_system_registry.count >= NYA_SYSTEM_REGISTRY_MAX) {
        // Refused rather than grown. See NYA_SYSTEM_REGISTRY_MAX.
        nya_log_warn("System registry is full at " FMTu32 "; '%s' was not registered.", (u32)NYA_SYSTEM_REGISTRY_MAX, entry.name);
        return;
    }

    // Loud rather than silently shadowed: two systems answering to the same name would leave `after`
    // pointing at whichever of them a linear search happened to find first, which is not a decision
    // anyone made on purpose.
    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        nya_assert(!nya_string_equals(_nya_system_registry.entries[i].name, entry.name), "system '%s' is already registered", entry.name);
    }

    _nya_system_registry.entries[_nya_system_registry.count] = entry;
    _nya_system_registry.count++;
}

NYA_Error nya_system_registry_finalize(void) {
    // See the header: a second call has nothing new to do.
    if (_nya_system_registry.finalized) return NYA_OK;

    enum { _NYA_SYS_UNVISITED, _NYA_SYS_IN_PROGRESS, _NYA_SYS_DONE };

    u8  state[NYA_SYSTEM_REGISTRY_MAX] = { 0 };
    u32 chain[NYA_SYSTEM_REGISTRY_MAX];

    NYA_SystemEntry sorted[NYA_SYSTEM_REGISTRY_MAX];
    u32             sorted_count = 0;

    // Each entry names at most one predecessor, so "resolve everything an entry transitively comes
    // after" is a chain to walk rather than a general graph to search. Walking it iteratively (this
    // loop) instead of recursively keeps the whole thing inside one fixed-size array of indices.
    for (u32 start = 0; start < _nya_system_registry.count; start++) {
        if (state[start] == _NYA_SYS_DONE) continue;

        u32 chain_length = 0;
        u32 current      = start;

        while (true) {
            if (state[current] == _NYA_SYS_DONE) break;

            if (state[current] == _NYA_SYS_IN_PROGRESS) {
                // `current` is already on this walk's own chain: everything from there to here
                // names the next as its `after`, and closes back on itself. That is the cycle.
                u32 cycle_start = 0;
                for (u32 i = 0; i < chain_length; i++) {
                    if (chain[i] == current) {
                        cycle_start = i;
                        break;
                    }
                }

                u8  message[NYA_ERROR_MESSAGE_MAX_LENGTH];
                u64 length  = 0;
                s32 written = snprintf((char*)message, sizeof(message), "system registry has a cycle: ");
                if (written > 0) length = (u64)written;

                for (u32 i = cycle_start; i < chain_length && length < sizeof(message); i++) {
                    written = snprintf((char*)&message[length], sizeof(message) - length, "%s -> ", _nya_system_registry.entries[chain[i]].name);
                    if (written > 0) length += (u64)written;
                }

                if (length < sizeof(message)) {
                    (void)snprintf((char*)&message[length], sizeof(message) - length, "%s", _nya_system_registry.entries[current].name);
                }

                return nya_error(NYA_ERROR_NOT_OK, "%s", (NYA_ConstCString)message);
            }

            state[current]      = _NYA_SYS_IN_PROGRESS;
            chain[chain_length] = current;
            chain_length++;

            NYA_ConstCString after = _nya_system_registry.entries[current].after;
            if (after == nullptr) break;

            u32 dependency = _nya_system_registry.count;
            for (u32 i = 0; i < _nya_system_registry.count; i++) {
                if (nya_string_equals(_nya_system_registry.entries[i].name, after)) {
                    dependency = i;
                    break;
                }
            }

            if (dependency == _nya_system_registry.count) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "system '%s' is registered to run after '%s', which was never registered",
                    _nya_system_registry.entries[current].name,
                    after
                );
            }

            current = dependency;
        }

        // Deepest dependency first, so it lands in `sorted` before everything that named it.
        for (u32 i = chain_length; i > 0; i--) {
            u32 index            = chain[i - 1];
            state[index]         = _NYA_SYS_DONE;
            sorted[sorted_count] = _nya_system_registry.entries[index];
            sorted_count++;
        }
    }

    nya_memcpy(_nya_system_registry.entries, sorted, sorted_count * sizeof(NYA_SystemEntry));
    _nya_system_registry.finalized = true;

    return NYA_OK;
}

NYA_Error nya_system_registry_run_init(void) {
    nya_assert(_nya_system_registry.finalized, "nya_system_registry_run_init was called before nya_system_registry_finalize");

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        NYA_SystemInitFn init = _nya_system_registry.entries[i].init;
        if (init == nullptr) continue;

        NYA_Error result = init();
        if (!result.ok) return result;
    }

    return NYA_OK;
}

void nya_system_registry_run_update(f32 delta_time_s) {
    nya_assert(_nya_system_registry.finalized, "nya_system_registry_run_update was called before nya_system_registry_finalize");

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        NYA_SystemUpdateFn update = _nya_system_registry.entries[i].update;
        if (update != nullptr) update(delta_time_s);
    }
}

void nya_system_registry_run_deinit(void) {
    nya_assert(_nya_system_registry.finalized, "nya_system_registry_run_deinit was called before nya_system_registry_finalize");

    for (u32 i = _nya_system_registry.count; i > 0; i--) {
        NYA_SystemDeinitFn deinit = _nya_system_registry.entries[i - 1].deinit;
        if (deinit != nullptr) deinit();
    }
}

u32 nya_system_registry_count(void) {
    return _nya_system_registry.count;
}

NYA_ConstCString nya_system_registry_name_at(u32 index) {
    nya_assert(
        index < _nya_system_registry.count,
        "system registry index " FMTu32 " is out of range (" FMTu32 " registered)",
        index,
        _nya_system_registry.count
    );

    return _nya_system_registry.entries[index].name;
}

NYA_SystemInitFn nya_system_registry_init_at(u32 index) {
    nya_assert(index < _nya_system_registry.count, "system registry index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_system_registry.count);

    return _nya_system_registry.entries[index].init;
}

NYA_SystemDeinitFn nya_system_registry_deinit_at(u32 index) {
    nya_assert(index < _nya_system_registry.count, "system registry index " FMTu32 " is out of range (" FMTu32 " registered)", index,
               _nya_system_registry.count);

    return _nya_system_registry.entries[index].deinit;
}

#ifdef NYA_TESTING
void _nya_system_registry_reset_for_test(void) {
    _nya_system_registry = (_NYA_SystemRegistry){ 0 };
}
#endif
