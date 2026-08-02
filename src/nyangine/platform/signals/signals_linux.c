#include <signal.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_SignalHandler NYA_SIGNALS_TO_CALLBACK_MAP[NYA_SIGNAL_COUNT] = { 0 };

NYA_INTERNAL int        _nya_signal_to_native(NYA_Signal signal);
NYA_INTERNAL NYA_Signal _nya_signal_from_native(int sig);
NYA_INTERNAL void       _nya_signal_handler(int sig);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_signals_init(void) {
    /* nothing to do on linux */
}

void nya_signals_deinit(void) {
    __sighandler_t ok;

    ok = signal(SIGINT, SIG_DFL);
    nya_assert(ok != SIG_ERR, "Failed to reset SIGINT handler: %s", strerror(errno));

    ok = signal(SIGTERM, SIG_DFL);
    nya_assert(ok != SIG_ERR, "Failed to reset SIGTERM handler: %s", strerror(errno));

    ok = signal(SIGHUP, SIG_DFL);
    nya_assert(ok != SIG_ERR, "Failed to reset SIGHUP handler: %s", strerror(errno));
}

void nya_signals_set_handler(NYA_Signal sig, NYA_SignalHandler handler) {
    if (sig >= NYA_SIGNAL_COUNT) return;

    NYA_SIGNALS_TO_CALLBACK_MAP[sig] = handler;

    int native_sig = _nya_signal_to_native(sig);
    if (native_sig >= 0) {
        __sighandler_t ok = signal(native_sig, _nya_signal_handler);
        nya_assert(ok != SIG_ERR, "Failed to set signal handler for signal %d: %s", native_sig, strerror(errno));
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL int _nya_signal_to_native(NYA_Signal signal) {
    switch (signal) {
        case NYA_SIGNAL_INTERRUPT: return SIGINT;
        case NYA_SIGNAL_TERMINATE: return SIGTERM;
        case NYA_SIGNAL_HANGUP:    return SIGHUP;
        default:                   return -1;
    }
}

NYA_INTERNAL NYA_Signal _nya_signal_from_native(int sig) {
    switch (sig) {
        case SIGINT:  return NYA_SIGNAL_INTERRUPT;
        case SIGTERM: return NYA_SIGNAL_TERMINATE;
        case SIGHUP:  return NYA_SIGNAL_HANGUP;
        default:      return NYA_SIGNAL_INVALID;
    }
}

NYA_INTERNAL void _nya_signal_handler(int sig) {
    NYA_Signal signal = _nya_signal_from_native(sig);
    if (signal == NYA_SIGNAL_INVALID) return;

    NYA_SignalHandler handler = NYA_SIGNALS_TO_CALLBACK_MAP[signal];
    if (handler != nullptr) handler(signal);
}
