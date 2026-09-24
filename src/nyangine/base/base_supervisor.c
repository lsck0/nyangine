#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_diagnostics.h"
#include "nyangine/base/base_rate.h"
#include "nyangine/base/base_supervisor.h"

#if OS_LINUX
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — DECISION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_supervisor_init(OUT NYA_Supervisor* supervisor, NYA_SupervisorPolicy policy) {
    nya_assert(supervisor != nullptr);

    *supervisor = (NYA_Supervisor){ .policy = policy };
}

b8 nya_supervisor_enabled(const NYA_Supervisor* supervisor) {
    nya_assert(supervisor != nullptr);

    return supervisor->policy.enabled;
}

b8 nya_supervisor_should_restart(NYA_Supervisor* supervisor, u64 now_s, b8 clean_exit) {
    nya_assert(supervisor != nullptr);

    if (!supervisor->policy.enabled) return false;

    // A clean exit and a user quit are never relaunched. Neither reaches the crash sink, so the sink
    // always passes false; the argument is here for the reader and for the test.
    if (clean_exit) return false;

    u32 max_restarts = supervisor->policy.max_restarts != 0 ? supervisor->policy.max_restarts : (u32)NYA_SUPERVISOR_MAX_RESTARTS;
    u64 window_s     = supervisor->policy.window_s != 0 ? supervisor->policy.window_s : (u64)NYA_SUPERVISOR_WINDOW_S;

    // Open a fresh window when there is none yet, when a whole one has passed since the last opened —
    // the crashes were not a tight loop — or when the clock ran backwards, which must not be read as a
    // still-open window. This is the "reset after a quiet window" that keeps a rare crash recoverable.
    if (supervisor->restarts == 0 || now_s < supervisor->window_start_s || now_s - supervisor->window_start_s >= window_s) {
        supervisor->restarts       = 0;
        supervisor->window_start_s = now_s;
    }

    // The budget for this window is spent: give up so the crash surfaces rather than looping on it.
    if (supervisor->restarts >= max_restarts) return false;

    supervisor->restarts += 1;

    return true;
}

u64 nya_supervisor_window_ms(const NYA_Supervisor* supervisor) {
    nya_assert(supervisor != nullptr);

    u64 base_ms = supervisor->policy.base_ms != 0 ? supervisor->policy.base_ms : (u64)NYA_SUPERVISOR_BASE_MS;
    u64 cap_ms  = supervisor->policy.cap_ms != 0 ? supervisor->policy.cap_ms : (u64)NYA_SUPERVISOR_CAP_MS;

    if (cap_ms < base_ms) cap_ms = base_ms;

    // restarts is 1-based once a restart is granted, and attempt 0 is base_ms; before the first grant
    // the schedule still reads as base_ms rather than as something undefined.
    u32 attempt = supervisor->restarts > 0 ? supervisor->restarts - 1 : 0;

    // Saturated rather than shifted past the width of the type, the same guard nya_backoff_ms uses.
    if (attempt >= 32) return cap_ms;

    u64 doubled = base_ms << attempt;

    return doubled < cap_ms && doubled >= base_ms ? doubled : cap_ms;
}

u64 nya_supervisor_backoff_ms(const NYA_Supervisor* supervisor) {
    nya_assert(supervisor != nullptr);

    u64 base_ms = supervisor->policy.base_ms != 0 ? supervisor->policy.base_ms : (u64)NYA_SUPERVISOR_BASE_MS;
    u64 cap_ms  = supervisor->policy.cap_ms != 0 ? supervisor->policy.cap_ms : (u64)NYA_SUPERVISOR_CAP_MS;

    u32 attempt = supervisor->restarts > 0 ? supervisor->restarts - 1 : 0;

    return nya_backoff_ms(attempt, .base_ms = base_ms, .cap_ms = cap_ms, .jitter = supervisor->policy.jitter);
}

u32 nya_supervisor_restart_count(const NYA_Supervisor* supervisor) {
    nya_assert(supervisor != nullptr);

    return supervisor->restarts;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RUNTIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The decision above is pure and portable; everything below is the syscall half — the environment, the
 * clock, the wait and the re-exec — kept apart from it, the way base_reconnect keeps the socket apart
 * from its state machine. Only Linux has the re-exec today; elsewhere arming is a no-op.
 */

/** The environment variable name that carries the count and window across a re-exec. */
#define _NYA_SUPERVISE_STATE_PREFIX     "NYA_SUPERVISE_STATE="
#define _NYA_SUPERVISE_STATE_PREFIX_LEN (sizeof(_NYA_SUPERVISE_STATE_PREFIX) - 1)

#if OS_LINUX

/** Environment slots snapshotted for a signal-safe re-exec. Room for a large environment plus our own. */
#define _NYA_SUPERVISOR_ENVP_MAX 4096

/** The one supervisor, armed once before any thread is spawned. */
NYA_INTERNAL NYA_Supervisor _nya_supervisor = { 0 };

/** The argv to hand the relaunched process, unchanged so argv[0] survives. Borrowed, not owned. */
NYA_INTERNAL NYA_CString* _nya_supervisor_argv = nullptr;

/** The environment for the relaunched process, built once so the fault path calls no getenv or setenv. */
NYA_INTERNAL char** _nya_supervisor_envp = nullptr;
NYA_INTERNAL char*  _nya_supervisor_envp_storage[_NYA_SUPERVISOR_ENVP_MAX];

/** Our own mutable environment slot: "NYA_SUPERVISE_STATE=<restarts> <window_start_s>". */
NYA_INTERNAL char _nya_supervisor_state_env[_NYA_SUPERVISE_STATE_PREFIX_LEN + 48] = _NYA_SUPERVISE_STATE_PREFIX;

extern char** environ;

NYA_INTERNAL void _nya_supervisor_env_u64(NYA_ConstCString name, u64 fallback, OUT u64* out);
NYA_INTERNAL u32  _nya_supervisor_u64_to_str(u64 value, OUT char* buffer);
NYA_INTERNAL void _nya_supervisor_write_state(u32 restarts, u64 window_start_s);
NYA_INTERNAL void _nya_supervisor_build_envp(void);
NYA_INTERNAL u64  _nya_supervisor_now_s(void) __attr_no_discard;
NYA_INTERNAL void _nya_supervisor_sleep_ms(u64 milliseconds);

/** Reads `name` as an unsigned number, falling back to `fallback` when it is absent or malformed. Returns it in `out`. */
NYA_INTERNAL void _nya_supervisor_env_u64(NYA_ConstCString name, u64 fallback, OUT u64* out) {
    const char* text = getenv((const char*)name);
    if (text == nullptr || text[0] == '\0') {
        *out = fallback;
        return;
    }

    char*              end   = nullptr;
    unsigned long long value = strtoull(text, &end, 10);

    // A trailing non-digit is a typo, not a number: keep the default rather than a half-read value.
    if (end == nullptr || *end != '\0') {
        *out = fallback;
        return;
    }

    *out = (u64)value;
}

/** Writes `value`'s decimal digits into `buffer` (never null terminated here) and returns how many. Async-signal-safe. */
NYA_INTERNAL u32 _nya_supervisor_u64_to_str(u64 value, OUT char* buffer) {
    // Digits come out backwards, then are reversed in place; no snprintf, which is not on the safe list.
    char scratch[20];
    u32  count = 0;

    do {
        scratch[count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(scratch));

    for (u32 i = 0; i < count; i++) buffer[i] = scratch[count - 1 - i];

    return count;
}

/** Fills the reserved environment slot with the state to carry across the re-exec. Async-signal-safe. */
NYA_INTERNAL void _nya_supervisor_write_state(u32 restarts, u64 window_start_s) {
    u32 at = (u32)_NYA_SUPERVISE_STATE_PREFIX_LEN;

    at += _nya_supervisor_u64_to_str(restarts, &_nya_supervisor_state_env[at]);
    _nya_supervisor_state_env[at++] = ' ';
    at += _nya_supervisor_u64_to_str(window_start_s, &_nya_supervisor_state_env[at]);
    _nya_supervisor_state_env[at] = '\0';
}

/** Snapshots the environment once, in ordinary context, reserving our own state slot. */
NYA_INTERNAL void _nya_supervisor_build_envp(void) {
    u32 count = 0;

    for (char** entry = environ; entry != nullptr && *entry != nullptr; entry++) {
        // Drop any inherited state slot; our own reserved one replaces it below.
        if (strncmp(*entry, _NYA_SUPERVISE_STATE_PREFIX, _NYA_SUPERVISE_STATE_PREFIX_LEN) == 0) continue;

        // No room to snapshot this environment and still re-exec without touching it in a signal
        // handler: disarm rather than fall back to an unsafe copy.
        if (count >= _NYA_SUPERVISOR_ENVP_MAX - 2) {
            _nya_supervisor.policy.enabled = false;
            nya_log_warn("Supervisor disarmed: the environment is larger than %d entries.", _NYA_SUPERVISOR_ENVP_MAX);
            return;
        }

        _nya_supervisor_envp_storage[count++] = *entry;
    }

    _nya_supervisor_write_state(_nya_supervisor.restarts, _nya_supervisor.window_start_s);
    _nya_supervisor_envp_storage[count++] = _nya_supervisor_state_env;
    _nya_supervisor_envp_storage[count]   = nullptr;

    _nya_supervisor_envp = _nya_supervisor_envp_storage;
}

/** Wall-clock seconds. clock_gettime is a lock-free vDSO read on Linux, safe enough for the fault path. */
NYA_INTERNAL u64 _nya_supervisor_now_s(void) {
    struct timespec now = { 0 };
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;

    return (u64)now.tv_sec;
}

/** Waits `milliseconds`, resuming across a signal. nanosleep is async-signal-safe. */
NYA_INTERNAL void _nya_supervisor_sleep_ms(u64 milliseconds) {
    struct timespec request = {
        .tv_sec  = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)((milliseconds % 1000U) * 1000000L),
    };

    while (nanosleep(&request, &request) != 0 && errno == EINTR) { /* interrupted: sleep out the rest */ }
}

void nya_supervisor_arm(s32 argc, NYA_CString* argv) {
    nya_unused(argc);
    nya_assert(argv != nullptr);

    const char* on = getenv("NYA_SUPERVISE");

    // Exactly "1" is on. Anything else, absent included, leaves the process behaving as it always did.
    if (on == nullptr || on[0] != '1' || on[1] != '\0') {
        _nya_supervisor.policy.enabled = false;
        return;
    }

    u64 max_restarts = 0;
    u64 window_s     = 0;
    u64 base_ms      = 0;
    u64 cap_ms       = 0;
    _nya_supervisor_env_u64("NYA_SUPERVISE_MAX", NYA_SUPERVISOR_MAX_RESTARTS, &max_restarts);
    _nya_supervisor_env_u64("NYA_SUPERVISE_WINDOW_S", NYA_SUPERVISOR_WINDOW_S, &window_s);
    _nya_supervisor_env_u64("NYA_SUPERVISE_BASE_MS", NYA_SUPERVISOR_BASE_MS, &base_ms);
    _nya_supervisor_env_u64("NYA_SUPERVISE_CAP_MS", NYA_SUPERVISOR_CAP_MS, &cap_ms);

    nya_supervisor_init(&_nya_supervisor, (NYA_SupervisorPolicy){
        .enabled      = true,
        .max_restarts = (u32)max_restarts,
        .window_s     = window_s,
        .base_ms      = base_ms,
        .cap_ms       = cap_ms,
    });

    // Seed the count and window from the life before this one, so restarts accumulate across the
    // re-exec and a crash loop is bounded even though nothing outside the process keeps score.
    const char* state = getenv("NYA_SUPERVISE_STATE");
    if (state != nullptr) {
        char*              end          = nullptr;
        unsigned long      restarts     = strtoul(state, &end, 10);
        unsigned long long window_start = 0;
        if (end != nullptr && *end == ' ') window_start = strtoull(end + 1, &end, 10);

        _nya_supervisor.restarts       = (u32)restarts;
        _nya_supervisor.window_start_s = (u64)window_start;
    }

    _nya_supervisor_argv = argv;

    _nya_supervisor_build_envp();

    nya_log_info("Supervisor armed: up to %u restart(s) in %llus, backoff %llu..%llums.", _nya_supervisor.policy.max_restarts,
                 (unsigned long long)_nya_supervisor.policy.window_s, (unsigned long long)_nya_supervisor.policy.base_ms,
                 (unsigned long long)_nya_supervisor.policy.cap_ms);
}

void _nya_supervisor_on_fatal(b8 fault_path) {
    if (!_nya_supervisor.policy.enabled || _nya_supervisor_argv == nullptr || _nya_supervisor_envp == nullptr) return;

    if (!nya_supervisor_should_restart(&_nya_supervisor, _nya_supervisor_now_s(), false)) {
        // Disabled, or the window's budget is spent: let the crash surface through the exit that follows.
        NYA_ConstCString giving_up = "\n[SUPERVISE] restart budget spent; letting the crash surface.\n";
        nya_log_write_stderr(giving_up, (u32)strlen(giving_up));
        return;
    }

    u64 delay_ms = nya_supervisor_backoff_ms(&_nya_supervisor);

    // Announced through write(2), the only output safe on the fault path, before the wait — so the tail
    // of the log shows the restart even if the re-exec below fails.
    {
        char line[96];
        u32  at = 0;
        NYA_ConstCString head = "\n[SUPERVISE] re-exec after fatal, restart ";
        for (const char* c = head; *c != '\0'; c++) line[at++] = *c;
        at += _nya_supervisor_u64_to_str(_nya_supervisor.restarts, &line[at]);
        line[at++] = ' ';
        line[at++] = 'i';
        line[at++] = 'n';
        line[at++] = ' ';
        at += _nya_supervisor_u64_to_str(delay_ms, &line[at]);
        NYA_ConstCString tail = "ms\n";
        for (const char* c = tail; *c != '\0'; c++) line[at++] = *c;
        nya_log_write_stderr((NYA_ConstCString)line, at);
    }

    _nya_supervisor_sleep_ms(delay_ms);

    // Carry the incremented state to the child, then relaunch. /proc/self/exe is this binary however it
    // was invoked, and execve is async-signal-safe, so this is safe from the fault handler too.
    _nya_supervisor_write_state(_nya_supervisor.restarts, _nya_supervisor.window_start_s);
    (void)execve("/proc/self/exe", _nya_supervisor_argv, _nya_supervisor_envp);

    // execve only returns on failure; fall through and let the crash surface through the exit that follows.
    NYA_ConstCString failed = "\n[SUPERVISE] re-exec failed; letting the crash surface.\n";
    nya_log_write_stderr(failed, (u32)strlen(failed));

    nya_unused(fault_path);
}

#else // OS_LINUX

void nya_supervisor_arm(s32 argc, NYA_CString* argv) {
    nya_unused(argc);
    nya_unused(argv);
    // No re-exec off Linux yet: supervision stays off, so a fatal behaves exactly as it always did.
}

void _nya_supervisor_on_fatal(b8 fault_path) {
    nya_unused(fault_path);
}

#endif // OS_LINUX
