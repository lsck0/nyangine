#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Most reads one non-blocking drain makes, so a chatty child cannot starve the others being polled. */
#define _NYA_COMMAND_DRAIN_MAX_READS 64

/**
 * Reads the child's pipes into the captured output and closes each one once it breaks. Blocks until
 * both are closed when `block` is set, otherwise returns once nothing more is ready.
 * */
NYA_INTERNAL void _nya_command_drain(NYA_Command* command, b8 block);

/** Fills in the results of an exited child and releases its handles. */
NYA_INTERNAL void _nya_command_finish(NYA_Command* command);

/**
 * Appends one argument quoted the way the CRT and msys runtimes split a command line: backslashes only
 * escape when they run into a quote, and any whitespace, not only a space, splits unquoted text.
 * */
NYA_INTERNAL void _nya_command_append_argument(NYA_String* cmdline, NYA_ConstCString argument) {
    nya_assert(cmdline != nullptr);
    nya_assert(argument != nullptr);

    b8 needs_quotes = argument[0] == '\0';
    for (NYA_ConstCString c = argument; *c != '\0'; c++) {
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\v' || *c == '"') needs_quotes = true;
    }
    if (!needs_quotes) {
        nya_string_extend(cmdline, argument);
        return;
    }

    nya_string_extend(cmdline, "\"");
    u64 backslashes = 0;
    for (NYA_ConstCString c = argument; *c != '\0'; c++) {
        if (*c == '\\') {
            backslashes++;
            continue;
        }
        // a quote doubles the backslashes before it and gets one of its own.
        u64 repeat = *c == '"' ? backslashes * 2 + 1 : backslashes;
        for (u64 i = 0; i < repeat; i++) nya_string_extend(cmdline, "\\");
        backslashes = 0;
        nya_string_extend(cmdline, &(NYA_String){ .items = (u8*)c, .length = 1 });
    }
    // the closing quote would otherwise be escaped by a trailing backslash.
    for (u64 i = 0; i < backslashes * 2; i++) nya_string_extend(cmdline, "\\");
    nya_string_extend(cmdline, "\"");
}

NYA_INTERNAL NYA_String* _nya_command_build_command_line(NYA_Command* command, NYA_Arena* arena) {
    NYA_String* cmdline = nya_string_create(arena);

    _nya_command_append_argument(cmdline, command->program);
    for (u32 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        if (command->arguments[i] == nullptr) break;
        nya_string_extend(cmdline, " ");
        _nya_command_append_argument(cmdline, command->arguments[i]);
    }

    nya_string_extend(cmdline, &(NYA_String){ .items = (u8[]){ '\0' }, .length = 1 });
    return cmdline;
}

/**
 * The parent's environment with the command's `NAME=value` entries replacing or adding to it, as the
 * double terminated block CreateProcess takes. Built rather than set with _putenv, which would leak
 * every variable into the parent.
 * */
NYA_INTERNAL NYA_String* _nya_command_build_environment(NYA_Command* command, NYA_Arena* arena) {
    nya_assert(command->environment[0] != nullptr);

    NYA_String* block  = nya_string_create(arena);
    NYA_String  nul    = { .items = (u8[]){ '\0' }, .length = 1 };
    LPCH        parent = GetEnvironmentStringsA();
    nya_assert(parent != nullptr);

    for (LPCH entry = parent; *entry != '\0'; entry += strlen(entry) + 1) {
        // entries starting with '=' are per drive working directories and have no name to override.
        u64 name_length = *entry == '=' ? 0 : strcspn(entry, "=");
        b8  overridden  = false;
        for (u32 i = 0; i < NYA_COMMAND_MAX_ENV_VARS && command->environment[i] != nullptr; i++) {
            NYA_ConstCString candidate = command->environment[i];
            if (name_length > 0 && _strnicmp(candidate, entry, name_length) == 0 && candidate[name_length] == '=') overridden = true;
        }
        if (overridden) continue;
        nya_string_extend(block, entry);
        nya_string_extend(block, &nul);
    }
    FreeEnvironmentStringsA(parent);

    for (u32 i = 0; i < NYA_COMMAND_MAX_ENV_VARS && command->environment[i] != nullptr; i++) {
        nya_assert(strchr(command->environment[i], '=') != nullptr, "Environment entries are NAME=value.");
        nya_string_extend(block, command->environment[i]);
        nya_string_extend(block, &nul);
    }
    nya_string_extend(block, &nul);
    return block;
}

NYA_Error nya_command_run(NYA_Command* command) {
    // Spawn then wait, so there is exactly one implementation of each half. See command_linux.c.
    NYA_TRY(nya_command_spawn(command));

    return nya_command_wait(command);
}

NYA_Error nya_command_spawn(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->program);

    command->start_time_ms = nya_clock_get_monotonic_ms();

    // Create pipes for stdout and stderr if capturing
    HANDLE stdout_read  = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read  = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE nul_handle   = nullptr;

    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        nya_assert(command->arena != nullptr, "Arena must be provided when capturing output.");
        command->stdout_content = nya_string_create(command->arena);
        command->stderr_content = nya_string_create(command->arena);

        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) { return nya_error(NYA_ERROR_IO, "failed to create stdout pipe"); }
        if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            return nya_error(NYA_ERROR_IO, "failed to set stdout pipe handle information");
        }

        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            return nya_error(NYA_ERROR_IO, "failed to create stderr pipe");
        }
        if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            CloseHandle(stderr_read);
            CloseHandle(stderr_write);
            return nya_error(NYA_ERROR_IO, "failed to set stderr pipe handle information");
        }
    }

    // Build command line
    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_String* cmdline = _nya_command_build_command_line(command, arena);

    // Setup startup info
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        si.hStdOutput  = stdout_write;
        si.hStdError   = stderr_write;
        si.dwFlags    |= STARTF_USESTDHANDLES;
    } else if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_SUPPRESS)) {
        // Redirect to NUL
        nul_handle     = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        si.hStdOutput  = nul_handle;
        si.hStdError   = nul_handle;
        si.dwFlags    |= STARTF_USESTDHANDLES;
    }

    // Setup process info
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    LPVOID env = nullptr;
    if (command->environment[0] != nullptr) env = _nya_command_build_environment(command, arena)->items;

    // Change working directory if specified
    LPCSTR working_dir = command->working_directory;
    if (working_dir != nullptr && strlen(working_dir) == 0) { working_dir = nullptr; }

    // Create the process
    BOOL created = CreateProcessA(
        nullptr,               // Application name (use command line)
        (LPSTR)cmdline->items, // Command line
        nullptr,               // Process security attributes
        nullptr,               // Thread security attributes
        TRUE,                  // Inherit handles
        0,                     // Creation flags
        env,                   // Environment
        working_dir,           // Working directory
        &si,                   // Startup info
        &pi                    // Process info
    );

    // Close write ends of pipes in parent
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    if (nul_handle) CloseHandle(nul_handle);

    if (!created) {
        if (stdout_read) CloseHandle(stdout_read);
        if (stderr_read) CloseHandle(stderr_read);
        return nya_error(NYA_ERROR_IO, "failed to create process for '%s'", command->program);
    }

    /*
     * Handed to nya_command_wait, which drains and closes them.
     */

    command->process_handle = (u64)(uintptr_t)pi.hProcess;
    command->thread_handle  = (u64)(uintptr_t)pi.hThread;
    command->stdout_pipe    = (s32)(intptr_t)stdout_read;
    command->stderr_pipe    = (s32)(intptr_t)stderr_read;

    return NYA_OK;
}

NYA_Error nya_command_wait(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->process_handle != 0, "nya_command_wait without a matching nya_command_spawn.");

    _nya_command_drain(command, true);
    nya_assert(command->stdout_pipe == 0 && command->stderr_pipe == 0);

    WaitForSingleObject((HANDLE)(uintptr_t)command->process_handle, INFINITE);

    _nya_command_finish(command);

    return NYA_OK;
}

NYA_Error nya_command_try_wait(NYA_Command* command, b8* out_finished) {
    nya_assert(command != nullptr);
    nya_assert(out_finished != nullptr);
    nya_assert(command->process_handle != 0, "nya_command_try_wait without a matching nya_command_spawn.");

    *out_finished = false;

    _nya_command_drain(command, false);

    // a child still holding its pipes is still writing, and finishing now would lose the rest.
    if (command->stdout_pipe != 0 || command->stderr_pipe != 0) return NYA_OK;

    DWORD state = WaitForSingleObject((HANDLE)(uintptr_t)command->process_handle, 0);
    if (state == WAIT_TIMEOUT) return NYA_OK;
    if (state != WAIT_OBJECT_0) return nya_error(NYA_ERROR_IO, "failed to wait for process '%s'", command->program);

    _nya_command_finish(command);
    *out_finished = true;

    return NYA_OK;
}

void nya_command_wait_ready(NYA_Command* const* commands, u32 count, u32 timeout_ms) {
    nya_assert(commands != nullptr);
    nya_assert(count <= NYA_COMMAND_MAX_WAIT_READY, "nya_command_wait_ready takes at most NYA_COMMAND_MAX_WAIT_READY commands.");

    if (count == 0) {
        Sleep(timeout_ms);
        return;
    }

    HANDLE processes[NYA_COMMAND_MAX_WAIT_READY];
    for (u32 i = 0; i < count; i++) {
        nya_assert(commands[i] != nullptr && commands[i]->process_handle != 0);
        processes[i] = (HANDLE)(uintptr_t)commands[i]->process_handle;
    }

    // anonymous pipes cannot be waited on, so only an exit wakes this early. a child blocked on a full
    // pipe is drained by the next try wait, at most one timeout later.
    (void)WaitForMultipleObjects(count, processes, FALSE, timeout_ms);
}

u32 nya_platform_processor_count(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);

    if (info.dwNumberOfProcessors < 1) return 1;

    return (u32)info.dwNumberOfProcessors;
}

void nya_command_destroy(NYA_Command* command) {
    nya_assert(command != nullptr);

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        nya_string_destroy(command->stdout_content);
        nya_string_destroy(command->stderr_content);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_command_drain(NYA_Command* command, b8 block) {
    nya_assert(command != nullptr);

    s32* pipes[2] = { &command->stdout_pipe, &command->stderr_pipe };

    // nothing was redirected into them, so there is nothing to read.
    if (!nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        for (u32 i = 0; i < 2; i++) {
            if (*pipes[i] == 0) continue;
            CloseHandle((HANDLE)(intptr_t)*pipes[i]);
            *pipes[i] = 0;
        }
        return;
    }

    NYA_String* targets[2] = { command->stdout_content, command->stderr_content };

    // a blocking drain ends when both pipes break, however long the child keeps writing.
    for (u32 reads = 0; block || reads < _NYA_COMMAND_DRAIN_MAX_READS; reads++) {
        if (*pipes[0] == 0 && *pipes[1] == 0) return;

        b8 made_progress = false;

        for (u32 i = 0; i < 2; i++) {
            if (*pipes[i] == 0) continue;

            HANDLE handle = (HANDLE)(intptr_t)*pipes[i];

            // Fails with ERROR_BROKEN_PIPE once the child has exited and closed its end, which
            // is what terminates this loop.
            DWORD available = 0;
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                CloseHandle(handle);
                *pipes[i] = 0;
                made_progress = true;
                continue;
            }

            if (available == 0) continue; // nothing yet; the child is still working

            char  buffer[4096];
            DWORD wanted = available < (DWORD)sizeof(buffer) ? available : (DWORD)sizeof(buffer);
            DWORD taken  = 0;

            if (!ReadFile(handle, buffer, wanted, &taken, nullptr) || taken == 0) {
                CloseHandle(handle);
                *pipes[i] = 0;
                made_progress = true;
                continue;
            }

            // the length carrying overload, so output containing zero bytes is not truncated.
            nya_string_extend(targets[i], &(NYA_String){ .length = (u64)taken, .items = (u8*)buffer });
            made_progress = true;
        }

        if (made_progress) continue;
        if (!block) return;

        // Neither pipe had anything ready. Yield instead of spinning on PeekNamedPipe.
        Sleep(1);
    }
}

NYA_INTERNAL void _nya_command_finish(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->process_handle != 0);

    HANDLE process = (HANDLE)(uintptr_t)command->process_handle;
    HANDLE thread  = (HANDLE)(uintptr_t)command->thread_handle;

    DWORD exit_code;
    if (GetExitCodeProcess(process, &exit_code)) {
        command->exit_code = (s32)exit_code;
    } else {
        command->exit_code = 255;
    }

    CloseHandle(process);
    CloseHandle(thread);

    u64 end_time               = nya_clock_get_monotonic_ms();
    command->execution_time_ms = end_time - command->start_time_ms;

    // Cleared so a second wait asserts rather than waiting on a closed handle.
    command->process_handle = 0;
    command->thread_handle  = 0;
}
