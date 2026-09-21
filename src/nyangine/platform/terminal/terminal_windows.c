/**
 * @file terminal_windows.c
 *
 * The same six calls on Windows. The console has had virtual terminal processing since Windows 10
 * 1511, so the escapes terminal.c writes go out unchanged and `ENABLE_VIRTUAL_TERMINAL_INPUT` makes
 * the console send the same escape sequences back, which means terminal.c's decoder is the decoder
 * here too. See terminal.h.
 *
 * Rejected: reading `INPUT_RECORD`s through `ReadConsoleInput` and translating virtual key codes.
 * That is a second decoder for the same keys, with its own table to keep in step with the Linux one,
 * to gain key-up events nothing in a TUI uses. VT input costs one flag and shares the decoder.
 *
 * There is no SIGWINCH. A console resize arrives as a `WINDOW_BUFFER_SIZE_EVENT` that VT input mode
 * does not deliver, so `_nya_terminal_resized_take` compares the size against the last one it saw.
 * Polling, which the style guide says costs nothing only when it is free: this is one
 * `GetConsoleScreenBufferInfo` per frame against a handle the process already owns.
 *
 * Untested. This repository is verified on Linux, and nothing here has run on a Windows console.
 * */
#include <windows.h>

#include "nyangine/base/base.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The console modes raw mode replaced, so close can put exactly those back. */
NYA_INTERNAL DWORD _nya_terminal_input_mode      = 0;
NYA_INTERNAL DWORD _nya_terminal_output_mode     = 0;
NYA_INTERNAL b8    _nya_terminal_modes_saved     = false;
NYA_INTERNAL UINT  _nya_terminal_output_codepage = 0;

/** The size the last query saw, which is what a resize is measured against. */
NYA_INTERNAL u16 _nya_terminal_last_columns = 0;
NYA_INTERNAL u16 _nya_terminal_last_rows    = 0;

b8 _nya_terminal_raw_mode_begin(void) {
    HANDLE input  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);

    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) return false;

    // GetConsoleMode fails on a handle that is not a console, which is how "is this a terminal" is
    // asked on Windows: there is no isatty that distinguishes a console from a pipe.
    if (!GetConsoleMode(input, &_nya_terminal_input_mode)) return false;
    if (!GetConsoleMode(output, &_nya_terminal_output_mode)) return false;

    _nya_terminal_modes_saved = true;

    DWORD raw_input = _nya_terminal_input_mode;
    raw_input      &= (DWORD) ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    raw_input      |= ENABLE_VIRTUAL_TERMINAL_INPUT;

    DWORD raw_output  = _nya_terminal_output_mode;
    raw_output       |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;

    // wrapping at the last column would scroll the screen every time the bottom right cell is drawn.
    raw_output &= (DWORD)~ENABLE_WRAP_AT_EOL_OUTPUT;

    if (!SetConsoleMode(input, raw_input)) {
        _nya_terminal_modes_saved = false;
        return false;
    }

    if (!SetConsoleMode(output, raw_output)) {
        (void)SetConsoleMode(input, _nya_terminal_input_mode);
        _nya_terminal_modes_saved = false;
        return false;
    }

    // terminal.c writes UTF-8, and a console in its default code page would render every multibyte
    // cell as two pieces of mojibake.
    _nya_terminal_output_codepage = GetConsoleOutputCP();
    (void)SetConsoleOutputCP(CP_UTF8);

    _nya_terminal_last_columns = 0;
    _nya_terminal_last_rows    = 0;

    return true;
}

void _nya_terminal_raw_mode_end(void) {
    if (!_nya_terminal_modes_saved) return;

    (void)SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), _nya_terminal_input_mode);
    (void)SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), _nya_terminal_output_mode);

    if (_nya_terminal_output_codepage != 0) (void)SetConsoleOutputCP(_nya_terminal_output_codepage);

    _nya_terminal_modes_saved = false;
}

b8 _nya_terminal_size_query(OUT u16* out_columns, OUT u16* out_rows) {
    nya_assert(out_columns != nullptr && out_rows != nullptr);

    CONSOLE_SCREEN_BUFFER_INFO info = { 0 };
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return false;

    // the window, not the buffer: the buffer is usually far taller and scrolls behind the window.
    s32 columns = info.srWindow.Right - info.srWindow.Left + 1;
    s32 rows    = info.srWindow.Bottom - info.srWindow.Top + 1;

    if (columns <= 0 || rows <= 0) return false;

    *out_columns = (u16)columns;
    *out_rows    = (u16)rows;

    return true;
}

b8 _nya_terminal_resized_take(void) {
    u16 columns = 0;
    u16 rows    = 0;

    if (!_nya_terminal_size_query(&columns, &rows)) return false;
    if (columns == _nya_terminal_last_columns && rows == _nya_terminal_last_rows) return false;

    _nya_terminal_last_columns = columns;
    _nya_terminal_last_rows    = rows;

    return true;
}

b8 _nya_terminal_send(const u8* bytes, u64 size) {
    nya_assert(bytes != nullptr || size == 0);

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);

    u64 at = 0;
    while (at < size) {
        // WriteFile takes a DWORD, and a full repaint is larger than one on a big terminal.
        DWORD ask     = (DWORD)nya_min(size - at, (u64)0x10000U);
        DWORD written = 0;

        if (!WriteFile(output, &bytes[at], ask, &written, nullptr) || written == 0) {
            nya_log_error("Writing to the console failed after " FMTu64 " of " FMTu64 " bytes.", at, size);
            return false;
        }

        at += written;
    }

    return true;
}

u64 _nya_terminal_receive(OUT u8* buffer, u64 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0, "reading zero bytes is a caller mistake, not an empty terminal");

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

    // ReadFile on a console blocks, and the poll must not, so nothing is read unless something is
    // waiting. PeekConsoleInput counts records rather than bytes, which is fine: it only has to
    // answer whether a read would return at once.
    DWORD pending = 0;
    if (!GetNumberOfConsoleInputEvents(input, &pending) || pending == 0) return 0;

    DWORD ask = (DWORD)nya_min(capacity, (u64)0x10000U);
    DWORD got = 0;

    if (!ReadFile(input, buffer, ask, &got, nullptr)) return 0;

    return (u64)got;
}

NYA_ConstCString _nya_terminal_environment(NYA_ConstCString name) {
    nya_assert(name != nullptr);
    return getenv(name);
}
