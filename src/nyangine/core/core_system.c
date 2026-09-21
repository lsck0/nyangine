#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What a mutation made from inside a phase run is remembered as until the barrier. See core_system.h. */
typedef enum {
    _NYA_SYSTEM_OP_REGISTER,
    _NYA_SYSTEM_OP_UNREGISTER,
    _NYA_SYSTEM_OP_ENABLE,
    _NYA_SYSTEM_OP_DISABLE,
    _NYA_SYSTEM_OP_COUNT,
} _NYA_SystemOp;

typedef struct {
    _NYA_SystemOp op;

    /** Whole for a register, and for everything else only `name` is read. */
    NYA_SystemEntry entry;
} _NYA_SystemPending;

typedef struct {
    NYA_SystemEntry entries[NYA_SYSTEM_REGISTRY_MAX];

    /** Parallel to `entries`, permuted with it by the sort. */
    b8 enabled[NYA_SYSTEM_REGISTRY_MAX];

    /** Whether `init` ran and succeeded, which is what decides whether `deinit` runs. */
    b8 initialized[NYA_SYSTEM_REGISTRY_MAX];

    /** This frame's measured time so far, and the last whole frame's. Both parallel to `entries`. */
    u64 measuring_ns[NYA_SYSTEM_REGISTRY_MAX];
    u64 frame_ns[NYA_SYSTEM_REGISTRY_MAX];

    /** Whether the run loop times each system. See nya_system_accounting_enable. */
    b8 accounting;

    u32 count;

    /** Whether `entries` is in run order yet. */
    b8 finalized;

    /** Whether membership changed since the last sort, so the next barrier has to redo it. */
    b8 order_dirty;

    /** Whether a phase run is in progress, and which entry's callback is executing while it is. */
    b8  running;
    u32 running_index;

    _NYA_SystemPending pending[NYA_SYSTEM_PENDING_MAX];
    u32                pending_count;
} _NYA_SystemRegistry;

/**
 * One schedule line, before it is truncated. Sixty-four names at an average of sixteen characters plus
 * four for the arrow, which is the whole registry in one line with room to spare.
 * */
#define _NYA_SYSTEM_REPORT_LINE_MAX 1536

/* No init: a zeroed registry is already a valid empty one, so there is nothing to bring up. */
NYA_INTERNAL _NYA_SystemRegistry _nya_system_registry = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registers the two ceilings, once per process however often the registry is emptied and refilled. */
NYA_INTERNAL void _nya_system_ceilings_register(void);

/** Whether `name` is registered, and where. `out_index` is untouched when it is not. */
NYA_INTERNAL b8 _nya_system_find(NYA_ConstCString name, OUT u32* out_index) __attr_no_discard;

/** The callback `phase` runs, which is null for most systems in most phases. */
NYA_INTERNAL NYA_SystemPhaseFn _nya_system_phase_fn(const NYA_SystemEntry* entry, NYA_SystemPhase phase) __attr_no_discard;

/** Queues `op` for the barrier. Warns and refuses past NYA_SYSTEM_PENDING_MAX. */
NYA_INTERNAL void _nya_system_defer(_NYA_SystemOp op, NYA_SystemEntry entry);

/* The three mutations, without the deferral decision: the barrier and the immediate path share them. */
NYA_INTERNAL void _nya_system_register_now(NYA_SystemEntry entry);
NYA_INTERNAL void _nya_system_unregister_now(NYA_ConstCString name);
NYA_INTERNAL void _nya_system_enabled_set_now(NYA_ConstCString name, b8 enabled);

/** Sorts `entries` by `after` into run order, permuting the parallel arrays with it. */
NYA_INTERNAL NYA_Error _nya_system_sort(void) __attr_no_discard;

/**
 * Applies everything a run queued, then re-sorts if membership changed. The single point where the
 * registry is allowed to change shape; see THE BARRIER in core_system.h.
 * */
NYA_INTERNAL NYA_Error _nya_system_barrier(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * REGISTRATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_register(NYA_SystemEntry entry) {
    _nya_system_ceilings_register();

    nya_assert(entry.name != nullptr, "a system must be registered with a name");

    if (_nya_system_registry.running) {
        _nya_system_defer(_NYA_SYSTEM_OP_REGISTER, entry);
        return;
    }

    _nya_system_register_now(entry);
}

void nya_system_unregister(NYA_ConstCString name) {
    nya_assert(name != nullptr, "a system must be unregistered by name");

    if (_nya_system_registry.running) {
        // Even deferred: the callback would return into a slot that is about to move or disappear, and
        // "stop me" is nya_system_disable, which is the operation that actually wants this.
        nya_assert(
            !nya_string_equals(_nya_system_registry.entries[_nya_system_registry.running_index].name, name),
            "system '%s' cannot unregister itself from its own callback; disable it instead",
            name
        );

        _nya_system_defer(_NYA_SYSTEM_OP_UNREGISTER, (NYA_SystemEntry){ .name = name });
        return;
    }

    _nya_system_unregister_now(name);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENABLING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_enable(NYA_ConstCString name) {
    nya_assert(name != nullptr, "a system must be enabled by name");

    if (_nya_system_registry.running) {
        _nya_system_defer(_NYA_SYSTEM_OP_ENABLE, (NYA_SystemEntry){ .name = name });
        return;
    }

    _nya_system_enabled_set_now(name, true);
}

void nya_system_disable(NYA_ConstCString name) {
    nya_assert(name != nullptr, "a system must be disabled by name");

    if (_nya_system_registry.running) {
        _nya_system_defer(_NYA_SYSTEM_OP_DISABLE, (NYA_SystemEntry){ .name = name });
        return;
    }

    _nya_system_enabled_set_now(name, false);
}

b8 nya_system_is_enabled(NYA_ConstCString name) {
    nya_assert(name != nullptr, "a system's enabled state must be asked for by name");

    u32 index = 0;
    if (!_nya_system_find(name, &index)) return false;

    return _nya_system_registry.enabled[index];
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RUNNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_system_registry_finalize(void) {
    nya_assert(!_nya_system_registry.running, "nya_system_registry_finalize was called from inside a phase run");

    NYA_Error ordered = _nya_system_barrier();
    if (!ordered.ok) return ordered;

    _nya_system_registry.finalized = true;

    return NYA_OK;
}

NYA_Error nya_system_registry_run_init(void) {
    nya_assert(_nya_system_registry.finalized, "nya_system_registry_run_init was called before nya_system_registry_finalize");
    nya_assert(!_nya_system_registry.running, "nya_system_registry_run_init was called from inside a phase run");

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        const NYA_SystemEntry* entry = &_nya_system_registry.entries[i];

        // A system with nothing to bring up still counts as up, so its `deinit` runs like anyone else's.
        if (entry->init == nullptr) {
            _nya_system_registry.initialized[i] = true;
            continue;
        }

        u64       init_start = nya_clock_get_monotonic_ns();
        NYA_Error result     = entry->init();

        if (!result.ok) {
            if (entry->optional) {
                // Degraded, not broken: the run asked for a mode this system has no place in. See
                // NYA_SystemEntry.optional.
                nya_log_debug("'%s' is unavailable in this run; continuing without it. %s", entry->name, (NYA_ConstCString)result.message);
                continue;
            }

            nya_log_error("Subsystem initialization failed at '%s'; unwinding. %s", entry->name, (NYA_ConstCString)result.message);

            // Reverse, and only as far as bring-up got: `initialized` was never set for this one.
            nya_system_registry_run_deinit();

            return result;
        }

        _nya_system_registry.initialized[i] = true;

        nya_log_debug("Brought up '%s' in %.1f ms.", entry->name, nya_time_ns_to_ms(nya_clock_get_monotonic_ns() - init_start));
    }

    return NYA_OK;
}

void nya_system_registry_run(NYA_SystemPhase phase, f32 delta_time_s) {
    nya_assert((u32)phase < (u32)NYA_SYSTEM_PHASE_COUNT, "system phase " FMTu32 " is not a phase", (u32)phase);
    nya_assert(_nya_system_registry.finalized, "nya_system_registry_run was called before nya_system_registry_finalize");

    // A phase inside a phase would iterate the same array twice and apply the barrier under the outer
    // loop's feet. Nesting the frame is the app's job, and it does it by running phases in sequence.
    nya_assert(
        !_nya_system_registry.running,
        "nya_system_registry_run(%s) was called from inside '%s'",
        nya_system_phase_name(phase),
        _nya_system_registry.entries[_nya_system_registry.running_index].name
    );

    // Whatever was registered between runs lands here, so the schedule is settled before the first
    // callback and cannot change while the loop walks it.
    NYA_Error ordered = _nya_system_barrier();
    nya_assert(ordered.ok, "the system registry cannot be ordered: %s", (NYA_ConstCString)ordered.message);

    _nya_system_registry.running = true;

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        if (!_nya_system_registry.enabled[i]) continue;

        NYA_SystemPhaseFn run = _nya_system_phase_fn(&_nya_system_registry.entries[i], phase);
        if (run == nullptr) continue;

        _nya_system_registry.running_index = i;

        // Two clock reads per system, and only when someone asked for the numbers. Off, this loop is a
        // load, a null check and the call.
        if (!_nya_system_registry.accounting) {
            run(delta_time_s);
            continue;
        }

        u64 started_ns = nya_clock_get_monotonic_ns();
        run(delta_time_s);
        _nya_system_registry.measuring_ns[i] += nya_clock_get_monotonic_ns() - started_ns;
    }

    _nya_system_registry.running       = false;
    _nya_system_registry.running_index = 0;

    ordered = _nya_system_barrier();
    nya_assert(ordered.ok, "the system registry cannot be ordered: %s", (NYA_ConstCString)ordered.message);
}

void nya_system_registry_run_deinit(void) {
    nya_assert(_nya_system_registry.finalized, "nya_system_registry_run_deinit was called before nya_system_registry_finalize");
    nya_assert(!_nya_system_registry.running, "nya_system_registry_run_deinit was called from inside a phase run");

    for (u32 i = _nya_system_registry.count; i > 0; i--) {
        u32 index = i - 1;

        // Cleared first, so a deinit that unregisters something cannot make this one run twice.
        if (!_nya_system_registry.initialized[index]) continue;
        _nya_system_registry.initialized[index] = false;

        NYA_SystemDeinitFn deinit = _nya_system_registry.entries[index].deinit;
        if (deinit != nullptr) deinit();
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_system_phase_name(NYA_SystemPhase phase) {
    switch (phase) {
        case NYA_SYSTEM_PHASE_FRAME: return "frame";
        case NYA_SYSTEM_PHASE_TICK: return "tick";
        case NYA_SYSTEM_PHASE_RENDER: return "render";

        case NYA_SYSTEM_PHASE_COUNT:
        default: nya_unreachable();
    }
}

NYA_ConstCString nya_system_owner_name(NYA_SystemOwner owner) {
    switch (owner.kind) {
        case NYA_SYSTEM_OWNER_ENGINE: return "engine";
        case NYA_SYSTEM_OWNER_GAME: return "game";

        case NYA_SYSTEM_OWNER_PLUGIN: {
            nya_assert(owner.plugin != nullptr, "a plugin system's owner has no plugin name");
            return owner.plugin;
        }

        case NYA_SYSTEM_OWNER_KIND_COUNT:
        default: nya_unreachable();
    }
}

b8 nya_system_registry_is_running(void) {
    return _nya_system_registry.running;
}

void nya_system_registry_report(void) {
    for (u32 phase = 0; phase < (u32)NYA_SYSTEM_PHASE_COUNT; phase++) {
        // One line per phase rather than one per system: the order is the thing being read, and forty
        // lines is a list where one line is a sentence.
        char line[_NYA_SYSTEM_REPORT_LINE_MAX];
        u64  length = 0;
        u32  shown  = 0;

        for (u32 i = 0; i < _nya_system_registry.count && length < sizeof(line); i++) {
            if (_nya_system_phase_fn(&_nya_system_registry.entries[i], (NYA_SystemPhase)phase) == nullptr) continue;

            // marked rather than dropped: a system missing from the list and a system switched off are
            // different problems and would otherwise read the same.
            s32 written = snprintf(&line[length], sizeof(line) - length, "%s%s%s", shown > 0 ? " -> " : "", _nya_system_registry.entries[i].name,
                                   _nya_system_registry.enabled[i] ? "" : " (off)");

            if (written > 0) length += (u64)written;
            shown++;
        }

        if (shown == 0) continue;

        nya_log_debug("System schedule, %s: %s", nya_system_phase_name((NYA_SystemPhase)phase), (NYA_ConstCString)line);
    }
}

u32 nya_system_registry_count(void) {
    return _nya_system_registry.count;
}

const NYA_SystemEntry* nya_system_registry_at(u32 index) {
    nya_assert(
        index < _nya_system_registry.count,
        "system registry index " FMTu32 " is out of range (" FMTu32 " registered)",
        index,
        _nya_system_registry.count
    );

    return &_nya_system_registry.entries[index];
}

b8 nya_system_registry_enabled_at(u32 index) {
    nya_assert(
        index < _nya_system_registry.count,
        "system registry index " FMTu32 " is out of range (" FMTu32 " registered)",
        index,
        _nya_system_registry.count
    );

    return _nya_system_registry.enabled[index];
}

b8 nya_system_registry_initialized_at(u32 index) {
    nya_assert(
        index < _nya_system_registry.count,
        "system registry index " FMTu32 " is out of range (" FMTu32 " registered)",
        index,
        _nya_system_registry.count
    );

    return _nya_system_registry.initialized[index];
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCOUNTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_accounting_enable(void) {
    if (_nya_system_registry.accounting) return;

    // From zero rather than from whatever the last session left, so the first frame after it is turned
    // on reads as a frame and not as a sum of everything since startup.
    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        _nya_system_registry.measuring_ns[i] = 0;
        _nya_system_registry.frame_ns[i]     = 0;
    }

    _nya_system_registry.accounting = true;
}

void nya_system_accounting_disable(void) {
    _nya_system_registry.accounting = false;
}

b8 nya_system_accounting_is_enabled(void) {
    return _nya_system_registry.accounting;
}

void nya_system_accounting_frame_end(void) {
    if (!_nya_system_registry.accounting) return;

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        _nya_system_registry.frame_ns[i]     = _nya_system_registry.measuring_ns[i];
        _nya_system_registry.measuring_ns[i] = 0;
    }
}

u64 nya_system_registry_time_ns_at(u32 index) {
    nya_assert(
        index < _nya_system_registry.count,
        "system registry index " FMTu32 " is out of range (" FMTu32 " registered)",
        index,
        _nya_system_registry.count
    );

    return _nya_system_registry.frame_ns[index];
}

u32 nya_system_owner_count(void) {
    u32 count = 0;

    // Distinct owners, in the order they first appear. A linear scan over at most a few dozen entries,
    // which beats a map that would have to be kept in step with every registration.
    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        NYA_ConstCString name = nya_system_owner_name(_nya_system_registry.entries[i].owner);

        b8 seen = false;
        for (u32 j = 0; j < i; j++) {
            if (!nya_string_equals(nya_system_owner_name(_nya_system_registry.entries[j].owner), name)) continue;

            seen = true;
            break;
        }

        if (seen) continue;
        if (count >= NYA_SYSTEM_OWNER_MAX) break;

        count++;
    }

    return count;
}

NYA_SystemOwnerStats nya_system_owner_stats_at(u32 index) {
    nya_assert(index < nya_system_owner_count(), "system owner index " FMTu32 " is out of range", index);

    // The same first-appearance walk nya_system_owner_count does, stopped at `index`.
    u32 seen_count = 0;
    u32 first      = 0;

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        NYA_ConstCString name = nya_system_owner_name(_nya_system_registry.entries[i].owner);

        b8 seen = false;
        for (u32 j = 0; j < i; j++) {
            if (!nya_string_equals(nya_system_owner_name(_nya_system_registry.entries[j].owner), name)) continue;

            seen = true;
            break;
        }

        if (seen) continue;

        if (seen_count == index) {
            first = i;
            break;
        }

        seen_count++;
    }

    NYA_SystemOwnerStats stats = {
        .name = nya_system_owner_name(_nya_system_registry.entries[first].owner),
        .kind = _nya_system_registry.entries[first].owner.kind,
    };

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        const NYA_SystemEntry* entry = &_nya_system_registry.entries[i];
        if (!nya_string_equals(nya_system_owner_name(entry->owner), stats.name)) continue;

        stats.system_count++;
        if (_nya_system_registry.enabled[i]) stats.enabled_count++;

        stats.time_ns += _nya_system_registry.frame_ns[i];

        // Polled rather than reported: a system that allocates has one number to hand back and no
        // bookkeeping to keep in step with the registry.
        if (entry->memory_bytes != nullptr) stats.memory_bytes += entry->memory_bytes();
    }

    return stats;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_system_ceilings_register(void) {
    // Registered from the first registration rather than at some dedicated init: this registry has
    // none, a zeroed array already being a valid empty one. Guarded so a test that resets and refills
    // the registry many times over one process does not add a copy of itself to the ceiling registry
    // each time.
    static b8 ceilings_registered = false;
    if (ceilings_registered) return;

    nya_ceiling_register("systems", NYA_SYSTEM_REGISTRY_MAX, &_nya_system_registry.count);
    nya_ceiling_register("system_pending", NYA_SYSTEM_PENDING_MAX, &_nya_system_registry.pending_count);

    ceilings_registered = true;
}

b8 _nya_system_find(NYA_ConstCString name, OUT u32* out_index) {
    nya_assert(name != nullptr);
    nya_assert(out_index != nullptr);

    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        if (!nya_string_equals(_nya_system_registry.entries[i].name, name)) continue;

        *out_index = i;
        return true;
    }

    return false;
}

NYA_SystemPhaseFn _nya_system_phase_fn(const NYA_SystemEntry* entry, NYA_SystemPhase phase) {
    nya_assert(entry != nullptr);

    switch (phase) {
        case NYA_SYSTEM_PHASE_FRAME: return entry->frame;
        case NYA_SYSTEM_PHASE_TICK: return entry->tick;
        case NYA_SYSTEM_PHASE_RENDER: return entry->render;

        case NYA_SYSTEM_PHASE_COUNT:
        default: nya_unreachable();
    }
}

void _nya_system_defer(_NYA_SystemOp op, NYA_SystemEntry entry) {
    nya_assert((u32)op < (u32)_NYA_SYSTEM_OP_COUNT);
    nya_assert(entry.name != nullptr);

    if (_nya_system_registry.pending_count >= NYA_SYSTEM_PENDING_MAX) {
        // Refused rather than grown, like the registry itself. See NYA_SYSTEM_PENDING_MAX.
        nya_log_warn("A phase run queued more than " FMTu32 " registry changes; '%s' was dropped.", (u32)NYA_SYSTEM_PENDING_MAX, entry.name);
        return;
    }

    _nya_system_registry.pending[_nya_system_registry.pending_count] = (_NYA_SystemPending){ .op = op, .entry = entry };
    _nya_system_registry.pending_count++;
}

void _nya_system_register_now(NYA_SystemEntry entry) {
    nya_assert(entry.name != nullptr);

    if (_nya_system_registry.count >= NYA_SYSTEM_REGISTRY_MAX) {
        // Refused rather than grown. See NYA_SYSTEM_REGISTRY_MAX.
        nya_log_warn("System registry is full at " FMTu32 "; '%s' was not registered.", (u32)NYA_SYSTEM_REGISTRY_MAX, entry.name);
        return;
    }

    // Loud rather than silently shadowed: two systems answering to the same name would leave `after`,
    // `enable` and `unregister` pointing at whichever of them a linear search happened to find first,
    // which is not a decision anyone made on purpose.
    u32 duplicate = 0;
    nya_assert(!_nya_system_find(entry.name, &duplicate), "system '%s' is already registered", entry.name);

    // A plugin that forgets its name would report its time as the engine's, which is the one thing the
    // owner field exists to prevent.
    nya_assert((u32)entry.owner.kind < (u32)NYA_SYSTEM_OWNER_KIND_COUNT, "system '%s' has no valid owner kind", entry.name);
    nya_assert(
        (entry.owner.kind == NYA_SYSTEM_OWNER_PLUGIN) == (entry.owner.plugin != nullptr),
        "system '%s': owner.plugin is required for a plugin system and must be null for any other",
        entry.name
    );

    _nya_system_registry.entries[_nya_system_registry.count]      = entry;
    _nya_system_registry.enabled[_nya_system_registry.count]      = true;
    _nya_system_registry.initialized[_nya_system_registry.count]  = false;
    _nya_system_registry.measuring_ns[_nya_system_registry.count] = 0;
    _nya_system_registry.frame_ns[_nya_system_registry.count]     = 0;
    _nya_system_registry.count++;

    // Appended for now; the barrier puts it where `after` says it goes.
    _nya_system_registry.order_dirty = true;
}

void _nya_system_unregister_now(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    u32 index = 0;
    if (!_nya_system_find(name, &index)) return;

    // Removing what something else is ordered against would leave that `after` dangling, and the next
    // sort would fail somewhere far from here. Unregister the dependents first.
    for (u32 i = 0; i < _nya_system_registry.count; i++) {
        if (_nya_system_registry.entries[i].after == nullptr) continue;

        nya_assert(
            !nya_string_equals(_nya_system_registry.entries[i].after, name),
            "system '%s' cannot be unregistered while '%s' is registered to run after it",
            name,
            _nya_system_registry.entries[i].name
        );
    }

    // Paired with the bring-up: what came up goes down, here rather than at shutdown, because after
    // this the registry has no record of it.
    if (_nya_system_registry.initialized[index]) {
        _nya_system_registry.initialized[index] = false;

        NYA_SystemDeinitFn deinit = _nya_system_registry.entries[index].deinit;
        if (deinit != nullptr) deinit();
    }

    // Shifted rather than swapped with the last: the array is the run order, and a swap would reorder
    // two systems that never asked to move.
    for (u32 i = index; i + 1 < _nya_system_registry.count; i++) {
        _nya_system_registry.entries[i]      = _nya_system_registry.entries[i + 1];
        _nya_system_registry.enabled[i]      = _nya_system_registry.enabled[i + 1];
        _nya_system_registry.initialized[i]  = _nya_system_registry.initialized[i + 1];
        _nya_system_registry.measuring_ns[i] = _nya_system_registry.measuring_ns[i + 1];
        _nya_system_registry.frame_ns[i]     = _nya_system_registry.frame_ns[i + 1];
    }

    _nya_system_registry.count--;

    // Nothing to re-sort: dropping one entry cannot break an order the rest already satisfied, and the
    // loop above proved no `after` pointed at it.
}

void _nya_system_enabled_set_now(NYA_ConstCString name, b8 enabled) {
    nya_assert(name != nullptr);

    u32 index = 0;
    nya_assert(_nya_system_find(name, &index), "system '%s' cannot be %s: it is not registered", name, enabled ? "enabled" : "disabled");

    _nya_system_registry.enabled[index] = enabled;
}

NYA_Error _nya_system_sort(void) {
    u32 count = _nya_system_registry.count;

    /*
     * The two constraints as edges, resolved from names once so the ordering loop below is pure index
     * work. At most one of each per entry, which is what keeps this two arrays rather than a graph.
     */
    u32 predecessor[NYA_SYSTEM_REGISTRY_MAX];
    b8  has_predecessor[NYA_SYSTEM_REGISTRY_MAX] = { 0 };
    u32 successor[NYA_SYSTEM_REGISTRY_MAX];
    b8  has_successor[NYA_SYSTEM_REGISTRY_MAX] = { 0 };

    // How many systems still have to be placed before this one can be. Bounded by the registry size.
    u8 blocked_by[NYA_SYSTEM_REGISTRY_MAX] = { 0 };

    static_assert(NYA_SYSTEM_REGISTRY_MAX <= 255, "blocked_by counts predecessors and has to hold the whole registry");

    for (u32 i = 0; i < count; i++) {
        const NYA_SystemEntry* entry = &_nya_system_registry.entries[i];

        if (entry->after != nullptr) {
            if (!_nya_system_find(entry->after, &predecessor[i])) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "system '%s' is registered to run after '%s', which was never registered", entry->name,
                                 entry->after);
            }

            has_predecessor[i] = true;
            blocked_by[i]++;
        }

        if (entry->before != nullptr) {
            if (!_nya_system_find(entry->before, &successor[i])) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "system '%s' is registered to run before '%s', which was never registered", entry->name,
                                 entry->before);
            }

            has_successor[i] = true;
            blocked_by[successor[i]]++;
        }
    }

    b8  placed[NYA_SYSTEM_REGISTRY_MAX] = { 0 };
    u32 order[NYA_SYSTEM_REGISTRY_MAX];
    u32 order_count = 0;

    /*
     * Repeatedly take the earliest registered system nothing is still waiting on. Taking the earliest
     * rather than any of them is what makes the result stable: a registration list with no constraints
     * at all comes back in exactly the order it was written, and adding one `after` moves one system
     * instead of reshuffling the rest.
     */
    while (order_count < count) {
        b8  found = false;
        u32 next  = 0;

        for (u32 i = 0; i < count; i++) {
            if (placed[i] || blocked_by[i] > 0) continue;

            next  = i;
            found = true;
            break;
        }

        if (!found) break;

        placed[next] = true;

        order[order_count] = next;
        order_count++;

        // Everything that was waiting on `next` is one step closer.
        for (u32 i = 0; i < count; i++) {
            if (placed[i] || !has_predecessor[i] || predecessor[i] != next) continue;
            blocked_by[i]--;
        }

        if (has_successor[next] && !placed[successor[next]]) blocked_by[successor[next]]--;
    }

    if (order_count < count) {
        /*
         * Everything left is waiting on something else that is left, so walking backwards from any of
         * them has to arrive somewhere it has already been. That is the cycle, and printing it is the
         * only useful thing to say: a "could not order 4 systems" would leave the reader to find it.
         */
        u32 start = 0;
        for (u32 i = 0; i < count; i++) {
            if (placed[i]) continue;

            start = i;
            break;
        }

        u32 path[NYA_SYSTEM_REGISTRY_MAX];
        b8  on_path[NYA_SYSTEM_REGISTRY_MAX] = { 0 };
        u32 path_length                      = 0;
        u32 current                          = start;

        while (!on_path[current]) {
            on_path[current]   = true;
            path[path_length]  = current;
            path_length++;

            b8  stepped = false;
            u32 earlier = 0;

            if (has_predecessor[current] && !placed[predecessor[current]]) {
                earlier = predecessor[current];
                stepped = true;
            } else {
                for (u32 i = 0; i < count; i++) {
                    if (placed[i] || !has_successor[i] || successor[i] != current) continue;

                    earlier = i;
                    stepped = true;
                    break;
                }
            }

            if (!stepped) break;
            current = earlier;
        }

        u32 cycle_start = 0;
        for (u32 i = 0; i < path_length; i++) {
            if (path[i] != current) continue;

            cycle_start = i;
            break;
        }

        u8  message[NYA_ERROR_MESSAGE_MAX_LENGTH];
        u64 length  = 0;
        s32 written = snprintf((char*)message, sizeof(message), "system registry has a cycle: ");
        if (written > 0) length = (u64)written;

        // `path` was walked backwards, so it is printed backwards to read as the order would run.
        for (u32 i = path_length; i > cycle_start && length < sizeof(message); i--) {
            written = snprintf((char*)&message[length], sizeof(message) - length, "%s -> ", _nya_system_registry.entries[path[i - 1]].name);
            if (written > 0) length += (u64)written;
        }

        if (length < sizeof(message)) {
            (void)snprintf((char*)&message[length], sizeof(message) - length, "%s", _nya_system_registry.entries[path[path_length - 1]].name);
        }

        return nya_error(NYA_ERROR_NOT_OK, "%s", (NYA_ConstCString)message);
    }

    nya_assert(order_count == count, "the sort dropped " FMTu32 " of " FMTu32 " systems", count - order_count, count);

    // Permuted only now that the order is known to be legal, so a rejected graph leaves the registry
    // exactly as the caller left it and the error can be fixed and finalize called again.
    NYA_SystemEntry sorted_entries[NYA_SYSTEM_REGISTRY_MAX];
    b8              sorted_enabled[NYA_SYSTEM_REGISTRY_MAX];
    b8              sorted_initialized[NYA_SYSTEM_REGISTRY_MAX];
    u64             sorted_measuring_ns[NYA_SYSTEM_REGISTRY_MAX];
    u64             sorted_frame_ns[NYA_SYSTEM_REGISTRY_MAX];

    for (u32 i = 0; i < order_count; i++) {
        sorted_entries[i]      = _nya_system_registry.entries[order[i]];
        sorted_enabled[i]      = _nya_system_registry.enabled[order[i]];
        sorted_initialized[i]  = _nya_system_registry.initialized[order[i]];
        sorted_measuring_ns[i] = _nya_system_registry.measuring_ns[order[i]];
        sorted_frame_ns[i]     = _nya_system_registry.frame_ns[order[i]];
    }

    nya_memcpy(_nya_system_registry.entries, sorted_entries, order_count * sizeof(NYA_SystemEntry));
    nya_memcpy(_nya_system_registry.enabled, sorted_enabled, order_count * sizeof(b8));
    nya_memcpy(_nya_system_registry.initialized, sorted_initialized, order_count * sizeof(b8));
    nya_memcpy(_nya_system_registry.measuring_ns, sorted_measuring_ns, order_count * sizeof(u64));
    nya_memcpy(_nya_system_registry.frame_ns, sorted_frame_ns, order_count * sizeof(u64));

    return NYA_OK;
}

NYA_Error _nya_system_barrier(void) {
    nya_assert(!_nya_system_registry.running, "the registry barrier ran while a phase was still running");

    // In the order they were made, so a register followed by a disable of the same name still works.
    for (u32 i = 0; i < _nya_system_registry.pending_count; i++) {
        const _NYA_SystemPending* change = &_nya_system_registry.pending[i];

        switch (change->op) {
            case _NYA_SYSTEM_OP_REGISTER: _nya_system_register_now(change->entry); break;
            case _NYA_SYSTEM_OP_UNREGISTER: _nya_system_unregister_now(change->entry.name); break;
            case _NYA_SYSTEM_OP_ENABLE: _nya_system_enabled_set_now(change->entry.name, true); break;
            case _NYA_SYSTEM_OP_DISABLE: _nya_system_enabled_set_now(change->entry.name, false); break;

            case _NYA_SYSTEM_OP_COUNT:
            default: nya_unreachable();
        }
    }

    _nya_system_registry.pending_count = 0;

    if (!_nya_system_registry.order_dirty) return NYA_OK;

    NYA_Error ordered = _nya_system_sort();
    if (!ordered.ok) return ordered;

    _nya_system_registry.order_dirty = false;

    return NYA_OK;
}

#ifdef NYA_TESTING
__attr_maybe_unused void _nya_system_registry_reset_for_test(void) {
    _nya_system_registry = (_NYA_SystemRegistry){ 0 };
}
#endif
