/**
 * @file terminal_linux.c
 *
 * The six calls terminal.c needs from the OS, on Linux: termios raw mode, the window size ioctl,
 * SIGWINCH, and non-blocking reads and writes on the terminal's own file descriptors. See terminal.h.
 * */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "nyangine/base/base.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Times a short write is retried before the terminal is called gone. A terminal that will not take
 * bytes after this many attempts is not going to, and a loop with no bound here would hang a TUI on
 * a closed pty rather than letting it exit.
 * */
#define _NYA_TERMINAL_WRITE_ATTEMPTS_MAX 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The terminal settings raw mode replaced, so close can put exactly those back. */
NYA_INTERNAL struct termios _nya_terminal_cooked          = { 0 };
NYA_INTERNAL b8             _nya_terminal_cooked_saved    = false;
NYA_INTERNAL s32            _nya_terminal_input_flags     = 0;
NYA_INTERNAL b8             _nya_terminal_input_flags_set = false;

/**
 * Set by the SIGWINCH handler and cleared by the poll that reads it.
 *
 * `volatile sig_atomic_t` and nothing else: a handler may only touch a lock free atomic or one of
 * these, and the whole reason the resize is a flag rather than work done in the handler is that
 * re-querying the size from a signal context is not async signal safe.
 * */
NYA_INTERNAL volatile sig_atomic_t _nya_terminal_resized = 0;

/** What the SIGWINCH handler replaced, so close leaves the process as it found it. */
NYA_INTERNAL struct sigaction _nya_terminal_winch_previous    = { 0 };
NYA_INTERNAL b8               _nya_terminal_winch_installed   = false;

NYA_INTERNAL void _nya_terminal_winch_handler(s32 signal_number) {
    nya_unused(signal_number);
    _nya_terminal_resized = 1;
}

b8 _nya_terminal_raw_mode_begin(void) {
    // both, because a TUI reads keys from one and paints through the other, and a program whose
    // output is piped somewhere is still driveable from the keyboard.
    if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) return false;

    if (tcgetattr(STDIN_FILENO, &_nya_terminal_cooked) != 0) return false;
    _nya_terminal_cooked_saved = true;

    struct termios raw = _nya_terminal_cooked;

    /*
     * cfmakeraw's flags, written out rather than called, because cfmakeraw also clears OPOST and
     * that is the one this module wants kept: with output post-processing off, a '\n' the log sink
     * writes lands at the column it was in rather than at the start of the next line.
     */
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= (tcflag_t)CS8;

    // a read returns whatever is there at once, since the poll must not block; VMIN 0 VTIME 0 is that.
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        _nya_terminal_cooked_saved = false;
        return false;
    }

    // raw mode alone still blocks in read() when nothing is waiting on some terminals, so the
    // descriptor is made non-blocking as well. Saved, because stdin is shared with everything else.
    _nya_terminal_input_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (_nya_terminal_input_flags >= 0) {
        _nya_terminal_input_flags_set = fcntl(STDIN_FILENO, F_SETFL, _nya_terminal_input_flags | O_NONBLOCK) == 0;
    }

    struct sigaction action = { 0 };
    action.sa_handler       = _nya_terminal_winch_handler;
    sigemptyset(&action.sa_mask);

    // SA_RESTART so the resize does not turn every read in the program into an EINTR the caller has
    // to know about. The flag is the reason this is sigaction and not signal().
    action.sa_flags = SA_RESTART;

    _nya_terminal_winch_installed = sigaction(SIGWINCH, &action, &_nya_terminal_winch_previous) == 0;

    // the first size is read as a resize, so a program that only draws on one still draws.
    _nya_terminal_resized = 1;

    return true;
}

void _nya_terminal_raw_mode_end(void) {
    if (_nya_terminal_winch_installed) {
        (void)sigaction(SIGWINCH, &_nya_terminal_winch_previous, nullptr);
        _nya_terminal_winch_installed = false;
    }

    if (_nya_terminal_input_flags_set) {
        (void)fcntl(STDIN_FILENO, F_SETFL, _nya_terminal_input_flags);
        _nya_terminal_input_flags_set = false;
    }

    if (_nya_terminal_cooked_saved) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &_nya_terminal_cooked);
        _nya_terminal_cooked_saved = false;
    }

    _nya_terminal_resized = 0;
}

b8 _nya_terminal_size_query(OUT u16* out_columns, OUT u16* out_rows) {
    nya_assert(out_columns != nullptr && out_rows != nullptr);

    struct winsize size = { 0 };
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) return false;

    // a pty that has not been sized yet reports zero, which is not a size and must not become one.
    if (size.ws_col == 0 || size.ws_row == 0) return false;

    *out_columns = size.ws_col;
    *out_rows    = size.ws_row;

    return true;
}

b8 _nya_terminal_resized_take(void) {
    if (_nya_terminal_resized == 0) return false;

    _nya_terminal_resized = 0;
    return true;
}

b8 _nya_terminal_send(const u8* bytes, u64 size) {
    nya_assert(bytes != nullptr || size == 0);

    u64 at = 0;
    for (u32 attempt = 0; at < size && attempt < _NYA_TERMINAL_WRITE_ATTEMPTS_MAX; attempt++) {
        ssize_t written = write(STDOUT_FILENO, &bytes[at], size - at);

        if (written > 0) {
            at += (u64)written;

            // progress resets the budget, so a large frame going out in many short writes is not
            // mistaken for a terminal that stopped taking bytes.
            attempt = 0;
            continue;
        }

        if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;

        nya_log_error("Writing to the terminal failed after " FMTu64 " of " FMTu64 " bytes: %s", at, size, strerror(errno));
        return false;
    }

    return at == size;
}

u64 _nya_terminal_receive(OUT u8* buffer, u64 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0, "reading zero bytes is a caller mistake, not an empty terminal");

    ssize_t got = read(STDIN_FILENO, buffer, capacity);

    // nothing waiting is the common case and not an error; anything else is, and losing a keystroke
    // is not worth a log line per poll, so it is silent by design.
    if (got <= 0) return 0;

    return (u64)got;
}

NYA_ConstCString _nya_terminal_environment(NYA_ConstCString name) {
    nya_assert(name != nullptr);
    return getenv(name);
}
