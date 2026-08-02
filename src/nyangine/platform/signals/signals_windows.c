#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_SignalHandler NYA_SIGNALS_TO_CALLBACK_MAP[NYA_SIGNAL_COUNT] = { 0 };

NYA_INTERNAL NYA_Signal  _nya_signal_from_native(DWORD ctrl_type);
NYA_INTERNAL BOOL WINAPI _nya_console_handler(DWORD ctrl_type);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_signals_init(void) {
    SetConsoleCtrlHandler(_nya_console_handler, TRUE);
}

void nya_signals_deinit(void) {
    SetConsoleCtrlHandler(_nya_console_handler, FALSE);
}

void nya_signals_set_handler(NYA_Signal sig, NYA_SignalHandler handler) {
    if (sig >= NYA_SIGNAL_COUNT) return;

    NYA_SIGNALS_TO_CALLBACK_MAP[sig] = handler;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_Signal _nya_signal_from_native(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:        return NYA_SIGNAL_INTERRUPT;
        case CTRL_CLOSE_EVENT:    return NYA_SIGNAL_TERMINATE;
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: return NYA_SIGNAL_HANGUP;
        default:                  return NYA_SIGNAL_INVALID;
    }
}

NYA_INTERNAL BOOL WINAPI _nya_console_handler(DWORD ctrl_type) {
    NYA_Signal signal = _nya_signal_from_native(ctrl_type);
    if (signal == NYA_SIGNAL_INVALID) return FALSE;

    NYA_SignalHandler handler = NYA_SIGNALS_TO_CALLBACK_MAP[signal];
    if (handler != nullptr) {
        handler(signal);
        return TRUE; /* Signal handled */
    }
    return FALSE; /* Use default handler */
}
