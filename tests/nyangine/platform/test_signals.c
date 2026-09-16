/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <signal.h>

#if OS_WINDOWS
// the Windows backend listens for console control events, which raise() does not produce.
s32 main(void) {
  return 0;
}
#else
static volatile NYA_Signal last_signal_received = NYA_SIGNAL_INVALID;
static volatile s32        signal_handler_count = 0;

void test_signal_handler(NYA_Signal signal) {
  last_signal_received = signal;
  signal_handler_count++;
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Init and deinit don't crash
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();
    nya_signals_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Set handler and receive SIGHUP
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();

    last_signal_received = NYA_SIGNAL_INVALID;
    signal_handler_count = 0;

    nya_signals_set_handler(NYA_SIGNAL_HANGUP, test_signal_handler);
    raise(SIGHUP);

    nya_assert(last_signal_received == NYA_SIGNAL_HANGUP, "Expected HANGUP signal");
    nya_assert(signal_handler_count == 1, "Expected handler called once");

    nya_signals_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Set handler and receive SIGINT
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();

    last_signal_received = NYA_SIGNAL_INVALID;
    signal_handler_count = 0;

    nya_signals_set_handler(NYA_SIGNAL_INTERRUPT, test_signal_handler);
    raise(SIGINT);

    nya_assert(last_signal_received == NYA_SIGNAL_INTERRUPT, "Expected INTERRUPT signal");
    nya_assert(signal_handler_count == 1);

    nya_signals_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Set handler and receive SIGTERM
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();

    last_signal_received = NYA_SIGNAL_INVALID;
    signal_handler_count = 0;

    nya_signals_set_handler(NYA_SIGNAL_TERMINATE, test_signal_handler);
    raise(SIGTERM);

    nya_assert(last_signal_received == NYA_SIGNAL_TERMINATE, "Expected TERMINATE signal");
    nya_assert(signal_handler_count == 1);

    nya_signals_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: All signals with same handler
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();

    last_signal_received = NYA_SIGNAL_INVALID;
    signal_handler_count = 0;

    nya_signals_set_handler(NYA_SIGNAL_HANGUP, test_signal_handler);
    nya_signals_set_handler(NYA_SIGNAL_INTERRUPT, test_signal_handler);
    nya_signals_set_handler(NYA_SIGNAL_TERMINATE, test_signal_handler);

    raise(SIGHUP);
    nya_assert(last_signal_received == NYA_SIGNAL_HANGUP);

    raise(SIGINT);
    nya_assert(last_signal_received == NYA_SIGNAL_INTERRUPT);

    raise(SIGTERM);
    nya_assert(last_signal_received == NYA_SIGNAL_TERMINATE);

    nya_assert(signal_handler_count == 3, "Expected 3 handler calls");

    nya_signals_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Handler can be overwritten
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();

    signal_handler_count = 0;

    nya_signals_set_handler(NYA_SIGNAL_HANGUP, test_signal_handler);
    raise(SIGHUP);
    nya_assert(signal_handler_count == 1);

    nya_signals_set_handler(NYA_SIGNAL_HANGUP, test_signal_handler);
    raise(SIGHUP);
    nya_assert(signal_handler_count == 2);

    nya_signals_deinit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Invalid signal is silently ignored
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_signals_init();

    nya_signals_set_handler(NYA_SIGNAL_COUNT, test_signal_handler);
    nya_signals_set_handler((NYA_Signal)99, test_signal_handler);

    nya_signals_deinit();
  }

  printf("PASSED: test_signals\n");
  return 0;
}
#endif
