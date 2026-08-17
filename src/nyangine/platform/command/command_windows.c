#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/nyangine.h"

NYA_INTERNAL NYA_String* _nya_command_build_command_line(NYA_Command* command, NYA_Arena* arena) {
    NYA_String* cmdline = nya_string_create(arena);

    // Start with program name (quote it if it contains spaces)
    b8 needs_quotes = nya_string_contains(command->program, " ");
    if (needs_quotes) nya_string_extend(cmdline, "\"");
    nya_string_extend(cmdline, command->program);
    if (needs_quotes) nya_string_extend(cmdline, "\"");

    // Add arguments
    for (u32 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        if (command->arguments[i] == nullptr) break;

        nya_string_extend(cmdline, " ");

        // Check if argument needs quotes (contains space or special chars)
        b8 arg_needs_quotes = nya_string_contains(command->arguments[i], " ");
        if (arg_needs_quotes) nya_string_extend(cmdline, "\"");
        nya_string_extend(cmdline, command->arguments[i]);
        if (arg_needs_quotes) nya_string_extend(cmdline, "\"");
    }

    // Null terminate
    nya_string_extend(cmdline, &(NYA_String){ .items = (u8[]){ '\0' }, .length = 1 });
    return cmdline;
}

NYA_INTERNAL void _nya_command_setup_environment(NYA_Command* command) {
    // Set environment variables for the child process
    for (u32 i = 0; i < NYA_COMMAND_MAX_ENV_VARS; i++) {
        if (command->environment[i] == nullptr) break;
        (void)_putenv(command->environment[i]);
    }
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

    // Setup environment if needed
    LPVOID env = nullptr;
    if (command->environment[0] != nullptr) {
        _nya_command_setup_environment(command);
        env = GetEnvironmentStringsA();
    }

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
        if (env) FreeEnvironmentStringsA((LPCH)env);
        return nya_error(NYA_ERROR_IO, "failed to create process for '%s'", command->program);
    }

    /*
     * Handed to nya_command_wait, which drains and closes them.
     *
     * The environment block is freed here rather than there: CreateProcessA has already copied it
     * into the child, so nothing after this point needs it.
     */
    if (env) FreeEnvironmentStringsA((LPCH)env);

    command->process_handle = (u64)(uintptr_t)pi.hProcess;
    command->thread_handle  = (u64)(uintptr_t)pi.hThread;
    command->stdout_pipe    = (s32)(intptr_t)stdout_read;
    command->stderr_pipe    = (s32)(intptr_t)stderr_read;

    return NYA_OK;
}

NYA_Error nya_command_wait(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->process_handle != 0, "nya_command_wait without a matching nya_command_spawn.");

    HANDLE process     = (HANDLE)(uintptr_t)command->process_handle;
    HANDLE thread      = (HANDLE)(uintptr_t)command->thread_handle;
    HANDLE stdout_read = (HANDLE)(intptr_t)command->stdout_pipe;
    HANDLE stderr_read = (HANDLE)(intptr_t)command->stderr_pipe;

    /*
     * Both pipes drained together, whichever has bytes waiting.
     *
     * Reading stdout to the end and only then starting on stderr deadlocks any child that fills the
     * stderr pipe: past the pipe's capacity the child blocks on the write, so it never exits, never
     * closes its stdout end, and this side waits on an end-of-file that cannot arrive. The same bug
     * was in the Linux path, where tests/nyangine/platform/test_bug_command_pipe_deadlock.c
     * reproduces it — that test cannot run here, so this is the Linux fix transposed rather than one
     * verified on target.
     *
     * PeekNamedPipe rather than overlapped I/O: a blocking ReadFile on an empty pipe is exactly the
     * wait that must not happen, and asking first is much less machinery than restructuring both
     * pipes to be asynchronous.
     */
    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        HANDLE      handles[2]   = { stdout_read, stderr_read };
        NYA_String* targets[2]   = { command->stdout_content, command->stderr_content };
        b8          is_open[2]   = { true, true };
        u32         still_open   = 2;

        while (still_open > 0) {
            b8 made_progress = false;

            for (u32 i = 0; i < 2; i++) {
                if (!is_open[i]) continue;

                // Fails with ERROR_BROKEN_PIPE once the child has exited and closed its end, which
                // is what terminates this loop.
                DWORD available = 0;
                if (!PeekNamedPipe(handles[i], nullptr, 0, nullptr, &available, nullptr)) {
                    is_open[i] = false;
                    still_open--;
                    continue;
                }

                if (available == 0) continue; // nothing yet; the child is still working

                char  buffer[4096];
                DWORD wanted = available < (DWORD)sizeof(buffer) ? available : (DWORD)sizeof(buffer);
                DWORD taken  = 0;

                if (!ReadFile(handles[i], buffer, wanted, &taken, nullptr) || taken == 0) {
                    is_open[i] = false;
                    still_open--;
                    continue;
                }

                // The length carrying overload. The cstring one was used here before, which stops at
                // the first zero byte — so any captured output containing one was silently truncated.
                nya_string_extend(targets[i], &(NYA_String){ .length = (u64)taken, .items = (u8*)buffer });
                made_progress = true;
            }

            // Neither pipe had anything ready. Yield instead of spinning on PeekNamedPipe.
            if (!made_progress && still_open > 0) Sleep(1);
        }
    }

    // Outside the capture branch, matching the Linux path. These were closed only when capturing, so
    // every command that suppressed or showed its output leaked both pipe handles.
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);

    // Wait for process to complete
    WaitForSingleObject(process, INFINITE);

    // Get exit code
    DWORD exit_code;
    if (GetExitCodeProcess(process, &exit_code)) {
        command->exit_code = (s32)exit_code;
    } else {
        command->exit_code = 255;
    }

    // Cleanup
    CloseHandle(process);
    CloseHandle(thread);

    u64 end_time               = nya_clock_get_monotonic_ms();
    command->execution_time_ms = end_time - command->start_time_ms;

    // Cleared so a second wait asserts rather than waiting on a closed handle.
    command->process_handle = 0;

    return NYA_OK;
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
