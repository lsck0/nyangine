#include "nyangine/os/os_process.h"

// after the engine's own header, which asks for POSIX 2008: these only declare what they declare once
// they have seen that request.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** How long a timed wait sleeps between asking again. waitpid has no timeout of its own. */
#define _NYA_OS_PROCESS_POLL_MS 1

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PIPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_OsProcessStatus nya_os_pipe_open(OUT NYA_OsPipe* out_read, OUT NYA_OsPipe* out_write) {
    if (out_read == nullptr || out_write == nullptr) return NYA_OS_PROCESS_FAILED;

    s32 ends[2];
    if (pipe(ends) != 0) return NYA_OS_PROCESS_FAILED;

    // close-on-exec, so a command spawned while another is running does not inherit its pipes. pipe2
    // would do this atomically but is not declared under _XOPEN_SOURCE, and nothing here forks from two
    // threads at once. The child's own end is put on its stdout or stderr by dup2, which clears the flag.
    (void)fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(ends[1], F_SETFD, FD_CLOEXEC);

    // the read end never blocks, so one reader can hold several children's pipes and take whatever is
    // ready on any of them; a blocking read on a silent child would be the deadlock this all avoids.
    (void)fcntl(ends[0], F_SETFL, O_NONBLOCK);

    *out_read  = (NYA_OsPipe)ends[0];
    *out_write = (NYA_OsPipe)ends[1];

    return NYA_OS_PROCESS_OK;
}

void nya_os_pipe_close(NYA_OsPipe pipe) {
    if (pipe == NYA_OS_PIPE_NONE) return;

    (void)close((s32)pipe);
}

NYA_OsProcessStatus nya_os_pipe_read(NYA_OsPipe pipe, OUT u8* buffer, u64 capacity, OUT u64* out_taken) {
    if (pipe == NYA_OS_PIPE_NONE || buffer == nullptr || capacity == 0 || out_taken == nullptr) return NYA_OS_PROCESS_FAILED;

    *out_taken = 0;

    ssize_t taken = read((s32)pipe, buffer, (size_t)capacity);
    if (taken > 0) {
        *out_taken = (u64)taken;
        return NYA_OS_PROCESS_OK;
    }

    // zero is end of file. an interrupted or empty read is not: the writer still has its end.
    if (taken == 0) return NYA_OS_PROCESS_CLOSED;
    if (errno == EAGAIN || errno == EINTR) return NYA_OS_PROCESS_PENDING;

    return NYA_OS_PROCESS_FAILED;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PROCESSES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 nya_os_process_id(void) {
    return (u32)getpid();
}

NYA_OsProcessStatus nya_os_process_spawn(const NYA_OsProcessSpawn* spawn, OUT NYA_OsProcess* out_process) {
    if (spawn == nullptr || out_process == nullptr) return NYA_OS_PROCESS_FAILED;
    if (spawn->program == nullptr || spawn->program[0] == '\0') return NYA_OS_PROCESS_FAILED;
    if (spawn->arguments == nullptr || spawn->arguments[0] == nullptr) return NYA_OS_PROCESS_FAILED;

    pid_t pid = fork();
    if (pid < 0) return NYA_OS_PROCESS_FAILED;

    // CHILD
    if (pid == 0) {
        // the parent may ignore SIGPIPE (CI runners do), and exec keeps an ignored disposition.
        // a child like `yes | head` then prints "Broken pipe" instead of dying quietly.
        (void)signal(SIGPIPE, SIG_DFL);

        s32 devnull_fd = -1;
        if (spawn->stdout_pipe != NYA_OS_PIPE_NONE) dup2((s32)spawn->stdout_pipe, STDOUT_FILENO);
        if (spawn->stderr_pipe != NYA_OS_PIPE_NONE) dup2((s32)spawn->stderr_pipe, STDERR_FILENO);

        if (spawn->stdout_pipe == NYA_OS_PIPE_NONE && spawn->stderr_pipe == NYA_OS_PIPE_NONE && spawn->suppress_output) {
            devnull_fd = open("/dev/null", O_WRONLY);
            if (devnull_fd >= 0) {
                dup2(devnull_fd, STDOUT_FILENO);
                dup2(devnull_fd, STDERR_FILENO);
            }
        }

        // the duplicates on 1 and 2 are what the child writes through; these are the originals, and
        // every one of them left open is one the parent's read end would never see end of file behind.
        if (spawn->stdout_pipe != NYA_OS_PIPE_NONE) close((s32)spawn->stdout_pipe);
        if (spawn->stderr_pipe != NYA_OS_PIPE_NONE && spawn->stderr_pipe != spawn->stdout_pipe) close((s32)spawn->stderr_pipe);
        if (devnull_fd >= 0) close(devnull_fd);

        if (spawn->working_directory != nullptr && spawn->working_directory[0] != '\0') {
            if (chdir(spawn->working_directory) != 0) {
                perror("chdir");
                _exit(127);
            }
        }

        for (u32 i = 0; spawn->environment != nullptr && spawn->environment[i] != nullptr; i++) (void)putenv(spawn->environment[i]);

        // the caller's vector, handed straight to the kernel: it is already argv, terminator and all.
        // _exit, not exit: exit would run the parent's atexit handlers and flush its stdio buffers a
        // second time from this copy of the process.
        execvp(spawn->program, (char* const*)spawn->arguments);
        perror("execvp");
        _exit(127);
    }

    // PARENT
    *out_process = (NYA_OsProcess){ .handle = (u64)pid };

    return NYA_OS_PROCESS_OK;
}

NYA_OsProcessStatus nya_os_process_wait(NYA_OsProcess process, u32 timeout_ms, OUT s32* out_exit_code) {
    if (process.handle == 0 || out_exit_code == nullptr) return NYA_OS_PROCESS_FAILED;

    u32 waited_ms = 0;

    while (true) {
        // Initialised, because a failed waitpid leaves it untouched and the macros below would then be
        // reading whatever the stack held.
        s32   status = 0;
        pid_t reaped = waitpid((pid_t)process.handle, &status, timeout_ms == NYA_OS_PROCESS_WAIT_FOREVER ? 0 : WNOHANG);

        // an interrupted wait has not learned anything yet; the caller asks again.
        if (reaped < 0) return errno == EINTR ? NYA_OS_PROCESS_PENDING : NYA_OS_PROCESS_FAILED;

        if (reaped > 0) {
            // 128 plus the signal for a killed child, which is what a shell reports and the number the
            // Windows side terminates with, so both targets answer the same thing.
            if (WIFEXITED(status)) {
                *out_exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                *out_exit_code = 128 + WTERMSIG(status);
            }

            return NYA_OS_PROCESS_OK;
        }

        if (waited_ms >= timeout_ms) return NYA_OS_PROCESS_PENDING;

        // waitpid takes no timeout, so a bounded wait is asking again on a slow tick. Only a caller that
        // wants one pays for it: zero answers at once above and forever blocks in waitpid itself.
        (void)nanosleep(&(struct timespec){ .tv_nsec = _NYA_OS_PROCESS_POLL_MS * 1'000'000L }, nullptr);
        waited_ms += _NYA_OS_PROCESS_POLL_MS;
    }
}

void nya_os_process_wait_any(const NYA_OsProcess* processes, u32 process_count, const NYA_OsPipe* pipes, u32 pipe_count, u32 timeout_ms) {
    // the pipes and not the processes: a process id cannot be waited on here without a pidfd, and it is
    // the output that has to be read anyway — a child filling a pipe cannot exit until someone does.
    (void)processes;
    (void)process_count;

    struct pollfd fds[NYA_OS_PROCESS_WAIT_PIPES_MAX];
    u32           fd_count = 0;

    for (u32 i = 0; i < pipe_count && fd_count < NYA_OS_PROCESS_WAIT_PIPES_MAX; i++) {
        if (pipes == nullptr || pipes[i] == NYA_OS_PIPE_NONE) continue;
        fds[fd_count++] = (struct pollfd){ .fd = (s32)pipes[i], .events = POLLIN };
    }

    s32 timeout = timeout_ms == NYA_OS_PROCESS_WAIT_FOREVER ? -1 : (s32)timeout_ms;

    // with no pipe left open this is a plain sleep, which is what a child between closing its output and
    // exiting needs. an interrupted poll just returns early, which a hint may do.
    if (fd_count == 0 && timeout < 0) timeout = _NYA_OS_PROCESS_POLL_MS;

    (void)poll(fd_count > 0 ? fds : nullptr, (nfds_t)fd_count, timeout);
}

NYA_OsProcessStatus nya_os_process_kill(NYA_OsProcess process) {
    if (process.handle == 0) return NYA_OS_PROCESS_FAILED;

    return kill((pid_t)process.handle, SIGKILL) == 0 ? NYA_OS_PROCESS_OK : NYA_OS_PROCESS_FAILED;
}
