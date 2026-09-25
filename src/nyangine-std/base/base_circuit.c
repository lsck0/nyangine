#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_circuit.h"
#include "nyangine-std/base/base_clock.h"

// PRIVATE TYPES

/** One dependency's breaker: its state and the counters that move it between states. */
typedef struct {
    char key[NYA_CIRCUIT_MAX_KEY];

    NYA_CircuitState state;

    /** Consecutive failures seen in CLOSED. Reset by any success; trips OPEN at the threshold. */
    u32 failures;

    /** Probe successes seen in HALF_OPEN. Closes the breaker at the threshold. */
    u32 successes;

    /** Probes let through in HALF_OPEN but not yet reported. Bounded by half_open_max. */
    u32 probes;

    /** Monotonic nanoseconds the breaker last went OPEN, so the cooldown can be measured. */
    u64 opened_at_ns;

    /** Monotonic nanoseconds of the last change, for choosing which CLOSED entry to reuse. */
    u64 updated_at_ns;
} _NYA_CircuitEntry;

struct NYA_CircuitBreaker {
    u32 failure_threshold;
    u32 success_threshold;
    u32 half_open_max;
    u64 open_ns;

    _NYA_CircuitEntry entries[NYA_CIRCUIT_MAX_KEYS];
    u32               entry_count;
};

// PRIVATE API DECLARATION

/** The entry for `key` if it already exists, mutable. Asking never makes one. */
NYA_INTERNAL _NYA_CircuitEntry* _nya_circuit_find(NYA_CircuitBreaker* breaker, NYA_ConstCString key) __attr_no_discard;

/** The entry for `key`, made CLOSED when there is none. Null only when the table is full of tripped keys. */
NYA_INTERNAL _NYA_CircuitEntry* _nya_circuit_entry(NYA_CircuitBreaker* breaker, NYA_ConstCString key, u64 now_ns) __attr_no_discard;

/** Moves an OPEN entry whose cooldown has elapsed to HALF_OPEN, and answers the entry's state. */
NYA_INTERNAL NYA_CircuitState _nya_circuit_resolve(NYA_CircuitBreaker* breaker, _NYA_CircuitEntry* entry, u64 now_ns);

/** nya_circuit_allow at an explicit time, so a test drives the cooldown without sleeping. */
NYA_INTERNAL b8 _nya_circuit_allow_at(NYA_CircuitBreaker* breaker, NYA_ConstCString key, u64 now_ns) __attr_no_discard;

/** nya_circuit_record at an explicit time. */
NYA_INTERNAL void _nya_circuit_record_at(NYA_CircuitBreaker* breaker, NYA_ConstCString key, b8 success, u64 now_ns);

/** nya_circuit_state at an explicit time, so a test reads the state on the same clock it drives the breaker with. */
NYA_INTERNAL NYA_CircuitState _nya_circuit_state_at(NYA_CircuitBreaker* breaker, NYA_ConstCString key, u64 now_ns) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error _nya_circuit_breaker_create(NYA_Arena* arena, NYA_CircuitBreaker** out_breaker, NYA_CircuitBreakerOptions options) {
    nya_assert(arena != nullptr && out_breaker != nullptr);

    *out_breaker = nullptr;

    NYA_CircuitBreaker* breaker = nya_arena_alloc(arena, sizeof(NYA_CircuitBreaker));
    if (breaker == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for a circuit breaker");

    nya_memset(breaker, 0, sizeof(*breaker));

    breaker->failure_threshold = options.failure_threshold > 0 ? options.failure_threshold : NYA_CIRCUIT_FAILURE_THRESHOLD;
    breaker->success_threshold = options.success_threshold > 0 ? options.success_threshold : NYA_CIRCUIT_SUCCESS_THRESHOLD;
    breaker->half_open_max     = options.half_open_max > 0 ? options.half_open_max : 1;
    breaker->open_ns           = (options.open_ms > 0 ? options.open_ms : NYA_CIRCUIT_OPEN_MS) * 1000000ULL;

    *out_breaker = breaker;

    return NYA_OK;
}

void nya_circuit_breaker_destroy(NYA_CircuitBreaker* breaker) {
    if (breaker == nullptr) return;

    nya_memset(breaker->entries, 0, sizeof(breaker->entries));
    breaker->entry_count = 0;
}

b8 nya_circuit_allow(NYA_CircuitBreaker* breaker, NYA_ConstCString key) {
    nya_assert(breaker != nullptr && key != nullptr);

    return _nya_circuit_allow_at(breaker, key, nya_clock_get_monotonic_ns());
}

void nya_circuit_record(NYA_CircuitBreaker* breaker, NYA_ConstCString key, b8 success) {
    nya_assert(breaker != nullptr && key != nullptr);

    _nya_circuit_record_at(breaker, key, success, nya_clock_get_monotonic_ns());
}

NYA_CircuitState nya_circuit_state(NYA_CircuitBreaker* breaker, NYA_ConstCString key) {
    nya_assert(breaker != nullptr && key != nullptr);

    return _nya_circuit_state_at(breaker, key, nya_clock_get_monotonic_ns());
}

u32 nya_circuit_key_count(const NYA_CircuitBreaker* breaker) {
    nya_assert(breaker != nullptr);

    return breaker->entry_count;
}

// PRIVATE API IMPLEMENTATION

_NYA_CircuitEntry* _nya_circuit_find(NYA_CircuitBreaker* breaker, NYA_ConstCString key) {
    for (u32 index = 0; index < breaker->entry_count; index++) {
        if (strcmp(breaker->entries[index].key, key) == 0) return &breaker->entries[index];
    }

    return nullptr;
}

_NYA_CircuitEntry* _nya_circuit_entry(NYA_CircuitBreaker* breaker, NYA_ConstCString key, u64 now_ns) {
    _NYA_CircuitEntry* existing = _nya_circuit_find(breaker, key);
    if (existing != nullptr) return existing;

    _NYA_CircuitEntry* entry = nullptr;

    if (breaker->entry_count < NYA_CIRCUIT_MAX_KEYS) {
        entry = &breaker->entries[breaker->entry_count++];
    } else {
        /* Only a CLOSED entry may be reused, the stalest among them: a tripped entry is the whole point of the breaker, and handing its slot away would let calls back through to a downed dependency; resolve first so a cooled-down OPEN counts as HALF_OPEN. */
        for (u32 index = 0; index < NYA_CIRCUIT_MAX_KEYS; index++) {
            _NYA_CircuitEntry* candidate = &breaker->entries[index];

            if (_nya_circuit_resolve(breaker, candidate, now_ns) != NYA_CIRCUIT_CLOSED) continue;
            if (entry == nullptr || candidate->updated_at_ns < entry->updated_at_ns) entry = candidate;
        }

        // Every slot is tripped, nothing safe to evict, so this key goes untracked and its calls are allowed; refusing a new dependency because 32 others are down would cause the outage the breaker contains.
        if (entry == nullptr) return nullptr;
    }

    nya_memset(entry, 0, sizeof(*entry));

    (void)snprintf(entry->key, sizeof(entry->key), "%s", key);

    entry->state         = NYA_CIRCUIT_CLOSED;
    entry->updated_at_ns = now_ns;

    return entry;
}

NYA_CircuitState _nya_circuit_state_at(NYA_CircuitBreaker* breaker, NYA_ConstCString key, u64 now_ns) {
    _NYA_CircuitEntry* entry = _nya_circuit_find(breaker, key);
    if (entry == nullptr) return NYA_CIRCUIT_CLOSED;

    return _nya_circuit_resolve(breaker, entry, now_ns);
}

NYA_CircuitState _nya_circuit_resolve(NYA_CircuitBreaker* breaker, _NYA_CircuitEntry* entry, u64 now_ns) {
    if (entry->state == NYA_CIRCUIT_OPEN && now_ns - entry->opened_at_ns >= breaker->open_ns) {
        entry->state         = NYA_CIRCUIT_HALF_OPEN;
        entry->probes        = 0;
        entry->successes     = 0;
        entry->updated_at_ns = now_ns;
    }

    return entry->state;
}

b8 _nya_circuit_allow_at(NYA_CircuitBreaker* breaker, NYA_ConstCString key, u64 now_ns) {
    _NYA_CircuitEntry* entry = _nya_circuit_entry(breaker, key, now_ns);
    if (entry == nullptr) return true;

    switch (_nya_circuit_resolve(breaker, entry, now_ns)) {
        case NYA_CIRCUIT_CLOSED: return true;
        case NYA_CIRCUIT_OPEN:   return false;
        case NYA_CIRCUIT_HALF_OPEN:
            // One probe at a time up to the cap, so a broken dependency is asked a bounded number of questions rather than the full flood CLOSED would allow.
            if (entry->probes >= breaker->half_open_max) return false;
            entry->probes++;
            entry->updated_at_ns = now_ns;
            return true;
        default: return true;
    }
}

void _nya_circuit_record_at(NYA_CircuitBreaker* breaker, NYA_ConstCString key, b8 success, u64 now_ns) {
    _NYA_CircuitEntry* entry = _nya_circuit_find(breaker, key);

    // Nothing was allowed for this key, so nothing to record; a record without a preceding allow is a caller bug, but ignoring it costs nothing.
    if (entry == nullptr) return;

    entry->updated_at_ns = now_ns;

    if (success) {
        switch (entry->state) {
            case NYA_CIRCUIT_HALF_OPEN:
                entry->successes++;
                if (entry->probes > 0) entry->probes--;
                if (entry->successes >= breaker->success_threshold) {
                    entry->state     = NYA_CIRCUIT_CLOSED;
                    entry->failures  = 0;
                    entry->successes = 0;
                    entry->probes    = 0;
                }
                break;
            case NYA_CIRCUIT_CLOSED:
                // A run of failures is what trips the breaker, so a single success ends the run.
                entry->failures = 0;
                break;
            case NYA_CIRCUIT_OPEN:
            default: break;
        }

        return;
    }

    switch (entry->state) {
        case NYA_CIRCUIT_CLOSED:
            entry->failures++;
            if (entry->failures >= breaker->failure_threshold) {
                entry->state        = NYA_CIRCUIT_OPEN;
                entry->opened_at_ns = now_ns;
            }
            break;
        case NYA_CIRCUIT_HALF_OPEN:
            // The probe failed: the dependency is still down, so straight back to OPEN for another full cooldown rather than letting more probes hit it.
            entry->state        = NYA_CIRCUIT_OPEN;
            entry->opened_at_ns = now_ns;
            entry->successes    = 0;
            entry->probes       = 0;
            break;
        case NYA_CIRCUIT_OPEN:
        default:
            // A failure reported while already OPEN (a call in flight when it tripped) pushes the cooldown out from now, so the dependency gets the full quiet window from the last failure.
            entry->opened_at_ns = now_ns;
            break;
    }
}
