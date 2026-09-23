/**
 * @file base_reconnect.h
 *
 * When a connection this program owns drops, whether to dial again and how long to wait first.
 *
 * ```c
 * NYA_Reconnect reconnect;
 * nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true, .base_ms = 500, .cap_ms = 30000, .max_attempts = 8 });
 *
 * // the socket just dropped for a reason we did not ask for
 * if (nya_reconnect_dropped(&reconnect, nya_clock_get_timestamp_ms())) {
 *     // a retry is scheduled; nothing to do until it is due
 * } else {
 *     // disabled, or out of attempts: the drop is final
 * }
 *
 * // once a tick
 * if (nya_reconnect_due(&reconnect, nya_clock_get_timestamp_ms())) {
 *     redial();                              // moves the connection on from waiting; and on success:
 *     nya_reconnect_connected(&reconnect);   // forget the attempt history
 * }
 * ```
 *
 * ── the state machine is the testable part, the clock is not ──
 *
 * This is the same split base_circuit.h and base_rate.h make: the arithmetic that decides *whether* to
 * retry, *which attempt number* this is, and *when* the next one is due is a pure function of the state
 * and a `now_ms` the caller passes in, so a test drives it deterministically with no socket and no
 * clock. The one thing that is not deterministic — the full-jitter pick inside the backoff window — is
 * base_rate's nya_backoff_ms, which this leans on and which has its own test. nya_reconnect_window_ms
 * hands back the un-jittered ceiling so the schedule's *shape* is checkable exactly, and
 * nya_reconnect_dropped_after lets a test schedule with a delay of its own choosing and no syscall at
 * all.
 *
 * ── opt-in ──
 *
 * A zeroed policy is `enabled = false`, and a disabled reconnect never schedules anything:
 * nya_reconnect_dropped returns false and the drop is the caller's to treat as final, which is the
 * behaviour every caller had before this existed. Reconnect is a thing a caller asks for, because
 * silently redialling a socket that a caller closed on purpose, or one that fails the same way every
 * time, is rarely what was wanted.
 *
 * ── full jitter, and why a cap on both ends ──
 *
 * The delay doubles from `base_ms` and stops at `cap_ms`, and the value handed out is a uniform pick
 * from zero up to that window — full jitter, for the reason base_rate.h gives: clients that dropped
 * together must not come back together. `max_attempts` bounds the *count* of retries so a socket that
 * will never succeed is eventually given up on; `cap_ms` bounds the *delay* so a long-lived socket
 * keeps trying forever at a sane interval when max_attempts is left unlimited.
 *
 * Thread safety: none. One reconnect belongs to one connection on one thread, like the socket it is for.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The first retry's window when the policy leaves `base_ms` at zero. */
#define NYA_RECONNECT_BASE_MS 500

/** The longest a retry ever waits when the policy leaves `cap_ms` at zero. */
#define NYA_RECONNECT_CAP_MS 30000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_ReconnectPolicy NYA_ReconnectPolicy;
typedef struct NYA_Reconnect       NYA_Reconnect;

/** How a reconnect behaves. Every field has a usable zero, and the whole of it zero is "off". */
struct NYA_ReconnectPolicy {
    /** Opt-in. False is the default and means a drop is final, exactly as it was before this existed. */
    b8 enabled;

    /** The first retry's window. Zero means NYA_RECONNECT_BASE_MS. */
    u64 base_ms;

    /** The window stops doubling here. Zero means NYA_RECONNECT_CAP_MS. */
    u64 cap_ms;

    /** How many retries before giving up. Zero is unlimited, which a long-lived socket wants. */
    u32 max_attempts;

    /**
     * How much of each wait is random, 0 to 1, handed straight to nya_backoff_ms. Zero is full jitter,
     * which is the default and the one to want; see base_rate.h.
     * */
    f64 jitter;
};

/**
 * One connection's reconnect state. Held by value inside whatever owns the socket, and read only
 * through the calls below.
 * */
struct NYA_Reconnect {
    NYA_ReconnectPolicy policy;

    /** Retries scheduled since the last successful connect. Zero while connected or freshly initialised. */
    u32 attempt;

    /** A retry is scheduled and has not been begun yet. */
    b8 waiting;

    /** When nya_reconnect_due turns true. Meaningful only while `waiting`. */
    u64 retry_at_ms;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Sets `reconnect` to a fresh state under `policy`: no attempts spent, nothing scheduled. */
NYA_API void nya_reconnect_init(OUT NYA_Reconnect* reconnect, NYA_ReconnectPolicy policy);

/** Whether the policy is opted in at all. False means every other call here is a no-op that says no. */
NYA_API b8 nya_reconnect_enabled(const NYA_Reconnect* reconnect) __attr_no_discard;

/**
 * The un-jittered ceiling for the retry that would be scheduled next: `base_ms` doubled `attempt`
 * times, capped at `cap_ms`, saturating rather than shifting past the width of the type.
 *
 * Pure and deterministic, so a test checks the schedule's shape against it exactly; the delay a real
 * drop waits is a full-jitter pick from zero up to this.
 * */
NYA_API u64 nya_reconnect_window_ms(const NYA_Reconnect* reconnect) __attr_no_discard;

/**
 * An unexpected drop happened at `now_ms`. Schedules the next retry and returns true, or returns false
 * when the policy is disabled or the attempts are spent, in which case the drop is final.
 *
 * The delay is nya_backoff_ms over the current attempt, a full-jitter pick inside nya_reconnect_window_ms.
 * `now_ms` is the clock passed in rather than read, which is what keeps the schedule testable.
 * */
NYA_API b8 nya_reconnect_dropped(NYA_Reconnect* reconnect, u64 now_ms);

/**
 * nya_reconnect_dropped with the delay supplied rather than drawn from the jittered backoff.
 *
 * This is the seam the socket I/O sits behind: a test drives the state machine with a fixed schedule
 * and touches no clock and no random source, and nya_reconnect_dropped is exactly this over
 * nya_backoff_ms. `delay_ms` is used as given; bounding it to the window is the jittered path's job.
 * */
NYA_API b8 nya_reconnect_dropped_after(NYA_Reconnect* reconnect, u64 now_ms, u64 delay_ms);

/** Whether a scheduled retry is due at `now_ms`. False when nothing is scheduled. */
NYA_API b8 nya_reconnect_due(const NYA_Reconnect* reconnect, u64 now_ms) __attr_no_discard;

/** Whether a retry is scheduled and not yet begun. */
NYA_API b8 nya_reconnect_waiting(const NYA_Reconnect* reconnect) __attr_no_discard;

/** Milliseconds until the scheduled retry, or zero when it is due now or nothing is scheduled. */
NYA_API u64 nya_reconnect_remaining_ms(const NYA_Reconnect* reconnect, u64 now_ms) __attr_no_discard;

/**
 * A connect succeeded: forgets the attempt history so the next drop starts its backoff from `base_ms`
 * again, and clears the pending wait. Keeps the policy. Call it on every successful (re)connect.
 * */
NYA_API void nya_reconnect_connected(NYA_Reconnect* reconnect);
