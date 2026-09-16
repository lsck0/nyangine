#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Most reads one non-blocking drain makes, so a chatty child cannot starve the others being polled. */
#define _NYA_COMMAND_DRAIN_MAX_READS 64

/**
 * Reads the child's pipes into the captured output and closes each one at end of file. Blocks until
 * both are closed when `block` is set, otherwise returns once nothing more is ready.
 * */
NYA_INTERNAL void _nya_command_drain(NYA_Command* command, b8 block);

/** Fills in the results of a reaped child from its wait status. */
NYA_INTERNAL void _nya_command_finish(NYA_Command* command, s32 status);

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

    command->start_time_ms = nya_clock_get_monotonic_ms();

    s32 stdout_pipe[2];
    s32 stderr_pipe[2];
    if (pipe(stdout_pipe) != 0) return nya_error_from_errno();
    if (pipe(stderr_pipe) != 0) {
        NYA_Error error = nya_error_from_errno();
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return error;
    }

    // close-on-exec, so a command spawned while another is running does not inherit its pipes. pipe2
    // would do this atomically but is not declared under _XOPEN_SOURCE, and nothing here forks from two
    // threads at once.
    for (u32 i = 0; i < 2; i++) {
        (void)fcntl(stdout_pipe[i], F_SETFD, FD_CLOEXEC);
        (void)fcntl(stderr_pipe[i], F_SETFD, FD_CLOEXEC);
    }

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        nya_assert(command->arena != nullptr, "Arena must be provided when capturing output.");
        command->stdout_content = nya_string_create(command->arena);
        command->stderr_content = nya_string_create(command->arena);
    }

    pid_t pid = fork();
    if (pid < 0) {
        NYA_Error error = nya_error_from_errno();
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return error;
    }

    // CHILD
    if (pid == 0) {
        // the parent may ignore SIGPIPE (CI runners do), and exec keeps an ignored disposition.
        // a child like `yes | head` then prints "Broken pipe" instead of dying quietly.
        (void)signal(SIGPIPE, SIG_DFL);

        int devnull_fd = -1;
        if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
            // capture output: redirect stdout/stderr to pipe write ends
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
        } else if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_SUPPRESS)) {
            // suppress output: redirect stdout/stderr to /dev/null
            devnull_fd = open("/dev/null", O_WRONLY);
            if (devnull_fd >= 0) {
                dup2(devnull_fd, STDOUT_FILENO);
                dup2(devnull_fd, STDERR_FILENO);
            }
        }

        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        if (devnull_fd >= 0) close(devnull_fd);

        // change working directory
        if (command->working_directory != nullptr && strlen(command->working_directory) != 0) {
            if (chdir(command->working_directory) != 0) {
                perror("chdir");
                _exit(127);
            }
        }

        // set environment variables
        for (u32 i = 0; i < nya_carray_length(command->environment); i++) {
            if (command->environment[i] == nullptr) break;
            (void)putenv(command->environment[i]);
        }

        // build argv
        NYA_ConstCString* argv = nya_alloca((nya_carray_length(command->arguments) + 2) * sizeof(NYA_ConstCString));
        argv[0]                = command->program;
        nya_memcpy(argv + 1, command->arguments, nya_carray_length(command->arguments) * sizeof(NYA_ConstCString));
        argv[nya_carray_length(command->arguments) + 1] = nullptr;

        // do the thing
        // _exit, not exit: exit would run the parent's atexit handlers and flush its stdio buffers a
        // second time from this copy of the process.
        execvp(command->program, (char* const*)argv);
        perror("execvp");
        _exit(127);
    }

    // PARENT
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Handed to nya_command_wait, which is what drains and closes them. Held open until then: a
    // child writing more than a pipe buffer's worth blocks until someone reads, so closing the read
    // end here would deadlock any command that produces real output.
    command->process_handle = (u64)pid;
    command->stdout_pipe    = stdout_pipe[0];
    command->stderr_pipe    = stderr_pipe[0];

    return NYA_OK;
}

NYA_Error nya_command_wait(NYA_Command* command) {
    nya_assert(command != nullptr);
    nya_assert(command->process_handle != 0, "nya_command_wait without a matching nya_command_spawn.");

    _nya_command_drain(command, true);
    nya_assert(command->stdout_pipe < 0 && command->stderr_pipe < 0);

    // Initialised, because a failed waitpid leaves it untouched and the macros below would then be
    // reading whatever the stack held.
    s32 status = 0;
    if (waitpid((pid_t)command->process_handle, &status, 0) < 0) return nya_error_from_errno();

    _nya_command_finish(command, status);

    return NYA_OK;
}

NYA_Error nya_command_try_wait(NYA_Command* command, b8* out_finished) {
    nya_assert(command != nullptr);
    nya_assert(out_finished != nullptr);
    nya_assert(command->process_handle != 0, "nya_command_try_wait without a matching nya_command_spawn.");

    *out_finished = false;

    _nya_command_drain(command, false);

    // a child still holding its pipes is still writing, and reaping it now would lose the rest.
    if (command->stdout_pipe >= 0 || command->stderr_pipe >= 0) return NYA_OK;

    s32   status = 0;
    pid_t reaped = waitpid((pid_t)command->process_handle, &status, WNOHANG);
    if (reaped < 0) return errno == EINTR ? NYA_OK : nya_error_from_errno();
    if (reaped == 0) return NYA_OK;

    _nya_command_finish(command, status);
    *out_finished = true;

    return NYA_OK;
}

void nya_command_wait_ready(NYA_Command* const* commands, u32 count, u32 timeout_ms) {
    nya_assert(commands != nullptr);
    nya_assert(count <= NYA_COMMAND_MAX_WAIT_READY, "nya_command_wait_ready takes at most NYA_COMMAND_MAX_WAIT_READY commands.");

    struct pollfd fds[NYA_COMMAND_MAX_WAIT_READY * 2];
    u32           fd_count = 0;

    for (u32 i = 0; i < count; i++) {
        nya_assert(commands[i] != nullptr);

        if (commands[i]->stdout_pipe >= 0) fds[fd_count++] = (struct pollfd){ .fd = commands[i]->stdout_pipe, .events = POLLIN };
        if (commands[i]->stderr_pipe >= 0) fds[fd_count++] = (struct pollfd){ .fd = commands[i]->stderr_pipe, .events = POLLIN };
    }

    // with no pipe left open this is a plain sleep, which is what a child between closing its output
    // and exiting needs. an interrupted poll just returns early, which a hint may do.
    (void)poll(fd_count > 0 ? fds : nullptr, (nfds_t)fd_count, (int)timeout_ms);
}

u32 nya_platform_processor_count(void) {
    // _SC_NPROCESSORS_ONLN, not _CONF: the online count is what is actually schedulable now, which
    // is the smaller number on a machine with cores offline and the honest answer either way.
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) return 1;

    return (u32)count;
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
            if (*pipes[i] < 0) continue;
            close(*pipes[i]);
            *pipes[i] = -1;
        }
        return;
    }

    NYA_String* targets[2] = { command->stdout_content, command->stderr_content };

    // a blocking drain ends at end of file on both pipes, however long the child keeps writing.
    for (u32 reads = 0; block || reads < _NYA_COMMAND_DRAIN_MAX_READS; reads++) {
        struct pollfd fds[2] = {
            { .fd = *pipes[0], .events = POLLIN },
            { .fd = *pipes[1], .events = POLLIN },
        };
        if (fds[0].fd < 0 && fds[1].fd < 0) return;

        // poll skips negative descriptors, so a pipe already closed takes no part.
        s32 ready = poll(fds, 2, block ? -1 : 0);
        if (ready == 0) return;
        if (ready < 0) {
            if (errno == EINTR) continue;

            // nothing better to do than stop reading; the child sees a closed pipe and the wait still reaps.
            for (u32 i = 0; i < 2; i++) {
                if (*pipes[i] < 0) continue;
                close(*pipes[i]);
                *pipes[i] = -1;
            }
            return;
        }

        for (u32 i = 0; i < 2; i++) {
            if (fds[i].fd < 0) continue;
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

            u8      buffer[4096];
            ssize_t taken = read(fds[i].fd, buffer, sizeof(buffer));

            if (taken > 0) {
                // The length carrying overload, because captured output may contain zero bytes
                // and the cstring one would stop at the first.
                nya_string_extend(targets[i], &(NYA_String){ .length = (u64)taken, .items = buffer });
                continue;
            }

            if (taken < 0 && errno == EINTR) continue;

            // zero is end of file.
            close(*pipes[i]);
            *pipes[i] = -1;
        }
    }
}

NYA_INTERNAL void _nya_command_finish(NYA_Command* command, s32 status) {
    nya_assert(command != nullptr);
    nya_assert(command->process_handle != 0);

    if (WIFEXITED(status)) {
        command->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        command->exit_code = 128 + WTERMSIG(status);
    }

    u64 end_time               = nya_clock_get_monotonic_ms();
    command->execution_time_ms = end_time - command->start_time_ms;

    // Cleared so a second wait on the same command asserts rather than reaping an unrelated process
    // that happened to reuse the pid.
    command->process_handle = 0;
}
