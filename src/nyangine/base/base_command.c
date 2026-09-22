#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Most reads one non-blocking drain makes, so a chatty child cannot starve the others being polled. */
#define _NYA_COMMAND_DRAIN_MAX_READS 64

/** What one read of a pipe takes at most. A page: a pipe's own buffer is a few of these. */
#define _NYA_COMMAND_DRAIN_BUFFER_BYTES 4096

/**
 * Reads the child's pipes into the captured output and closes each one at end of file. Blocks until
 * both are closed when `block` is set, otherwise returns once nothing more is ready.
 * */
NYA_INTERNAL void _nya_command_drain(NYA_Command* command, b8 block);

/** Fills in the results of a command that has exited. */
NYA_INTERNAL void _nya_command_finish(NYA_Command* command, s32 exit_code);

/** The monotonic clock in milliseconds, which is what a command is timed with. */
NYA_INTERNAL u64 _nya_command_now_ms(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_command_run(NYA_Command* command) {
    // Spawn then wait, so there is exactly one implementation of each half and no way for the
    // blocking and non-blocking paths to drift apart.
    NYA_TRY(nya_command_spawn(command));

    return nya_command_wait(command);
}

NYA_Error nya_command_spawn(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->program != nullptr && strlen(command->program) != 0);

    // both arrays are handed on as they are, and what terminates them is the null the last slot still
    // holds. A caller that fills every slot has lost the terminator, not just the room.
    nya_assert(command->arguments[NYA_COMMAND_MAX_ARGUMENTS - 1] == nullptr, "A command takes at most NYA_COMMAND_MAX_ARGUMENTS - 1 arguments.");
    nya_assert(command->environment[NYA_COMMAND_MAX_ENV_VARS - 1] == nullptr, "A command takes at most NYA_COMMAND_MAX_ENV_VARS - 1 variables.");

    command->start_time_ms = _nya_command_now_ms();
    command->stdout_pipe   = NYA_OS_PIPE_NONE;
    command->stderr_pipe   = NYA_OS_PIPE_NONE;

    NYA_OsPipe stdout_write = NYA_OS_PIPE_NONE;
    NYA_OsPipe stderr_write = NYA_OS_PIPE_NONE;

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        nya_assert(command->arena != nullptr, "Arena must be provided when capturing output.");
        command->stdout_content = nya_string_create(command->arena);
        command->stderr_content = nya_string_create(command->arena);

        if (nya_os_pipe_open(&command->stdout_pipe, &stdout_write) != NYA_OS_PROCESS_OK) {
            return nya_error(NYA_ERROR_IO, "failed to create the stdout pipe for '%s'", command->program);
        }
        if (nya_os_pipe_open(&command->stderr_pipe, &stderr_write) != NYA_OS_PROCESS_OK) {
            nya_os_pipe_close(command->stdout_pipe);
            nya_os_pipe_close(stdout_write);
            command->stdout_pipe = NYA_OS_PIPE_NONE;

            return nya_error(NYA_ERROR_IO, "failed to create the stderr pipe for '%s'", command->program);
        }
    }

    // argv, with the program in front of it as the child's own name. Bounded by the arguments array
    // plus that name and the terminator.
    NYA_ConstCString argv[NYA_COMMAND_MAX_ARGUMENTS + 2] = { nullptr };
    u32              argc                                = 0;

    argv[argc++] = command->program;
    for (u32 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS && command->arguments[i] != nullptr; i++) argv[argc++] = command->arguments[i];
    argv[argc] = nullptr;

    NYA_OsProcessSpawn spawn = {
        .program           = command->program,
        .arguments         = argv,
        .working_directory = command->working_directory,
        .environment       = command->environment,
        .stdout_pipe       = stdout_write,
        .stderr_pipe       = stderr_write,
        .suppress_output   = nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_SUPPRESS) != 0,
    };

    NYA_OsProcessStatus status = nya_os_process_spawn(&spawn, &command->process);

    // The child has its own copies of the write ends now, and these have to go: a read end whose writer
    // is still held open in this process never reaches end of file, and the drain would wait forever.
    nya_os_pipe_close(stdout_write);
    nya_os_pipe_close(stderr_write);

    if (status != NYA_OS_PROCESS_OK) {
        nya_os_pipe_close(command->stdout_pipe);
        nya_os_pipe_close(command->stderr_pipe);
        command->stdout_pipe = NYA_OS_PIPE_NONE;
        command->stderr_pipe = NYA_OS_PIPE_NONE;

        return nya_error(NYA_ERROR_IO, "failed to start '%s'", command->program);
    }

    /*
     * The read ends are handed to nya_command_wait, which is what drains and closes them. Held open
     * until then: a child writing more than a pipe buffer's worth blocks until someone reads, so
     * closing them here would deadlock any command that produces real output.
     */

    return NYA_OK;
}

NYA_Error nya_command_wait(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->process.handle != 0, "nya_command_wait without a matching nya_command_spawn.");

    _nya_command_drain(command, true);
    nya_assert(command->stdout_pipe == NYA_OS_PIPE_NONE && command->stderr_pipe == NYA_OS_PIPE_NONE);

    s32 exit_code = 0;
    if (nya_os_process_wait(command->process, NYA_OS_PROCESS_WAIT_FOREVER, &exit_code) != NYA_OS_PROCESS_OK) {
        return nya_error(NYA_ERROR_IO, "failed to wait for process '%s'", command->program);
    }

    _nya_command_finish(command, exit_code);

    return NYA_OK;
}

NYA_Error nya_command_try_wait(NYA_Command* command, b8* out_finished) {
    nya_assert(command != nullptr);
    nya_assert(out_finished != nullptr);
    nya_assert(command->process.handle != 0, "nya_command_try_wait without a matching nya_command_spawn.");

    *out_finished = false;

    _nya_command_drain(command, false);

    // a child still holding its pipes is still writing, and reaping it now would lose the rest.
    if (command->stdout_pipe != NYA_OS_PIPE_NONE || command->stderr_pipe != NYA_OS_PIPE_NONE) return NYA_OK;

    s32                 exit_code = 0;
    NYA_OsProcessStatus status    = nya_os_process_wait(command->process, 0, &exit_code);
    if (status == NYA_OS_PROCESS_PENDING) return NYA_OK;
    if (status != NYA_OS_PROCESS_OK) return nya_error(NYA_ERROR_IO, "failed to wait for process '%s'", command->program);

    _nya_command_finish(command, exit_code);
    *out_finished = true;

    return NYA_OK;
}

void nya_command_wait_ready(NYA_Command* const* commands, u32 count, u32 timeout_ms) {
    nya_assert(commands != nullptr);
    nya_assert(count <= NYA_COMMAND_MAX_WAIT_READY, "nya_command_wait_ready takes at most NYA_COMMAND_MAX_WAIT_READY commands.");

    NYA_OsProcess processes[NYA_COMMAND_MAX_WAIT_READY];
    NYA_OsPipe    pipes[NYA_COMMAND_MAX_WAIT_READY * 2];
    u32           process_count = 0;
    u32           pipe_count    = 0;

    for (u32 i = 0; i < count; i++) {
        nya_assert(commands[i] != nullptr);

        if (commands[i]->process.handle != 0) processes[process_count++] = commands[i]->process;
        if (commands[i]->stdout_pipe != NYA_OS_PIPE_NONE) pipes[pipe_count++] = commands[i]->stdout_pipe;
        if (commands[i]->stderr_pipe != NYA_OS_PIPE_NONE) pipes[pipe_count++] = commands[i]->stderr_pipe;
    }

    // Both halves, because the two targets wake on different things: one waits on the processes and one
    // on the pipes, and with neither left it is the plain sleep a child between closing its output and
    // exiting needs.
    nya_os_process_wait_any(processes, process_count, pipes, pipe_count, timeout_ms);
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

    NYA_OsPipe* pipes[2] = { &command->stdout_pipe, &command->stderr_pipe };

    // nothing was redirected into them, so there is nothing to read.
    if (!nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        for (u32 i = 0; i < 2; i++) {
            nya_os_pipe_close(*pipes[i]);
            *pipes[i] = NYA_OS_PIPE_NONE;
        }
        return;
    }

    NYA_String* targets[2] = { command->stdout_content, command->stderr_content };

    // Both pipes every pass, and neither one to end of file before the other is looked at: a child that
    // fills one while the reader waits on the other can never exit, which is the deadlock this shape
    // exists to avoid. A blocking drain ends when both have closed, however long the child keeps writing.
    for (u32 reads = 0; block || reads < _NYA_COMMAND_DRAIN_MAX_READS; reads++) {
        if (*pipes[0] == NYA_OS_PIPE_NONE && *pipes[1] == NYA_OS_PIPE_NONE) return;

        b8 made_progress = false;

        for (u32 i = 0; i < 2; i++) {
            if (*pipes[i] == NYA_OS_PIPE_NONE) continue;

            u8  buffer[_NYA_COMMAND_DRAIN_BUFFER_BYTES];
            u64 taken = 0;

            NYA_OsProcessStatus status = nya_os_pipe_read(*pipes[i], buffer, sizeof(buffer), &taken);

            // nothing ready; the child is still working.
            if (status == NYA_OS_PROCESS_PENDING) continue;

            if (status == NYA_OS_PROCESS_OK) {
                // The length carrying overload, because captured output may contain zero bytes
                // and the cstring one would stop at the first.
                nya_string_extend(targets[i], &(NYA_String){ .length = taken, .items = buffer });
                made_progress = true;
                continue;
            }

            // end of file, or a read that will not come back: nothing better to do than stop reading;
            // the child sees a closed pipe and the wait still reaps it.
            nya_os_pipe_close(*pipes[i]);
            *pipes[i]     = NYA_OS_PIPE_NONE;
            made_progress = true;
        }

        if (made_progress) continue;
        if (!block) return;

        // Neither had anything ready. Wait on whichever is still open rather than spinning on them.
        NYA_OsPipe live[2];
        u32        live_count = 0;
        for (u32 i = 0; i < 2; i++) {
            if (*pipes[i] != NYA_OS_PIPE_NONE) live[live_count++] = *pipes[i];
        }

        nya_os_process_wait_any(nullptr, 0, live, live_count, NYA_OS_PROCESS_WAIT_FOREVER);
    }
}

NYA_INTERNAL void _nya_command_finish(NYA_Command* command, s32 exit_code) {
    nya_assert(command != nullptr);
    nya_assert(command->process.handle != 0);

    command->exit_code         = exit_code;
    command->execution_time_ms = _nya_command_now_ms() - command->start_time_ms;

    // Cleared so a second wait on the same command asserts rather than reaping an unrelated process
    // that happened to reuse the pid, or waiting on a handle that is already closed.
    command->process = (NYA_OsProcess){ 0 };
}

NYA_INTERNAL u64 _nya_command_now_ms(void) {
    // the os clock and not nya_clock_get_monotonic_ms, which is the same number through a module above
    // this one. It is the division that platform/clock does, written here so base calls downwards only.
    return nya_os_time_monotonic_ns() / 1'000'000ULL;
}
