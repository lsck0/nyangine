#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One slot in the pool. `generation` is odd while running, so a zeroed slot is never a live handle. */
typedef struct {
    b8              active;
    NYA_TweenTarget target;
    void*           address;

    /** The value when this run began, read at the first sample rather than at creation. See `started`. */
    f32x4 from;
    f32x4 to;

    f32 elapsed_s;
    f32 duration_s;
    f32 delay_s;

    u32 repeat_left;
    b8  yoyo;
    b8  reversed;
    b8  started;

    NYA_EaseType       ease;
    NYA_CallbackHandle on_complete;

    u32 generation;
} _NYA_TweenSlot;

typedef struct {
    _NYA_TweenSlot slots[NYA_TWEEN_MAX];
    u32            count;

    /** Bumped per allocation so a reused slot never answers to the handle the last one had. */
    u32 next_generation;
} _NYA_TweenSystem;

NYA_INTERNAL _NYA_TweenSystem _nya_tween_system = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many floats a target kind writes. Everything is stored as an f32x4 and truncated on write. */
NYA_INTERNAL u32 _nya_tween_width(NYA_TweenTarget target) {
    switch (target) {
        case NYA_TWEEN_TARGET_F32: return 1;
        case NYA_TWEEN_TARGET_F32X2: return 2;
        case NYA_TWEEN_TARGET_F32X3: return 3;
        case NYA_TWEEN_TARGET_F32X4: return 4;
        case NYA_TWEEN_TARGET_COUNT:
        default: break;
    }
    return 0;
}

/** Reads the current value of a slot's target into the common f32x4 form. */
NYA_INTERNAL f32x4 _nya_tween_read(const _NYA_TweenSlot* slot) {
    f32x4      out   = { 0 };
    const f32* floats = (const f32*)slot->address;

    for (u32 i = 0; i < _nya_tween_width(slot->target); i++) out[i] = floats[i];

    return out;
}

NYA_INTERNAL void _nya_tween_write(const _NYA_TweenSlot* slot, f32x4 value) {
    f32* floats = (f32*)slot->address;

    for (u32 i = 0; i < _nya_tween_width(slot->target); i++) floats[i] = value[i];
}

/** Takes a free slot, or index 0 with generation 0 when the pool is full. */
NYA_INTERNAL _NYA_TweenSlot* _nya_tween_acquire(OUT NYA_Tween* out_handle) {
    for (u32 i = 1; i < NYA_TWEEN_MAX; i++) {
        _NYA_TweenSlot* slot = &_nya_tween_system.slots[i];
        if (slot->active) continue;

        *slot = (_NYA_TweenSlot){ .active = true, .generation = ++_nya_tween_system.next_generation };

        _nya_tween_system.count++;
        *out_handle = (NYA_Tween){ .index = i, .generation = slot->generation };

        return slot;
    }

    // Refused rather than grown. See NYA_TWEEN_MAX.
    nya_log_warn("Tween pool is full at " FMTu32 "; the request was dropped.", (u32)NYA_TWEEN_MAX);
    *out_handle = NYA_TWEEN_NONE;

    return nullptr;
}

NYA_INTERNAL void _nya_tween_free(_NYA_TweenSlot* slot) {
    if (!slot->active) return;

    slot->active = false;
    slot->address = nullptr;
    _nya_tween_system.count--;
}

NYA_INTERNAL _NYA_TweenSlot* _nya_tween_resolve(NYA_Tween tween) {
    if (tween.index == 0 || tween.index >= NYA_TWEEN_MAX) return nullptr;

    _NYA_TweenSlot* slot = &_nya_tween_system.slots[tween.index];
    if (!slot->active || slot->generation != tween.generation) return nullptr;

    return slot;
}

NYA_INTERNAL NYA_Tween _nya_tween_start(NYA_TweenTarget target, void* address, f32x4 to, f32 duration_s, NYA_TweenOptions options) {
    nya_assert(address != nullptr, "a tween needs somewhere to write");

    NYA_Tween       handle = NYA_TWEEN_NONE;
    _NYA_TweenSlot* slot   = _nya_tween_acquire(&handle);
    if (slot == nullptr) return NYA_TWEEN_NONE;

    slot->target      = target;
    slot->address     = address;
    slot->to          = to;
    slot->duration_s  = duration_s > 0.0F ? duration_s : 0.0F;
    slot->delay_s     = options.delay > 0.0F ? options.delay : 0.0F;
    slot->ease        = options.ease;
    slot->on_complete = options.on_complete;
    slot->yoyo        = options.yoyo;
    slot->repeat_left = options.repeat == 0 ? 1 : options.repeat;

    return handle;
}

/** Runs the completion callback, if there is one. Never called on the cancel path. */
NYA_INTERNAL void _nya_tween_complete(_NYA_TweenSlot* slot) {
    NYA_CallbackHandle handle  = slot->on_complete;
    void*              address = slot->address;

    _nya_tween_free(slot);

    if (handle == 0) return;

    // Resolved by name through the callback registry, so it survives a hot reload. The slot is freed
    // first, so a callback that starts another tween can reuse this one.
    NYA_TweenOnCompleteFn on_complete = nya_callback_get(handle);
    if (on_complete != nullptr) on_complete(address);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_system_tween_init(void) {
    _nya_tween_system = (_NYA_TweenSystem){ 0 };
    nya_log_info("Tween system initialized (" FMTu32 " slots).", (u32)NYA_TWEEN_MAX);

    // Registered once rather than on every init: a game that brings the app up and down within one
    // process (tests do this a lot) should not fill the ceiling registry with copies of itself.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("tweens", NYA_TWEEN_MAX, &_nya_tween_system.count);
        ceiling_registered = true;
    }
}

void nya_system_tween_deinit(void) {
    _nya_tween_system = (_NYA_TweenSystem){ 0 };
    nya_log_info("Tween system deinitialized.");
}

void nya_system_tween_update(f32 delta_time_s) {
    if (delta_time_s <= 0.0F) return;

    for (u32 i = 1; i < NYA_TWEEN_MAX; i++) {
        _NYA_TweenSlot* slot = &_nya_tween_system.slots[i];
        if (!slot->active) continue;

        // Per slot, not the parameter: a delay expiring hands the rest of the frame to that one tween,
        // and writing that remainder back into delta_time_s would shorten the frame for every slot
        // after it in this loop.
        f32 step_s = delta_time_s;

        // The delay runs down before anything is read, so `from` is the value at the moment the tween
        // actually begins — not the value it had when it was created, which may be several tweens ago.
        if (slot->delay_s > 0.0F) {
            slot->delay_s -= step_s;
            if (slot->delay_s > 0.0F) continue;

            // Whatever is left of the frame belongs to the tween itself.
            step_s        = -slot->delay_s;
            slot->delay_s = 0.0F;
        }

        if (!slot->started) {
            slot->from    = _nya_tween_read(slot);
            slot->started = true;
        }

        // A zero duration is a set, not a tween: land on the target and finish this frame.
        if (slot->duration_s <= 0.0F) {
            _nya_tween_write(slot, slot->to);
            _nya_tween_complete(slot);
            continue;
        }

        slot->elapsed_s += step_s;

        f32 t = nya_clamp(slot->elapsed_s / slot->duration_s, 0.0F, 1.0F);
        f32 e = nya_ease(slot->ease, slot->reversed ? 1.0F - t : t);

        f32x4 value = slot->from + ((slot->to - slot->from) * e);
        _nya_tween_write(slot, value);

        if (t < 1.0F) continue;

        if (slot->repeat_left != NYA_TWEEN_REPEAT_FOREVER) slot->repeat_left--;

        if (slot->repeat_left == 0) {
            _nya_tween_complete(slot);
            continue;
        }

        // Another run. Restarting from `from` rather than from where it ended is what stops a loop
        // drifting; yoyo plays the same interval backwards instead.
        slot->elapsed_s = 0.0F;
        if (slot->yoyo) slot->reversed = !slot->reversed;
    }
}

NYA_Tween nya_tween_f32_with_options(f32* address, f32 to, f32 duration_s, NYA_TweenOptions options) {
    return _nya_tween_start(NYA_TWEEN_TARGET_F32, address, (f32x4){ to, 0.0F, 0.0F, 0.0F }, duration_s, options);
}

NYA_Tween nya_tween_f32x2_with_options(f32x2* address, f32x2 to, f32 duration_s, NYA_TweenOptions options) {
    return _nya_tween_start(NYA_TWEEN_TARGET_F32X2, address, (f32x4){ to.x, to.y, 0.0F, 0.0F }, duration_s, options);
}

NYA_Tween nya_tween_f32x3_with_options(f32x3* address, f32x3 to, f32 duration_s, NYA_TweenOptions options) {
    return _nya_tween_start(NYA_TWEEN_TARGET_F32X3, address, (f32x4){ to.x, to.y, to.z, 0.0F }, duration_s, options);
}

NYA_Tween nya_tween_f32x4_with_options(f32x4* address, f32x4 to, f32 duration_s, NYA_TweenOptions options) {
    return _nya_tween_start(NYA_TWEEN_TARGET_F32X4, address, to, duration_s, options);
}

NYA_Tween nya_tween_sequence(const NYA_TweenSpec* specs, u32 count) {
    if (specs == nullptr || count == 0) return NYA_TWEEN_NONE;

    NYA_Tween last   = NYA_TWEEN_NONE;
    f32       offset = 0.0F;

    for (u32 i = 0; i < count; i++) {
        NYA_TweenSpec    spec    = specs[i];
        NYA_TweenOptions options = spec.options;

        // Each step waits out everything before it, plus whatever gap it asked for itself.
        offset       += options.delay;
        options.delay = offset;

        f32x4 to = { 0 };
        switch (spec.target) {
            case NYA_TWEEN_TARGET_F32:   to = (f32x4){ spec.to_f32, 0.0F, 0.0F, 0.0F }; break;
            case NYA_TWEEN_TARGET_F32X2: to = (f32x4){ spec.to_f32x2.x, spec.to_f32x2.y, 0.0F, 0.0F }; break;
            case NYA_TWEEN_TARGET_F32X3: to = (f32x4){ spec.to_f32x3.x, spec.to_f32x3.y, spec.to_f32x3.z, 0.0F }; break;
            case NYA_TWEEN_TARGET_F32X4: to = spec.to_f32x4; break;
            case NYA_TWEEN_TARGET_COUNT:
            default: continue;
        }

        last = _nya_tween_start(spec.target, spec.address, to, spec.duration, options);

        offset += spec.duration > 0.0F ? spec.duration : 0.0F;
    }

    return last;
}

void nya_tween_cancel(NYA_Tween tween) {
    _NYA_TweenSlot* slot = _nya_tween_resolve(tween);
    if (slot != nullptr) _nya_tween_free(slot);
}

u32 nya_tween_cancel_target(const void* address) {
    if (address == nullptr) return 0;

    u32 cancelled = 0;

    for (u32 i = 1; i < NYA_TWEEN_MAX; i++) {
        _NYA_TweenSlot* slot = &_nya_tween_system.slots[i];
        if (!slot->active || slot->address != address) continue;

        _nya_tween_free(slot);
        cancelled++;
    }

    return cancelled;
}

void nya_tween_cancel_all(void) {
    for (u32 i = 1; i < NYA_TWEEN_MAX; i++) _nya_tween_free(&_nya_tween_system.slots[i]);
}

b8 nya_tween_active(NYA_Tween tween) {
    return _nya_tween_resolve(tween) != nullptr;
}

u32 nya_tween_count(void) {
    return _nya_tween_system.count;
}

f32 nya_tween_progress(NYA_Tween tween) {
    const _NYA_TweenSlot* slot = _nya_tween_resolve(tween);

    // A handle that no longer resolves is a tween that finished, and a finished tween is at its target.
    if (slot == nullptr) return 1.0F;

    // Still waiting out its delay, so it has not moved anything yet.
    if (slot->delay_s > 0.0F || slot->duration_s <= 0.0F) return 0.0F;

    f32 t = nya_clamp(slot->elapsed_s / slot->duration_s, 0.0F, 1.0F);

    return nya_ease(slot->ease, slot->reversed ? 1.0F - t : t);
}
