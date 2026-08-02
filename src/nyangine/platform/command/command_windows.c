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
    nya_assert(command != nullptr);
    nya_assert(command->program);

    u64 start_time = nya_clock_get_timestamp_ms();

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

        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) { return nya_error(NYA_ERROR_IO, "Failed to create stdout pipe."); }
        if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            return nya_error(NYA_ERROR_IO, "Failed to set stdout pipe handle information.");
        }

        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            return nya_error(NYA_ERROR_IO, "Failed to create stderr pipe.");
        }
        if (!SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            CloseHandle(stderr_read);
            CloseHandle(stderr_write);
            return nya_error(NYA_ERROR_IO, "Failed to set stderr pipe handle information.");
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
        return nya_error(NYA_ERROR_IO, "Failed to create process for '%s'.", command->program);
    }

    // Read output if capturing
    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        char  buffer[4096];
        DWORD bytes_read;
        BOOL  success;

        // Read stdout
        while (true) {
            success = ReadFile(stdout_read, buffer, sizeof(buffer) - 1, &bytes_read, nullptr);
            if (!success || bytes_read == 0) break;
            buffer[bytes_read] = '\0';
            nya_string_extend(command->stdout_content, buffer);
        }

        // Read stderr
        while (true) {
            success = ReadFile(stderr_read, buffer, sizeof(buffer) - 1, &bytes_read, nullptr);
            if (!success || bytes_read == 0) break;
            buffer[bytes_read] = '\0';
            nya_string_extend(command->stderr_content, buffer);
        }

        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
    }

    // Wait for process to complete
    WaitForSingleObject(pi.hProcess, INFINITE);

    // Get exit code
    DWORD exit_code;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        command->exit_code = (s32)exit_code;
    } else {
        command->exit_code = 255;
    }

    // Cleanup
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (env) FreeEnvironmentStringsA((LPCH)env);

    u64 end_time               = nya_clock_get_timestamp_ms();
    command->execution_time_ms = end_time - start_time;

    return NYA_OK;
}

void nya_command_destroy(NYA_Command* command) {
    nya_assert(command != nullptr);

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        nya_string_destroy(command->stdout_content);
        nya_string_destroy(command->stderr_content);
    }
}
