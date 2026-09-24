#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_rate.h"
#include "nyangine/base/base_reconnect.h"

// PUBLIC API IMPLEMENTATION

void nya_reconnect_init(OUT NYA_Reconnect* reconnect, NYA_ReconnectPolicy policy) {
    nya_assert(reconnect != nullptr);

    *reconnect = (NYA_Reconnect){ .policy = policy };
}

b8 nya_reconnect_enabled(const NYA_Reconnect* reconnect) {
    nya_assert(reconnect != nullptr);

    return reconnect->policy.enabled;
}

u64 nya_reconnect_window_ms(const NYA_Reconnect* reconnect) {
    nya_assert(reconnect != nullptr);

    u64 base_ms = reconnect->policy.base_ms != 0 ? reconnect->policy.base_ms : (u64)NYA_RECONNECT_BASE_MS;
    u64 cap_ms  = reconnect->policy.cap_ms != 0 ? reconnect->policy.cap_ms : (u64)NYA_RECONNECT_CAP_MS;

    if (cap_ms < base_ms) cap_ms = base_ms;

    // Saturated rather than shifted past the width of the type, the guard nya_backoff_ms uses: an attempt in the dozens should mean the cap, not undefined behaviour.
    if (reconnect->attempt >= 32) return cap_ms;

    u64 doubled = base_ms << reconnect->attempt;

    return doubled < cap_ms && doubled >= base_ms ? doubled : cap_ms;
}

b8 nya_reconnect_dropped_after(NYA_Reconnect* reconnect, u64 now_ms, u64 delay_ms) {
    nya_assert(reconnect != nullptr);

    if (!reconnect->policy.enabled) return false;

    // Zero max_attempts is unlimited; otherwise attempts 0..max_attempts-1 are allowed and the one past that is where the socket is given up on.
    if (reconnect->policy.max_attempts != 0 && reconnect->attempt >= reconnect->policy.max_attempts) return false;

    reconnect->waiting     = true;
    reconnect->retry_at_ms = now_ms + delay_ms;
    reconnect->attempt += 1;

    return true;
}

b8 nya_reconnect_dropped(NYA_Reconnect* reconnect, u64 now_ms) {
    nya_assert(reconnect != nullptr);

    if (!reconnect->policy.enabled) return false;

    // Drawn before the attempt is counted, so the window matches nya_reconnect_window_ms's reading of the current attempt; full jitter unless the policy asked for less.
    u64 base_ms = reconnect->policy.base_ms != 0 ? reconnect->policy.base_ms : (u64)NYA_RECONNECT_BASE_MS;
    u64 cap_ms  = reconnect->policy.cap_ms != 0 ? reconnect->policy.cap_ms : (u64)NYA_RECONNECT_CAP_MS;

    u64 delay_ms = nya_backoff_ms(reconnect->attempt, .base_ms = base_ms, .cap_ms = cap_ms, .jitter = reconnect->policy.jitter);

    return nya_reconnect_dropped_after(reconnect, now_ms, delay_ms);
}

b8 nya_reconnect_due(const NYA_Reconnect* reconnect, u64 now_ms) {
    nya_assert(reconnect != nullptr);

    return reconnect->waiting && now_ms >= reconnect->retry_at_ms;
}

b8 nya_reconnect_waiting(const NYA_Reconnect* reconnect) {
    nya_assert(reconnect != nullptr);

    return reconnect->waiting;
}

u64 nya_reconnect_remaining_ms(const NYA_Reconnect* reconnect, u64 now_ms) {
    nya_assert(reconnect != nullptr);

    if (!reconnect->waiting || now_ms >= reconnect->retry_at_ms) return 0;

    return reconnect->retry_at_ms - now_ms;
}

void nya_reconnect_connected(NYA_Reconnect* reconnect) {
    nya_assert(reconnect != nullptr);

    reconnect->attempt     = 0;
    reconnect->waiting     = false;
    reconnect->retry_at_ms = 0;
}
