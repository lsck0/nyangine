#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nyangine/nyangine.h"

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
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) return nya_error_from_errno();

    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        nya_assert(command->arena != nullptr, "Arena must be provided when capturing output.");
        command->stdout_content = nya_string_create(command->arena);
        command->stderr_content = nya_string_create(command->arena);
    }

    pid_t pid = fork();
    if (pid < 0) return nya_error_from_errno();

    // CHILD
    if (pid == 0) {
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
                exit(1);
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
        execvp(command->program, (char* const*)argv);
        perror("execvp");
        exit(1);
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

    pid_t pid         = (pid_t)command->process_handle;
    s32   stdout_pipe = command->stdout_pipe;
    s32   stderr_pipe = command->stderr_pipe;

    /*
     * Both pipes drained together, whichever has bytes ready.
     *
     * Reading stdout to EOF and only then starting on stderr deadlocks any child that fills the
     * stderr pipe. A pipe holds about sixty four kibibytes, so past that the child blocks on the
     * write — which means it never exits, never closes its stdout end, and the parent waits on an
     * EOF that cannot arrive while the child waits on a reader that will not come.
     *
     * Not a corner case for this API: the build system is its main user and captures the compiler's
     * output, and a compile with a few hundred diagnostics clears that bound on stderr alone.
     *
     * This is also why the read is spelled out here rather than reusing nya_file_read_string as it
     * did before — that reads one descriptor to the end, which is precisely the thing that cannot
     * be done to either of these two in isolation.
     */
    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
        struct pollfd fds[2] = {
            { .fd = stdout_pipe, .events = POLLIN },
            { .fd = stderr_pipe, .events = POLLIN },
        };
        NYA_String* targets[2] = { command->stdout_content, command->stderr_content };

        u32 still_open = 2;
        while (still_open > 0) {
            if (poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                break; // nothing better to do than stop draining; the wait below still reaps.
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

                // Zero is EOF. Retired rather than closed here, so the close below stays in one place.
                fds[i].fd = -1;
                still_open--;
            }
        }
    }

    close(stdout_pipe);
    close(stderr_pipe);

    // Initialised, because a failed waitpid leaves it untouched and the macros below would then be
    // reading whatever the stack held.
    s32 status = 0;
    if (waitpid(pid, &status, 0) < 0) return nya_error_from_errno();

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

    return NYA_OK;
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
