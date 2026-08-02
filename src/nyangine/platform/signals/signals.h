#pragma once

#include "nyangine/base/base_types.h"

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
