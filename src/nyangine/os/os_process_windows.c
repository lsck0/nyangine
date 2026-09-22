#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/os/os_page.h"
#include "nyangine/os/os_process.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How long a wait with nothing waitable in it sleeps, since an anonymous pipe cannot be waited on. */
#define _NYA_OS_PROCESS_POLL_MS 1

/**
 * Room for one command line and one environment block. CreateProcess takes a command line of at most
 * 32767 characters, and an environment block is the parent's plus the overrides, so a megabyte each is
 * past anything either can hold.
 * */
#define _NYA_OS_PROCESS_SCRATCH_BYTES (1024 * 1024)

/** A bounded writer over that scratch: it refuses rather than growing, because there is nothing to grow into. */
typedef struct {
    char* items;
    u64   capacity;
    u64   length;
    b8    overflowed;
} _NYA_OsProcessText;

NYA_INTERNAL void _nya_os_process_text_push(_NYA_OsProcessText* text, const char* bytes, u64 length);
NYA_INTERNAL void _nya_os_process_text_push_cstring(_NYA_OsProcessText* text, NYA_ConstCString cstring);

/**
 * Appends one argument quoted the way the CRT and msys runtimes split a command line: backslashes only
 * escape when they run into a quote, and any whitespace, not only a space, splits unquoted text.
 * */
NYA_INTERNAL void _nya_os_process_text_push_argument(_NYA_OsProcessText* text, NYA_ConstCString argument);

/** The whole argument vector as the one string CreateProcess takes, terminated. */
NYA_INTERNAL void _nya_os_process_command_line(_NYA_OsProcessText* text, const NYA_OsProcessSpawn* spawn);

/**
 * The parent's environment with the spawn's `NAME=value` entries replacing or adding to it, as the
 * double terminated block CreateProcess takes. Built rather than set with _putenv, which would leak
 * every variable into the parent.
 * */
NYA_INTERNAL void _nya_os_process_environment(_NYA_OsProcessText* text, const NYA_OsProcessSpawn* spawn);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PIPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_OsProcessStatus nya_os_pipe_open(OUT NYA_OsPipe* out_read, OUT NYA_OsPipe* out_write) {
    if (out_read == nullptr || out_write == nullptr) return NYA_OS_PROCESS_FAILED;

    SECURITY_ATTRIBUTES sa = { .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE };

    HANDLE read_end  = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return NYA_OS_PROCESS_FAILED;

    // the read end stays in this process alone: a child holding a copy of it is a writer that never
    // goes away, and the pipe would not break when the one that matters exits.
    if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return NYA_OS_PROCESS_FAILED;
    }

    *out_read  = (NYA_OsPipe)(intptr_t)read_end;
    *out_write = (NYA_OsPipe)(intptr_t)write_end;

    return NYA_OS_PROCESS_OK;
}

void nya_os_pipe_close(NYA_OsPipe pipe) {
    if (pipe == NYA_OS_PIPE_NONE) return;

    CloseHandle((HANDLE)(intptr_t)pipe);
}

NYA_OsProcessStatus nya_os_pipe_read(NYA_OsPipe pipe, OUT u8* buffer, u64 capacity, OUT u64* out_taken) {
    if (pipe == NYA_OS_PIPE_NONE || buffer == nullptr || capacity == 0 || out_taken == nullptr) return NYA_OS_PROCESS_FAILED;

    *out_taken = 0;

    HANDLE handle = (HANDLE)(intptr_t)pipe;

    // Asked first because ReadFile on a pipe blocks until something arrives, and it fails with
    // ERROR_BROKEN_PIPE once the child has exited and closed its end, which is the end of file here.
    DWORD available = 0;
    if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return NYA_OS_PROCESS_CLOSED;
    if (available == 0) return NYA_OS_PROCESS_PENDING;

    u64   room  = capacity < (u64)available ? capacity : (u64)available;
    DWORD taken = 0;
    if (!ReadFile(handle, buffer, (DWORD)room, &taken, nullptr) || taken == 0) return NYA_OS_PROCESS_CLOSED;

    *out_taken = (u64)taken;

    return NYA_OS_PROCESS_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PROCESSES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 nya_os_process_id(void) {
    return (u32)GetCurrentProcessId();
}

NYA_OsProcessStatus nya_os_process_spawn(const NYA_OsProcessSpawn* spawn, OUT NYA_OsProcess* out_process) {
    if (spawn == nullptr || out_process == nullptr) return NYA_OS_PROCESS_FAILED;
    if (spawn->program == nullptr || spawn->program[0] == '\0') return NYA_OS_PROCESS_FAILED;
    if (spawn->arguments == nullptr || spawn->arguments[0] == nullptr) return NYA_OS_PROCESS_FAILED;

    // One reservation for both blocks, for the length of this call: there is no arena below base, and
    // neither block is worth a fixed buffer of a megabyte on the stack.
    const u64 scratch_bytes = 2 * _NYA_OS_PROCESS_SCRATCH_BYTES;

    char* scratch = nya_os_page_reserve(scratch_bytes);
    if (scratch == nullptr) return NYA_OS_PROCESS_FAILED;
    defer (void)nya_os_page_release(scratch, scratch_bytes);

    if (!nya_os_page_commit(scratch, scratch_bytes)) return NYA_OS_PROCESS_FAILED;

    _NYA_OsProcessText command_line = { .items = scratch, .capacity = _NYA_OS_PROCESS_SCRATCH_BYTES };
    _NYA_OsProcessText environment  = { .items = scratch + _NYA_OS_PROCESS_SCRATCH_BYTES, .capacity = _NYA_OS_PROCESS_SCRATCH_BYTES };

    _nya_os_process_command_line(&command_line, spawn);
    if (command_line.overflowed) return NYA_OS_PROCESS_FAILED;

    b8 has_environment = spawn->environment != nullptr && spawn->environment[0] != nullptr;
    if (has_environment) {
        _nya_os_process_environment(&environment, spawn);
        if (environment.overflowed) return NYA_OS_PROCESS_FAILED;
    }

    STARTUPINFOA si         = { .cb = sizeof(si) };
    HANDLE       nul_handle = nullptr;

    if (spawn->stdout_pipe != NYA_OS_PIPE_NONE || spawn->stderr_pipe != NYA_OS_PIPE_NONE) {
        // whichever end was not given keeps this process's own stream: naming one of the three in
        // STARTUPINFO means naming all three, and a null handle would leave the child without it.
        si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput  = spawn->stdout_pipe != NYA_OS_PIPE_NONE ? (HANDLE)(intptr_t)spawn->stdout_pipe : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError   = spawn->stderr_pipe != NYA_OS_PIPE_NONE ? (HANDLE)(intptr_t)spawn->stderr_pipe : GetStdHandle(STD_ERROR_HANDLE);
        si.dwFlags    |= STARTF_USESTDHANDLES;
    } else if (spawn->suppress_output) {
        SECURITY_ATTRIBUTES sa = { .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE };

        nul_handle     = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput  = nul_handle;
        si.hStdError   = nul_handle;
        si.dwFlags    |= STARTF_USESTDHANDLES;
    }

    LPCSTR working_directory = spawn->working_directory;
    if (working_directory != nullptr && working_directory[0] == '\0') working_directory = nullptr;

    PROCESS_INFORMATION pi = { 0 };

    // No application name: the command line names the program, which is what lets a bare name be found
    // on PATH the way execvp finds it. Handles are inherited, since that is what hands over the pipes.
    BOOL created = CreateProcessA(
        nullptr,
        command_line.items,
        nullptr,
        nullptr,
        TRUE,
        0,
        has_environment ? environment.items : nullptr,
        working_directory,
        &si,
        &pi
    );

    if (nul_handle != nullptr) CloseHandle(nul_handle);
    if (!created) return NYA_OS_PROCESS_FAILED;

    *out_process = (NYA_OsProcess){ .handle = (u64)(uintptr_t)pi.hProcess, .thread = (u64)(uintptr_t)pi.hThread };

    return NYA_OS_PROCESS_OK;
}

NYA_OsProcessStatus nya_os_process_wait(NYA_OsProcess process, u32 timeout_ms, OUT s32* out_exit_code) {
    if (process.handle == 0 || out_exit_code == nullptr) return NYA_OS_PROCESS_FAILED;

    HANDLE handle = (HANDLE)(uintptr_t)process.handle;

    DWORD state = WaitForSingleObject(handle, timeout_ms == NYA_OS_PROCESS_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms);
    if (state == WAIT_TIMEOUT) return NYA_OS_PROCESS_PENDING;
    if (state != WAIT_OBJECT_0) return NYA_OS_PROCESS_FAILED;

    // 255 when Windows will not say, rather than a zero that would read as a clean exit.
    DWORD exit_code = 0;
    *out_exit_code  = GetExitCodeProcess(handle, &exit_code) ? (s32)exit_code : 255;

    CloseHandle(handle);
    if (process.thread != 0) CloseHandle((HANDLE)(uintptr_t)process.thread);

    return NYA_OS_PROCESS_OK;
}

void nya_os_process_wait_any(const NYA_OsProcess* processes, u32 process_count, const NYA_OsPipe* pipes, u32 pipe_count, u32 timeout_ms) {
    // the processes and not the pipes: an anonymous pipe is not a waitable object, so only an exit wakes
    // this early. a child blocked on a full pipe is drained by the next read, at most one timeout later.
    (void)pipes;
    (void)pipe_count;

    HANDLE handles[NYA_OS_PROCESS_WAIT_MAX];
    DWORD  count = 0;

    for (u32 i = 0; i < process_count && count < NYA_OS_PROCESS_WAIT_MAX; i++) {
        if (processes == nullptr || processes[i].handle == 0) continue;
        handles[count++] = (HANDLE)(uintptr_t)processes[i].handle;
    }

    if (count == 0) {
        // nothing to wait on, so the wait is the sleep the caller asked for. A wait meant to last until
        // something happens becomes a short one: with no object behind it, it would never end.
        Sleep(timeout_ms == NYA_OS_PROCESS_WAIT_FOREVER ? _NYA_OS_PROCESS_POLL_MS : timeout_ms);
        return;
    }

    (void)WaitForMultipleObjects(count, handles, FALSE, timeout_ms == NYA_OS_PROCESS_WAIT_FOREVER ? INFINITE : timeout_ms);
}

NYA_OsProcessStatus nya_os_process_kill(NYA_OsProcess process) {
    if (process.handle == 0) return NYA_OS_PROCESS_FAILED;

    // 137 is what the Linux side reports for a killed child, 128 plus SIGKILL, and Windows lets the
    // killer pick the number, so both targets answer the same thing for the same end.
    return TerminateProcess((HANDLE)(uintptr_t)process.handle, 137) != 0 ? NYA_OS_PROCESS_OK : NYA_OS_PROCESS_FAILED;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_os_process_text_push(_NYA_OsProcessText* text, const char* bytes, u64 length) {
    if (text->length + length > text->capacity) {
        text->overflowed = true;
        return;
    }

    for (u64 i = 0; i < length; i++) text->items[text->length + i] = bytes[i];
    text->length += length;
}

NYA_INTERNAL void _nya_os_process_text_push_cstring(_NYA_OsProcessText* text, NYA_ConstCString cstring) {
    _nya_os_process_text_push(text, cstring, (u64)strlen(cstring));
}

NYA_INTERNAL void _nya_os_process_text_push_argument(_NYA_OsProcessText* text, NYA_ConstCString argument) {
    b8 needs_quotes = argument[0] == '\0';
    for (NYA_ConstCString c = argument; *c != '\0'; c++) {
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\v' || *c == '"') needs_quotes = true;
    }
    if (!needs_quotes) {
        _nya_os_process_text_push_cstring(text, argument);
        return;
    }

    _nya_os_process_text_push(text, "\"", 1);
    u64 backslashes = 0;
    for (NYA_ConstCString c = argument; *c != '\0'; c++) {
        if (*c == '\\') {
            backslashes++;
            continue;
        }
        // a quote doubles the backslashes before it and gets one of its own.
        u64 repeat = *c == '"' ? backslashes * 2 + 1 : backslashes;
        for (u64 i = 0; i < repeat; i++) _nya_os_process_text_push(text, "\\", 1);
        backslashes = 0;
        _nya_os_process_text_push(text, c, 1);
    }
    // the closing quote would otherwise be escaped by a trailing backslash.
    for (u64 i = 0; i < backslashes * 2; i++) _nya_os_process_text_push(text, "\\", 1);
    _nya_os_process_text_push(text, "\"", 1);
}

NYA_INTERNAL void _nya_os_process_command_line(_NYA_OsProcessText* text, const NYA_OsProcessSpawn* spawn) {
    for (u32 i = 0; spawn->arguments[i] != nullptr; i++) {
        if (i > 0) _nya_os_process_text_push(text, " ", 1);
        _nya_os_process_text_push_argument(text, spawn->arguments[i]);
    }

    _nya_os_process_text_push(text, "\0", 1);
}

NYA_INTERNAL void _nya_os_process_environment(_NYA_OsProcessText* text, const NYA_OsProcessSpawn* spawn) {
    LPCH parent = GetEnvironmentStringsA();

    for (LPCH entry = parent; parent != nullptr && *entry != '\0'; entry += strlen(entry) + 1) {
        // entries starting with '=' are per drive working directories and have no name to override.
        u64 name_length = *entry == '=' ? 0 : (u64)strcspn(entry, "=");
        b8  overridden  = false;

        for (u32 i = 0; spawn->environment[i] != nullptr; i++) {
            NYA_ConstCString candidate = spawn->environment[i];
            if (name_length > 0 && _strnicmp(candidate, entry, (size_t)name_length) == 0 && candidate[name_length] == '=') overridden = true;
        }
        if (overridden) continue;

        _nya_os_process_text_push_cstring(text, entry);
        _nya_os_process_text_push(text, "\0", 1);
    }
    if (parent != nullptr) FreeEnvironmentStringsA(parent);

    for (u32 i = 0; spawn->environment[i] != nullptr; i++) {
        // an entry with no '=' is not a variable; the caller above is where that is a programming mistake.
        if (strchr(spawn->environment[i], '=') == nullptr) continue;

        _nya_os_process_text_push_cstring(text, spawn->environment[i]);
        _nya_os_process_text_push(text, "\0", 1);
    }

    _nya_os_process_text_push(text, "\0", 1);
}
