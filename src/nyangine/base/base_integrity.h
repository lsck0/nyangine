/**
 * @file base_integrity.h
 *
 * Tamper detection for shipping builds, in layers that check each other:
 *
 * - The executable file carries a keyed hash of itself, stamped after linking and verified on a thread of its own at
 *   startup, which then hashes the mapped code in chunks as a baseline.
 * - A sweep re-hashes one chunk at a time from the frame loop, covering all of the code every few seconds.
 * - Every asset blob entry carries its hash, verified the first time the entry loads.
 * - A watchdog, copied into more than one call site, requires the startup check to have produced the stamped hash
 *   and the sweep to keep completing passes that agree with the baseline. Patching one check out, or making it
 *   return early, is noticed by code that is not in it.
 *
 * A failed check logs one line and exits with NYA_INTEGRITY_EXIT_CODE. It never panics, so a tampered build does not
 * produce crash reports.
 *
 * Limits: all of this runs on a machine the player controls. The key is in the binary, every check is code that can
 * be patched, and a debugger or hypervisor reads memory as it likes. It raises the cost of patching the executable,
 * editing code in memory and swapping assets; it does not stop a determined attacker. Data pages are not watched, so
 * a memory editor changing game state is out of scope. Anything that matters, such as scores, inventories and match
 * results, stays authoritative on the server.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Code hashed per sweep step, about 15 µs of SipHash. Code past NYA_INTEGRITY_CODE_CHUNK_MAX chunks widens them. */
#define NYA_INTEGRITY_CODE_CHUNK_BYTES (64ULL * 1024ULL)
#define NYA_INTEGRITY_CODE_CHUNK_MAX   512

/** Time between sweep steps. A 7 MB code segment is 107 chunks, a pass every 27 s. */
#define NYA_INTEGRITY_SWEEP_INTERVAL_NS 250'000'000ULL

/** Steps one frame may catch up on after a hitch, so a slow frame does not stretch the pass. */
#define NYA_INTEGRITY_SWEEP_CATCH_UP_MAX 4

/** How long the startup check may take before the watchdog counts it as skipped. */
#define NYA_INTEGRITY_STARTUP_DEADLINE_NS 30'000'000'000ULL

/** How many expected pass durations may go by without a completed pass, on top of the startup deadline. */
#define NYA_INTEGRITY_PASS_DEADLINE_FACTOR 4

/**
 * Most time one watchdog call counts, so a suspended machine or a frame stuck in a loading screen does not read as a
 * stalled sweep.
 * */
#define NYA_INTEGRITY_WATCHDOG_STEP_MAX_NS 1'000'000'000ULL

/** Distinct from a crash and from a normal exit, so a launcher or a support log can tell what happened. */
#define NYA_INTEGRITY_EXIT_CODE 86

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_IntegrityStatus NYA_IntegrityStatus;
typedef struct NYA_IntegrityState NYA_IntegrityState;

enum NYA_IntegrityStatus {
    NYA_INTEGRITY_OK,

    /** The executable file does not match the hash stamped into it, or could not be read. */
    NYA_INTEGRITY_FILE_MODIFIED,

    /** The mapped code no longer matches its baseline. Hooked, patched, or a breakpoint. */
    NYA_INTEGRITY_CODE_MODIFIED,

    /** An embedded asset's bytes do not match the hash the build recorded. */
    NYA_INTEGRITY_ASSET_MODIFIED,

    /** A check did not run, did not finish, or reported something other than what it computed. */
    NYA_INTEGRITY_CHECK_SKIPPED,

    NYA_INTEGRITY_STATUS_COUNT,
};

__attr_allow_unused static NYA_ConstCString NYA_INTEGRITY_STATUS_NAME_MAP[NYA_INTEGRITY_STATUS_COUNT] = {
    [NYA_INTEGRITY_OK]             = "OK",
    [NYA_INTEGRITY_FILE_MODIFIED]  = "FILE_MODIFIED",
    [NYA_INTEGRITY_CODE_MODIFIED]  = "CODE_MODIFIED",
    [NYA_INTEGRITY_ASSET_MODIFIED] = "ASSET_MODIFIED",
    [NYA_INTEGRITY_CHECK_SKIPPED]  = "CHECK_SKIPPED",
};

/**
 * What the checks share. One per process in a shipping build; tests make their own over a buffer.
 * */
struct NYA_IntegrityState {
    /** The hash the startup thread computed over the executable file. Zero until it has. */
    atomic u64 executable_mac;

    /** Set once the code baseline below is complete; nothing else is read before it. */
    atomic b8 baseline_ready;

    const u8* code;
    u64       code_size;
    u64       chunk_bytes;
    u32       chunk_count;
    u64       chunk_hashes[NYA_INTEGRITY_CODE_CHUNK_MAX];

    /** The chunk hashes folded in order. A completed pass has to fold to the same value. */
    u64 baseline_digest;

    /*
     * Main thread only from here.
     */

    u32 sweep_cursor;
    u64 sweep_next_ns;
    u64 sweep_digest;
    u64 sweep_passes;
    u64 last_pass_digest;

    u64 watchdog_ns;
    u64 watchdog_passes;
    u64 watchdog_stalled_ns;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The keyed hash every check compares: over the file, over code chunks, over asset entries. */
NYA_API u64 nya_integrity_hash(const void* data, u64 size) __attr_no_discard;

/** Logs why and exits with NYA_INTEGRITY_EXIT_CODE. Safe from any thread. */
NYA_API void nya_integrity_fail(NYA_IntegrityStatus status, NYA_ConstCString detail) __attr_noreturn;

/*
 * ─────────────────────────────────────────────────────────
 * ON DISK
 * ─────────────────────────────────────────────────────────
 */

/** Stamps the MAC into a freshly linked binary. Called by the build system after linking. */
NYA_API NYA_Error nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_mac) __attr_no_discard;

/** Whether the file at `path` still matches the hash stamped into it. */
NYA_API b8 nya_integrity_verify_file(NYA_ConstCString path) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * AT RUNTIME
 * ─────────────────────────────────────────────────────────
 */

/**
 * Starts the checks on a thread of their own: verifies the executable file, then captures the code baseline. Shipping
 * builds only. Call once, first thing.
 * */
NYA_API void nya_integrity_start(void);

/** Re-hashes the next code chunks that are due. Once a frame, from the main thread. */
NYA_API void nya_integrity_sweep(u64 now_ns);

/**
 * Hashes `size` bytes of `code` into `state` as the baseline. What nya_integrity_start does over the executable's
 * code; separate so a test can watch a buffer.
 * */
NYA_API void nya_integrity_capture(NYA_IntegrityState* state, const u8* code, u64 size);

/** One sweep step over `state`: the verdict on the chunk it hashed. */
NYA_API NYA_IntegrityStatus nya_integrity_sweep_step(NYA_IntegrityState* state) __attr_no_discard;

/**
 * The watchdog's verdict on `state` at `now_ns`: whether the startup check produced `stamped_mac` in time, and whether
 * sweep passes keep completing and agree with the baseline.
 *
 * Always inlined, so every call site carries its own copy of the comparisons.
 * */
__attr_always_inline static inline NYA_IntegrityStatus nya_integrity_watchdog_verdict(NYA_IntegrityState* state, u64 stamped_mac, u64 now_ns) {
    u64 elapsed        = state->watchdog_ns == 0 ? 0 : now_ns - state->watchdog_ns;
    state->watchdog_ns = now_ns;

    if (state->sweep_passes != state->watchdog_passes) {
        state->watchdog_passes     = state->sweep_passes;
        state->watchdog_stalled_ns = 0;
    } else {
        state->watchdog_stalled_ns += elapsed < NYA_INTEGRITY_WATCHDOG_STEP_MAX_NS ? elapsed : NYA_INTEGRITY_WATCHDOG_STEP_MAX_NS;
    }

    u64 mac = atomic_load(&state->executable_mac);
    if (mac != 0 && mac != stamped_mac) return NYA_INTEGRITY_CHECK_SKIPPED;
    if (state->sweep_passes > 0 && state->last_pass_digest != state->baseline_digest) return NYA_INTEGRITY_CHECK_SKIPPED;

    if (state->watchdog_stalled_ns <= NYA_INTEGRITY_STARTUP_DEADLINE_NS) return NYA_INTEGRITY_OK;
    if (mac == 0 || !atomic_load(&state->baseline_ready)) return NYA_INTEGRITY_CHECK_SKIPPED;

    u64 pass_ns = (u64)state->chunk_count * NYA_INTEGRITY_SWEEP_INTERVAL_NS * NYA_INTEGRITY_PASS_DEADLINE_FACTOR;
    return state->watchdog_stalled_ns > NYA_INTEGRITY_STARTUP_DEADLINE_NS + pass_ns ? NYA_INTEGRITY_CHECK_SKIPPED : NYA_INTEGRITY_OK;
}

/**
 * The process watchdog: nya_integrity_watchdog_verdict over this process, exiting on anything but OK. Shipping builds
 * only. Call it from more than one place.
 * */
#define nya_integrity_watchdog(now_ns)                                                                                                               \
    do {                                                                                                                                             \
        if (!NYA_SHIPPING_BUILD || !_nya_integrity_started) break;                                                                                   \
        NYA_IntegrityStatus _nya_integrity_verdict = nya_integrity_watchdog_verdict(&_nya_integrity_state, _nya_integrity_stamped_mac(), (now_ns));  \
        if (_nya_integrity_verdict != NYA_INTEGRITY_OK) nya_integrity_fail(_nya_integrity_verdict, "a check did not run as built");                  \
    } while (0)

/** The process's state and whether nya_integrity_start ran. For nya_integrity_watchdog. */
NYA_API NYA_IntegrityState _nya_integrity_state;
NYA_API b8                 _nya_integrity_started;

/** The MAC stamped into this executable, read from memory. For nya_integrity_watchdog. */
NYA_API u64 _nya_integrity_stamped_mac(void) __attr_no_discard;
