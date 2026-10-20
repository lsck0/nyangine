#include "nyangine-core/nyangine.h"

/*
 * -----------------------------------------------------------------------------------------------------------------------
 * STATE
 * -----------------------------------------------------------------------------------------------------------------------
 */

#if NYA_SCOPE_ENABLED

/** One open scope. A release build keeps the site and nothing else. */
typedef struct {
    NYA_ScopeSite* site;

#if NYA_SCOPE_MEASURED
    u32          slot;
    NYA_ScopeTag tag;
    b8           tagged;

    u64 started_ns;
    u64 children_ns;

    u64 allocated_at_entry_bytes;
    u64 children_allocated_bytes;

    /** Signed: a thread can free what another allocated, or reset an arena that was full before it entered. */
    s64 live_at_entry_bytes;
    s64 peak_live_bytes;
#endif
} _NYA_ScopeFrame;

typedef struct {
    _NYA_ScopeFrame frames[NYA_SCOPE_DEPTH_MAX];
    u32             depth;

#if NYA_SCOPE_MEASURED
    u64 allocated_bytes;
    s64 live_bytes;
#endif
} _NYA_ScopeThread;

NYA_INTERNAL thread_local _NYA_ScopeThread _nya_scope_thread = { 0 };

#endif // NYA_SCOPE_ENABLED

#if NYA_SCOPE_MEASURED

typedef struct {
    char name[NYA_SCOPE_NAME_MAX];
    char function[NYA_SCOPE_NAME_MAX];
    char file[NYA_SCOPE_FILE_MAX];
    u32  line;
    u64  hash;

    /* The open frame, from any thread. */
    atomic u32 calls;
    atomic u64 total_ns;
    atomic u64 self_ns;
    atomic u64 allocated_bytes;
    atomic u64 allocated_self_bytes;
    atomic u64 peak_bytes;
    atomic u64 high_water_bytes;

    /* Closed frames, a ring per site, saturating at the u32 bound: four seconds, four gibibytes. */
    u32 history_calls[NYA_SCOPE_HISTORY];
    u32 history_total_ns[NYA_SCOPE_HISTORY];
    u32 history_self_ns[NYA_SCOPE_HISTORY];
    u32 history_allocated_bytes[NYA_SCOPE_HISTORY];
    u32 history_allocated_self_bytes[NYA_SCOPE_HISTORY];
    u32 history_peak_bytes[NYA_SCOPE_HISTORY];
} _NYA_ScopeEntry;

typedef struct {
    _NYA_ScopeEntry entries[NYA_SCOPE_SITE_MAX];

    /** Published after the entry it counts is written, so a lock free scan below it reads whole entries. */
    atomic u32 count;

    /** Taken only to add a site. Scanning for one that exists needs nothing. */
    atomic_flag lock;

    atomic(NYA_ScopeTagSinkFn) sink;
    atomic u32                 depth_deepest;
    atomic b8                  overflow_reported;

    u32 history_cursor;
    u32 history_count;

    /* Plain copies of the atomics above for the ceiling registry, written once a frame on the main thread. */
    u32 count_live;
    u32 depth_deepest_live;
    b8  registered;
} _NYA_Scope;

/** Row zero is where a site goes once the table is full. */
NYA_INTERNAL _NYA_Scope _nya_scope = {
    .entries = { [0] = { .name = "untracked", .function = "", .file = "" } },
    .count   = 1,
    .lock    = ATOMIC_FLAG_INIT,
};

#endif // NYA_SCOPE_MEASURED

/*
 * -----------------------------------------------------------------------------------------------------------------------
 * PRIVATE API DECLARATION
 * -----------------------------------------------------------------------------------------------------------------------
 */

#if NYA_SCOPE_MEASURED
/** The site's row, added on its first entry. Lock free unless the site is new. */
NYA_INTERNAL u32 _nya_scope_slot_resolve(NYA_ScopeSite* site) __attr_no_discard;

/** The row in [first, count) that is `site`, or zero. */
NYA_INTERNAL u32 _nya_scope_slot_find(const NYA_ScopeSite* site, u64 hash, u32 first, u32 count) __attr_no_discard;

NYA_INTERNAL u64  _nya_scope_site_hash(const NYA_ScopeSite* site) __attr_no_discard;
NYA_INTERNAL void _nya_scope_atomic_max(atomic u64* target, u64 value);
NYA_INTERNAL u32  _nya_scope_saturate(u64 value) __attr_no_discard;
NYA_INTERNAL void _nya_scope_window(const u32* samples, u32 count, OUT f64* out_average, OUT u32* out_max);
#endif

/*
 * -----------------------------------------------------------------------------------------------------------------------
 * PUBLIC API IMPLEMENTATION
 * -----------------------------------------------------------------------------------------------------------------------
 */

void nya_scope_frame_end(void) {
#if NYA_SCOPE_MEASURED
    if (!_nya_scope.registered) {
        nya_ceiling_register("scope_sites", NYA_SCOPE_SITE_MAX, &_nya_scope.count_live);
        nya_ceiling_register("scope_depth", NYA_SCOPE_DEPTH_MAX, &_nya_scope.depth_deepest_live);
        _nya_scope.registered = true;
    }

    const u32 cursor = _nya_scope.history_cursor;
    const u32 count  = atomic_load_explicit(&_nya_scope.count, memory_order_acquire);

    for (u32 i = 0; i < count; i++) {
        _NYA_ScopeEntry* entry = &_nya_scope.entries[i];

        entry->history_calls[cursor]                = atomic_exchange_explicit(&entry->calls, 0, memory_order_relaxed);
        entry->history_total_ns[cursor]             = _nya_scope_saturate(atomic_exchange_explicit(&entry->total_ns, 0, memory_order_relaxed));
        entry->history_self_ns[cursor]              = _nya_scope_saturate(atomic_exchange_explicit(&entry->self_ns, 0, memory_order_relaxed));
        entry->history_allocated_bytes[cursor]      = _nya_scope_saturate(atomic_exchange_explicit(&entry->allocated_bytes, 0, memory_order_relaxed));
        entry->history_allocated_self_bytes[cursor] = _nya_scope_saturate(atomic_exchange_explicit(&entry->allocated_self_bytes, 0, memory_order_relaxed));
        entry->history_peak_bytes[cursor]           = _nya_scope_saturate(atomic_exchange_explicit(&entry->peak_bytes, 0, memory_order_relaxed));
    }

    _nya_scope.history_cursor     = (cursor + 1) % NYA_SCOPE_HISTORY;
    _nya_scope.history_count      = nya_min(_nya_scope.history_count + 1, (u32)NYA_SCOPE_HISTORY);
    _nya_scope.count_live         = count;
    _nya_scope.depth_deepest_live = atomic_load_explicit(&_nya_scope.depth_deepest, memory_order_relaxed);
#endif
}

u32 nya_scope_depth(void) {
#if NYA_SCOPE_ENABLED
    return _nya_scope_thread.depth;
#else
    return 0;
#endif
}

NYA_ScopeView nya_scope_at(u32 index) {
#if NYA_SCOPE_ENABLED
    const _NYA_ScopeThread* thread = &_nya_scope_thread;
    nya_assert(index < thread->depth, "scope %u asked for with %u open", index, thread->depth);

    const _NYA_ScopeFrame* frame = &thread->frames[index];

#if NYA_SCOPE_MEASURED
    const _NYA_ScopeEntry* entry = &_nya_scope.entries[frame->slot];

    // a frame's peak only reaches its parent when a child closes, so the open children are read here.
    s64 peak_live_bytes = frame->peak_live_bytes;
    for (u32 deeper = index + 1; deeper < thread->depth; deeper++) peak_live_bytes = nya_max(peak_live_bytes, thread->frames[deeper].peak_live_bytes);

    return (NYA_ScopeView){
        .name            = entry->name,
        .function        = entry->function,
        .file            = entry->file,
        .line            = entry->line,
        .tag             = frame->tag,
        .elapsed_ns      = nya_clock_get_monotonic_ns() - frame->started_ns,
        .allocated_bytes = thread->allocated_bytes - frame->allocated_at_entry_bytes,
        .peak_bytes      = peak_live_bytes > frame->live_at_entry_bytes ? (u64)(peak_live_bytes - frame->live_at_entry_bytes) : 0,
    };
#else
    return (NYA_ScopeView){
        .name     = frame->site->name,
        .function = frame->site->function,
        .file     = frame->site->file,
        .line     = frame->site->line,
    };
#endif
#else
    // nya_scope_depth is zero here, so any index is past it.
    nya_unused(index);
    nya_unreachable();
#endif
}

NYA_ScopeTag nya_scope_tag_current(void) {
#if NYA_SCOPE_MEASURED
    const _NYA_ScopeThread* thread = &_nya_scope_thread;
    return thread->depth > 0 ? thread->frames[thread->depth - 1].tag : NYA_SCOPE_TAG_NONE;
#else
    return NYA_SCOPE_TAG_NONE;
#endif
}

void nya_scope_tag_sink_set(NYA_ScopeTagSinkFn sink) {
#if NYA_SCOPE_MEASURED
    atomic_store_explicit(&_nya_scope.sink, sink, memory_order_release);
#else
    nya_unused(sink);
#endif
}

u32 nya_scope_site_count(void) {
#if NYA_SCOPE_MEASURED
    return atomic_load_explicit(&_nya_scope.count, memory_order_acquire);
#else
    return 0;
#endif
}

u32 nya_scope_stats(OUT NYA_ScopeStats* out, u32 capacity, NYA_ScopeSort sort) {
    nya_assert(out != nullptr || capacity == 0);
    nya_assert(sort < NYA_SCOPE_SORT_COUNT);

#if NYA_SCOPE_MEASURED
    const u32 sites   = atomic_load_explicit(&_nya_scope.count, memory_order_acquire);
    const u32 history = _nya_scope.history_count;

    u32 count = 0;

    for (u32 i = 0; i < sites && count < capacity; i++) {
        const _NYA_ScopeEntry* entry = &_nya_scope.entries[i];

        f64 calls = 0.0, total_ns = 0.0, self_ns = 0.0, allocated = 0.0, allocated_self = 0.0, peak_average = 0.0;
        u32 calls_max = 0, self_max_ns = 0, peak_max = 0, unused = 0;

        _nya_scope_window(entry->history_calls, history, &calls, &calls_max);
        if (calls_max == 0) continue;

        _nya_scope_window(entry->history_total_ns, history, &total_ns, &unused);
        _nya_scope_window(entry->history_self_ns, history, &self_ns, &self_max_ns);
        _nya_scope_window(entry->history_allocated_bytes, history, &allocated, &unused);
        _nya_scope_window(entry->history_allocated_self_bytes, history, &allocated_self, &unused);
        _nya_scope_window(entry->history_peak_bytes, history, &peak_average, &peak_max);

        out[count++] = (NYA_ScopeStats){
            .name                 = entry->name,
            .function             = entry->function,
            .file                 = entry->file,
            .line                 = entry->line,
            .calls                = (f32)calls,
            .total_ms             = total_ns / 1e6,
            .self_ms              = self_ns / 1e6,
            .self_max_ms          = (f64)self_max_ns / 1e6,
            .allocated_bytes      = (u64)allocated,
            .allocated_self_bytes = (u64)allocated_self,
            .peak_bytes           = peak_max,
            .high_water_bytes     = atomic_load_explicit(&entry->high_water_bytes, memory_order_relaxed),
        };
    }

    nya_scope_sort(out, count, sort);

    return count;
#else
    return 0;
#endif
}

void nya_scope_sort(NYA_ScopeStats* stats, u32 count, NYA_ScopeSort sort) {
    nya_assert(stats != nullptr || count == 0);
    nya_assert(sort < NYA_SCOPE_SORT_COUNT);

    // insertion, since it has to be stable and runs on a readout's refresh, not every frame.
    for (u32 i = 1; i < count; i++) {
        NYA_ScopeStats row = stats[i];
        u32            j   = i;

        while (j > 0) {
            const NYA_ScopeStats* before = &stats[j - 1];

            b8 moves = false;

            switch (sort) {
                case NYA_SCOPE_SORT_SELF:      moves = row.self_ms > before->self_ms; break;
                case NYA_SCOPE_SORT_TOTAL:     moves = row.total_ms > before->total_ms; break;
                case NYA_SCOPE_SORT_ALLOCATED: moves = row.allocated_self_bytes > before->allocated_self_bytes; break;
                case NYA_SCOPE_SORT_PEAK:      moves = row.high_water_bytes > before->high_water_bytes; break;
                case NYA_SCOPE_SORT_NAME:      moves = strcmp(row.name, before->name) < 0; break;
                case NYA_SCOPE_SORT_COUNT:
                default:                       nya_unreachable();
            }

            if (!moves) break;

            stats[j] = *before;
            j--;
        }

        stats[j] = row;
    }
}

void nya_scope_report(void) {
#if NYA_SCOPE_MEASURED
    NYA_ScopeStats rows[NYA_SCOPE_SITE_MAX];
    const u32      count = nya_scope_stats(rows, NYA_SCOPE_SITE_MAX, NYA_SCOPE_SORT_SELF);

    nya_log_info("Scopes over %u frames: %-28s %7s %9s %9s %9s %10s %10s %10s  %s", _nya_scope.history_count, "scope", "calls", "self ms", "self max",
                 "total ms", "alloc", "peak", "high", "site");

    for (u32 i = 0; i < count; i++) {
        const NYA_ScopeStats* row = &rows[i];

        nya_log_info("Scopes: %-28s %7.1f %9.3f %9.3f %9.3f %10llu %10llu %10llu  %s:%u", row->name, (f64)row->calls, row->self_ms, row->self_max_ms,
                     row->total_ms, (unsigned long long)row->allocated_self_bytes, (unsigned long long)row->peak_bytes,
                     (unsigned long long)row->high_water_bytes, row->file, row->line);
    }
#else
    nya_log_info("Scopes: this build keeps scope names only, for crash reports; nothing is measured.");
#endif
}

/*
 * -----------------------------------------------------------------------------------------------------------------------
 * INTERNAL API IMPLEMENTATION
 * -----------------------------------------------------------------------------------------------------------------------
 */

#if NYA_SCOPE_ENABLED

NYA_ScopeMark _nya_scope_push(NYA_ScopeSite* site, NYA_ScopeTag tag, b8 tagged) {
    nya_assert(site != nullptr && site->name != nullptr);

    _NYA_ScopeThread* thread = &_nya_scope_thread;
    nya_assert(thread->depth < NYA_SCOPE_DEPTH_MAX, "scope '%s' opened %u deep; raise NYA_SCOPE_DEPTH_MAX or scope less", site->name,
               thread->depth + 1);

    const NYA_ScopeMark mark  = { .depth = thread->depth };
    _NYA_ScopeFrame*    frame = &thread->frames[thread->depth];

#if NYA_SCOPE_MEASURED
    const u32          slot      = _nya_scope_slot_resolve(site);
    const NYA_ScopeTag inherited = thread->depth > 0 ? thread->frames[thread->depth - 1].tag : NYA_SCOPE_TAG_NONE;

    *frame = (_NYA_ScopeFrame){
        .site                     = site,
        .slot                     = slot,
        .tag                      = tagged ? tag : inherited,
        .tagged                   = tagged,
        .allocated_at_entry_bytes = thread->allocated_bytes,
        .live_at_entry_bytes      = thread->live_bytes,
        .peak_live_bytes          = thread->live_bytes,
    };
    thread->depth++;

    // rare after the first frames, so the shared line is read far more often than it is written.
    if (thread->depth > atomic_load_explicit(&_nya_scope.depth_deepest, memory_order_relaxed)) {
        atomic_store_explicit(&_nya_scope.depth_deepest, thread->depth, memory_order_relaxed);
    }

    // last, so the bookkeeping above is not part of the scope's own time.
    frame->started_ns = nya_clock_get_monotonic_ns();
#else
    nya_unused(tag, tagged);
    frame->site = site;
    thread->depth++;
#endif

    return mark;
}

void _nya_scope_pop(NYA_ScopeSite* site) {
#if NYA_SCOPE_MEASURED
    // first, so the bookkeeping below is not part of the scope's own time.
    const u64 ended_ns = nya_clock_get_monotonic_ns();
#endif

    _NYA_ScopeThread* thread = &_nya_scope_thread;
    nya_assert(thread->depth > 0, "scope '%s' closed with none open", site->name);

    _NYA_ScopeFrame* frame = &thread->frames[thread->depth - 1];
    nya_assert(frame->site == site, "scope '%s' closed while '%s' is the innermost", site->name, frame->site->name);

    thread->depth--;

#if NYA_SCOPE_MEASURED
    const u64 elapsed_ns      = ended_ns - frame->started_ns;
    const u64 self_ns         = elapsed_ns > frame->children_ns ? elapsed_ns - frame->children_ns : 0;
    const u64 allocated_bytes = thread->allocated_bytes - frame->allocated_at_entry_bytes;
    const u64 allocated_self  = allocated_bytes - frame->children_allocated_bytes;
    const u64 peak_bytes      = frame->peak_live_bytes > frame->live_at_entry_bytes ? (u64)(frame->peak_live_bytes - frame->live_at_entry_bytes) : 0;

    nya_assert(frame->children_allocated_bytes <= allocated_bytes);

    if (thread->depth > 0) {
        _NYA_ScopeFrame* parent           = &thread->frames[thread->depth - 1];
        parent->children_ns              += elapsed_ns;
        parent->children_allocated_bytes += allocated_bytes;
        parent->peak_live_bytes           = nya_max(parent->peak_live_bytes, frame->peak_live_bytes);
    }

    _NYA_ScopeEntry* entry = &_nya_scope.entries[frame->slot];
    atomic_fetch_add_explicit(&entry->calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&entry->total_ns, elapsed_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&entry->self_ns, self_ns, memory_order_relaxed);

    if (allocated_bytes > 0) {
        atomic_fetch_add_explicit(&entry->allocated_bytes, allocated_bytes, memory_order_relaxed);
        atomic_fetch_add_explicit(&entry->allocated_self_bytes, allocated_self, memory_order_relaxed);
    }

    if (peak_bytes > 0) {
        _nya_scope_atomic_max(&entry->peak_bytes, peak_bytes);
        _nya_scope_atomic_max(&entry->high_water_bytes, peak_bytes);
    }

    if (frame->tag == NYA_SCOPE_TAG_NONE) return;

    NYA_ScopeTagSinkFn sink = atomic_load_explicit(&_nya_scope.sink, memory_order_acquire);
    if (sink != nullptr) sink(frame->tag, frame->started_ns, ended_ns, self_ns, frame->tagged);
#endif
}

void _nya_scope_end(NYA_ScopeMark* mark) {
    nya_assert(mark != nullptr);

    const _NYA_ScopeThread* thread = &_nya_scope_thread;
    nya_assert_eq(thread->depth, mark->depth + 1);

    _nya_scope_pop(thread->frames[mark->depth].site);
}

#else

NYA_ScopeMark _nya_scope_push(NYA_ScopeSite* site, NYA_ScopeTag tag, b8 tagged) {
    nya_unused(site, tag, tagged);
    return (NYA_ScopeMark){ 0 };
}

void _nya_scope_pop(NYA_ScopeSite* site) { nya_unused(site); }

void _nya_scope_end(NYA_ScopeMark* mark) { nya_unused(mark); }

#endif // NYA_SCOPE_ENABLED

#if NYA_SCOPE_MEASURED

void _nya_scope_arena_alloc(u64 size_bytes) {
    _NYA_ScopeThread* thread  = &_nya_scope_thread;
    thread->allocated_bytes  += size_bytes;
    thread->live_bytes       += (s64)size_bytes;

    // the innermost frame only; nya_scope_pop hands it up to the parent.
    if (thread->depth == 0) return;

    _NYA_ScopeFrame* frame = &thread->frames[thread->depth - 1];
    if (thread->live_bytes > frame->peak_live_bytes) frame->peak_live_bytes = thread->live_bytes;
}

void _nya_scope_arena_free(u64 size_bytes) {
    _nya_scope_thread.live_bytes -= (s64)size_bytes;
}

u32 _nya_scope_slot_resolve(NYA_ScopeSite* site) {
    const u32 cached = atomic_load_explicit(&site->slot, memory_order_acquire);
    if (cached != 0) return cached - 1;

    // looked up by content, so a site in a reloaded game DLL finds the row its previous copy made.
    const u64 hash  = _nya_scope_site_hash(site);
    const u32 count = atomic_load_explicit(&_nya_scope.count, memory_order_acquire);
    u32       slot  = _nya_scope_slot_find(site, hash, 1, count);

    b8 overflowed = false;

    if (slot == 0) {
        while (atomic_flag_test_and_set_explicit(&_nya_scope.lock, memory_order_acquire)) {}

        // another thread may have added it between the scan and the lock.
        const u32 locked_count = atomic_load_explicit(&_nya_scope.count, memory_order_relaxed);
        slot                   = _nya_scope_slot_find(site, hash, count, locked_count);

        if (slot == 0 && locked_count < NYA_SCOPE_SITE_MAX) {
            _NYA_ScopeEntry* entry = &_nya_scope.entries[locked_count];

            u64 file_length = strlen(site->file);
            u64 file_skip   = file_length >= NYA_SCOPE_FILE_MAX ? file_length - (NYA_SCOPE_FILE_MAX - 1) : 0;

            // copied: the strings may live in a game DLL that a reload unmaps.
            (void)snprintf(entry->name, sizeof(entry->name), "%s", site->name);
            (void)snprintf(entry->function, sizeof(entry->function), "%s", site->function);
            (void)snprintf(entry->file, sizeof(entry->file), "%s", site->file + file_skip);
            entry->line = site->line;
            entry->hash = hash;

            slot = locked_count;
            atomic_store_explicit(&_nya_scope.count, locked_count + 1, memory_order_release);
        }

        overflowed = slot == 0;

        atomic_flag_clear_explicit(&_nya_scope.lock, memory_order_release);
    }

    // after the lock: the warning allocates, and an allocation must not wait on a scope's table.
    if (overflowed && !atomic_exchange(&_nya_scope.overflow_reported, true)) {
        nya_log_warn("The scope table is full at %d; '%s' and every later site are counted as untracked. Raise NYA_SCOPE_SITE_MAX.", NYA_SCOPE_SITE_MAX,
                     site->name);
    }

    atomic_store_explicit(&site->slot, slot + 1, memory_order_release);

    return slot;
}

u32 _nya_scope_slot_find(const NYA_ScopeSite* site, u64 hash, u32 first, u32 count) {
    for (u32 i = first; i < count; i++) {
        const _NYA_ScopeEntry* entry = &_nya_scope.entries[i];
        if (entry->hash != hash || entry->line != site->line) continue;
        if (strncmp(entry->name, site->name, NYA_SCOPE_NAME_MAX - 1) != 0) continue;

        return i;
    }

    return 0;
}

u64 _nya_scope_site_hash(const NYA_ScopeSite* site) {
    u64 hash = nya_hash_fnv1a(site->name);
    hash     = nya_hash_fnv1a_continue(hash, site->file, strlen(site->file));

    return nya_hash_fnv1a_continue(hash, &site->line, sizeof(site->line));
}

void _nya_scope_atomic_max(atomic u64* target, u64 value) {
    u64 seen = atomic_load_explicit(target, memory_order_relaxed);
    while (value > seen && !atomic_compare_exchange_weak_explicit(target, &seen, value, memory_order_relaxed, memory_order_relaxed)) {}
}

u32 _nya_scope_saturate(u64 value) {
    return (u32)nya_min(value, (u64)UINT32_MAX);
}

void _nya_scope_window(const u32* samples, u32 count, OUT f64* out_average, OUT u32* out_max) {
    u64 total   = 0;
    u32 maximum = 0;

    for (u32 i = 0; i < count; i++) {
        total   += samples[i];
        maximum  = nya_max(maximum, samples[i]);
    }

    *out_average = count > 0 ? (f64)total / (f64)count : 0.0;
    *out_max     = maximum;
}

#else

void _nya_scope_arena_alloc(u64 size_bytes) { nya_unused(size_bytes); }

void _nya_scope_arena_free(u64 size_bytes) { nya_unused(size_bytes); }

#endif // NYA_SCOPE_MEASURED
