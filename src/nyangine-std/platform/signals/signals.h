#pragma once

#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_Signal NYA_Signal;

typedef void (*NYA_SignalHandler)(NYA_Signal signal);

enum NYA_Signal {
    NYA_SIGNAL_INVALID,

    /** Ctrl+C (SIGINT / CTRL_C_EVENT) */
    NYA_SIGNAL_INTERRUPT,

    /** kill (SIGTERM / CTRL_CLOSE_EVENT) */
    NYA_SIGNAL_TERMINATE,

    /** Terminal closed (SIGHUP / CTRL_LOGOFF_EVENT) */
    NYA_SIGNAL_HANGUP,

    NYA_SIGNAL_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API void nya_signals_init(void);
NYA_API void nya_signals_deinit(void);
NYA_API void nya_signals_set_handler(NYA_Signal signal, NYA_SignalHandler handler);

/**
 * The run budget: NYA_EXIT_AFTER_FRAMES=N and NYA_EXIT_AFTER_SECONDS=S bound any program, so a script can
 * run it unattended. Every loop the engine owns calls this once per pass with its own `loop`; the first
 * to call owns the count, so an app frame that also ticks the HTTP server counts once. When either runs
 * out it raises SIGINT once, the clean quit a ctrl-c asks for, so the program shuts down through its own
 * path and the sanitizers see a normal exit. Unset, it reads the environment once and then costs a branch.
 * */
NYA_API void nya_signals_budget_step(const void* loop);
