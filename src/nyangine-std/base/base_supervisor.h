/**
 * @file base_supervisor.h
 *
 * When the process dies on a fatal, whether to relaunch it and how long to wait first.
 *
 * ```c
 * // once, as early in main as the backtrace handlers, and only when NYA_SUPERVISE is set:
 * nya_supervisor_arm(argc, argv);
 *
 * // and the crash sink calls this by itself, after the report is written; nothing else has to.
 * // _nya_supervisor_on_fatal re-execs the same argv when the policy still allows it, or returns and
 * // lets the crash surface through the exit that follows.
 * ```
 *
 * ── fail-fast has a floor, and this is under it ──
 *
 * [[base_circuit]] and [[base_reconnect]] keep a running program from hammering something that is down.
 * They assume the program itself is alive. This is the case where it is not: an assertion, a panic, a
 * thrown error or a hardware fault has reached the crash sink and the process is about to end. The
 * crash reporter writes the report and, until now, that was all — the process died and stayed dead. A
 * supervisor asks the one more question a server wants asked: should it come back up?
 *
 * There is no parent process here. The dying process re-execs itself with execv, and the restart count
 * and the window it is counted in ride across the exec in an environment variable, so a crash loop is
 * still bounded even though nothing outside the process is keeping score. A real init supervisor
 * (systemd, a container runtime) can sit above this and does not conflict with it: this bounds the
 * in-process relaunches, and whatever is above bounds those.
 *
 * ── the decision is the testable part, the re-exec is not ──
 *
 * This is the split base_circuit.h and base_reconnect.h make. nya_supervisor_should_restart is a pure
 * function of the state and a `now_s` the caller passes in: at most `max_restarts` restarts inside a
 * rolling `window_s`, give up past that so the crash surfaces instead of hiding in a loop, and reset
 * the count once a whole window has passed quietly so an occasional crash over a long uptime is always
 * recovered. A test drives it with an injected clock and no process ever dies. The delay between the
 * decision and the re-exec is base_rate's nya_backoff_ms, which this leans on and which has its own
 * test; nya_supervisor_window_ms hands back the un-jittered ceiling so the schedule's shape is checkable
 * exactly.
 *
 * ── opt-in ──
 *
 * A zeroed policy is `enabled = false`, and nya_supervisor_arm does nothing at all unless the
 * environment sets `NYA_SUPERVISE=1`. Default off, so behaviour is exactly what it was before this
 * existed: a fatal reports and the process ends. Relaunching a process that was killed on purpose, or
 * one that faults the same way every time, is rarely what was wanted, so it is asked for rather than
 * assumed. A clean exit and a user quit never reach the crash sink, so they are never restarted; the
 * `clean_exit` argument to the decision is there for completeness and for the test.
 *
 * ── async-signal-safety ──
 *
 * A hardware fault runs the crash sink in a signal handler, so _nya_supervisor_on_fatal can reach the
 * re-exec from async signal context. It is written for it: the environment is snapshotted once in
 * ordinary context by nya_supervisor_arm, so the fault path calls no getenv, setenv or malloc; the
 * restart state is formatted into a fixed buffer with no stdio; the wait is nanosleep and the relaunch
 * is execve, both async-signal-safe; and the jitter comes from getrandom, which is a bare syscall. The
 * one call not on the formal POSIX safe list is clock_gettime, which on Linux is a lock-free vDSO read.
 *
 * Thread safety: none. There is one supervisor, armed once before any thread is spawned, and only the
 * single thread that latches the crash ever reaches the re-exec.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** Restarts allowed inside one window when the policy leaves `max_restarts` at zero. */
#define NYA_SUPERVISOR_MAX_RESTARTS 5

/** The rolling window the restarts are counted in, in seconds, when the policy leaves `window_s` at zero. */
#define NYA_SUPERVISOR_WINDOW_S 60

/** The first restart's backoff window when the policy leaves `base_ms` at zero. */
#define NYA_SUPERVISOR_BASE_MS 500

/** The longest a restart ever waits when the policy leaves `cap_ms` at zero. */
#define NYA_SUPERVISOR_CAP_MS 30000

// TYPES

typedef struct NYA_SupervisorPolicy NYA_SupervisorPolicy;
typedef struct NYA_Supervisor       NYA_Supervisor;

/** How a supervisor behaves. Every field has a usable zero, and the whole of it zero is "off". */
struct NYA_SupervisorPolicy {
    /** Opt-in. False is the default and means a fatal is final, exactly as it was before this existed. */
    b8 enabled;

    /** Restarts allowed inside one window before giving up. Zero means NYA_SUPERVISOR_MAX_RESTARTS. */
    u32 max_restarts;

    /** The rolling window the restarts are counted in, in seconds. Zero means NYA_SUPERVISOR_WINDOW_S. */
    u64 window_s;

    /** The first restart's backoff window, in milliseconds. Zero means NYA_SUPERVISOR_BASE_MS. */
    u64 base_ms;

    /** The backoff stops doubling here, in milliseconds. Zero means NYA_SUPERVISOR_CAP_MS. */
    u64 cap_ms;

    /**
     * How much of each wait is random, 0 to 1, handed straight to nya_backoff_ms. Zero is full jitter,
     * which is the default and the one to want; see base_rate.h.
     * */
    f64 jitter;
};

/**
 * The supervisor's state. There is one, held inside base_supervisor.c and armed once; the calls below
 * read and drive it. Kept a named type, not hidden, so the test can hold one of its own and inspect it.
 * */
struct NYA_Supervisor {
    NYA_SupervisorPolicy policy;

    /** Restarts counted in the current window. Rides across a re-exec in the environment. */
    u32 restarts;

    /** Wall-clock second the current window began. Rides across a re-exec in the environment. */
    u64 window_start_s;
};

// DECISION

/** Sets `supervisor` to a fresh state under `policy`: no restarts spent, no window open. */
NYA_API void nya_supervisor_init(OUT NYA_Supervisor* supervisor, NYA_SupervisorPolicy policy);

/** Whether the policy is opted in at all. False means the decision always says no and nothing re-execs. */
NYA_API b8 nya_supervisor_enabled(const NYA_Supervisor* supervisor) __attr_no_discard;

/**
 * Whether the process should be relaunched after a fatal at `now_s`, counting this restart if so.
 *
 * The rule: at most `max_restarts` inside a rolling `window_s`. A restart requested once a whole window
 * has passed since the window opened — or on a clock that went backwards — starts a fresh window, which
 * is the reset that keeps an occasional crash over a long uptime always recoverable. A restart requested
 * with the window's budget already spent returns false: the crash loop is given up on so the fault
 * surfaces rather than hiding. `clean_exit` is always false in the crash sink and never restarts.
 *
 * Mutates: on a true answer the restart is counted, and the window may be reset. `now_s` is passed in
 * rather than read, which is what keeps the decision testable.
 * */
NYA_API b8 nya_supervisor_should_restart(NYA_Supervisor* supervisor, u64 now_s, b8 clean_exit);

/**
 * The un-jittered ceiling for the restart just granted: `base_ms` doubled once per restart past the
 * first, capped at `cap_ms`, saturating rather than shifting past the width of the type.
 *
 * Pure and deterministic, so a test checks the schedule's shape against it exactly; the delay a real
 * restart waits is a full-jitter pick from zero up to this.
 * */
NYA_API u64 nya_supervisor_window_ms(const NYA_Supervisor* supervisor) __attr_no_discard;

/**
 * The jittered delay to wait before the restart just granted, a full-jitter pick inside
 * nya_supervisor_window_ms. This is base_rate's nya_backoff_ms over the current restart count.
 * */
NYA_API u64 nya_supervisor_backoff_ms(const NYA_Supervisor* supervisor) __attr_no_discard;

/** Restarts counted in the current window. Zero when freshly initialised or after a quiet window. */
NYA_API u32 nya_supervisor_restart_count(const NYA_Supervisor* supervisor) __attr_no_discard;

// RUNTIME

/**
 * Arms the one supervisor from the environment, saving `argv` and snapshotting the environment for a
 * re-exec that touches neither in a signal handler. Does nothing unless `NYA_SUPERVISE=1`; then
 * `NYA_SUPERVISE_MAX`, `NYA_SUPERVISE_WINDOW_S`, `NYA_SUPERVISE_BASE_MS` and `NYA_SUPERVISE_CAP_MS`
 * override the defaults, and `NYA_SUPERVISE_STATE`, which the previous life set, seeds the count.
 *
 * Call it as early as nya_backtrace_init, before any thread is spawned. Off Linux it is a no-op:
 * there is no re-exec here yet.
 * */
NYA_API void nya_supervisor_arm(s32 argc, NYA_CString* argv);

/**
 * The crash sink's hook, called once from the thread that latched the crash after the report is written
 * and the log is flushed. Re-execs the same argv when the armed policy still allows a restart, waiting
 * the backoff first; returns when it does not — disabled, out of budget, or the exec itself failed — so
 * the crash surfaces through the exit that follows. `fault_path` is true in async signal context.
 * */
NYA_API void _nya_supervisor_on_fatal(b8 fault_path);
