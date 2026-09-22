#include "nyangine/nyangine.h"

#ifdef NYA_TESTING

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How long the stuck thread gets to report itself before the watchdog ends the process anyway. The fault
 * path formats a backtrace and flushes the log, which takes milliseconds; a thread with the signal
 * blocked never starts, and this is what stops that from being a second hang.
 * */
#define _NYA_TEST_DEADLINE_REPORT_GRACE_MS 5000

/** Room for the test's name in the failure line. Longer names are cut, not refused. */
#define _NYA_TEST_DEADLINE_NAME_MAX 64

/** A day. Past that a limit is a typo, and it keeps the seconds well inside a time_t. */
#define _NYA_TEST_DEADLINE_LIMIT_MAX_S (24 * 60 * 60)

/*
 * pthreads rather than SDL's threads, on Windows too through mingw's winpthreads. The deadline covers
 * teardown, and after SDL_Quit SDL treats every thread it made as invalid: SDL_WaitThread then returns
 * without joining or freeing, which LeakSanitizer reported as 158 bytes on every run of test_agent.
 */
typedef struct {
    pthread_t       watchdog;
    pthread_t       armed_by;
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    b8              armed;
    b8              stopped;
    u32             limit_s;
    char            name[_NYA_TEST_DEADLINE_NAME_MAX];
} _NYA_TestDeadline;

NYA_INTERNAL _NYA_TestDeadline _nya_test_deadline = { 0 };

/** The watchdog: waits for a stop, and fails the process if none comes in time. */
NYA_INTERNAL void* _nya_test_deadline_watch(void* user_data);

#if OS_LINUX
/** Runs on the stuck thread, so the fault path's backtrace is that thread's. */
NYA_INTERNAL void _nya_test_deadline_on_signal(int signal_number);
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_test_deadline_start(NYA_ConstCString name, u32 limit_s) {
    nya_assert(name != nullptr);
    nya_assert(limit_s > 0 && limit_s <= _NYA_TEST_DEADLINE_LIMIT_MAX_S, "a deadline of %u s is not one a test means", limit_s);
    nya_assert(!_nya_test_deadline.armed, "a deadline is already armed");

    _nya_test_deadline = (_NYA_TestDeadline){ .limit_s = limit_s, .armed_by = pthread_self() };
    (void)snprintf(_nya_test_deadline.name, sizeof(_nya_test_deadline.name), "%s", name);

    // the report is only worth having with a backtrace in it, and a test does not init one by itself.
    // It also installs the engine's fault handlers, so a test with a deadline crashes the way the game does.
    nya_backtrace_init();

#if OS_LINUX
    // SIGALRM because nothing in the engine or its vendors uses it: alarm() is never called.
    struct sigaction action = { 0 };
    action.sa_handler       = _nya_test_deadline_on_signal;
    sigemptyset(&action.sa_mask);
    nya_assert(sigaction(SIGALRM, &action, nullptr) == 0);
#endif

    // the monotonic clock where the platform lets the wait use it, so a wall clock step cannot move the deadline.
    pthread_condattr_t attributes;
    nya_assert(pthread_condattr_init(&attributes) == 0);
#if OS_LINUX
    nya_assert(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0);
#endif
    nya_assert(pthread_cond_init(&_nya_test_deadline.wake, &attributes) == 0);
    (void)pthread_condattr_destroy(&attributes);
    nya_assert(pthread_mutex_init(&_nya_test_deadline.lock, nullptr) == 0);

    nya_assert(pthread_create(&_nya_test_deadline.watchdog, nullptr, _nya_test_deadline_watch, nullptr) == 0, "the watchdog thread did not start");
    _nya_test_deadline.armed = true;
}

void nya_test_deadline_stop(void) {
    if (!_nya_test_deadline.armed) return;

    nya_assert(pthread_equal(pthread_self(), _nya_test_deadline.armed_by), "a deadline is stopped from the thread that armed it");

    (void)pthread_mutex_lock(&_nya_test_deadline.lock);
    _nya_test_deadline.stopped = true;
    (void)pthread_cond_signal(&_nya_test_deadline.wake);
    (void)pthread_mutex_unlock(&_nya_test_deadline.lock);

    nya_assert(pthread_join(_nya_test_deadline.watchdog, nullptr) == 0);
    (void)pthread_cond_destroy(&_nya_test_deadline.wake);
    (void)pthread_mutex_destroy(&_nya_test_deadline.lock);

#if OS_LINUX
    (void)signal(SIGALRM, SIG_DFL);
#endif

    _nya_test_deadline = (_NYA_TestDeadline){ 0 };
    nya_assert(!_nya_test_deadline.armed);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void* _nya_test_deadline_watch(void* user_data) {
    nya_unused(user_data);

    struct timespec until = { 0 };
#if OS_LINUX
    (void)clock_gettime(CLOCK_MONOTONIC, &until);
#else
    (void)clock_gettime(CLOCK_REALTIME, &until);
#endif
    until.tv_sec += (time_t)_nya_test_deadline.limit_s;

    // a loop, since a condition variable may wake with nothing having signalled it.
    (void)pthread_mutex_lock(&_nya_test_deadline.lock);
    b8 expired = false;
    while (!_nya_test_deadline.stopped && !expired) expired = pthread_cond_timedwait(&_nya_test_deadline.wake, &_nya_test_deadline.lock, &until) == ETIMEDOUT;
    b8 stopped = _nya_test_deadline.stopped;
    (void)pthread_mutex_unlock(&_nya_test_deadline.lock);

    if (stopped) return nullptr;

    // stderr directly: the logger is not the watchdog's to use while another thread may hold it.
    (void)fprintf(stderr, "\n[DEADLINE] %s is still running after %u s, which is past its deadline. Failing it.\n", _nya_test_deadline.name,
                  _nya_test_deadline.limit_s);
    (void)fflush(stderr);

#if OS_LINUX
    (void)pthread_kill(_nya_test_deadline.armed_by, SIGALRM);

    struct timespec grace = { .tv_sec = _NYA_TEST_DEADLINE_REPORT_GRACE_MS / 1000, .tv_nsec = (_NYA_TEST_DEADLINE_REPORT_GRACE_MS % 1000) * 1000000L };
    (void)nanosleep(&grace, nullptr);
    (void)fprintf(stderr, "[DEADLINE] the stuck thread did not report within %d ms.\n", _NYA_TEST_DEADLINE_REPORT_GRACE_MS);
#endif

    // _Exit rather than exit: the other thread is still running, and atexit handlers would race it.
    _Exit(EXIT_FAILURE);
}

#if OS_LINUX
void _nya_test_deadline_on_signal(int signal_number) {
    _nya_crash_raise_fault((s32)signal_number, 0);
}
#endif

#endif // NYA_TESTING
