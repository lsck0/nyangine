#include "nyangine/nyangine.h"

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

void nya_system_sim_init(void) {
    // Its own arena rather than the app frame allocator: records must outlive every update in the
    // frame and are dropped only after the observers have read them, right when this system chooses to.
    NYA_Arena* allocator = nya_arena_create(.name = "sim_allocator");

    nya_world()->sim_system = (NYA_SimSystem){
        .allocator = allocator,
        .records   = nya_array_create(allocator, NYA_SimRecord),
        .commands  = nya_array_create(allocator, NYA_SimCommand),
    };

    nya_log_info("Simulation system initialized.");

    // Guarded so bringing the app up and down within one process — tests do this a lot — does not
    // add a copy of itself to the ceiling registry on every cycle.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("sim_observers", NYA_SIM_OBSERVER_MAX, &nya_world()->sim_system.observer_count);
        ceiling_registered = true;
    }
}

void nya_system_sim_deinit(void) {
    NYA_SimSystem* sim = &nya_world()->sim_system;

    nya_arena_destroy(sim->allocator);
    *sim = (NYA_SimSystem){ 0 };

    nya_log_info("Simulation system deinitialized.");
}

void nya_system_sim_apply_commands(void) {
    nya_perf_time_this_function();

    NYA_SimSystem* sim = &nya_world()->sim_system;

    if (sim->draining) return; // already inside a drain; the outer loop will pick these up

    sim->draining = true;

    // A command may defer more work, so the queue is drained rather than iterated once: each pass
    // takes the current contents and clears it, so new commands run in the next pass, in deferred order.
    u32 pass = 0;
    while (sim->commands->length > 0) {
        nya_assert(
            pass < NYA_SIM_COMMAND_DRAIN_MAX,
            "Deferred commands are still being produced after %d drain passes; this is a cycle.",
            NYA_SIM_COMMAND_DRAIN_MAX
        );
        pass++;

        u64             count = sim->commands->length;
        NYA_SimCommand* batch = nya_arena_alloc(sim->allocator, count * sizeof(NYA_SimCommand));
        nya_memcpy(batch, sim->commands->items, count * sizeof(NYA_SimCommand));

        nya_array_clear(sim->commands);

        for (u64 i = 0; i < count; i++) batch[i].apply(batch[i].data);
    }

    sim->draining = false;
}

void nya_system_sim_end_frame(void) {
    nya_perf_time_this_function();

    NYA_SimSystem* sim = &nya_world()->sim_system;

    // Anything still queued missed every barrier this frame; applying it here rather than carrying it
    // over avoids a command landing a frame late — the kind of bug that shows up as a flicker much later.
    nya_system_sim_apply_commands();

    for (u32 i = 0; i < sim->observer_count; i++) {
        NYA_SimObserverFn observer = nya_callback_get(sim->observers[i].callback);
        if (observer != nullptr) observer(sim->records, sim->observers[i].user_data);
    }

    // The arena holds the records, the commands and both arrays, so it cannot simply be reset out
    // from under them; the arrays are rebuilt on the cleared arena.
    u64 tick                          = sim->tick;
    u32 observer_count                = sim->observer_count;
    typeof(sim->observers[0]) saved[NYA_SIM_OBSERVER_MAX];
    nya_memcpy(saved, sim->observers, sizeof(saved));

    nya_arena_free_all(sim->allocator);

    sim->records        = nya_array_create(sim->allocator, NYA_SimRecord);
    sim->commands       = nya_array_create(sim->allocator, NYA_SimCommand);
    sim->tick           = tick;
    sim->observer_count = observer_count;
    nya_memcpy(sim->observers, saved, sizeof(saved));
    sim->draining = false;
}

/*
 * ─────────────────────────────────────────────────────────
 * SIMULATION FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_sim_record(u32 type, const void* data, u64 size) {
    nya_assert((data == nullptr) == (size == 0), "nya_sim_record needs either both data and size, or neither.");

    NYA_SimSystem* sim = &nya_world()->sim_system;

    void* copy = nullptr;
    if (size > 0) {
        copy = nya_arena_alloc(sim->allocator, size);
        nya_memcpy(copy, data, size);
    }

    nya_array_push_back(
        sim->records,
        ((NYA_SimRecord){
            .type = type,
            .tick = sim->tick,
            .data = copy,
            .size = size,
        })
    );
}

void nya_sim_defer(NYA_SimCommandFn apply, const void* data, u64 size) {
    nya_assert(apply != nullptr);
    nya_assert((data == nullptr) == (size == 0), "nya_sim_defer needs either both data and size, or neither.");

    NYA_SimSystem* sim = &nya_world()->sim_system;

    // Copied rather than referenced: the caller's payload is usually a stack local that will have
    // returned long before the barrier runs.
    void* copy = nullptr;
    if (size > 0) {
        copy = nya_arena_alloc(sim->allocator, size);
        nya_memcpy(copy, data, size);
    }

    nya_array_push_back(
        sim->commands,
        ((NYA_SimCommand){
            .apply = apply,
            .data  = copy,
            .size  = size,
        })
    );
}

const NYA_ArrayᐸNYA_SimRecordᐳ* nya_sim_records(void) {
    return nya_world()->sim_system.records;
}

u64 nya_sim_tick(void) {
    return nya_world()->sim_system.tick;
}

NYA_Error nya_sim_observer_add(NYA_CallbackHandle observer, void* user_data) {
    // Zero is the "no callback" handle in both builds: a null pointer in the shipping one, and an
    // index the registry never hands out in the hot reloading one.
    if (observer == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "simulation observer is null");

    NYA_SimSystem* sim = &nya_world()->sim_system;

    if (sim->observer_count >= NYA_SIM_OBSERVER_MAX) {
        return nya_error(NYA_ERROR_NOT_OK, "cannot register more than %d simulation observers", NYA_SIM_OBSERVER_MAX);
    }

    sim->observers[sim->observer_count].callback  = observer;
    sim->observers[sim->observer_count].user_data = user_data;
    sim->observer_count++;

    return NYA_OK;
}

void nya_sim_observer_clear(void) {

    nya_world()->sim_system.observer_count = 0;
}
